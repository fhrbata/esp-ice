/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/component/prepare/prepare.c
 * @brief "ice idf component prepare" -- resolve + fetch managed components.
 *
 * Stub.  The full flow reads the project manifests and existing
 * lock file, runs the version solver (see pubgrub.h) to pick
 * compatible versions, downloads anything missing into
 * @c managed_components/, and emits the CMake include file the
 * build reads back (@c managed_components_list.temp.cmake).
 *
 * Invoked indirectly via @c sitecustomize.py -- CMake's
 * `python -m idf_component_manager.prepare_components
 * ... prepare_dependencies ...` becomes
 * `ice idf component prepare ...`.  Argument names keep Python's
 * underscore style so the rewrite is transparent.
 */
#include "ice.h"

#include "cmd/idf/component/cmake_out.h"
#include "cmd/idf/component/fetch.h"
#include "cmd/idf/component/lockfile.h"
#include "cmd/idf/component/manifest.h"
#include "cmd/idf/component/solve.h"
#include "json.h"
#include "yaml.h"

#include <unistd.h>

/*
 * The default ESP Component Registry.  Manifest deps without a
 * @c service_url override resolve here.  Hard-coded for now -- a
 * config knob (e.g. @c project.registry-url) can be added once we
 * have a reason to point at a mirror.
 */
#define DEFAULT_REGISTRY_URL "https://components.espressif.com/"

static const char *opt_project_dir;
static const char *opt_lock_path;
static const char *opt_sdkconfig_json_file;
static const char *opt_interface_version;
static const char *opt_local_components_list_file;
static const char *opt_managed_components_list_file;

/* clang-format off */
static const struct cmd_manual idf_component_prepare_manual = {
	.name = "ice idf component prepare",
	.summary = "resolve and download managed components (build-system hook)",

	.description =
	H_PARA("Replaces IDF's `python -m "
	       "idf_component_manager.prepare_components ... "
	       "prepare_dependencies ...` invocation from "
	       "@c tools/cmake/build.cmake.  Not intended for direct "
	       "use; the sitecustomize.py shim installed by "
	       "@b{ice init} rewrites the Python call into this one.")
	H_PARA("Inputs: project manifests (discovered via "
	       "@c --local_components_list_file) and the existing "
	       "@c dependencies.lock.  Outputs: downloaded components "
	       "in @c managed_components/ plus the CMake include file "
	       "at @c --managed_components_list_file."),
};
/* clang-format on */

static const struct option cmd_idf_component_prepare_opts[] = {
    OPT_STRING(0, "project_dir", &opt_project_dir, "path",
	       "project root (CMAKE_SOURCE_DIR)", NULL),
    OPT_STRING(0, "lock_path", &opt_lock_path, "path",
	       "dependencies.lock location", NULL),
    OPT_STRING(0, "sdkconfig_json_file", &opt_sdkconfig_json_file, "path",
	       "build/config/sdkconfig.json for conditional dep evaluation",
	       NULL),
    OPT_STRING(0, "interface_version", &opt_interface_version, "N",
	       "component-manager interface version (4 or 5)", NULL),
    OPT_STRING(0, "local_components_list_file", &opt_local_components_list_file,
	       "path", "YAML list of local components discovered by CMake",
	       NULL),
    OPT_STRING(0, "managed_components_list_file",
	       &opt_managed_components_list_file, "path",
	       "CMake include file to write for the build to consume", NULL),
    OPT_END(),
};

int cmd_idf_component_prepare(int argc, const char **argv);

const struct cmd_desc cmd_idf_component_prepare_desc = {
    .name = "prepare",
    .fn = cmd_idf_component_prepare,
    .opts = cmd_idf_component_prepare_opts,
    .manual = &idf_component_prepare_manual,
};

/*
 * Expand @c ${VAR} references in @p raw using @c getenv().  Mirrors
 * the Python @c idf_component_tools subst_vars_in_str helper used by
 * @c LocalSource, so manifests can write paths like
 * @c "${IDF_PATH}/examples/...".  Dies if a referenced variable is
 * unset -- silent fallback would surface as a confusing "directory
 * does not exist" later.  Caller frees the returned heap string.
 */
