/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/repo/checkout/checkout.c
 * @brief `ice repo checkout` -- create a working checkout at a given ref.
 *
 * The checkout clones the reference locally (hardlinks, no network),
 * retargets @c origin back at upstream, then mirrors the reference's
 * @c .git/modules skeleton (objects replaced with alternates) so
 * submodules resolve through the reference without a fresh fetch.
 */
#include "cmd/repo/repo.h"

/* ------------------------------------------------------------------ */
/* Destination-path resolution                                         */
/* ------------------------------------------------------------------ */

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
		sbuf_addf(&p, "%s/%s", repo_checkouts_path(), arg);
	else
		sbuf_addstr(&p, arg);
	return sbuf_detach(&p);
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

/* ------------------------------------------------------------------ */
/* Reference preparation                                               */
/* ------------------------------------------------------------------ */

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
		rc = repo_run_git_capture(base, argv, &out);
	}
	sbuf_release(&spec);
	sbuf_release(&out);
	return rc == 0;
}

/**
 * Bring the reference up to @p ref and populate its submodules.
 *
 * The reference accumulates objects over time: every checkout leaves
 * its submodule gitdirs under @p reference/.git/modules/, so the next
 * checkout of a nearby version has most objects already available
 * locally and runs in seconds.  The `clean -ffdx` up front discards
 * whatever working-tree files the previous checkout left -- double -f
 * is required to cross submodule worktree boundaries, otherwise stale
 * submodule dirs stick around and later `git checkout` warns "unable
 * to rmdir ...".
 */
static void prepare_reference(const char *ref, int jobs)
{
	const char *base = repo_reference_path();
	char jobs_str[16];

	snprintf(jobs_str, sizeof(jobs_str), "%d", jobs);

	{
		const char *argv[] = {"git", "clean", "-ffdx", NULL};
		repo_run_git(base, argv);
	}

	if (!ref_exists(base, ref)) {
		const char *argv[] = {"git", "fetch", "origin", ref, NULL};
		if (repo_run_git(base, argv) != 0)
			die("git fetch '%s' failed", ref);
	}

	{
		const char *argv[] = {"git", "checkout", "--force", ref, NULL};
		if (repo_run_git(base, argv) != 0)
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
		if (repo_run_git(base, argv) != 0)
			warn("some submodules failed to update");
	}
}

/* ------------------------------------------------------------------ */
/* Completion callback                                                 */
/* ------------------------------------------------------------------ */

/** Emit supported branches + tags from the reference for TAB completion. */
static void complete_refs(void)
{
	const char *base = repo_reference_path();
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

		if (repo_run_git_capture(base, argv, &out) == 0) {
			pos = 0;
			while ((line = sbuf_getline(out.buf, out.len, &pos))) {
				if (!strncmp(line, "origin/", 7))
					line += 7;
				if (!strcmp(line, "master") ||
				    (!strncmp(line, "release/v", 9) &&
				     repo_version_supported(line + 8) &&
				     !strchr(line + 9, '_')))
					printf("%s\n", line);
			}
		}
		sbuf_release(&out);
	}

	{
		const char *argv[] = {"git", "tag", "--sort=-version:refname",
				      "-l",  "v*",  NULL};

		if (repo_run_git_capture(base, argv, &out) == 0) {
			pos = 0;
			while ((line = sbuf_getline(out.buf, out.len, &pos))) {
				if (repo_version_supported(line) &&
				    !strstr(line, "-dev"))
					printf("%s\n", line);
			}
		}
		sbuf_release(&out);
	}
}

/* ------------------------------------------------------------------ */
/* Subcommand                                                          */
/* ------------------------------------------------------------------ */

static int opt_checkout_list;
static int checkout_jobs = 8;

static const struct option checkout_opts[] = {
    OPT_BOOL(0, "list", &opt_checkout_list,
	     "list existing checkouts instead of creating one"),
    OPT_INT('j', "jobs", &checkout_jobs, "n",
	    "parallel submodule clones (default 8)", NULL),
    OPT_POSITIONAL("ref", complete_refs),
    OPT_POSITIONAL_OPT("name|path", NULL),
    OPT_END(),
};

int cmd_repo_checkout(int argc, const char **argv);
int cmd_repo___checkout(int argc, const char **argv);

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

