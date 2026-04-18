/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file idf.c
 * @brief `ice idf` -- manage the ESP-IDF source tree.
 *
 * Subcommands:
 *   clone   - clone esp-idf into ~/.ice/esp-idf/repo
 *   repo    - point ice at an existing esp-idf clone
 *   switch  - checkout a version (tag/branch) and update submodules
 *   list    - list available versions (git tags)
 *   pull    - fetch latest from upstream (managed repo only)
 *   info    - show current IDF path and version
 */
#include "ice.h"

#include "fs.h"

#define IDF_CLONE_URL "https://github.com/espressif/esp-idf.git"

/*
 * Minimum supported IDF major.minor -- tags and release branches
 * older than this are filtered from `ice idf list`.
 */
#define IDF_MIN_MAJOR 5
#define IDF_MIN_MINOR 0

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int version_supported(const char *ver);

/** Return ~/.ice/esp-idf/base.git (bare object store). */
static const char *base_repo_path(void)
{
	static struct sbuf path = SBUF_INIT;

	if (!path.len)
		sbuf_addf(&path, "%s/esp-idf/base.git", ice_home());
	return path.buf;
}

/** Return ~/.ice/esp-idf/current (active working tree). */
static const char *current_tree_path(void)
{
	static struct sbuf path = SBUF_INIT;

	if (!path.len)
		sbuf_addf(&path, "%s/esp-idf/current", ice_home());
	return path.buf;
}

/**
 * Return the active IDF path.
 * idf.path config > managed current tree > NULL.
 */
static const char *idf_path(void)
{
	const char *configured = config_get("idf.path");

	if (configured && *configured)
		return configured;

	if (!access(current_tree_path(), F_OK))
		return current_tree_path();

	return NULL;
}

/** Check whether @p path looks like an esp-idf tree. */
static int is_idf_repo(const char *path)
{
	struct sbuf check = SBUF_INIT;
	int ok;

	sbuf_addf(&check, "%s/tools/tools.json", path);
	ok = !access(check.buf, F_OK);
	sbuf_release(&check);
	return ok;
}

/** Run a git command in @p dir.  Returns the exit code. */
static int run_git(const char *dir, const char **argv)
{
	struct process proc = PROCESS_INIT;

	proc.argv = argv;
	proc.dir = dir;
	return process_run(&proc);
}

/** Run a git command and capture stdout into @p out. */
static int run_git_capture(const char *dir, const char **argv, struct sbuf *out)
{
	struct process proc = PROCESS_INIT;
	char buf[4096];
	ssize_t n;
	int rc;

	proc.argv = argv;
	proc.dir = dir;
	proc.pipe_out = 1;
	if (process_start(&proc))
		return -1;

	while ((n = read(proc.out, buf, sizeof(buf))) > 0)
		sbuf_add(out, buf, (size_t)n);

	rc = process_finish(&proc);
	return rc;
}

/**
 * Describes one entry from .gitmodules after URL resolution.
 */
struct submod {
	char *name; /**< section name, e.g. "components/esp_wifi/lib" */
	char *path; /**< worktree path (may equal name) */
	char *url;  /**< absolute URL (relative paths already resolved) */
};

static void submod_release(struct submod *m)
{
	free(m->name);
	free(m->path);
	free(m->url);
}

static struct submod *submod_find(struct submod *list, size_t nr,
				  const char *name)
{
	for (size_t i = 0; i < nr; i++)
		if (!strcmp(list[i].name, name))
			return &list[i];
	return NULL;
}

/**
 * Resolve a possibly-relative submodule URL against the superproject's URL.
 *
 * Only URLs starting with "./" or "../" are treated as relative.  Each
 * "../" strips one path component from the superproject's URL, stopping
 * at the "scheme://" boundary.  Other forms (https://…, git@host:…,
 * /abs/path) are returned as a plain copy.
 */
static char *resolve_submod_url(const char *super, const char *rel)
{
	struct sbuf out = SBUF_INIT;
	const char *p = rel;

	if (p[0] != '.' || (p[1] != '/' && !(p[1] == '.' && p[2] == '/')))
		return sbuf_strdup(rel);

	sbuf_addstr(&out, super);
	while (out.len && out.buf[out.len - 1] == '/')
		sbuf_setlen(&out, out.len - 1);

	while (*p) {
		if (p[0] == '.' && p[1] == '/') {
			p += 2;
		} else if (p[0] == '.' && p[1] == '.' && p[2] == '/') {
			const char *scheme = strstr(out.buf, "://");
			size_t floor =
			    scheme ? (size_t)(scheme - out.buf) + 3 : 0;
			size_t cut = out.len;
			while (cut > floor && out.buf[cut - 1] != '/')
				cut--;
			if (cut > floor)
				sbuf_setlen(&out, cut - 1);
			p += 3;
		} else {
			break;
		}
	}

	sbuf_addch(&out, '/');
	sbuf_addstr(&out, p);
	return sbuf_detach(&out);
}