static char *expand_vars(const char *raw)
{
	struct sbuf out = SBUF_INIT;
	const char *p = raw;

	while (*p) {
		if (p[0] == '$' && p[1] == '{') {
			const char *end = strchr(p + 2, '}');
			char *name;
			const char *val;

			if (!end)
				die("unterminated ${...} in '%s'", raw);
			name = sbuf_strndup(p + 2, (size_t)(end - p - 2));
			val = getenv(name);
			if (!val)
				die("env var '%s' referenced by '%s' is not "
				    "set",
				    name, raw);
			sbuf_addstr(&out, val);
			free(name);
			p = end + 1;
		} else {
			sbuf_addch(&out, *p++);
		}
	}
	return sbuf_detach(&out);
}

/*
 * Resolve a @c path: dep value to an absolute path.  Expands @c ${VAR}
 * references first, then -- if the result is still relative -- joins
 * it onto @p manifest_dir (the directory containing the manifest the
 * dep was declared in), matching Python @c LocalSource._get_raw_path.
 * Caller frees.
 */
static char *resolve_path_dep(const char *manifest_dir, const char *raw)
{
	char *expanded = expand_vars(raw);
	struct sbuf out = SBUF_INIT;

	if (expanded[0] == '/') {
		return expanded;
	}
	sbuf_addf(&out, "%s/%s", manifest_dir, expanded);
	free(expanded);
	return sbuf_detach(&out);
}

/* One @c local_components_list_file entry: a project-local component
 * with a manifest CMake found during component discovery. */
struct local_entry {
	char *name;
	char *path;
};

static void local_entry_release(struct local_entry *e)
{
	free(e->name);
	free(e->path);
}

/*
 * Parse @p path's @c components: list into @p out.  The file is the
 * temp YAML CMake hands us; shape is:
 *
 *   components:
 *     - name: main
 *       path: /abs/path/to/main
 *     - name: helper
 *       path: /abs/path/to/helper
 *
 * Dies on parse error.
 */
static void load_local_components(const char *path, struct local_entry **out,
				  size_t *out_nr)
{
	struct sbuf sb = SBUF_INIT;
	struct yaml_value *root;
	struct yaml_value *list;
	struct local_entry *arr = NULL;
	int n;

	if (sbuf_read_file(&sb, path) < 0)
		die_errno("read '%s'", path);

	root = yaml_parse(sb.buf, sb.len);
	sbuf_release(&sb);
	if (!root)
		die("cannot parse local components list at '%s'", path);

	list = yaml_get(root, "components");
	if (yaml_type(list) != YAML_SEQ) {
		yaml_free(root);
		die("local components list at '%s' has no 'components:' "
		    "sequence",
		    path);
	}

	n = yaml_seq_size(list);
	if (n > 0) {
		arr = calloc((size_t)n, sizeof(*arr));
		if (!arr)
			die_errno("calloc");
		for (int i = 0; i < n; i++) {
			struct yaml_value *e = yaml_seq_at(list, i);
			struct yaml_value *name = yaml_get(e, "name");
			struct yaml_value *p = yaml_get(e, "path");
			if (yaml_type(name) != YAML_STRING ||
			    yaml_type(p) != YAML_STRING) {
				yaml_free(root);
				die("malformed entry %d in '%s'", i, path);
			}
			arr[i].name = sbuf_strdup(yaml_as_string(name));
			arr[i].path = sbuf_strdup(yaml_as_string(p));
		}
	}

	yaml_free(root);
	*out = arr;
	*out_nr = (size_t)n;
}

/*
 * Build the @c cmake_managed_list inputs from a parsed lockfile and
 * a list of local components.
 *
 * Locals carry a version property only if their @c idf_component.yml
 * declares one; we load each manifest in turn.  Managed entries from
 * the lockfile that aren't @c idf or @c local become @c cmake_dl_component
 * rows -- all path strings stored in @p sb_arena so the resulting
 * arrays' @c const char * pointers stay valid until the caller
 * @c sbuf_release()s the arena.
 */
/*
 * String table built on top of @p sb_arena.  Pointers into the arena
 * are unstable -- @c sbuf_grow reallocates the buffer and silently
 * invalidates anything captured before the next append.  Callers
 * therefore stash byte offsets while writing and resolve them all
 * at once at the end via @c arena_at(), once the arena has stopped
 * growing.
 */
