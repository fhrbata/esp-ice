/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file repo.c
 * @brief `ice repo` -- manage the ESP-IDF source tree.
 *
 * ice maintains a single ice-managed reference clone at
 * ~/.ice/esp-idf and creates per-version working checkouts under
 * ~/.ice/checkouts/<name>/ that borrow objects from the reference
 * via git's own --reference / submodule.alternateLocation machinery.
 * Checkouts are fast to create (no extra network transfer) and cheap
 * on disk (submodule objects live in the reference).
 *
 * Subcommands:
 *   clone    - create the reference at ~/.ice/esp-idf/
 *   pull     - refresh the reference to latest master
 *   list     - list available versions (branches + tags)
 *   checkout - create ~/.ice/checkouts/<name>/ at a given ref
 *   info     - show reference and checkout status
 */
#include "ice.h"

#define IDF_CLONE_URL "https://github.com/espressif/esp-idf.git"

/*
 * Minimum supported IDF major.minor -- tags and release branches
 * older than this are filtered from `ice repo list`.
 */
#define IDF_MIN_MAJOR 5
#define IDF_MIN_MINOR 0

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int version_supported(const char *ver);

/** Return ~/.ice/esp-idf (the ice-managed reference clone). */
static const char *reference_path(void)
{
	static struct sbuf path = SBUF_INIT;

	if (!path.len)
		sbuf_addf(&path, "%s/esp-idf", ice_home());
	return path.buf;
}

/** Return ~/.ice/checkouts (parent of named checkouts). */
static const char *checkouts_path(void)
{
	static struct sbuf path = SBUF_INIT;

	if (!path.len)
		sbuf_addf(&path, "%s/checkouts", ice_home());
	return path.buf;
}

/** Return ~/.ice/esp-idf.lock -- serialises clone/pull/checkout. */
static const char *reference_lock_path(void)
{
	static struct sbuf path = SBUF_INIT;

	if (!path.len)
		sbuf_addf(&path, "%s/esp-idf.lock", ice_home());
	return path.buf;
}

/**
 * Take the reference lock for the duration of the caller, die() with
 * a user-facing message if another ice holds it.  The parent
 * directory (~/.ice) is created lazily so the very first clone works.
 */
static void reference_lock(void)
{
	const char *lock = reference_lock_path();

	if (mkdirp_for_file(lock) < 0)
		die_errno("cannot create parent of '%s'", lock);

	if (lock_acquire(lock) < 0) {
		if (errno == EEXIST)
			die("another @b{ice} process holds the reference "
			    "lock at @b{%s}\n"
			    "hint: remove it if no ice is running",
			    lock);
		die_errno("cannot create lock '%s'", lock);
	}
}

static void reference_unlock(void) { lock_release(reference_lock_path()); }

/**
 * Resolve a checkout destination argument.
 *
 * A bare name (no path separator, no leading '.' or '~', not absolute)
 * maps to ~/.ice/checkouts/<name>.  Anything else is taken as-is,
 * letting the user drop a checkout anywhere in the filesystem.
 *
 * Returns a malloc'd path.
 */