/**
 * Append submodules declared at @p base's <@p refname>:.gitmodules to
 * @p mods, deduplicating by name.  Absolute URLs are resolved against
 * @p super_url.
 *
 * Uses `git cat-file -p <ref>:.gitmodules` to get the raw INI, then
 * parses it via config_load_buf so git-style `[section "subsection"]`
 * is handled uniformly with the rest of the codebase.
 */
static void read_submodules_at(const char *base, const char *super_url,
			       const char *refname, struct submod **mods,
			       size_t *nr, size_t *alloc)
{
	struct sbuf blob = SBUF_INIT;
	struct sbuf content = SBUF_INIT;
	struct config cfg;
	int i;

	sbuf_addf(&blob, "%s:.gitmodules", refname);

	const char *argv[] = {"git", "cat-file", "-p", blob.buf, NULL};
	if (run_git_capture(base, argv, &content) != 0) {
		sbuf_release(&content);
		sbuf_release(&blob);
		return;
	}

	config_init(&cfg);
	config_load_buf(&cfg, CONFIG_SCOPE_DEFAULT, blob.buf, content.buf,
			content.len);
	sbuf_release(&blob);
	sbuf_release(&content);

	for (i = 0; i < cfg.nr; i++) {
		const char *key = cfg.entries[i].key;
		const char *value = cfg.entries[i].value;
		const char *dot;
		size_t klen = strlen(key);
		struct submod *m;
		char *name;
		int is_path, is_url;

		if (strncmp(key, "submodule.", 10) != 0)
			continue;
		is_path = klen > 5 && !strcmp(key + klen - 5, ".path");
		is_url =
		    !is_path && klen > 4 && !strcmp(key + klen - 4, ".url");
		if (!is_path && !is_url)
			continue;

		/* Extract submodule name: between "submodule." and
		 * ".path"/".url". */
		dot = key + 10;
		name = sbuf_strndup(dot, klen - 10 - (is_path ? 5 : 4));

		m = submod_find(*mods, *nr, name);
		if (!m) {
			ALLOC_GROW(*mods, *nr + 1, *alloc);
			m = &(*mods)[(*nr)++];
			m->name = name;
			m->path = NULL;
			m->url = NULL;
		} else {
			free(name);
		}

		if (is_path) {
			if (!m->path)
				m->path = sbuf_strdup(value);
		} else if (!m->url) {
			m->url = resolve_submod_url(super_url, value);
		}
	}

	config_release(&cfg);
}

/**
 * Parse .gitmodules across master plus every `release/v*` branch and
 * return the union of declared submodules.  This captures submodules
 * that were removed in master but are still referenced by older
 * supported releases.
 *
 * @return number of submodules on success (may be 0).
 *         Caller must submod_release() each entry and free(*out).
 */
static int read_submodules(const char *base, const char *super_url,
			   struct submod **out)
{
	struct submod *mods = NULL;
	size_t nr = 0, alloc = 0;
	struct sbuf branches = SBUF_INIT;
	size_t pos = 0;
	char *line;

	*out = NULL;

	/* Always scan default HEAD first. */
	read_submodules_at(base, super_url, "HEAD", &mods, &nr, &alloc);

	/* Enumerate local branches in the bare repo: master + release/v*. */
	{
		const char *argv[] = {"git",
				      "branch",
				      "--list",
				      "--format",
				      "%(refname:short)",
				      "master",
				      "release/v*",
				      NULL};
		if (run_git_capture(base, argv, &branches) != 0) {
			sbuf_release(&branches);
			goto done;
		}
	}

	while ((line = sbuf_getline(branches.buf, branches.len, &pos))) {
		while (*line == ' ' || *line == '*')
			line++;
		if (!*line)
			continue;
		if (strcmp(line, "master") != 0 &&
		    (strncmp(line, "release/v", 9) != 0 ||
		     !version_supported(line + 8) || strchr(line + 9, '_')))
			continue;
		read_submodules_at(base, super_url, line, &mods, &nr, &alloc);
	}
	sbuf_release(&branches);

done:
	/* Drop entries missing path or url. */
	{
		size_t valid = 0;
		for (size_t i = 0; i < nr; i++) {
			if (mods[i].path && mods[i].url) {
				if (valid != i)
					mods[valid] = mods[i];
				valid++;
			} else {
				submod_release(&mods[i]);
			}
		}
		nr = valid;
	}