static size_t arena_intern(struct sbuf *sb_arena, const char *s)
{
	size_t off = sb_arena->len;
	sbuf_addstr(sb_arena, s);
	sbuf_addch(sb_arena, '\0');
	return off;
}

static const char *arena_at(const struct sbuf *sb_arena, size_t off)
{
	return sb_arena->buf + off;
}

/* Per-entry offset bundles populated during the write phase and
 * resolved to @c const char * pointers at the end. */
struct local_off {
	size_t name;
	size_t version; /**< 0 if no version; otherwise stored offset + 1. */
};

struct dl_off {
	size_t name;
	size_t abs_path;
	size_t version;
};

static void
build_cmake_inputs(const struct lockfile *lf, const struct local_entry *locals,
		   size_t locals_nr, const char *managed_components_dir,
		   struct cmake_local_component **out_locals,
		   size_t *out_nr_locals, struct cmake_dl_component **out_dl,
		   size_t *out_nr_dl, struct sbuf *sb_arena,
		   const char ***out_paths, size_t *out_nr_paths)
{
	struct cmake_local_component *lcomps = NULL;
	struct cmake_dl_component *dcomps = NULL;
	struct local_off *loff = NULL;
	struct dl_off *doff = NULL;
	size_t *path_off = NULL;
	size_t nr_dl = 0, cap_dcomps = 0, cap_doff = 0;
	size_t nr_paths = 0, cap_paths = 0;
	const char **paths = NULL;

	/*
	 * Local components: load each manifest for its version (if any),
	 * and add the component's path to the sidecar so the later
	 * @c inject step can read its dep list.
	 */
	if (locals_nr) {
		lcomps = calloc(locals_nr, sizeof(*lcomps));
		loff = calloc(locals_nr, sizeof(*loff));
		if (!lcomps || !loff)
			die_errno("calloc");
	}
	for (size_t i = 0; i < locals_nr; i++) {
		struct manifest m = MANIFEST_INIT;
		struct sbuf manifest_path = SBUF_INIT;
		int have_manifest;

		loff[i].name = arena_intern(sb_arena, locals[i].name);

		sbuf_addf(&manifest_path, "%s/idf_component.yml",
			  locals[i].path);
		have_manifest = access(manifest_path.buf, F_OK) == 0 &&
				manifest_load(&m, manifest_path.buf) == 0;
		if (have_manifest && m.version)
			loff[i].version = arena_intern(sb_arena, m.version) + 1;
		manifest_release(&m);
		sbuf_release(&manifest_path);

		/* Every local that has a manifest goes to the sidecar so
		 * inject can process its dep list -- not just those that
		 * declare a @c version: field. */
		if (have_manifest) {
			ALLOC_GROW(path_off, nr_paths + 1, cap_paths);
			path_off[nr_paths++] =
			    arena_intern(sb_arena, locals[i].path);
		}
	}

	/*
	 * Path-override deps from each local manifest.  Mirrors what
	 * Python's @c LocalSource does: the dep is treated as a
	 * @c project_managed_components entry pointing at the resolved
	 * directory, so CMake builds the component in place rather than
	 * fetching from the registry.
	 */
	for (size_t i = 0; i < locals_nr; i++) {
		struct manifest m = MANIFEST_INIT;
		struct sbuf manifest_path = SBUF_INIT;

		sbuf_addf(&manifest_path, "%s/idf_component.yml",
			  locals[i].path);
		if (access(manifest_path.buf, F_OK) != 0 ||
		    manifest_load(&m, manifest_path.buf) < 0) {
			sbuf_release(&manifest_path);
			continue;
		}
		sbuf_release(&manifest_path);

		for (size_t d = 0; d < m.deps_nr; d++) {
			const struct manifest_dep *md = &m.deps[d];
			char *resolved;
			struct manifest dep_m = MANIFEST_INIT;
			struct sbuf dep_manifest = SBUF_INIT;
			const char *ver;

			if (md->source != MANIFEST_SRC_PATH || !md->path)
				continue;

			resolved = resolve_path_dep(locals[i].path, md->path);
			if (access(resolved, F_OK) != 0)
				die("path-override dep '%s' in '%s' resolves "
				    "to '%s', which does not exist",
				    md->name, locals[i].name, resolved);

			/* Match Python @c LocalSource.versions: read the
			 * dep's own manifest if it has one, else "*". */
			sbuf_addf(&dep_manifest, "%s/idf_component.yml",
				  resolved);
			ver = "*";
			if (access(dep_manifest.buf, F_OK) == 0 &&
			    manifest_load(&dep_m, dep_manifest.buf) == 0 &&
			    dep_m.version)
				ver = dep_m.version;

			ALLOC_GROW(dcomps, nr_dl + 1, cap_dcomps);
			ALLOC_GROW(doff, nr_dl + 1, cap_doff);
			doff[nr_dl].name = arena_intern(sb_arena, md->name);
			doff[nr_dl].abs_path = arena_intern(sb_arena, resolved);
			doff[nr_dl].version = arena_intern(sb_arena, ver);
			dcomps[nr_dl].targets = NULL;
			dcomps[nr_dl].targets_nr = 0;

			manifest_release(&dep_m);
			sbuf_release(&dep_manifest);

			ALLOC_GROW(path_off, nr_paths + 1, cap_paths);
			path_off[nr_paths++] = doff[nr_dl].abs_path;

			nr_dl++;
			free(resolved);
		}

		manifest_release(&m);
	}

	/* Managed downloads: anything in the lockfile that isn't IDF/LOCAL. */
	for (size_t i = 0; lf && i < lf->nr; i++) {
		const struct lockfile_entry *e = &lf->entries[i];
		struct sbuf bn = SBUF_INIT;
		struct sbuf abs = SBUF_INIT;

		if (e->src_type == LOCKFILE_SRC_IDF ||
		    e->src_type == LOCKFILE_SRC_LOCAL ||
		    e->src_type == LOCKFILE_SRC_UNKNOWN)
			continue;

		fetch_build_name(&bn, e->name);
		sbuf_addf(&abs, "%s/%s", managed_components_dir, bn.buf);

		/* Python uses the normalized build-name everywhere -- in
		 * the @c COMPONENT_VERSION property name, in the trailing
		 * @c set(managed_components ...) list, AND in the on-disk
		 * directory path.  Match that. */
		ALLOC_GROW(dcomps, nr_dl + 1, cap_dcomps);
		ALLOC_GROW(doff, nr_dl + 1, cap_doff);
		doff[nr_dl].name = arena_intern(sb_arena, bn.buf);
		doff[nr_dl].abs_path = arena_intern(sb_arena, abs.buf);
		doff[nr_dl].version =
		    arena_intern(sb_arena, e->version ? e->version : "");
		dcomps[nr_dl].targets = NULL; /* lockfile has no targets */
		dcomps[nr_dl].targets_nr = 0;

		ALLOC_GROW(path_off, nr_paths + 1, cap_paths);
		path_off[nr_paths++] = doff[nr_dl].abs_path;

		nr_dl++;
		sbuf_release(&abs);
		sbuf_release(&bn);
	}

	/*
	 * Resolve offsets to pointers.  All arena writes are done by
	 * this point, so @c sb_arena->buf is now stable.
	 */
	for (size_t i = 0; i < locals_nr; i++) {
		lcomps[i].name = arena_at(sb_arena, loff[i].name);
		lcomps[i].version =
		    loff[i].version ? arena_at(sb_arena, loff[i].version - 1)
				    : NULL;
	}
	for (size_t i = 0; i < nr_dl; i++) {
		dcomps[i].name = arena_at(sb_arena, doff[i].name);
		dcomps[i].abs_path = arena_at(sb_arena, doff[i].abs_path);
		dcomps[i].version = arena_at(sb_arena, doff[i].version);
	}
	if (nr_paths) {
		paths = malloc(nr_paths * sizeof(*paths));
		if (!paths)
			die_errno("malloc");
		for (size_t i = 0; i < nr_paths; i++)
			paths[i] = arena_at(sb_arena, path_off[i]);
	}

	free(loff);
	free(doff);
	free(path_off);

	*out_locals = lcomps;
	*out_nr_locals = locals_nr;
	*out_dl = dcomps;
	*out_nr_dl = nr_dl;
	*out_paths = paths;
	*out_nr_paths = nr_paths;
}

