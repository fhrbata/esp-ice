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

	/* On failure, remove CMakeCache.txt to prevent half-valid state.
	 * cmake may create a partial cache even when configure fails the
	 * first time (has_cache == 0), so always unlink on failure. */
	if (rc) {
		struct config_entry **entries;
		int n, has_cli = 0;

		unlink(cache_path.buf);

		/*
		 * -D entries from the command line live at CLI scope only
		 * and are gone when the process exits.  Warn so the user
		 * can either re-pass them or persist via `ice config --add`.
		 */
		n = config_get_all("cmake.define", &entries);
		for (int i = 0; i < n; i++) {
			if (entries[i]->scope == CONFIG_SCOPE_CLI) {
				has_cli = 1;
				break;
			}
		}
		free(entries);

		if (has_cli)
			warn("--define entries passed on the command line are "
			     "not retained across ice invocations; re-pass "
			     "them, or persist with 'ice config --add "
			     "cmake.define KEY=VAL'");
	}

out:
	sbuf_release(&logpath);
	sbuf_release(&cache_path);
	cmakecache_release(&cache);
	svec_clear(&defines);
	return rc;
}

/* ------------------------------------------------------------------ */
/*  build.ninja patching                                              */
/*    gen_esp32part.py   → ice partition-table                        */
/*    esptool elf2image  → ice image elf2image                        */
/* ------------------------------------------------------------------ */

static const char *mem_find(const char *p, const char *end, const char *needle,
			    size_t nlen)
{
	for (; p + nlen <= end; p++)
		if (!memcmp(p, needle, nlen))
			return p;
	return NULL;
}

static void patch_command_line(struct sbuf *out, const char *line, size_t len)
{
	static const char needle[] = "gen_esp32part.py";
	static const size_t nlen = sizeof(needle) - 1;
	const char *p = line;
	const char *end = line + len;
	int occurrence = 0;

	while (p < end) {
		const char *found = mem_find(p, end, needle, nlen);
		if (!found) {
			sbuf_add(out, p, end - p);
			return;
		}

		const char *script_start = found;
		while (script_start > p && script_start[-1] != ' ')
			script_start--;

		const char *python_start = script_start;
		if (python_start > p)
			python_start--;
		while (python_start > p && python_start[-1] != ' ')
			python_start--;

		sbuf_add(out, p, python_start - p);

		/*
		 * IDF calls gen_esp32part.py twice per COMMAND line:
		 *   1st call: generate  -- replace with ice partition-table
		 *   2nd call: display   -- replace with true
		 */
		if (occurrence == 0) {
			const char *exe = process_exe();
			sbuf_addstr(out, exe ? exe : "ice");
			sbuf_addstr(out, " partition-table");
		} else {
			sbuf_addstr(out, "true");
		}
		occurrence++;

		p = found + nlen;
	}
}

static void patch_ninja(const char *build_dir)
{
	static const char needle[] = "gen_esp32part.py";
	static const size_t nlen = sizeof(needle) - 1;
	struct sbuf ninja_path = SBUF_INIT;
	struct sbuf content = SBUF_INIT;
	struct sbuf out = SBUF_INIT;
	const char *p, *end, *nl;
	int modified = 0;

	sbuf_addf(&ninja_path, "%s/build.ninja", build_dir);

	if (sbuf_read_file(&content, ninja_path.buf) < 0)
		goto done;

	p = content.buf;
	end = content.buf + content.len;

	while (p < end) {
		nl = memchr(p, '\n', end - p);
		if (!nl)
			nl = end;

		size_t line_len = (size_t)(nl - p);

		if (line_len > 11 && !memcmp(p, "  COMMAND =", 11) &&
		    mem_find(p, nl, needle, nlen)) {
			patch_command_line(&out, p, line_len);
			modified = 1;
		} else {
			sbuf_add(&out, p, line_len);
		}

		if (nl < end)
			sbuf_addch(&out, '\n');
		p = nl + (nl < end ? 1 : 0);
	}

	if (modified)
		write_file_atomic(ninja_path.buf, out.buf, out.len);

done:
	sbuf_release(&ninja_path);
	sbuf_release(&content);
	sbuf_release(&out);
}

/*
 * Walk back @p n whitespace-delimited tokens from @p p (which must
 * point at the start of a token) toward @p line_start, returning a
 * pointer to the start of the token @p n positions back.  Each
 * iteration first skips separating spaces, then skips the token
 * itself, ending up at the start of the previous token.
 */
static const char *back_n_tokens(const char *p, const char *line_start, int n)
{
	while (n-- > 0) {
		while (p > line_start && p[-1] == ' ')
			p--;
		while (p > line_start && p[-1] != ' ')
			p--;
	}
	return p;
}

