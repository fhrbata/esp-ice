/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmake.c
 * @brief Shared cmake orchestration used by every cmake-based "ice" command.
 *
 * Public API: ensure_build_directory() runs cmake's configure step;
 * run_cmake_target() ensures configured and then invokes a target
 * with progress + log capture.  Both gather core.build-dir, core.generator,
 * core.verbose and cmake.define from the config store internally.
 */
#include <time.h>

#include "ice.h"

#define TAIL_LINES 30

/* ------------------------------------------------------------------ */
/*  Progress display, logging, colorization                           */
/* ------------------------------------------------------------------ */

static const char *fmt_time(time_t start, struct sbuf *buf)
{
	double elapsed = difftime(time(NULL), start);

	if (elapsed < 1)
		return "<1s";

	sbuf_reset(buf);
	sbuf_addf(buf, "%.0fs", elapsed);
	return buf->buf;
}

/** Keyword -> color rules for compiler/build output. */
static const struct color_rule color_rules[] = {
    COLOR_RULE("fatal error:", "COLOR_BOLD_RED"),
    COLOR_RULE("undefined reference to", "COLOR_RED"),
    COLOR_RULE("multiple definition of", "COLOR_RED"),
    COLOR_RULE("In file included from", "COLOR_CYAN"),
    COLOR_RULE("In function", "COLOR_CYAN"),
    COLOR_RULE("CMake Error", "COLOR_RED"),
    COLOR_RULE("CMake Warning", "COLOR_YELLOW"),
    COLOR_RULE("FAILED:", "COLOR_BOLD_RED"),
    COLOR_RULE("warning:", "COLOR_BOLD_YELLOW"),
    COLOR_RULE("error:", "COLOR_BOLD_RED"),
    COLOR_RULE("note:", "COLOR_CYAN"),
    COLOR_RULE("***", "COLOR_RED"),
    {NULL},
};

/**
 * Show the last TAIL_LINES non-progress lines from the log,
 * colorized for terminal display.
 */
static void show_tail(const char *logpath)
{
	struct sbuf log = SBUF_INIT;
	struct sbuf filtered = SBUF_INIT;
	int count = 0;

	if (sbuf_read_file(&log, logpath) < 0) {
		sbuf_release(&log);
		return;
	}

	const char *p = log.buf;
	const char *end = log.buf + log.len;

	while (p < end) {
		const char *nl = memchr(p, '\n', end - p);
		if (!nl)
			nl = end;

		if (*p != '[' && *p != '-') {
			struct sbuf colored = SBUF_INIT;
			color_text(&colored, p, nl - p, color_rules);
			sbuf_addstr(&filtered, colored.buf);
			sbuf_addch(&filtered, '\n');
			sbuf_release(&colored);
		}

		p = nl + 1;
	}

	p = filtered.buf + filtered.len;
	while (p > filtered.buf && count < TAIL_LINES) {
		p--;
		if (*p == '\n')
			count++;
	}
	if (*p == '\n')
		p++;

	fprintf(stderr, "\n");
	fputs(p, stderr);

	sbuf_release(&filtered);
	sbuf_release(&log);
}

/**
 * Run a command with labeled progress display and logging.
 *
 * Progress line: "   Running <label>: <last-output-line>"
 * On success:    " * <label> (<elapsed>)"
 * On failure:    " * <label> failed (<elapsed>) -- last N lines above, ..."
 */
static int run_with_log(const char **argv, const char *label,
			const char *logpath)
{
	struct process proc = PROCESS_INIT;
	struct sbuf partial = SBUF_INIT;
	struct sbuf tsb = SBUF_INIT;
	FILE *logfp;
	time_t start;
	char buf[8192];
	ssize_t n;
	int rc;
	int width;
	int prefix_len;

	logfp = fopen(logpath, "w");
	if (!logfp)
		die_errno("cannot create '%s'", logpath);

	prefix_len = strlen("Running ") + strlen(label) + 2;
	start = time(NULL);
	proc.argv = argv;
	proc.pipe_out = 1;
	proc.merge_err = 1;

	if (process_start(&proc)) {
		fclose(logfp);
		return -1;
	}

	while ((n = read(proc.out, buf, sizeof(buf))) > 0) {
		fwrite(buf, 1, n, logfp);

		const char *p = buf;
		const char *end = buf + n;

		while (p < end) {
			const char *nl = memchr(p, '\n', end - p);

			if (!nl) {
				sbuf_add(&partial, p, end - p);
				break;
			}

			const char *line = p;
			size_t len = nl - p;

			if (partial.len > 0) {
				sbuf_add(&partial, p, nl - p);
				line = partial.buf;
				len = partial.len;
			}

			width = term_width(STDERR_FILENO);
			int avail = width - 3 - prefix_len - 4;

			fprintf(stderr, "\r\033[K   Running @b{%s}: ", label);
			if (avail > 0 && (int)len > avail) {
				fwrite(line, 1, avail, stderr);
				fprintf(stderr, "...");
			} else {
				fwrite(line, 1, len, stderr);
			}

			sbuf_reset(&partial);
			p = nl + 1;
		}
	}

	rc = process_finish(&proc);
	fclose(logfp);

	fprintf(stderr, "\r\033[K");
	if (rc == 0) {
		fprintf(stderr, " @g{*} @b{%s} (%s)\n", label,
			fmt_time(start, &tsb));
	} else {
		show_tail(logpath);
		fprintf(stderr,
			"\n @r{*} @b{%s failed (%s) -- last %d lines above, "
			"full log: %s}\n",
			label, fmt_time(start, &tsb), TAIL_LINES, logpath);
	}

	sbuf_release(&tsb);
	sbuf_release(&partial);
	return rc;
}