/* "<dir>/components_with_manifests_list.temp" given the cmake-list
 * file's parent directory. */
static void sidecar_path(struct sbuf *out, const char *cmake_list_file)
{
	const char *slash = strrchr(cmake_list_file, '/');
	if (slash)
		sbuf_add(out, cmake_list_file,
			 (size_t)(slash - cmake_list_file));
	else
		sbuf_addch(out, '.');
	sbuf_addstr(out, "/components_with_manifests_list.temp");
}

/* -------------------------------------------------------------------- */
/* Fresh-resolve path: needed when no @c dependencies.lock exists yet.   */
/* -------------------------------------------------------------------- */

/*
 * Read @c $IDF_PATH/tools/cmake/version.cmake and return
 * "MAJOR.MINOR.PATCH" as a heap string.  Returns NULL if @c IDF_PATH
 * is unset, the file is missing, or the macros aren't found.
 */
static char *detect_idf_version(void)
{
	const char *idf_path = getenv("IDF_PATH");
	struct sbuf path = SBUF_INIT;
	struct sbuf body = SBUF_INIT;
	struct sbuf out = SBUF_INIT;
	int major = -1, minor = -1, patch = -1;
	char *result = NULL;

	if (!idf_path || !*idf_path)
		return NULL;

	sbuf_addf(&path, "%s/tools/cmake/version.cmake", idf_path);
	if (sbuf_read_file(&body, path.buf) < 0)
		goto out;

	{
		const char *p;
		if ((p = strstr(body.buf, "set(IDF_VERSION_MAJOR ")))
			major = atoi(p + sizeof("set(IDF_VERSION_MAJOR ") - 1);
		if ((p = strstr(body.buf, "set(IDF_VERSION_MINOR ")))
			minor = atoi(p + sizeof("set(IDF_VERSION_MINOR ") - 1);
		if ((p = strstr(body.buf, "set(IDF_VERSION_PATCH ")))
			patch = atoi(p + sizeof("set(IDF_VERSION_PATCH ") - 1);
	}

	if (major >= 0 && minor >= 0 && patch >= 0) {
		sbuf_addf(&out, "%d.%d.%d", major, minor, patch);
		result = sbuf_detach(&out);
	}

out:
	sbuf_release(&path);
	sbuf_release(&body);
	sbuf_release(&out);
	return result;
}