/*
 * Replace the esptool elf2image invocation on a COMMAND line with
 * the native `ice image elf2image` equivalent.  IDF's COMMAND line
 * has the form:
 *
 *   cd <dir> && <python> -m esptool --chip <chip> elf2image <args> \
 *       -o <out.bin> <in.elf> && cmake -E echo "..." && ...
 *
 * We rewrite `<python> -m esptool` to `ice image elf2image`, then
 * re-emit the captured `--chip <chip>` argument (which lives between
 * `esptool` and `elf2image`) plus everything after `elf2image`.
 */
static void patch_elf2image_line(struct sbuf *out, const char *line, size_t len)
{
	static const char e2i_needle[] = "elf2image";
	static const size_t e2ilen = sizeof(e2i_needle) - 1;
	static const char esptool_needle[] = "esptool";
	static const size_t elen = sizeof(esptool_needle) - 1;
	static const char chip_needle[] = "--chip";
	static const size_t chip_nlen = sizeof(chip_needle) - 1;

	const char *end = line + len;
	const char *etool = mem_find(line, end, esptool_needle, elen);

	if (!etool)
		goto passthrough;

	const char *e2i = mem_find(etool + elen, end, e2i_needle, e2ilen);
	if (!e2i)
		goto passthrough;

	/* Walk back to the start of the "esptool" token, then two more
	 * tokens to skip "-m" and "<python>" (IDF's canonical shape). */
	const char *etool_start = etool;
	while (etool_start > line && etool_start[-1] != ' ')
		etool_start--;
	const char *invoke_start = back_n_tokens(etool_start, line, 2);

	/* Capture --chip <value> between esptool and elf2image. */
	const char *chip_arg =
	    mem_find(etool + elen, e2i, chip_needle, chip_nlen);
	const char *chip_val_start = NULL;
	const char *chip_val_end = NULL;
	if (chip_arg) {
		chip_val_start = chip_arg + chip_nlen;
		while (chip_val_start < e2i && *chip_val_start == ' ')
			chip_val_start++;
		chip_val_end = chip_val_start;
		while (chip_val_end < e2i && *chip_val_end != ' ')
			chip_val_end++;
	}

	sbuf_add(out, line, invoke_start - line);

	{
		const char *exe = process_exe();

		sbuf_addstr(out, exe ? exe : "ice");
	}
	sbuf_addstr(out, " image elf2image");
	if (chip_val_start && chip_val_end > chip_val_start) {
		sbuf_addstr(out, " --chip ");
		sbuf_add(out, chip_val_start, chip_val_end - chip_val_start);
	}

	/* Everything after "elf2image" (flash params, -o, input, && tail). */
	sbuf_add(out, e2i + e2ilen, end - (e2i + e2ilen));
	return;

passthrough:
	sbuf_add(out, line, len);
}

static void patch_ninja_elf2image(const char *build_dir)
{
	static const char esptool_needle[] = "esptool";
	static const char e2i_needle[] = "elf2image";
	static const size_t elen = sizeof(esptool_needle) - 1;
	static const size_t e2ilen = sizeof(e2i_needle) - 1;

	struct sbuf ninja_path = SBUF_INIT;
	struct sbuf content = SBUF_INIT;
	struct sbuf out = SBUF_INIT;
	const char *p, *end, *nl;
	int modified = 0;

	sbuf_addf(&ninja_path, "%s/build.ninja", build_dir);

	if (sbuf_read_file(&content, ninja_path.buf) < 0)
		goto done;

	p = content.buf;
	end = content.buf + content.len;

	while (p < end) {
		nl = memchr(p, '\n', end - p);
		if (!nl)
			nl = end;

		size_t line_len = (size_t)(nl - p);

		if (line_len > 11 && !memcmp(p, "  COMMAND =", 11) &&
		    mem_find(p, nl, esptool_needle, elen) &&
		    mem_find(p, nl, e2i_needle, e2ilen)) {
			patch_elf2image_line(&out, p, line_len);
			modified = 1;
		} else {
			sbuf_add(&out, p, line_len);
		}

		if (nl < end)
			sbuf_addch(&out, '\n');
		p = nl + (nl < end ? 1 : 0);
	}

	if (modified)
		write_file_atomic(ninja_path.buf, out.buf, out.len);

done:
	sbuf_release(&ninja_path);
	sbuf_release(&content);
	sbuf_release(&out);
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

	/* Replace gen_esp32part.py with ice partition-table on every build. */
	patch_ninja(build_dir);
	/* Replace `<python> -m esptool ... elf2image` with `ice image
	 * elf2image` on every build. */
	patch_ninja_elf2image(build_dir);

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