	*out = mods;
	return (int)nr;
}

/**
 * Find the submodule gitdir inside a reference repo for path @p sub_path.
 * Looks at @p ref/.git/modules/<path> (working-tree layout) first, then
 * @p ref/modules/<path> (bare layout).  Shallow gitdirs are rejected
 * because `git clone --reference` refuses them.
 *
 * Returns a malloc'd path or NULL.
 */
static char *find_reference_moduledir(const char *ref, const char *sub_path)
{
	struct sbuf p = SBUF_INIT;
	size_t dir_len;

	sbuf_addf(&p, "%s/.git/modules/%s", ref, sub_path);
	if (!is_directory(p.buf)) {
		sbuf_reset(&p);
		sbuf_addf(&p, "%s/modules/%s", ref, sub_path);
		if (!is_directory(p.buf)) {
			sbuf_release(&p);
			return NULL;
		}
	}

	dir_len = p.len;
	sbuf_addstr(&p, "/shallow");
	if (!access(p.buf, F_OK)) {
		sbuf_release(&p);
		return NULL;
	}
	sbuf_setlen(&p, dir_len);
	return sbuf_detach(&p);
}

/**
 * Clone @p mods as bare repositories into @p parent_bare/modules/<path>/,
 * up to @p jobs processes in parallel.  When @p reference_parent is
 * non-NULL and has a matching submodule gitdir under it, that gitdir
 * is passed as --reference for object reuse.  Existing destinations
 * are skipped.
 *
 * @p label_prefix is prepended to progress lines (use "" for top level,
 * or the parent's path for nested modules).
 *
 * @return number of submodules that failed to clone.
 */
static int clone_submodules_parallel(const char *parent_bare,
				     const char *reference_parent,
				     const char *label_prefix,
				     const struct submod *mods, size_t nmods,
				     int jobs)
{
	int errors = 0;
	size_t i;

	if (jobs < 1)
		jobs = 1;

	for (i = 0; i < nmods;) {
		size_t batch =
		    nmods - i < (size_t)jobs ? nmods - i : (size_t)jobs;
		struct process procs[16] = {0};
		struct svec argvs[16] = {0};
		char *dests[16] = {0};
		char *refs[16] = {0};

		for (size_t j = 0; j < batch; j++) {
			const struct submod *m = &mods[i + j];
			struct sbuf dst = SBUF_INIT;

			sbuf_addf(&dst, "%s/modules/%s", parent_bare, m->path);
			dests[j] = sbuf_detach(&dst);

			if (is_directory(dests[j])) {
				fprintf(stderr, "  skip @b{%s%s%s} (exists)\n",
					label_prefix, *label_prefix ? "/" : "",
					m->path);
				continue;
			}

			if (mkdirp_for_file(dests[j]) < 0) {
				warn_errno("mkdir for '%s'", dests[j]);
				errors++;
				continue;
			}

			if (reference_parent)
				refs[j] = find_reference_moduledir(
				    reference_parent, m->path);

			svec_push(&argvs[j], "git");
			svec_push(&argvs[j], "clone");
			svec_push(&argvs[j], "--bare");
			svec_push(&argvs[j], "--quiet");
			if (refs[j]) {
				svec_push(&argvs[j], "--reference");
				svec_push(&argvs[j], refs[j]);
			}
			svec_push(&argvs[j], m->url);
			svec_push(&argvs[j], dests[j]);

			procs[j].argv = argvs[j].v;

			fprintf(stderr, "  fetch @b{%s%s%s}\n", label_prefix,
				*label_prefix ? "/" : "", m->path);
			if (process_start(&procs[j]) != 0) {
				warn("could not start git clone for '%s'",
				     m->path);
				errors++;
			}
		}

		for (size_t j = 0; j < batch; j++) {
			if (procs[j].pid > 0) {
				int rc = process_finish(&procs[j]);
				if (rc != 0) {
					warn("submodule '%s' clone failed",
					     mods[i + j].path);
					errors++;
				}
			}
			svec_clear(&argvs[j]);
			free(dests[j]);
			free(refs[j]);
		}

		i += batch;
	}

	return errors;
}