/* ------------------------------------------------------------------ */
/*  Config gathering + cache comparison                               */
/* ------------------------------------------------------------------ */

/*
 * Pull build_dir / generator / verbose / cmake.define entries from
 * the config store.  The defines svec is filled and must be cleared
 * by the caller with svec_clear().
 */
static void gather_cmake_config(const char **build_dir, const char **generator,
				struct svec *defines, int *verbose)
{
	struct config_entry **entries;
	int n;

	*build_dir = config_get("core.build-dir");
	*generator = config_get("core.generator");

	*verbose = 0;
	config_get_bool("core.verbose", verbose);

	n = config_get_all("cmake.define", &entries);
	for (int i = 0; i < n; i++)
		svec_push(defines, entries[i]->value);
	free(entries);
}

/**
 * Check whether any -D entry is missing from, or differs in, the cache.
 *
 * Each entry in @p defines is "KEY=VALUE" or "KEY:TYPE=VALUE".
 * The optional :TYPE is stripped before lookup.
 */
static int cache_needs_configure(const struct cmakecache *cache,
				 const struct svec *defines)
{
	for (size_t i = 0; i < defines->nr; i++) {
		const char *entry = defines->v[i];
		const char *eq = strchr(entry, '=');
		const char *colon;
		struct sbuf key = SBUF_INIT;
		const char *cached;
		int differs;

		if (!eq)
			continue;

		colon = memchr(entry, ':', eq - entry);
		if (colon)
			sbuf_add(&key, entry, colon - entry);
		else
			sbuf_add(&key, entry, eq - entry);

		cached = cmakecache_get(cache, key.buf);
		differs = !cached || strcmp(cached, eq + 1) != 0;

		sbuf_release(&key);
		if (differs)
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int ensure_build_directory(int force)
{
	const char *build_dir, *generator;
	struct svec defines = SVEC_INIT;
	struct sbuf cache_path = SBUF_INIT;
	struct cmakecache cache = CMAKECACHE_INIT;
	struct sbuf logpath = SBUF_INIT;
	int verbose;
	int has_cache;
	int needs_configure;
	int rc = 0;

	if (access("CMakeLists.txt", F_OK) != 0)
		die("no CMakeLists.txt found in current directory");

	gather_cmake_config(&build_dir, &generator, &defines, &verbose);

	/* Ensure build and log directories exist. */
	{
		struct sbuf logdir = SBUF_INIT;
		sbuf_addf(&logdir, "%s/log", build_dir);
		mkdir(build_dir, 0755);
		mkdir(logdir.buf, 0755);
		sbuf_release(&logdir);
	}

	/* Parse CMakeCache.txt once if it exists. */
	sbuf_addf(&cache_path, "%s/CMakeCache.txt", build_dir);
	has_cache = (cmakecache_load(&cache, cache_path.buf) == 0);

	needs_configure =
	    force || !has_cache ||
	    (defines.nr && cache_needs_configure(&cache, &defines));

	/* Validate generator against existing cache. */
	if (has_cache) {
		const char *cached_gen =
		    cmakecache_get(&cache, "CMAKE_GENERATOR");

		if (cached_gen && strcmp(cached_gen, generator) != 0)
			die("build is configured for generator '%s' "
			    "not '%s' -- run 'ice reconfigure' or "
			    "remove '%s'",
			    cached_gen, generator, build_dir);
	}

	if (!needs_configure)
		goto out;

	/* Build cmake argv dynamically (variable number of -D entries). */
	{
		struct svec args = SVEC_INIT;

		svec_push(&args, "cmake");
		svec_push(&args, "-G");
		svec_push(&args, generator);
		svec_push(&args, "-B");
		svec_push(&args, build_dir);

		for (size_t i = 0; i < defines.nr; i++)
			svec_pushf(&args, "-D%s", defines.v[i]);

		sbuf_addf(&logpath, "%s/log/configure.log", build_dir);

		if (verbose) {
			struct process proc = PROCESS_INIT;
			proc.argv = args.v;
			rc = process_run(&proc);
		} else {
			rc = run_with_log(args.v, "configure", logpath.buf);
		}

		svec_clear(&args);
	}

	/* On failure, remove CMakeCache.txt to prevent half-valid state. */
	if (rc && has_cache)
		unlink(cache_path.buf);

out:
	sbuf_release(&logpath);
	sbuf_release(&cache_path);
	cmakecache_release(&cache);
	svec_clear(&defines);
	return rc;
}

int run_cmake_target(const char *target, const char *label, int interactive)
{
	const char *build_dir;
	struct sbuf logpath = SBUF_INIT;
	int verbose = 0;
	int rc;

	rc = ensure_build_directory(0);
	if (rc)
		return rc;

	build_dir = config_get("core.build-dir");
	config_get_bool("core.verbose", &verbose);

	const char *argv[] = {"cmake",	  "--build", build_dir,
			      "--target", target,    NULL};

	if (interactive || verbose) {
		struct process proc = PROCESS_INIT;
		proc.argv = argv;
		rc = process_run(&proc);
	} else {
		sbuf_addf(&logpath, "%s/log/%s.log", build_dir, target);
		rc = run_with_log(argv, label, logpath.buf);
	}

	sbuf_release(&logpath);
	return rc;
}