/* Read @c IDF_TARGET from @p sdkconfig_json_file; NULL on error. */
static char *detect_target(const char *sdkconfig_json_file)
{
	struct sbuf body = SBUF_INIT;
	struct json_value *root;
	const char *s;
	char *result = NULL;

	if (!sdkconfig_json_file)
		return NULL;
	if (sbuf_read_file(&body, sdkconfig_json_file) < 0)
		goto out;
	root = json_parse(body.buf, body.len);
	if (root) {
		s = json_as_string(json_get(root, "IDF_TARGET"));
		if (s)
			result = sbuf_strdup(s);
		json_free(root);
	}
out:
	sbuf_release(&body);
	return result;
}

/*
 * Walk every local component's @c idf_component.yml and collect each
 * registry-source dep as a @c solve_root.  PoC limitation: git/path
 * deps are skipped (callers of @c lockfile_save still see them via the
 * raw manifest if needed once those source kinds are wired up).
 *
 * Caller owns the returned array and the strings inside; release with
 * @c free_roots().
 */
static void collect_roots(const struct local_entry *locals, size_t locals_nr,
			  struct solve_root **out, size_t *out_nr)
{
	struct solve_root *roots = NULL;
	size_t nr = 0, alloc = 0;

	for (size_t i = 0; i < locals_nr; i++) {
		struct manifest m = MANIFEST_INIT;
		struct sbuf mp = SBUF_INIT;

		sbuf_addf(&mp, "%s/idf_component.yml", locals[i].path);
		if (access(mp.buf, F_OK) == 0 &&
		    manifest_load(&m, mp.buf) == 0) {
			for (size_t d = 0; d < m.deps_nr; d++) {
				const struct manifest_dep *md = &m.deps[d];
				if (md->source != MANIFEST_SRC_REGISTRY)
					continue;
				if (!md->name)
					continue;
				ALLOC_GROW(roots, nr + 1, alloc);
				roots[nr].name = sbuf_strdup(md->name);
				roots[nr].spec =
				    sbuf_strdup(md->spec ? md->spec : "*");
				roots[nr].is_idf = !strcmp(md->name, "idf");
				roots[nr].registry_url =
				    md->service_url
					? sbuf_strdup(md->service_url)
					: NULL;
				nr++;
			}
		}
		manifest_release(&m);
		sbuf_release(&mp);
	}
	*out = roots;
	*out_nr = nr;
}