/**
 * Read .gitmodules at HEAD of repo @p base (typically a submodule's
 * bare clone) and return the declared sub-submodules with URLs
 * resolved against @p super_url.  Caller must release each entry.
 */
static int read_submodules_head(const char *base, const char *super_url,
				struct submod **out)
{
	struct submod *mods = NULL;
	size_t nr = 0, alloc = 0, valid = 0;

	*out = NULL;
	read_submodules_at(base, super_url, "HEAD", &mods, &nr, &alloc);

	for (size_t i = 0; i < nr; i++) {
		if (mods[i].path && mods[i].url) {
			if (valid != i)
				mods[valid] = mods[i];
			valid++;
		} else {
			submod_release(&mods[i]);
		}
	}
	*out = mods;
	return (int)valid;
}

/**
 * After the top-level submodules have been cloned under @p base/modules/,
 * walk each one and fetch any nested sub-submodules it declares.
 * Recurses up to @p max_depth levels.
 */
static int clone_subsubmodules(const char *base, const char *reference,
			       const char *label_prefix,
			       const struct submod *parents, size_t nparents,
			       int jobs, int max_depth)
{
	int errors = 0;

	if (max_depth <= 0)
		return 0;

	for (size_t i = 0; i < nparents; i++) {
		const struct submod *p = &parents[i];
		struct sbuf parent_bare = SBUF_INIT;
		struct sbuf parent_label = SBUF_INIT;
		char *parent_ref = NULL;
		struct submod *subs;
		int nsubs;

		sbuf_addf(&parent_bare, "%s/modules/%s", base, p->path);
		if (!is_directory(parent_bare.buf)) {
			sbuf_release(&parent_bare);
			continue;
		}

		nsubs = read_submodules_head(parent_bare.buf, p->url, &subs);
		if (nsubs <= 0) {
			sbuf_release(&parent_bare);
			continue;
		}

		sbuf_addf(&parent_label, "%s%s%s", label_prefix,
			  *label_prefix ? "/" : "", p->path);

		if (reference)
			parent_ref =
			    find_reference_moduledir(reference, p->path);

		fprintf(stderr,
			"Fetching %d nested submodule%s under @b{%s} ...\n",
			nsubs, nsubs == 1 ? "" : "s", parent_label.buf);

		errors += clone_submodules_parallel(parent_bare.buf, parent_ref,
						    parent_label.buf, subs,
						    (size_t)nsubs, jobs);

		errors += clone_subsubmodules(
		    parent_bare.buf, parent_ref, parent_label.buf, subs,
		    (size_t)nsubs, jobs, max_depth - 1);

		for (int k = 0; k < nsubs; k++)
			submod_release(&subs[k]);
		free(subs);
		free(parent_ref);
		sbuf_release(&parent_bare);
		sbuf_release(&parent_label);
	}

	return errors;
}

/**
 * Populate submodules in @p tree_path for version @p version.
 *
 * For each gitlink (160000) entry in the tree, create a --shared
 * clone from the base repo's .git/modules/<path>/ and checkout
 * the pinned commit.  Handles one level of nested submodules.
 *
 * @p base is the path to a repo that has .git/modules/ populated
 * (either the base.git bare repo or a user-provided repo).
 */
