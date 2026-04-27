/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/component/inject/inject.c
 * @brief "ice idf component inject" -- patch component requires file.
 *
 * Stub.  The full flow reads each component's manifest, translates
 * the @c dependencies: section into build-system @c REQUIRES /
 * @c PRIV_REQUIRES entries, and appends the relevant
 * @c __component_set_property(... MANAGED_REQUIRES ...) /
 * @c MANAGED_PRIV_REQUIRES lines to the CMake-generated requires
 * file so managed components participate in the IDF component graph.
 *
 * Invoked indirectly via @c ice_shim -- CMake's
 * `python -m idf_component_manager.prepare_components
 * ... inject_requirements ...` becomes
 * `ice idf component inject ...`.
 */
#include "ice.h"

#include "cmd/idf/component/fetch.h"
#include "cmd/idf/component/manifest.h"

#include <ctype.h>
#include <unistd.h>

static const char *opt_project_dir;
static const char *opt_lock_path;
static const char *opt_sdkconfig_json_file;
static const char *opt_interface_version;
static const char *opt_idf_path;
static const char *opt_build_dir;
static const char *opt_component_requires_file;

/* clang-format off */
static const struct cmd_manual idf_component_inject_manual = {
	.name = "ice idf component inject",
	.summary = "inject managed dep requirements into a CMake component list",

	.description =
	H_PARA("Replaces IDF's `python -m "
	       "idf_component_manager.prepare_components ... "
	       "inject_requirements ...` invocation from "
	       "@c tools/cmake/component.cmake.  Not intended for "
	       "direct use; ice_shim rewrites the Python call.")
	H_PARA("Reads the CMake-generated "
	       "@c --component_requires_file, looks up each "
	       "component's manifest, and appends the managed "
	       "@c REQUIRES / @c PRIV_REQUIRES entries derived from "
	       "the lock file, respecting the component-source "
	       "precedence (project < project_extra < project_managed "
	       "< idf_managed < idf_components)."),
};
/* clang-format on */

static const struct option cmd_idf_component_inject_opts[] = {
    OPT_STRING(0, "project_dir", &opt_project_dir, "path",
	       "project root (CMAKE_SOURCE_DIR)", NULL),
    OPT_STRING(0, "lock_path", &opt_lock_path, "path",
	       "dependencies.lock location", NULL),
    OPT_STRING(0, "sdkconfig_json_file", &opt_sdkconfig_json_file, "path",
	       "build/config/sdkconfig.json for conditional dep evaluation",
	       NULL),
    OPT_STRING(0, "interface_version", &opt_interface_version, "N",
	       "component-manager interface version (4 or 5)", NULL),
    OPT_STRING(0, "idf_path", &opt_idf_path, "path",
	       "IDF_PATH (for resolving idf-source components)", NULL),
    OPT_STRING(0, "build_dir", &opt_build_dir, "path", "CMake binary directory",
	       NULL),
    OPT_STRING(0, "component_requires_file", &opt_component_requires_file,
	       "path", "CMake-generated requires file to patch in place", NULL),
    OPT_END(),
};

int cmd_idf_component_inject(int argc, const char **argv);

const struct cmd_desc cmd_idf_component_inject_desc = {
    .name = "inject",
    .fn = cmd_idf_component_inject,
    .opts = cmd_idf_component_inject_opts,
    .manual = &idf_component_inject_manual,
};

/*
 * In-memory model for component_requires.temp.cmake.
 *
 *   __component_set_property(___<prefix>_<name> <prop> <value>)
 *
 * Lines are stored in a list of entries (one per (prefix, name)
 * pair) preserving insertion order; each entry has an ordered prop
 * list with values stored as either a parsed string list (for the
 * iterable @c REQUIRES family) or a single raw value.  Output is a
 * complete rewrite of the file so the formatting Python's @c dump()
 * produces is reproduced exactly.
 */

static const char *const ITERABLE_PROPS[] = {
    "REQUIRES",
    "PRIV_REQUIRES",
    "MANAGED_REQUIRES",
    "MANAGED_PRIV_REQUIRES",
};