static void free_roots(struct solve_root *roots, size_t nr)
{
	for (size_t i = 0; i < nr; i++) {
		free((char *)roots[i].name);
		free((char *)roots[i].spec);
		free((char *)roots[i].registry_url);
	}
	free(roots);
}

/* Translate @p resolved into a @c lockfile suitable for @c lockfile_save. */
static void build_lockfile(struct lockfile *lf,
			   const struct solve_resolved *resolved,
			   size_t resolved_nr, const struct solve_root *roots,
			   size_t roots_nr, const char *target)
{
	memset(lf, 0, sizeof(*lf));
	lf->lock_version = sbuf_strdup("3.0.0");
	if (target)
		lf->target = sbuf_strdup(target);

	if (roots_nr) {
		lf->direct_dependencies =
		    calloc(roots_nr, sizeof(*lf->direct_dependencies));
		if (!lf->direct_dependencies)
			die_errno("calloc");
		for (size_t i = 0; i < roots_nr; i++) {
			lf->direct_dependencies[lf->direct_dependencies_nr++] =
			    sbuf_strdup(roots[i].name);
		}
	}

	if (resolved_nr) {
		lf->entries = calloc(resolved_nr, sizeof(*lf->entries));
		if (!lf->entries)
			die_errno("calloc");
		lf->nr = resolved_nr;
		for (size_t i = 0; i < resolved_nr; i++) {
			const struct solve_resolved *r = &resolved[i];
			struct lockfile_entry *e = &lf->entries[i];

			e->name = sbuf_strdup(r->name);
			e->version = sbuf_strdup(r->version);

			if (r->is_idf) {
				e->src_type = LOCKFILE_SRC_IDF;
			} else {
				e->src_type = LOCKFILE_SRC_SERVICE;
				e->registry_url = sbuf_strdup(
				    r->registry_url ? r->registry_url
						    : DEFAULT_REGISTRY_URL);
				if (r->component_hash)
					e->component_hash =
					    sbuf_strdup(r->component_hash);
			}

			if (r->nested_nr) {
				e->nested =
				    calloc(r->nested_nr, sizeof(*e->nested));
				if (!e->nested)
					die_errno("calloc");
				e->nested_nr = r->nested_nr;
				for (size_t j = 0; j < r->nested_nr; j++) {
					e->nested[j].name =
					    sbuf_strdup(r->nested[j].name);
					e->nested[j].version =
					    sbuf_strdup(r->nested[j].version);
					if (r->nested[j].require)
						e->nested[j].require =
						    sbuf_strdup(
							r->nested[j].require);
				}
			}
		}
	}
}