static int populate_submodules(const char *base, const char *tree_path,
			       const char *version)
{
	struct sbuf ls = SBUF_INIT;
	size_t pos;
	char *line;
	int errors = 0;

	/* Get all gitlink entries for this version. */
	{
		const char *argv[] = {"git", "ls-tree", "-r", version, NULL};
		if (run_git_capture(base, argv, &ls) != 0) {
			sbuf_release(&ls);
			return -1;
		}
	}

	pos = 0;
	while ((line = sbuf_getline(ls.buf, ls.len, &pos))) {
		char mode[8], type[8], sha[64], path[1024];

		if (sscanf(line, "%7s %7s %63s %1023[^\n]", mode, type, sha,
			   path) != 4)
			continue;
		if (strcmp(mode, "160000") != 0)
			continue;

		struct sbuf mod_git = SBUF_INIT;
		struct sbuf dest = SBUF_INIT;

		sbuf_addf(&mod_git, "%s/.git/modules/%s", base, path);
		sbuf_addf(&dest, "%s/%s", tree_path, path);

		/* If the module object store doesn't exist, skip. */
		if (!is_directory(mod_git.buf)) {
			/* Try bare repo layout (modules/ at top level). */
			sbuf_release(&mod_git);
			sbuf_addf(&mod_git, "%s/modules/%s", base, path);
			if (!is_directory(mod_git.buf)) {
				warn("submodule '%s': no local objects, "
				     "skipping",
				     path);
				errors++;
				goto next;
			}
		}

		/* Remove existing dir and clone --shared. */
		if (is_directory(dest.buf))
			rmtree(dest.buf, 0);
		mkdirp_for_file(dest.buf);

		{
			const char *clone_argv[] = {
			    "git",     "clone",	    "--shared", "--no-checkout",
			    "--quiet", mod_git.buf, dest.buf,	NULL};
			if (run_git(NULL, clone_argv) != 0) {
				warn("submodule '%s': clone failed", path);
				errors++;
				goto next;
			}
		}

		{
			const char *co_argv[] = {"git", "checkout", "--quiet",
						 sha, NULL};
			if (run_git(dest.buf, co_argv) != 0) {
				warn("submodule '%s': checkout %s failed", path,
				     sha);
				errors++;
				goto next;
			}
		}

		/* Handle nested submodules (one level deep). */
		{
			struct sbuf nested_ls = SBUF_INIT;
			const char *nested_argv[] = {"git", "ls-tree", sha,
						     NULL};

			if (run_git_capture(mod_git.buf, nested_argv,
					    &nested_ls) == 0) {
				size_t npos = 0;
				char *nline;

				while ((nline = sbuf_getline(nested_ls.buf,
							     nested_ls.len,
							     &npos))) {
					char nm[8], nt[8], ns[64], np[1024];
					if (sscanf(nline,
						   "%7s %7s %63s "
						   "%1023[^\n]",
						   nm, nt, ns, np) != 4)
						continue;
					if (strcmp(nm, "160000") != 0)
						continue;

					struct sbuf nmod = SBUF_INIT;
					struct sbuf ndest = SBUF_INIT;

					sbuf_addf(&nmod, "%s/modules/%s",
						  mod_git.buf, np);
					sbuf_addf(&ndest, "%s/%s", dest.buf,
						  np);

					if (!is_directory(nmod.buf)) {
						sbuf_release(&nmod);
						sbuf_release(&ndest);
						continue;
					}

					if (is_directory(ndest.buf))
						rmtree(ndest.buf, 0);
					mkdirp_for_file(ndest.buf);

					const char *nc[] = {
					    "git",	"clone",
					    "--shared", "--no-checkout",
					    "--quiet",	nmod.buf,
					    ndest.buf,	NULL};
					if (run_git(NULL, nc) == 0) {
						const char *nco[] = {
						    "git", "checkout",
						    "--quiet", ns, NULL};
						run_git(ndest.buf, nco);
					}

					sbuf_release(&nmod);
					sbuf_release(&ndest);
				}
			}
			sbuf_release(&nested_ls);
		}

	next:
		sbuf_release(&mod_git);
		sbuf_release(&dest);
	}

	sbuf_release(&ls);
	return errors ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Subcommands                                                         */
/* ------------------------------------------------------------------ */

static const char *clone_reference;
static int clone_jobs = 8;

static const struct option clone_opts[] = {
    OPT_STRING(0, "reference", &clone_reference, "path",
	       "borrow objects from an existing esp-idf clone", NULL),
    OPT_INT('j', "jobs", &clone_jobs, "n",
	    "parallel submodule clones (default 8)", NULL),
    OPT_END_COMPLETE("[url]", NULL),
};

static int cmd_idf_clone(int argc, const char **argv)
{
	static const struct cmd_manual manual = {.name = "ice idf clone"};
	const char *base = base_repo_path();
	const char *url;
	struct submod *mods;
	int nmods;
	int errors = 0;

	argc = parse_options(argc, argv, clone_opts, &manual);
	url = argc >= 1 ? argv[0] : IDF_CLONE_URL;

	if (!access(base, F_OK)) {
		fprintf(stderr,
			"ESP-IDF already cloned at @b{%s}\n"
			"hint: use @b{ice idf pull} to update\n",
			base);
		return 0;
	}

	{
		struct sbuf parent = SBUF_INIT;
		sbuf_addf(&parent, "%s/esp-idf", ice_home());
		if (mkdirp(parent.buf) < 0)
			die_errno("cannot create '%s'", parent.buf);
		sbuf_release(&parent);
	}

	/* Step 1: bare clone of the superproject. */
	fprintf(stderr, "Cloning @b{%s} into @b{%s} ...\n", url, base);
	{
		struct svec args = SVEC_INIT;

		svec_push(&args, "git");
		svec_push(&args, "clone");
		svec_push(&args, "--bare");
		if (clone_reference) {
			svec_push(&args, "--reference");
			svec_push(&args, clone_reference);
		}
		svec_push(&args, url);
		svec_push(&args, base);
		if (run_git(NULL, args.v) != 0)
			die("git clone failed");
		svec_clear(&args);
	}

	/* Step 2: parse .gitmodules and clone each submodule as bare. */
	nmods = read_submodules(base, url, &mods);
	if (nmods <= 0) {
		fprintf(stderr, "No submodules declared.\n@g{done}\n");
		return 0;
	}

	fprintf(stderr, "Fetching %d submodule%s (up to %d in parallel) ...\n",
		nmods, nmods == 1 ? "" : "s", clone_jobs);
	errors = clone_submodules_parallel(base, clone_reference, "", mods,
					   (size_t)nmods, clone_jobs);

	/* Recurse one level into nested submodules (e.g. openthread's mbedtls).
	 */
	errors += clone_subsubmodules(base, clone_reference, "", mods,
				      (size_t)nmods, clone_jobs, 2);

	for (int i = 0; i < nmods; i++)
		submod_release(&mods[i]);
	free(mods);

	if (errors) {
		warn("%d submodule%s failed to clone", errors,
		     errors == 1 ? "" : "s");
		return 1;
	}

	fprintf(stderr, "@g{done}\n");
	return 0;
}

static int cmd_idf_repo(int argc, const char **argv)
{
	static const struct cmd_manual manual = {.name = "ice idf repo"};
	struct option opts[] = {OPT_END_COMPLETE("path", NULL)};
	const char *path;
	struct config c;
	const char *cfg_path;

	argc = parse_options(argc, argv, opts, &manual);
	if (argc < 1)
		die("missing <path> argument");

	path = argv[0];

	if (!is_idf_repo(path))
		die("'%s' does not look like an ESP-IDF tree "
		    "(tools/tools.json not found)",
		    path);

	/* Write to user config. */
	config_init(&c);
	config_load_file(&c, CONFIG_SCOPE_USER, user_config_path());
	config_set(&c, "idf.path", path, CONFIG_SCOPE_USER);
	cfg_path = user_config_path();
	if (!cfg_path)
		die("cannot determine user config path");
	if (config_write_file(&c, CONFIG_SCOPE_USER, cfg_path))
		die_errno("cannot write '%s'", cfg_path);
	config_release(&c);

	fprintf(stderr, "idf.path set to @b{%s}\n", path);
	return 0;
}

static const char *switch_worktree;

static const struct option switch_opts[] = {
    OPT_STRING(0, "worktree", &switch_worktree, "path",
	       "create the working tree at a custom path", NULL),
    OPT_END_COMPLETE("ref", NULL),
};

static int cmd_idf_switch(int argc, const char **argv)
{
	static const struct cmd_manual manual = {.name = "ice idf switch"};
	const char *base = base_repo_path();
	const char *current = current_tree_path();
	const char *version;

	argc = parse_options(argc, argv, switch_opts, &manual);
	if (argc < 1)
		die("expected a branch, tag, or commit SHA");
	version = argv[0];

	/*
	 * Objects for the worktree and its submodules always come from
	 * base.git when we manage the clone.  A user-configured idf.path
	 * (via `ice idf repo`) only wins when base.git doesn't exist --
	 * in that case the user is bringing their own populated tree.
	 */
	const char *obj_source;
	if (!access(base, F_OK))
		obj_source = base;
	else {
		const char *configured = config_get("idf.path");
		obj_source = configured && *configured ? configured : NULL;
	}

	if (!obj_source || access(obj_source, F_OK) != 0)
		die("no ESP-IDF configured\n"
		    "hint: run @b{ice idf clone} or @b{ice idf repo <path>}");

	const char *dest = switch_worktree ? switch_worktree : current;

	fprintf(stderr, "Switching to @b{%s} in @b{%s} ...\n", version, dest);

	/*
	 * Ensure the working tree exists with up-to-date refs.  Using
	 * --no-checkout lets us `git checkout` an arbitrary ref or SHA
	 * afterwards -- `git clone --branch` would reject commit SHAs.
	 */
	if (!is_directory(dest)) {
		mkdirp_for_file(dest);
		const char *clone_argv[] = {
		    "git",     "clone",	   "--shared", "--no-checkout",
		    "--quiet", obj_source, dest,       NULL};
		if (run_git(NULL, clone_argv) != 0)
			die("git clone failed");
	} else {
		/* Refresh refs so recently-pulled branches/tags are visible. */
		const char *fetch[] = {"git", "fetch", "--quiet", "origin",
				       NULL};
		run_git(dest, fetch);
	}

	/* Resolve any branch/tag/SHA -- objects are local via --shared. */
	{
		const char *checkout[] = {"git",     "checkout", "--force",
					  "--quiet", version,	 NULL};
		if (run_git(dest, checkout) != 0)
			die("git checkout '%s' failed", version);
	}

	/* Populate submodules from local objects. */
	fprintf(stderr, "Populating submodules ...\n");
	if (populate_submodules(obj_source, dest, version) < 0)
		warn("some submodules could not be populated");

	/*
	 * Register submodules in .git/config and move their gitdirs to
	 * the canonical <dest>/.git/modules/<name>/ layout so the result
	 * matches `git clone --recurse-submodules`.
	 *
	 * The -c override tells submodule init to resolve relative URLs
	 * against the real upstream URL (stored in base.git), not against
	 * `dest`'s local origin (which points at base.git).
	 */
	{
		struct sbuf origin_url = SBUF_INIT;
		const char *get_url[] = {"git", "config", "--get",
					 "remote.origin.url", NULL};
		run_git_capture(obj_source, get_url, &origin_url);
		sbuf_rtrim(&origin_url);

		struct svec args = SVEC_INIT;
		svec_push(&args, "git");
		if (origin_url.len) {
			svec_push(&args, "-c");
			svec_pushf(&args, "remote.origin.url=%s",
				   origin_url.buf);
		}
		svec_push(&args, "submodule");
		svec_push(&args, "init");
		svec_push(&args, "--quiet");
		run_git(dest, args.v);
		svec_clear(&args);
		sbuf_release(&origin_url);

		const char *absorb[] = {"git", "submodule", "absorbgitdirs",
					NULL};
		run_git(dest, absorb);
	}

	fprintf(stderr, "@g{Switched to %s}\n", version);
	return 0;
}

/**
 * Check if a version string (e.g. "v5.4.1", "5.3") meets the
 * minimum major.minor threshold.  Skips a leading 'v' or
 * "release/" prefix.
 */
static int version_supported(const char *ver)
{
	int major, minor;

	if (*ver == 'v')
		ver++;
	if (sscanf(ver, "%d.%d", &major, &minor) < 2)
		return 0;
	return major > IDF_MIN_MAJOR ||
	       (major == IDF_MIN_MAJOR && minor >= IDF_MIN_MINOR);
}

static int cmd_idf_list(int argc, const char **argv)
{
	static const struct cmd_manual manual = {.name = "ice idf list"};
	struct option opts[] = {OPT_END()};
	const char *repo;
	struct sbuf out = SBUF_INIT;
	size_t pos;
	char *line;

	parse_options(argc, argv, opts, &manual);

	repo = idf_path();
	if (!repo) {
		/* Fall back to bare repo for listing. */
		repo = base_repo_path();
	}
	if (access(repo, F_OK) != 0)
		die("no ESP-IDF configured\n"
		    "hint: run @b{ice idf clone} or @b{ice idf repo <path>}");

	/* Branches: master + release branches */
	{
		const char *git_argv[] = {
		    "git",	     "branch",		 "-r", "--list",
		    "origin/master", "origin/release/*", NULL};
		struct sbuf branches = SBUF_INIT;

		if (run_git_capture(repo, git_argv, &branches) == 0) {
			fprintf(stdout, "Branches:\n");
			pos = 0;
			while ((line = sbuf_getline(branches.buf, branches.len,
						    &pos))) {
				/* strip leading whitespace and "origin/" */
				while (*line == ' ')
					line++;
				if (!strncmp(line, "origin/", 7))
					line += 7;
				if (!strcmp(line, "master") ||
				    (!strncmp(line, "release/v", 9) &&
				     version_supported(line + 8) &&
				     !strchr(line + 9, '_')))
					fprintf(stdout, "  %s\n", line);
			}
		}
		sbuf_release(&branches);
	}

	/* Tags: v* sorted by version, newest first */
	{
		const char *git_argv[] = {
		    "git", "tag", "--sort=-version:refname", "-l", "v*", NULL};

		if (run_git_capture(repo, git_argv, &out) == 0) {
			fprintf(stdout, "\nTags:\n");
			pos = 0;
			while ((line = sbuf_getline(out.buf, out.len, &pos))) {
				if (version_supported(line) &&
				    !strstr(line, "-dev"))
					fprintf(stdout, "  %s\n", line);
			}
		}
		sbuf_release(&out);
	}

	return 0;
}

static int cmd_idf_pull(int argc, const char **argv)
{
	static const struct cmd_manual manual = {.name = "ice idf pull"};
	struct option opts[] = {OPT_END()};
	const char *base = base_repo_path();

	parse_options(argc, argv, opts, &manual);

	if (access(base, F_OK) != 0)
		die("no managed ESP-IDF repo found\n"
		    "hint: run @b{ice idf clone} first");

	/* Fetch new tags and branches into the bare repo. */
	fprintf(stderr, "Fetching updates into @b{%s} ...\n", base);
	{
		const char *fetch[] = {"git", "fetch", "--all", "--tags", NULL};
		if (run_git(base, fetch) != 0)
			die("git fetch failed");
	}

	/* Update submodule objects: re-fetch into the temp checkout's
	 * modules dir.  For now, just advise the user to re-run switch. */
	fprintf(stderr, "@g{done}\n"
			"hint: run @b{ice idf switch <version>} to update "
			"your working tree\n");
	return 0;
}

static int cmd_idf_info(int argc, const char **argv)
{
	static const struct cmd_manual manual = {.name = "ice idf info"};
	struct option opts[] = {OPT_END()};
	const char *repo;
	struct sbuf head = SBUF_INIT;
	const char *git_argv[] = {"git", "describe", "--tags", "--always",
				  NULL};

	parse_options(argc, argv, opts, &manual);

	repo = idf_path();
	if (!repo) {
		fprintf(stderr, "No ESP-IDF configured.\n"
				"hint: run @b{ice idf clone} or "
				"@b{ice idf repo <path>}\n");
		return 1;
	}

	fprintf(stdout, "Path:     %s\n", repo);
	fprintf(stdout, "Base:     %s\n", base_repo_path());

	if (config_get("idf.path"))
		fprintf(stdout, "Source:   external (set via idf.path)\n");
	else
		fprintf(stdout, "Source:   managed (ice idf clone)\n");

	if (run_git_capture(repo, git_argv, &head) == 0) {
		sbuf_rtrim(&head);
		fprintf(stdout, "Version:  %s\n", head.buf);
	}

	sbuf_release(&head);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

static subcmd_fn idf_fn;

static const struct option cmd_idf_opts[] = {
    OPT_SUBCOMMAND("clone", &idf_fn, cmd_idf_clone,
		   "clone ESP-IDF into ~/.ice/esp-idf/"),
    OPT_SUBCOMMAND("repo", &idf_fn, cmd_idf_repo,
		   "point ice at an existing ESP-IDF clone"),
    OPT_SUBCOMMAND("switch", &idf_fn, cmd_idf_switch,
		   "checkout a version and update submodules"),
    OPT_SUBCOMMAND("list", &idf_fn, cmd_idf_list,
		   "list available versions (tags)"),
    OPT_SUBCOMMAND("pull", &idf_fn, cmd_idf_pull,
		   "fetch latest from upstream (managed repo)"),
    OPT_SUBCOMMAND("info", &idf_fn, cmd_idf_info,
		   "show current IDF path and version"),
    OPT_END(),
};

/* clang-format off */
static const struct cmd_manual manual = {
	.name = "ice idf",
	.summary = "manage the ESP-IDF source tree",

	.description =
	H_PARA("Manage the ESP-IDF source tree.  Use @b{ice idf clone} to "
	       "get ESP-IDF, or @b{ice idf repo <path>} to point ice at "
	       "an existing clone.  Then @b{ice idf switch <version>} to "
	       "select a release.")
	H_PARA("Run @b{ice idf <subcommand> --help} for details."),

	.examples =
	H_EXAMPLE("ice idf clone")
	H_EXAMPLE("ice idf switch v5.4")
	H_EXAMPLE("ice idf list | head -20")
	H_EXAMPLE("ice idf repo ~/work/esp-idf")
	H_EXAMPLE("ice idf info"),
};
/* clang-format on */

int cmd_idf(int argc, const char **argv)
{
	argc = parse_options(argc, argv, cmd_idf_opts, &manual);
	if (idf_fn)
		return idf_fn(argc, argv);

	die("expected a subcommand. See 'ice idf --help'.");
}