const struct cmd_desc cmd_repo_checkout_desc = {
    .name = "checkout",
    .fn = cmd_repo_checkout,
    .opts = checkout_opts,
    .manual = &repo_checkout_manual,
};

const struct cmd_desc cmd_repo___checkout_desc = {
    .name = "__checkout",
    .fn = cmd_repo___checkout,
    .opts = checkout_opts,
    .manual = &repo_checkout_manual,
};

int cmd_repo_checkout(int argc, const char **argv)
{
	struct svec cmd = SVEC_INIT;
	struct process proc = PROCESS_INIT;
	const char *exe = process_exe();
	int rc;

	svec_push(&cmd, exe ? exe : "ice");
	svec_push(&cmd, "repo");
	svec_push(&cmd, "__checkout");
	for (int i = 1; i < argc; i++)
		svec_push(&cmd, argv[i]);

	parse_options(argc, argv, &cmd_repo_checkout_desc);

	/* --list is an instant local listing; running it through
	 * process_run_progress would hide the output inside the log and
	 * show only a "done" line.  Fall through to the plumbing
	 * in-process so the names print normally to stdout.  The child
	 * branch below handles every other mode.  cmd is
	 * [exe, "repo", "__checkout", ...forwarded args], so cmd.v + 2
	 * puts "__checkout" at argv[0] -- parse_options skips argv[0] as
	 * the prog-name slot, so any real options/positionals start at
	 * index 1 as they would under the normal dispatcher path. */
	if (opt_checkout_list) {
		rc = cmd_repo___checkout((int)cmd.nr - 2, cmd.v + 2);
		svec_clear(&cmd);
		return rc;
	}

	proc.argv = cmd.v;
	rc = process_run_progress(&proc, "Checking out", "repo-checkout", NULL);
	svec_clear(&cmd);
	return rc;
}

int cmd_repo___checkout(int argc, const char **argv)
{
	const char *base = repo_reference_path();
	const char *ref;
	char *dest;
	struct sbuf origin_url = SBUF_INIT;
	struct sbuf lock = SBUF_INIT;

	argc = parse_options(argc, argv, &cmd_repo___checkout_desc);

	if (opt_checkout_list) {
		struct svec names = SVEC_INIT;

		repo_collect_checkouts(&names);
		if (!names.nr) {
			fprintf(stderr, "No checkouts under @b{%s}.\n",
				repo_checkouts_path());
			svec_clear(&names);
			return 0;
		}
		printf("Checkouts under %s:\n", repo_checkouts_path());
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

	repo_ensure_reference();
	if (!access(dest, F_OK))
		die("destination '%s' already exists", dest);

	if (checkout_jobs < 1)
		checkout_jobs = 1;

	sbuf_addf(&lock, "%s/esp-idf.lock", ice_home());
	if (lock_acquire(lock.buf, 2000) < 0)
		die_errno("lock '%s' (remove if no ice is running)", lock.buf);

	fprintf(stderr, "Preparing reference at @b{%s} ...\n", base);
	prepare_reference(ref, checkout_jobs);

	{
		const char *git_argv[] = {"git", "config", "--get",
					  "remote.origin.url", NULL};
		if (repo_run_git_capture(base, git_argv, &origin_url) != 0 ||
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
		if (repo_run_git(NULL, argv) != 0)
			die("git clone --local failed");
	}

	{
		const char *argv[] = {"git",	"remote",	"set-url",
				      "origin", origin_url.buf, NULL};
		if (repo_run_git(dest, argv) != 0)
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
		if (repo_run_git(dest, argv) != 0)
			die("git checkout '%s' in '%s' failed", ref, dest);
	}

	{
		const char *argv[] = {
		    "git",     "submodule",  "update", "--init", "--recursive",
		    "--force", "--no-fetch", "--jobs", jobs_str, NULL};
		if (repo_run_git(dest, argv) != 0)
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
		if (repo_run_git(base, argv) != 0)
			warn("could not restore reference to master");
	}

	lock_release(lock.buf);
	sbuf_release(&lock);
	fprintf(stderr, "@g{Checked out %s at %s}\n", ref, dest);
	sbuf_release(&origin_url);
	free(dest);
	return 0;
}
