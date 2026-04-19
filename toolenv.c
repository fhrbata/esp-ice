/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file toolenv.c
 * @brief Build PATH and environment from installed ESP-IDF tools.
 *
 * Reads tools.json under @p idf_path, finds which tools are actually
 * installed under @c ice_home()/tools/, and prepends their
 * export_paths to PATH.  Also sets export_vars (e.g. OPENOCD_SCRIPTS,
 * ESP_ROM_ELF_DIR).
 *
 * Called by load_profile() with the active profile's IDF path so
 * cmake child processes find the right compilers without
 * @c export.sh.
 */
#include "ice.h"

#include "json.h"

struct find_version_ctx {
	const char *parent;
	struct sbuf *result;
	int found;
};

static int find_version_cb(const char *name, void *ud)
{
	struct find_version_ctx *ctx = ud;
	struct sbuf check = SBUF_INIT;

	sbuf_addf(&check, "%s/%s", ctx->parent, name);
	if (is_directory(check.buf) && !ctx->found) {
		sbuf_addf(ctx->result, "%s/%s", ctx->parent, name);
		ctx->found = 1;
	}
	sbuf_release(&check);
	return ctx->found; /* stop after first */
}

/**
 * Find the installed version directory for a tool.
 * Returns a pointer into @p buf (caller owns), or NULL if not installed.
 */
static const char *find_installed_version(const char *tools_dir,
					  const char *tool_name,
					  struct sbuf *buf)
{
	struct sbuf dir = SBUF_INIT;
	struct find_version_ctx ctx;

	sbuf_addf(&dir, "%s/tools/%s", tools_dir, tool_name);

	if (!is_directory(dir.buf)) {
		sbuf_release(&dir);
		return NULL;
	}

	ctx.parent = dir.buf;
	ctx.result = buf;
	ctx.found = 0;
	dir_foreach(dir.buf, find_version_cb, &ctx);

	sbuf_release(&dir);
	return ctx.found ? buf->buf : NULL;
}

void setup_tool_env(const char *idf_path)
{
	const char *tools_dir;
	struct sbuf manifest_path = SBUF_INIT;
	struct sbuf manifest = SBUF_INIT;
	struct json_value *root;
	struct json_value *tools;
	struct sbuf path_prepend = SBUF_INIT;
	const char *old_path;
	int n;

	if (!idf_path || !*idf_path)
		return; /* no IDF, nothing to do */

	tools_dir = ice_home();

	sbuf_addf(&manifest_path, "%s/tools/tools.json", idf_path);
	if (sbuf_read_file(&manifest, manifest_path.buf) < 0)
		goto out; /* no tools.json, silently skip */

	root = json_parse(manifest.buf, manifest.len);
	if (!root)
		goto out;

	tools = json_get(root, "tools");
	n = json_array_size(tools);

	for (int i = 0; i < n; i++) {
		struct json_value *tool = json_array_at(tools, i);
		const char *name = json_as_string(json_get(tool, "name"));
		struct json_value *export_paths =
		    json_get(tool, "export_paths");
		struct json_value *export_vars = json_get(tool, "export_vars");
		struct sbuf ver_path = SBUF_INIT;
		const char *installed;

		if (!name)
			continue;

		installed = find_installed_version(tools_dir, name, &ver_path);
		if (!installed) {
			sbuf_release(&ver_path);
			continue;
		}

		/* Append export_paths entries to PATH prepend string. */
		for (int j = 0; j < json_array_size(export_paths); j++) {
			struct json_value *segments =
			    json_array_at(export_paths, j);
			struct sbuf entry = SBUF_INIT;

			sbuf_addstr(&entry, installed);
			for (int k = 0; k < json_array_size(segments); k++) {
				const char *seg =
				    json_as_string(json_array_at(segments, k));
				if (seg && *seg) {
					sbuf_addch(&entry, '/');
					sbuf_addstr(&entry, seg);
				}
			}

			if (is_directory(entry.buf)) {
				if (path_prepend.len)
					sbuf_addch(&path_prepend, ':');
				sbuf_addstr(&path_prepend, entry.buf);
			}
			sbuf_release(&entry);
		}

		/* Set export_vars, replacing ${TOOL_PATH} with the
		 * installed version path. */
		/* Walk the JSON object keys for export_vars. */
		if (export_vars) {
			/* export_vars is an object -- iterate its keys
			 * by checking known ones from tools.json. Common
			 * keys: OPENOCD_SCRIPTS, ESP_ROM_ELF_DIR,
			 * ESP_CLANG_LIBS_PATH, IDF_CCACHE_ENABLE. */
			static const char *known_vars[] = {
			    "OPENOCD_SCRIPTS",
			    "ESP_ROM_ELF_DIR",
			    "ESP_CLANG_LIBS_PATH",
			    "IDF_CCACHE_ENABLE",
			    NULL,
			};

			for (const char **kv = known_vars; *kv; kv++) {
				const char *val =
				    json_as_string(json_get(export_vars, *kv));
				if (!val)
					continue;

				struct sbuf envstr = SBUF_INIT;
				sbuf_addf(&envstr, "%s=", *kv);

				/* Replace ${TOOL_PATH} */
				const char *p = val;
				while (*p) {
					if (!strncmp(p, "${TOOL_PATH}", 12)) {
						sbuf_addstr(&envstr, installed);
						p += 12;
					} else {
						sbuf_addch(&envstr, *p);
						p++;
					}
				}

				putenv(sbuf_detach(&envstr));
			}
		}

		sbuf_release(&ver_path);
	}

	/* Prepend tool paths to existing PATH. */
	if (path_prepend.len) {
		old_path = getenv("PATH");
		if (old_path) {
			sbuf_addch(&path_prepend, ':');
			sbuf_addstr(&path_prepend, old_path);
		}
		{
			struct sbuf env = SBUF_INIT;
			sbuf_addf(&env, "PATH=%s", path_prepend.buf);
			putenv(sbuf_detach(&env));
		}
	}

	json_free(root);
out:
	sbuf_release(&manifest_path);
	sbuf_release(&manifest);
	sbuf_release(&path_prepend);
}