static int prop_is_iterable(const char *key)
{
	for (size_t i = 0; i < sizeof(ITERABLE_PROPS) / sizeof(*ITERABLE_PROPS);
	     i++)
		if (!strcmp(key, ITERABLE_PROPS[i]))
			return 1;
	return 0;
}

struct prop {
	char *key;
	char **items; /* iterable: parsed list; non-iterable: items[0] = raw
			 value */
	size_t items_nr;
	size_t items_alloc;
	int iterable;
};

struct entry {
	char *prefix;
	char *name;
	struct prop *props;
	size_t props_nr;
	size_t props_alloc;
};

struct req_file {
	struct entry *entries;
	size_t nr;
	size_t alloc;
};

static struct entry *req_find(struct req_file *rf, const char *prefix,
			      const char *name)
{
	for (size_t i = 0; i < rf->nr; i++)
		if (!strcmp(rf->entries[i].prefix, prefix) &&
		    !strcmp(rf->entries[i].name, name))
			return &rf->entries[i];
	return NULL;
}

static struct prop *prop_find(struct entry *e, const char *key)
{
	for (size_t i = 0; i < e->props_nr; i++)
		if (!strcmp(e->props[i].key, key))
			return &e->props[i];
	return NULL;
}

/* Append @p key with no values; returns the newly added prop. */
static struct prop *prop_append(struct entry *e, const char *key)
{
	struct prop *p;
	ALLOC_GROW(e->props, e->props_nr + 1, e->props_alloc);
	p = &e->props[e->props_nr++];
	memset(p, 0, sizeof(*p));
	p->key = sbuf_strdup(key);
	p->iterable = prop_is_iterable(key);
	return p;
}

static int prop_contains(const struct prop *p, const char *item)
{
	for (size_t i = 0; i < p->items_nr; i++)
		if (!strcmp(p->items[i], item))
			return 1;
	return 0;
}

static void prop_add_unique(struct prop *p, const char *item)
{
	if (prop_contains(p, item))
		return;
	ALLOC_GROW(p->items, p->items_nr + 1, p->items_alloc);
	p->items[p->items_nr++] = sbuf_strdup(item);
}

/* "abc;def;" -> ["abc", "def"] (drops empty fields and the surrounding quotes).
 */
static void parse_iterable_value(struct prop *p, const char *raw)
{
	const char *s = raw;
	size_t len = strlen(raw);

	if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
		s++;
		len -= 2;
	}

	const char *cur = s;
	const char *end = s + len;
	while (cur < end) {
		const char *sep = memchr(cur, ';', (size_t)(end - cur));
		const char *tok_end = sep ? sep : end;
		if (tok_end > cur) {
			ALLOC_GROW(p->items, p->items_nr + 1, p->items_alloc);
			p->items[p->items_nr++] =
			    sbuf_strndup(cur, (size_t)(tok_end - cur));
		}
		if (!sep)
			break;
		cur = sep + 1;
	}
}

static int parse_line(const char *line, char **out_prefix, char **out_name,
		      char **out_prop, char **out_value)
{
	const char *p = line;
	while (*p == ' ' || *p == '\t')
		p++;
	if (strncmp(p, "__component_set_property(___", 28) != 0)
		return -1;
	p += 28;

	const char *prefix_start = p;
	while (isalnum((unsigned char)*p) || *p == '-')
		p++;
	if (p == prefix_start || *p != '_')
		return -1;
	*out_prefix = sbuf_strndup(prefix_start, (size_t)(p - prefix_start));
	p++;

	const char *name_start = p;
	while (isalnum((unsigned char)*p) || *p == '_' || *p == '-' ||
	       *p == '.' || *p == '+')
		p++;
	if (p == name_start || *p != ' ')
		goto fail_prefix;
	*out_name = sbuf_strndup(name_start, (size_t)(p - name_start));
	p++;

	const char *prop_start = p;
	while (isalnum((unsigned char)*p) || *p == '_')
		p++;
	if (p == prop_start || *p != ' ')
		goto fail_name;
	*out_prop = sbuf_strndup(prop_start, (size_t)(p - prop_start));
	p++;

	{
		const char *closing = strrchr(p, ')');
		if (!closing)
			goto fail_prop;
		*out_value = sbuf_strndup(p, (size_t)(closing - p));
	}
	return 0;

fail_prop:
	free(*out_prop);
fail_name:
	free(*out_name);
fail_prefix:
	free(*out_prefix);
	*out_prefix = *out_name = *out_prop = *out_value = NULL;
	return -1;
}