int cmd_idf_component_prepare(int argc, const char **argv)
{
	struct local_entry *locals = NULL;
	size_t locals_nr = 0;
	struct lockfile lf = LOCKFILE_INIT;
	int have_lock = 0;
	struct sbuf managed_dir = SBUF_INIT;
	struct sbuf sidecar = SBUF_INIT;
	struct sbuf arena = SBUF_INIT;
	struct sbuf lock_path_buf = SBUF_INIT;
	const char *lock_path = NULL;
	struct cmake_local_component *cl = NULL;
	struct cmake_dl_component *cd = NULL;
	const char **paths = NULL;
	size_t nr_cl = 0, nr_cd = 0, nr_paths = 0;

	argc = parse_options(argc, argv, &cmd_idf_component_prepare_desc);
	(void)argc;

	if (!opt_project_dir)
		die("--project_dir is required");
	if (!opt_managed_components_list_file)
		die("--managed_components_list_file is required");

	/*
	 * Mirror the python tool's @c --lock_path resolution: empty -> a
	 * @c dependencies.lock under @c --project_dir; relative -> rooted
	 * at @c --project_dir; absolute -> used verbatim.  CMake's
	 * @c DEPENDENCIES_LOCK property is empty by default, so without
	 * this normalisation we'd write to "" and die on open.
	 */
	if (!opt_lock_path || !*opt_lock_path) {
		sbuf_addf(&lock_path_buf, "%s/dependencies.lock",
			  opt_project_dir);
		lock_path = lock_path_buf.buf;
	} else if (opt_lock_path[0] == '/') {
		lock_path = opt_lock_path;
	} else {
		sbuf_addf(&lock_path_buf, "%s/%s", opt_project_dir,
			  opt_lock_path);
		lock_path = lock_path_buf.buf;
	}

	if (opt_local_components_list_file &&
	    access(opt_local_components_list_file, F_OK) == 0)
		load_local_components(opt_local_components_list_file, &locals,
				      &locals_nr);

	if (access(lock_path, F_OK) == 0) {
		if (lockfile_load(&lf, lock_path) < 0)
			die("cannot parse lockfile '%s'", lock_path);
		have_lock = 1;
	}

	/*
	 * Fresh-resolve path.  When the project has no @c dependencies.lock
	 * yet (typical for an upstream IDF example), walk the project's
	 * manifests, run PubGrub against the registry, and write the
	 * resulting lockfile in place.  The downstream fetch + emit code
	 * is unchanged -- it sees the in-memory lockfile we built here as
	 * if it had been parsed from disk.
	 */
	if (!have_lock && locals_nr) {
		struct solve_root *roots = NULL;
		size_t roots_nr = 0;

		collect_roots(locals, locals_nr, &roots, &roots_nr);

		if (roots_nr) {
			char *idf_version = detect_idf_version();
			char *target = detect_target(opt_sdkconfig_json_file);
			struct solve_resolved *resolved = NULL;
			size_t resolved_nr = 0;
			struct sbuf err = SBUF_INIT;

			if (solve_resolve(roots, roots_nr, idf_version,
					  DEFAULT_REGISTRY_URL, &resolved,
					  &resolved_nr, &err) < 0)
				die("dependency solver failed: %s",
				    err.len ? err.buf : "no solution");
			sbuf_release(&err);

			build_lockfile(&lf, resolved, resolved_nr, roots,
				       roots_nr, target);

			if (lockfile_save(&lf, lock_path) < 0)
				die_errno("write '%s'", lock_path);

			for (size_t i = 0; i < resolved_nr; i++)
				solve_resolved_release(&resolved[i]);
			free(resolved);
			free(idf_version);
			free(target);
			have_lock = 1;
		}
		free_roots(roots, roots_nr);
	}

	sbuf_addf(&managed_dir, "%s/managed_components", opt_project_dir);

	/* Materialise everything the lockfile says we need. */
	for (size_t i = 0; have_lock && i < lf.nr; i++) {
		const struct lockfile_entry *e = &lf.entries[i];
		if (e->src_type == LOCKFILE_SRC_IDF ||
		    e->src_type == LOCKFILE_SRC_LOCAL)
			continue;
		if (fetch_component(e, managed_dir.buf) < 0)
			die("cannot fetch '%s' (%s)", e->name,
			    e->version ? e->version : "?");
	}

	build_cmake_inputs(have_lock ? &lf : NULL, locals, locals_nr,
			   managed_dir.buf, &cl, &nr_cl, &cd, &nr_cd, &arena,
			   &paths, &nr_paths);

	if (cmake_out_emit_managed_list(opt_managed_components_list_file, cl,
					nr_cl, cd, nr_cd) < 0)
		die_errno("write '%s'", opt_managed_components_list_file);

	sidecar_path(&sidecar, opt_managed_components_list_file);
	if (cmake_out_emit_components_paths(sidecar.buf, paths, nr_paths) < 0)
		die_errno("write '%s'", sidecar.buf);

	for (size_t i = 0; i < locals_nr; i++)
		local_entry_release(&locals[i]);
	free(locals);
	free(cl);
	free(cd);
	free(paths);
	if (have_lock)
		lockfile_release(&lf);
	sbuf_release(&managed_dir);
	sbuf_release(&sidecar);
	sbuf_release(&arena);
	sbuf_release(&lock_path_buf);
	return 0;
}