static char *checkout_path(const char *arg)
{
	struct sbuf p = SBUF_INIT;
	int bare;

	bare = *arg && arg[0] != '/' && arg[0] != '.' && arg[0] != '~' &&
	       !strchr(arg, '/');

	if (bare)
		sbuf_addf(&p, "%s/%s", checkouts_path(), arg);
	else
		sbuf_addstr(&p, arg);
	return sbuf_detach(&p);
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

/** die if ~/.ice/esp-idf doesn't exist. */
static void ensure_reference(void)
{
	if (access(reference_path(), F_OK) != 0)
		die("no ESP-IDF reference at @b{%s}\n"
		    "hint: run @b{ice repo clone} first",
		    reference_path());
}

/* ------------------------------------------------------------------ */
/* Skeleton mirror of .git/modules                                    */
/* ------------------------------------------------------------------ */

/** Copy a regular file byte-for-byte. */
static int copy_file(const char *src, const char *dst)
{
	struct sbuf contents = SBUF_INIT;
	int rc = -1;

	if (sbuf_read_file(&contents, src) < 0) {
		warn_errno("read '%s'", src);
		goto done;
	}
	rc = write_file_atomic(dst, contents.buf, contents.len);
	if (rc < 0)
		warn_errno("write '%s'", dst);
done:
	sbuf_release(&contents);
	return rc;
}

/** Test whether @p path has the signature of a git gitdir (HEAD + config). */
static int is_gitdir(const char *path)
{
	struct sbuf p = SBUF_INIT;
	int ok = 0;

	sbuf_addf(&p, "%s/HEAD", path);
	if (!access(p.buf, F_OK)) {
		sbuf_reset(&p);
		sbuf_addf(&p, "%s/config", path);
		ok = !access(p.buf, F_OK);
	}
	sbuf_release(&p);
	return ok;
}

static int skeleton_mirror(const char *src, const char *dst);

struct skel_ctx {
	const char *src_dir;
	const char *dst_dir;
	int src_is_gitdir;
	int rc;
};

/**
 * For each entry in @p src_dir, recreate it in @p dst_dir.  When the
 * parent is a gitdir and we encounter the `objects` subtree, don't
 * recurse -- instead write `dst/objects/info/alternates` pointing at
 * `src/objects`, letting git resolve all history through the
 * reference's existing object store.
 */
static int skeleton_entry_cb(const char *name, void *ud)
{
	struct skel_ctx *ctx = ud;
	struct sbuf src = SBUF_INIT;
	struct sbuf dst = SBUF_INIT;
	int is_dir;

	sbuf_addf(&src, "%s/%s", ctx->src_dir, name);
	sbuf_addf(&dst, "%s/%s", ctx->dst_dir, name);
	is_dir = is_directory(src.buf);

	if (ctx->src_is_gitdir && is_dir && !strcmp(name, "objects")) {
		struct sbuf info = SBUF_INIT;
		struct sbuf alt = SBUF_INIT;
		struct sbuf content = SBUF_INIT;

		if (mkdir(dst.buf, 0755) < 0 && errno != EEXIST) {
			warn_errno("mkdir '%s'", dst.buf);
			ctx->rc = -1;
			goto alt_done;
		}
		sbuf_addf(&info, "%s/info", dst.buf);
		if (mkdir(info.buf, 0755) < 0 && errno != EEXIST) {
			warn_errno("mkdir '%s'", info.buf);
			ctx->rc = -1;
			goto alt_done;
		}
		sbuf_addf(&alt, "%s/alternates", info.buf);
		sbuf_addf(&content, "%s\n", src.buf);
		if (write_file_atomic(alt.buf, content.buf, content.len) < 0) {
			warn_errno("write '%s'", alt.buf);
			ctx->rc = -1;
		}
	alt_done:
		sbuf_release(&info);
		sbuf_release(&alt);
		sbuf_release(&content);
	} else if (is_dir) {
		if (mkdir(dst.buf, 0755) < 0 && errno != EEXIST) {
			warn_errno("mkdir '%s'", dst.buf);
			ctx->rc = -1;
		} else if (skeleton_mirror(src.buf, dst.buf) < 0) {
			ctx->rc = -1;
		}
	} else if (copy_file(src.buf, dst.buf) < 0) {
		ctx->rc = -1;
	}

	sbuf_release(&src);
	sbuf_release(&dst);
	return 0;
}

/**
 * Mirror @p src into @p dst recursively.  Plain directories are copied
 * file by file; directories that are gitdirs get the skeleton
 * treatment (objects replaced by alternates, mutable metadata copied).
 * The recursion naturally descends into a gitdir's `modules/` subtree,
 * where nested submodule gitdirs are detected and skeleton-ified in
 * turn.
 *
 * Cross-filesystem safe: uses only mkdir / file copy / write.  Dest's
 * submodule gitdirs depend on @p src remaining at its original path
 * for object lookups; the reference is effectively "pinned" for the
 * life of the checkout.
 */
static int skeleton_mirror(const char *src, const char *dst)
{
	struct skel_ctx ctx = {.src_dir = src,
			       .dst_dir = dst,
			       .src_is_gitdir = is_gitdir(src),
			       .rc = 0};

	if (dir_foreach(src, skeleton_entry_cb, &ctx) < 0) {
		warn_errno("read dir '%s'", src);
		return -1;
	}
	return ctx.rc;
}

/** Test whether @p ref resolves to a commit in @p base's object store. */
static int ref_exists(const char *base, const char *ref)
{
	struct sbuf spec = SBUF_INIT;
	struct sbuf out = SBUF_INIT;
	int rc;

	sbuf_addf(&spec, "%s^{commit}", ref);
	{
		const char *argv[] = {"git",	 "rev-parse", "--verify",
				      "--quiet", spec.buf,    NULL};
		rc = run_git_capture(base, argv, &out);
	}
	sbuf_release(&spec);
	sbuf_release(&out);
	return rc == 0;
}

/**
 * Bring the reference up to @p ref and populate its submodules.
 *
 * The reference accumulates objects over time: every checkout
 * leaves its submodule gitdirs under @p reference/.git/modules/, so
 * the next checkout of a nearby version has most objects already
 * available locally and runs in seconds.  The `clean -ffdx` up
 * front discards whatever working-tree files the previous
 * checkout left -- double -f is required to cross submodule
 * worktree boundaries, otherwise stale submodule dirs stick around
 * and later `git checkout` warns "unable to rmdir ...".
 */
static void prepare_reference(const char *ref, int jobs)
{
	const char *base = reference_path();
	char jobs_str[16];

	snprintf(jobs_str, sizeof(jobs_str), "%d", jobs);

	{
		const char *argv[] = {"git", "clean", "-ffdx", NULL};
		run_git(base, argv);
	}

	if (!ref_exists(base, ref)) {
		const char *argv[] = {"git", "fetch", "origin", ref, NULL};
		if (run_git(base, argv) != 0)
			die("git fetch '%s' failed", ref);
	}

	{
		const char *argv[] = {"git", "checkout", "--force", ref, NULL};
		if (run_git(base, argv) != 0)
			die("git checkout '%s' failed", ref);
	}

	{
		const char *argv[] = {"git",
				      "submodule",
				      "update",
				      "--init",
				      "--recursive",
				      "--force",
				      "--no-recommend-shallow",
				      "--jobs",
				      jobs_str,
				      NULL};
		if (run_git(base, argv) != 0)
			warn("some submodules failed to update");
	}
}

static int collect_dir(const char *name, void *ud)
{
	svec_push((struct svec *)ud, name);
	return 0;
}

/** Collect names of subdirectories of ~/.ice/checkouts/, sorted. */
static void collect_checkouts(struct svec *out)
{
	struct svec raw = SVEC_INIT;

	if (access(checkouts_path(), F_OK) != 0)
		return;
	dir_foreach(checkouts_path(), collect_dir, &raw);
	svec_sort(&raw);
	for (size_t i = 0; i < raw.nr; i++) {
		struct sbuf full = SBUF_INIT;

		sbuf_addf(&full, "%s/%s", checkouts_path(), raw.v[i]);
		if (is_directory(full.buf))
			svec_push(out, raw.v[i]);
		sbuf_release(&full);
	}
	svec_clear(&raw);
}

/* ------------------------------------------------------------------ */
/* Completion callbacks                                                */
/* ------------------------------------------------------------------ */

/** Emit supported branches + tags from the reference for TAB completion. */
static void complete_refs(void)
{
	const char *base = reference_path();
	struct sbuf out = SBUF_INIT;
	size_t pos;
	char *line;

	if (access(base, F_OK) != 0)
		return;

	{
		const char *argv[] = {"git",
				      "branch",
				      "-r",
				      "--list",
				      "--format=%(refname:short)",
				      "origin/master",
				      "origin/release/*",
				      NULL};

		if (run_git_capture(base, argv, &out) == 0) {
			pos = 0;
			while ((line = sbuf_getline(out.buf, out.len, &pos))) {
				if (!strncmp(line, "origin/", 7))
					line += 7;
				if (!strcmp(line, "master") ||
				    (!strncmp(line, "release/v", 9) &&
				     version_supported(line + 8) &&
				     !strchr(line + 9, '_')))
					printf("%s\n", line);
			}
		}
		sbuf_release(&out);
	}

	{
		const char *argv[] = {"git", "tag", "--sort=-version:refname",
				      "-l",  "v*",  NULL};

		if (run_git_capture(base, argv, &out) == 0) {
			pos = 0;
			while ((line = sbuf_getline(out.buf, out.len, &pos))) {
				if (version_supported(line) &&
				    !strstr(line, "-dev"))
					printf("%s\n", line);
			}
		}
		sbuf_release(&out);
	}
}

/* ------------------------------------------------------------------ */
/* Subcommands                                                         */
/* ------------------------------------------------------------------ */

static const char *clone_reference;
static int clone_dissociate;
static int clone_jobs = 8;

static const struct option clone_opts[] = {
    OPT_STRING(0, "reference", &clone_reference, "path",
	       "borrow objects from an existing esp-idf clone", NULL),
    OPT_BOOL(0, "dissociate", &clone_dissociate,
	     "copy borrowed objects locally, then stop borrowing"),
    OPT_INT('j', "jobs", &clone_jobs, "n",
	    "parallel submodule clones (default 8)", NULL),
    OPT_POSITIONAL("[<url>]", NULL),
    OPT_END(),
};

static int cmd_repo_clone(int argc, const char **argv);

/* clang-format off */
static const struct cmd_manual repo_clone_manual = {
	.name = "ice repo clone",
	.summary = "create the reference clone at ~/.ice/esp-idf/",

	.description =
	H_PARA("Clone ESP-IDF into @b{~/.ice/esp-idf}.  The reference "
	       "is ice-managed; do not work in it directly.  Use "
	       "@b{ice repo checkout} to create per-version working "
	       "trees that share objects with the reference.")
	H_PARA("@b{--reference <path>} borrows objects from an existing "
	       "ESP-IDF clone to avoid redundant network transfer.  The "
	       "@b{-if-able} flavor is used under the hood, so a "
	       "partially populated or shallow source is tolerated.  "
	       "Pass @b{--dissociate} to copy borrowed objects locally "
	       "so the reference no longer depends on @b{<path>}."),

	.examples =
	H_EXAMPLE("ice repo clone")
	H_EXAMPLE("ice repo clone --reference ~/work/esp-idf")
	H_EXAMPLE("ice repo clone --reference ~/work/esp-idf --dissociate"),
};
/* clang-format on */

static const struct cmd_desc cmd_repo_clone_desc = {
    .name = "clone",
    .fn = cmd_repo_clone,
    .opts = clone_opts,
    .manual = &repo_clone_manual,
};

static int cmd_repo_clone(int argc, const char **argv)
{
	const char *url;
	char jobs_str[16];
	struct svec args = SVEC_INIT;

	argc = parse_options(argc, argv, &cmd_repo_clone_desc);
	url = argc >= 1 ? argv[0] : IDF_CLONE_URL;

	reference_lock();

	if (!access(reference_path(), F_OK))
		die("ESP-IDF already cloned at @b{%s}\n"
		    "hint: use @b{ice repo pull} to update",
		    reference_path());

	if (clone_jobs < 1)
		clone_jobs = 1;
	snprintf(jobs_str, sizeof(jobs_str), "%d", clone_jobs);

	if (mkdirp_for_file(reference_path()) < 0)
		die_errno("cannot create parent of '%s'", reference_path());

	fprintf(stderr, "Cloning @b{%s} into @b{%s} ...\n", url,
		reference_path());

	svec_push(&args, "git");
	svec_push(&args, "clone");
	if (clone_reference) {
		svec_push(&args, "--reference-if-able");
		svec_push(&args, clone_reference);
		svec_push(&args, "-c");
		svec_push(&args, "submodule.alternateLocation=superproject");
		svec_push(&args, "-c");
		svec_push(&args, "submodule.alternateErrorStrategy=info");
		if (clone_dissociate)
			svec_push(&args, "--dissociate");
	}
	svec_push(&args, url);
	svec_push(&args, reference_path());

	if (run_git(NULL, args.v) != 0)
		die("git clone failed");
	svec_clear(&args);

	/*
	 * Submodules are cloned in a separate step so we can pass
	 * --no-recommend-shallow: .gitmodules marks several binary blob
	 * submodules `shallow = true`, and a depth=1 clone drops older
	 * commits whenever a later checkout moves the submodule SHA --
	 * which forces a re-fetch the next time an earlier ref is
	 * checked out.  Full-depth submodules accumulate permanently, so
	 * any SHA once fetched stays for every subsequent checkout.
	 */
	{
		const char *argv[] = {
		    "git",    "submodule",   "update",
		    "--init", "--recursive", "--no-recommend-shallow",
		    "--jobs", jobs_str,	     NULL};
		if (run_git(reference_path(), argv) != 0)
			die("git submodule update failed");
	}

	reference_unlock();
	fprintf(stderr, "@g{done}\n");
	return 0;
}

static int pull_jobs = 8;

static const struct option pull_opts[] = {
    OPT_INT('j', "jobs", &pull_jobs, "n",
	    "parallel submodule updates (default 8)", NULL),
    OPT_END(),
};

static int cmd_repo_pull(int argc, const char **argv);

/* clang-format off */
static const struct cmd_manual repo_pull_manual = {
	.name = "ice repo pull",
	.summary = "refresh the reference to latest master",

	.description =
	H_PARA("Clean any working-tree state left by a previous "
	       "@b{ice repo checkout}, switch the reference to "
	       "@b{master}, fast-forward to @b{origin/master}, and "
	       "recursively update submodules.  Existing checkouts "
	       "under @b{~/.ice/checkouts/} are left untouched -- use "
	       "@b{ice repo checkout} again to build a new working tree "
	       "against the refreshed reference."),

	.examples =
	H_EXAMPLE("ice repo pull"),
};
/* clang-format on */

static const struct cmd_desc cmd_repo_pull_desc = {
    .name = "pull",
    .fn = cmd_repo_pull,
    .opts = pull_opts,
    .manual = &repo_pull_manual,
};

static int cmd_repo_pull(int argc, const char **argv)
{
	const char *base = reference_path();
	char jobs_str[16];

	parse_options(argc, argv, &cmd_repo_pull_desc);
	ensure_reference();
	reference_lock();

	if (pull_jobs < 1)
		pull_jobs = 1;
	snprintf(jobs_str, sizeof(jobs_str), "%d", pull_jobs);

	fprintf(stderr, "Refreshing reference at @b{%s} ...\n", base);

	{
		const char *argv[] = {"git", "clean", "-ffdx", NULL};
		run_git(base, argv);
	}

	{
		const char *argv[] = {"git", "checkout", "--force", "master",
				      NULL};
		if (run_git(base, argv) != 0)
			die("git checkout master failed");
	}

	{
		const char *argv[] = {"git", "pull", "--ff-only", NULL};
		if (run_git(base, argv) != 0)
			die("git pull failed");
	}

	{
		const char *argv[] = {"git",
				      "submodule",
				      "update",
				      "--init",
				      "--recursive",
				      "--force",
				      "--no-recommend-shallow",
				      "--jobs",
				      jobs_str,
				      NULL};
		if (run_git(base, argv) != 0)
			warn("some submodules failed to update");
	}

	reference_unlock();
	fprintf(stderr, "@g{done}\n");
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

static int cmd_repo_list(int argc, const char **argv);

/* clang-format off */
static const struct cmd_manual repo_list_manual = {
	.name = "ice repo list",
	.summary = "list available versions (branches + tags)",

	.description =
	H_PARA("List supported release branches and tags in the "
	       "reference clone.  Versions older than "
	       "@b{v5.0} are hidden."),

	.examples =
	H_EXAMPLE("ice repo list")
	H_EXAMPLE("ice repo list | head -20"),
};
/* clang-format on */

static const struct option cmd_repo_list_opts[] = {OPT_END()};

static const struct cmd_desc cmd_repo_list_desc = {
    .name = "list",
    .fn = cmd_repo_list,
    .opts = cmd_repo_list_opts,
    .manual = &repo_list_manual,
};

static int cmd_repo_list(int argc, const char **argv)
{
	const char *base = reference_path();
	struct sbuf out = SBUF_INIT;
	size_t pos;
	char *line;

	parse_options(argc, argv, &cmd_repo_list_desc);
	ensure_reference();

	{
		const char *git_argv[] = {
		    "git",	     "branch",		 "-r", "--list",
		    "origin/master", "origin/release/*", NULL};
		struct sbuf branches = SBUF_INIT;

		if (run_git_capture(base, git_argv, &branches) == 0) {
			fprintf(stdout, "Branches:\n");
			pos = 0;
			while ((line = sbuf_getline(branches.buf, branches.len,
						    &pos))) {
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

	{
		const char *git_argv[] = {
		    "git", "tag", "--sort=-version:refname", "-l", "v*", NULL};

		if (run_git_capture(base, git_argv, &out) == 0) {
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

static int opt_checkout_list;
static int checkout_jobs = 8;

static const struct option checkout_opts[] = {
    OPT_BOOL(0, "list", &opt_checkout_list,
	     "list existing checkouts instead of creating one"),
    OPT_INT('j', "jobs", &checkout_jobs, "n",
	    "parallel submodule clones (default 8)", NULL),
    OPT_POSITIONAL("ref", complete_refs),
    OPT_POSITIONAL("[<name|path>]", NULL),
    OPT_END(),
};

static int cmd_repo_checkout(int argc, const char **argv);

/* clang-format off */
static const struct cmd_manual repo_checkout_manual = {
	.name = "ice repo checkout",
	.summary = "create a working checkout at a given ref",

	.description =
	H_PARA("Create a working ESP-IDF tree at @b{<ref>} (a branch, "
	       "tag, or commit SHA).  @b{<name|path>} is optional and "
	       "defaults to @b{<ref>} itself.")
	H_PARA("A bare @b{<name>} (no path separator) lands at "
	       "@b{~/.ice/checkouts/<name>/}.  Anything else -- a "
	       "relative or absolute path -- is used verbatim, letting "
	       "you drop a checkout anywhere.  If @b{<ref>} contains "
	       "a path separator (e.g. @b{release/v5.2}) you must pass "
	       "an explicit @b{<name|path>}.")
	H_PARA("The reference at @b{~/.ice/esp-idf} is advanced to "
	       "@b{<ref>} first (@b{clean}, @b{fetch} if needed, "
	       "@b{checkout}, recursive @b{submodule update}) so its "
	       "object store accumulates everything @b{<ref>} needs.  "
	       "The new checkout is then cloned locally from the "
	       "reference -- no extra network transfer -- with "
	       "@b{origin} pointed back at the real upstream URL.")
	H_PARA("Pass @b{--list} to enumerate existing named checkouts "
	       "under @b{~/.ice/checkouts/}."),

	.examples =
	H_EXAMPLE("ice repo checkout v5.4")
	H_EXAMPLE("ice repo checkout release/v5.2 v5.2")
	H_EXAMPLE("ice repo checkout master /tmp/scratch")
	H_EXAMPLE("ice repo checkout --list"),
};
/* clang-format on */

static const struct cmd_desc cmd_repo_checkout_desc = {
    .name = "checkout",
    .fn = cmd_repo_checkout,
    .opts = checkout_opts,
    .manual = &repo_checkout_manual,
};

static int cmd_repo_checkout(int argc, const char **argv)
{
	const char *base = reference_path();
	const char *ref;
	char *dest;
	struct sbuf origin_url = SBUF_INIT;

	argc = parse_options(argc, argv, &cmd_repo_checkout_desc);

	if (opt_checkout_list) {
		struct svec names = SVEC_INIT;

		collect_checkouts(&names);
		if (!names.nr) {
			fprintf(stderr, "No checkouts under @b{%s}.\n",
				checkouts_path());
			svec_clear(&names);
			return 0;
		}
		printf("Checkouts under %s:\n", checkouts_path());
		for (size_t i = 0; i < names.nr; i++)
			printf("  %s\n", names.v[i]);
		svec_clear(&names);
		return 0;
	}

	if (argc < 1)
		die("expected <ref> [<name|path>]");
	if (argc > 2)
		die("too many arguments");

	ref = argv[0];
	if (argc >= 2) {
		dest = checkout_path(argv[1]);
	} else {
		/*
		 * Default <name> = <ref>.  Refs containing a path separator
		 * (release/v5.2, origin/master) would land at a literal
		 * relative path and surprise the user -- require an explicit
		 * name in that case.
		 */
		if (strchr(ref, '/'))
			die("ref @b{%s} contains a path separator; "
			    "provide an explicit <name|path>",
			    ref);
		dest = checkout_path(ref);
	}

	ensure_reference();
	if (!access(dest, F_OK))
		die("destination '%s' already exists", dest);

	if (checkout_jobs < 1)
		checkout_jobs = 1;

	reference_lock();

	fprintf(stderr, "Preparing reference at @b{%s} ...\n", base);
	prepare_reference(ref, checkout_jobs);

	{
		const char *git_argv[] = {"git", "config", "--get",
					  "remote.origin.url", NULL};
		if (run_git_capture(base, git_argv, &origin_url) != 0 ||
		    !origin_url.len)
			die("could not read origin URL from reference");
		sbuf_rtrim(&origin_url);
	}

	fprintf(stderr, "Creating checkout at @b{%s} ...\n", dest);
	if (mkdirp_for_file(dest) < 0)
		die_errno("cannot create parent of '%s'", dest);

	char jobs_str[16];
	snprintf(jobs_str, sizeof(jobs_str), "%d", checkout_jobs);

	/*
	 * `--local` hardlinks reference's objects + refs + config into dest
	 * in milliseconds -- no remote clone protocol, no network.
	 * `--no-checkout` skips materializing reference's current worktree
	 * since we'll overwrite it with <ref> below.
	 */
	{
		const char *argv[] = {
		    "git", "clone", "--local", "--no-checkout",
		    base,  dest,    NULL};
		if (run_git(NULL, argv) != 0)
			die("git clone --local failed");
	}

	{
		const char *argv[] = {"git",	"remote",	"set-url",
				      "origin", origin_url.buf, NULL};
		if (run_git(dest, argv) != 0)
			warn("could not retarget origin at '%s'",
			     origin_url.buf);
	}

	/*
	 * Mirror reference's .git/modules tree as skeleton gitdirs: mutable
	 * metadata (HEAD, config, refs, packed-refs) gets copied, but the
	 * objects/ subtree is replaced by an objects/info/alternates file
	 * pointing back at reference.  Cross-filesystem safe -- no hardlinks
	 * required -- and git resolves all objects transparently via the
	 * alternate.  With the gitdirs in place, `git submodule update
	 * --init` takes the fast path: no per-submodule clone, no network.
	 * `--no-fetch` is safe because prepare_reference() already pulled
	 * every object <ref> needs into the reference's modules.
	 */
	{
		struct sbuf ref_modules = SBUF_INIT;
		struct sbuf dest_modules = SBUF_INIT;

		sbuf_addf(&ref_modules, "%s/.git/modules", base);
		sbuf_addf(&dest_modules, "%s/.git/modules", dest);
		if (is_directory(ref_modules.buf)) {
			if (mkdir(dest_modules.buf, 0755) < 0 &&
			    errno != EEXIST)
				warn_errno("mkdir '%s'", dest_modules.buf);
			else if (skeleton_mirror(ref_modules.buf,
						 dest_modules.buf) < 0)
				warn("could not fully mirror reference's "
				     ".git/modules");
		}
		sbuf_release(&ref_modules);
		sbuf_release(&dest_modules);
	}

	{
		const char *argv[] = {"git", "checkout", "--force", ref, NULL};
		if (run_git(dest, argv) != 0)
			die("git checkout '%s' in '%s' failed", ref, dest);
	}

	{
		const char *argv[] = {
		    "git",     "submodule",  "update", "--init", "--recursive",
		    "--force", "--no-fetch", "--jobs", jobs_str, NULL};
		if (run_git(dest, argv) != 0)
			warn("some submodules failed to update");
	}

	/*
	 * Restore the reference to master so it is left in a stable,
	 * attached state instead of detached at <ref>.  The skeleton
	 * mirror above captured reference's gitdirs at the <ref> state,
	 * so dest is already self-contained; flipping reference here
	 * affects only its own worktree, which nobody uses directly.
	 * Stale submodule worktree content is cleaned by the `git clean
	 * -ffdx` at the start of the next prepare_reference().
	 */
	{
		const char *argv[] = {"git", "checkout", "--force", "master",
				      NULL};
		if (run_git(base, argv) != 0)
			warn("could not restore reference to master");
	}

	reference_unlock();
	fprintf(stderr, "@g{Checked out %s at %s}\n", ref, dest);
	sbuf_release(&origin_url);
	free(dest);
	return 0;
}

static int cmd_repo_info(int argc, const char **argv);

/* clang-format off */
static const struct cmd_manual repo_info_manual = {
	.name = "ice repo info",
	.summary = "show reference and checkout status",

	.description =
	H_PARA("Show the ice-managed reference path and its current "
	       "HEAD, followed by every named checkout under "
	       "@b{~/.ice/checkouts/} with its own HEAD.  Ends with a "
	       "reminder that the reference must not be used as a "
	       "working tree -- create a checkout instead."),

	.examples =
	H_EXAMPLE("ice repo info"),
};
/* clang-format on */

static const struct option cmd_repo_info_opts[] = {OPT_END()};

static const struct cmd_desc cmd_repo_info_desc = {
    .name = "info",
    .fn = cmd_repo_info,
    .opts = cmd_repo_info_opts,
    .manual = &repo_info_manual,
};

static int cmd_repo_info(int argc, const char **argv)
{
	const char *base = reference_path();
	struct sbuf head = SBUF_INIT;
	struct svec names = SVEC_INIT;

	parse_options(argc, argv, &cmd_repo_info_desc);

	if (access(base, F_OK) != 0) {
		fprintf(stderr, "No ESP-IDF reference configured.\n"
				"hint: run @b{ice repo clone}\n");
		return 1;
	}

	printf("Reference: %s\n", base);
	{
		const char *git_argv[] = {"git", "describe", "--tags",
					  "--always", NULL};
		if (run_git_capture(base, git_argv, &head) == 0) {
			sbuf_rtrim(&head);
			printf("HEAD:      %s\n", head.buf);
		}
	}
	sbuf_release(&head);

	printf("\nCheckouts under %s:\n", checkouts_path());
	collect_checkouts(&names);
	if (!names.nr) {
		printf("  (none)\n");
	} else {
		for (size_t i = 0; i < names.nr; i++) {
			struct sbuf full = SBUF_INIT;
			struct sbuf ver = SBUF_INIT;
			const char *git_argv[] = {"git", "describe", "--tags",
						  "--always", NULL};

			sbuf_addf(&full, "%s/%s", checkouts_path(), names.v[i]);
			if (run_git_capture(full.buf, git_argv, &ver) == 0) {
				sbuf_rtrim(&ver);
				printf("  %-20s %s\n", names.v[i], ver.buf);
			} else {
				printf("  %-20s (no HEAD)\n", names.v[i]);
			}
			sbuf_release(&full);
			sbuf_release(&ver);
		}
	}
	svec_clear(&names);

	printf("\nThe reference is ice-managed; do not work in it.\n"
	       "Use @b{ice repo checkout <ref> <name>} for a working copy.\n");

	return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

static const struct option cmd_repo_opts[] = {OPT_END()};

/* clang-format off */
static const struct cmd_manual repo_manual = {
	.name = "ice repo",
	.summary = "manage the ESP-IDF source tree",

	.description =
	H_PARA("Manage the ESP-IDF source tree.  @b{ice} keeps a single "
	       "reference clone at @b{~/.ice/esp-idf} and creates per-version "
	       "working checkouts under @b{~/.ice/checkouts/<name>/} that "
	       "borrow objects from the reference.")
	H_PARA("Run @b{ice repo <subcommand> --help} for details.")
	H_PARA("The reference is ice-managed; do not work in it.  Create "
	       "checkouts with @b{ice repo checkout} and work there instead."),

	.examples =
	H_EXAMPLE("ice repo clone")
	H_EXAMPLE("ice repo checkout v5.4 v5.4")
	H_EXAMPLE("ice repo list | head -20")
	H_EXAMPLE("ice repo info"),
};
/* clang-format on */

static const struct cmd_desc *const repo_subs[] = {
    &cmd_repo_clone_desc,    &cmd_repo_pull_desc, &cmd_repo_list_desc,
    &cmd_repo_checkout_desc, &cmd_repo_info_desc, NULL,
};

const struct cmd_desc cmd_repo_desc = {
    .name = "repo",
    .opts = cmd_repo_opts,
    .manual = &repo_manual,
    .subcommands = repo_subs,
};