static void req_release(struct req_file *rf)
{
	for (size_t i = 0; i < rf->nr; i++) {
		struct entry *e = &rf->entries[i];
		free(e->prefix);
		free(e->name);
		for (size_t j = 0; j < e->props_nr; j++) {
			free(e->props[j].key);
			for (size_t k = 0; k < e->props[j].items_nr; k++)
				free(e->props[j].items[k]);
			free(e->props[j].items);
		}
		free(e->props);
	}
	free(rf->entries);
	memset(rf, 0, sizeof(*rf));
}

static void req_load(struct req_file *rf, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	size_t pos = 0;

	if (sbuf_read_file(&sb, path) < 0)
		die_errno("read '%s'", path);

	for (char *line; (line = sbuf_getline(sb.buf, sb.len, &pos));) {
		char *prefix = NULL, *name = NULL, *prop = NULL, *value = NULL;
		struct entry *e;
		struct prop *p;

		/* Skip blank lines (matches Python's `if line.strip()`). */
		const char *t = line;
		while (*t == ' ' || *t == '\t' || *t == '\r')
			t++;
		if (*t == '\0')
			continue;

		if (parse_line(line, &prefix, &name, &prop, &value) < 0) {
			sbuf_release(&sb);
			req_release(rf);
			die("cannot parse requires line: %s", line);
		}

		e = req_find(rf, prefix, name);
		if (!e) {
			ALLOC_GROW(rf->entries, rf->nr + 1, rf->alloc);
			e = &rf->entries[rf->nr++];
			memset(e, 0, sizeof(*e));
			e->prefix = prefix;
			e->name = name;
		} else {
			free(prefix);
			free(name);
		}

		p = prop_append(e, prop);
		free(prop);
		if (p->iterable) {
			parse_iterable_value(p, value);
			free(value);
		} else {
			ALLOC_GROW(p->items, 1, p->items_alloc);
			p->items[0] = value;
			p->items_nr = 1;
		}
	}

	sbuf_release(&sb);
}

static void req_save(const struct req_file *rf, const char *path)
{
	struct sbuf out = SBUF_INIT;

	for (size_t i = 0; i < rf->nr; i++) {
		const struct entry *e = &rf->entries[i];
		for (size_t j = 0; j < e->props_nr; j++) {
			const struct prop *p = &e->props[j];
			sbuf_addf(&out, "__component_set_property(___%s_%s %s ",
				  e->prefix, e->name, p->key);
			if (p->iterable) {
				sbuf_addch(&out, '"');
				for (size_t k = 0; k < p->items_nr; k++) {
					if (k)
						sbuf_addch(&out, ';');
					sbuf_addstr(&out, p->items[k]);
				}
				sbuf_addch(&out, '"');
			} else if (p->items_nr) {
				sbuf_addstr(&out, p->items[0]);
			}
			sbuf_addstr(&out, ")\n");
		}
	}

	if (write_file_atomic(path, out.buf, out.len) < 0)
		die_errno("write '%s'", path);
	sbuf_release(&out);
}

/*
 * Read the components-with-manifests sidecar at @c <build_dir>/
 * components_with_manifests_list.temp -- newline-separated absolute
 * component paths.  Returns a NULL-terminated list of strdup'd paths.
 */
