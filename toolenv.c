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

/**
 * Pick the best installed version of @p tool for this IDF.  Prefers
 * the @c "recommended" version if its directory exists; otherwise
 * falls back to the highest-named installed @c "supported" version;
 * otherwise NULL.  Mirrors @c get_preferred_installed_version in
 * @b{esp-idf/tools/idf_tools.py} so a minor drift in tools.json (e.g.
 * the recommended version bumps between @b{ice init} runs) keeps an
 * older-but-still-supported install usable.  @c "deprecated" entries
 * are ignored.
 *
 * The candidate set comes from the IDF's own tools.json, not from
 * @c readdir on the tools directory -- a sibling toolchain left over
 * from a previous @b{ice init} against a different IDF won't appear
 * here, even if it happens to be alphabetically first on disk.
 *
 * Returned pointer is owned by the JSON tree.
 */
static const char *preferred_installed_version(const char *tools_dir,
					       const char *tool_name,
					       struct json_value *tool)
{
	struct json_value *versions = json_get(tool, "versions");
	int n = json_array_size(versions);
	const char *best = NULL;
	struct sbuf path = SBUF_INIT;

	for (int i = 0; i < n; i++) {
		struct json_value *v = json_array_at(versions, i);
		const char *status = json_as_string(json_get(v, "status"));
		const char *name = json_as_string(json_get(v, "name"));

		if (!name || !status)
			continue;

		sbuf_reset(&path);
		sbuf_addf(&path, "%s/tools/%s/%s", tools_dir, tool_name, name);
		if (!is_directory(path.buf))
			continue;

		if (!strcmp(status, "recommended")) {
			best = name;
			break;
		}
		if (!strcmp(status, "supported")) {
			if (!best || strcmp(name, best) > 0)
				best = name;
		}
	}

	sbuf_release(&path);
	return best;
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
		const char *ver_name;
		struct sbuf ver_path = SBUF_INIT;
		const char *installed;

		if (!name)
			continue;

		ver_name = preferred_installed_version(tools_dir, name, tool);
		if (!ver_name) {
			sbuf_release(&ver_path);
			continue;
		}

		sbuf_addf(&ver_path, "%s/tools/%s/%s", tools_dir, name,
			  ver_name);
		installed = ver_path.buf;

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

				setenv(*kv, envstr.buf, 1);
				sbuf_release(&envstr);
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
		setenv("PATH", path_prepend.buf, 1);
	}

	json_free(root);
out:
	sbuf_release(&manifest_path);
	sbuf_release(&manifest);
	sbuf_release(&path_prepend);
}