static char **load_manifest_paths(const char *build_dir, size_t *out_nr)
{
	struct sbuf sidecar = SBUF_INIT;
	struct sbuf body = SBUF_INIT;
	char **paths = NULL;
	size_t nr = 0, alloc = 0, pos = 0;

	sbuf_addf(&sidecar, "%s/components_with_manifests_list.temp",
		  build_dir);
	if (sbuf_read_file(&body, sidecar.buf) < 0) {
		/* No sidecar -> nothing to inject. */
		sbuf_release(&sidecar);
		sbuf_release(&body);
		*out_nr = 0;
		return NULL;
	}

	for (char *line; (line = sbuf_getline(body.buf, body.len, &pos));) {
		char *t = line;
		while (*t == ' ' || *t == '\t' || *t == '\r')
			t++;
		if (*t == '\0')
			continue;
		ALLOC_GROW(paths, nr + 1, alloc);
		paths[nr++] = sbuf_strdup(t);
	}

	(void)remove(sidecar.buf); /* matches Python's os.remove() */
	sbuf_release(&sidecar);
	sbuf_release(&body);
	*out_nr = nr;
	return paths;
}

/* "/abs/path/to/foo" -> "foo" (last path segment). */
static const char *path_basename(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

int cmd_idf_component_inject(int argc, const char **argv)
{
	struct req_file rf = {0};
	char **paths = NULL;
	size_t paths_nr = 0;

	argc = parse_options(argc, argv, &cmd_idf_component_inject_desc);
	(void)argc;

	if (!opt_component_requires_file)
		die("--component_requires_file is required");
	if (!opt_build_dir)
		die("--build_dir is required");

	req_load(&rf, opt_component_requires_file);

	paths = load_manifest_paths(opt_build_dir, &paths_nr);

	for (size_t i = 0; i < paths_nr; i++) {
		const char *name = path_basename(paths[i]);
		struct entry *e = req_find(&rf, "idf", name);
		struct manifest m = MANIFEST_INIT;
		struct sbuf manifest_path = SBUF_INIT;

		if (!e) {
			/* CMake didn't produce an entry for this component
			 * (e.g. the build skipped it).  Nothing to augment. */
			continue;
		}

		sbuf_addf(&manifest_path, "%s/idf_component.yml", paths[i]);
		if (manifest_load(&m, manifest_path.buf) < 0) {
			sbuf_release(&manifest_path);
			continue;
		}
		sbuf_release(&manifest_path);

		for (size_t j = 0; j < m.deps_nr; j++) {
			const struct manifest_dep *d = &m.deps[j];
			struct sbuf bn = SBUF_INIT;
			const char *key, *managed_key;
			struct prop *p;

			/* Meta deps (just `idf`) aren't real components. */
			if (!strcmp(d->name, "idf"))
				continue;

			/* Default = private + required.  Only `is_public == 1`
			 * routes to REQUIRES; everything else to PRIV_REQUIRES,
			 * matching @c IfClause/ComponentRequirement semantics.
			 */
			if (d->is_public == 1) {
				key = "REQUIRES";
				managed_key = "MANAGED_REQUIRES";
			} else {
				key = "PRIV_REQUIRES";
				managed_key = "MANAGED_PRIV_REQUIRES";
			}

			fetch_build_name(&bn, d->name);

			p = prop_find(e, key);
			if (!p)
				p = prop_append(e, key);
			prop_add_unique(p, bn.buf);

			p = prop_find(e, managed_key);
			if (!p)
				p = prop_append(e, managed_key);
			prop_add_unique(p, bn.buf);

			sbuf_release(&bn);
		}

		manifest_release(&m);
	}

	req_save(&rf, opt_component_requires_file);

	/*
	 * TODO(phase 7b): emit the @c interface_version==4 sdkconfig
	 * backup/restore stanzas + cm_run_counter handling that Python
	 * appends after the body.  Verified against Python on a real
	 * project: the BODY lines (above) are byte-identical; the
	 * trailing CMake-script blocks are not yet generated, which is
	 * fine for projects whose managed components don't add Kconfig
	 * symbols (the common case) but breaks the second-pass kconfgen
	 * dance when they do.
	 */

	for (size_t i = 0; i < paths_nr; i++)
		free(paths[i]);
	free(paths);
	req_release(&rf);
	return 0;
}
