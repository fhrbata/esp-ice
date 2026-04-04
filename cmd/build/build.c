/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/build/build.c
 * @brief "ice build" subcommand.
 *
 * Configures and builds with "cmake --build".
 *
 * Default mode: each output line updates a single progress line
 * with a label prefix. All output is logged to a file (plain text,
 * no ANSI codes). On failure, the last N lines are colorized and
 * shown.
 *
 * Verbose mode (-v): output passes through to the terminal.
 */
#include <time.h>

#include "../../ice.h"

#define TAIL_LINES 30

static int verbose;

static const char *build_usage[] = {
	"ice build [-B <path>] [-v]",
	NULL,
};

static const char *fmt_time(time_t start, struct sbuf *buf)
{
	double elapsed = difftime(time(NULL), start);

	if (elapsed < 1)
		return "<1s";

	sbuf_reset(buf);
	sbuf_addf(buf, "%.0fs", elapsed);
	return buf->buf;
}

/**
 * @brief Colorize a compiler output line using @x{...} tokens.
 *
 * Wraps known patterns with color tokens:
 *   "error:"   → red bold
 *   "warning:" → yellow bold
 *   "note:"    → cyan
 *   "FAILED:"  → red bold
 *   '...'      → bold (single-quoted strings)
 *   `...`      → bold (backtick-quoted)
 */
/** Keyword → color mapping for compiler/build output. */
static const struct {
	const char *keyword;
	int len;
	const char *color;
} keywords[] = {
	/* Order matters: longer matches first. */
	{"fatal error:",          12, "@R{"},
	{"undefined reference to", 22, "@r{"},
	{"multiple definition of", 22, "@r{"},
	{"In file included from",  21, "@c{"},
	{"In function",            11, "@c{"},
	{"CMake Error",            11, "@r{"},
	{"CMake Warning",          13, "@y{"},
	{"FAILED:",                7,  "@R{"},
	{"warning:",               8,  "@Y{"},
	{"error:",                 6,  "@R{"},
	{"note:",                  5,  "@c{"},
	{"***",                    3,  "@r{"},
};

#define NKEYWORDS (sizeof(keywords) / sizeof(keywords[0]))

static void colorize_line(struct sbuf *out, const char *line, size_t len)
{
	const char *p = line;
	const char *end = line + len;

	while (p < end) {
		/* Check keyword table. */
		int matched = 0;
		for (size_t i = 0; i < NKEYWORDS; i++) {
			if (end - p >= keywords[i].len &&
			    !memcmp(p, keywords[i].keyword, keywords[i].len)) {
				sbuf_addstr(out, keywords[i].color);
				sbuf_add(out, p, keywords[i].len);
				sbuf_addch(out, '}');
				p += keywords[i].len;
				matched = 1;
				break;
			}
		}
		if (matched)
			continue;

		/* Quoted strings: 'x', `x`, "x" → bold */
		if ((*p == '\'' || *p == '`' || *p == '"') &&
		    p + 1 < end) {
			char q = *p;
			const char *close = memchr(p + 1, q, end - p - 1);
			if (close && close - p < 80) {
				sbuf_addf(out, "@b{%c", q);
				sbuf_add(out, p + 1, close - p - 1);
				sbuf_addf(out, "%c}", q);
				p = close + 1;
				continue;
			}
		}

		/* GCC caret+range: ^~~~~~~~~ → red */
		if (*p == '^' && p + 1 < end && *(p + 1) == '~') {
			const char *s = p;
			p++;
			while (p < end && *p == '~')
				p++;
			sbuf_addstr(out, "@r{");
			sbuf_add(out, s, p - s);
			sbuf_addch(out, '}');
			continue;
		}

		/* Numbers: 0x1a2b, 0777, 42 → cyan (only whitespace-bounded) */
		if (*p >= '0' && *p <= '9' &&
		    (p == line || isspace((unsigned char)*(p - 1)))) {
			const char *s = p;
			if (*p == '0' && p + 1 < end &&
			    (*(p + 1) == 'x' || *(p + 1) == 'X')) {
				p += 2;
				while (p < end && ((*p >= '0' && *p <= '9') ||
				       (*p >= 'a' && *p <= 'f') ||
				       (*p >= 'A' && *p <= 'F')))
					p++;
			} else {
				while (p < end && *p >= '0' && *p <= '9')
					p++;
			}
			if (p == end || isspace((unsigned char)*p)) {
				sbuf_addstr(out, "@c{");
				sbuf_add(out, s, p - s);
				sbuf_addch(out, '}');
				continue;
			}
			p = s;
		}

		/* Escape @ and } for color token safety. */
		if (*p == '@') {
			sbuf_addstr(out, "@@");
			p++;
			continue;
		}
		if (*p == '}') {
			sbuf_addstr(out, "}}");
			p++;
			continue;
		}

		sbuf_addch(out, *p++);
	}
}

/**
 * @brief Show the last TAIL_LINES non-progress lines from the log,
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

	/* Collect non-progress lines. */
	const char *p = log.buf;
	const char *end = log.buf + log.len;

	while (p < end) {
		const char *nl = memchr(p, '\n', end - p);
		if (!nl)
			nl = end;

		/* Skip progress lines (ninja [n/total], make [nn%],
		 * cmake --) */
		if (*p != '[' && *p != '-') {
			struct sbuf colored = SBUF_INIT;
			colorize_line(&colored, p, nl - p);
			sbuf_addstr(&filtered, colored.buf);
			sbuf_addch(&filtered, '\n');
			sbuf_release(&colored);
		}

		p = nl + 1;
	}

	/* Take last TAIL_LINES. */
	p = filtered.buf + filtered.len;
	while (p > filtered.buf && count < TAIL_LINES) {
		p--;
		if (*p == '\n')
			count++;
	}
	if (*p == '\n')
		p++;

	fprintf(stderr, "\n");
	/* Use fputs so @x{...} tokens get expanded. */
	fputs(p, stderr);

	sbuf_release(&filtered);
	sbuf_release(&log);
}

/**
 * @brief Run a command with labeled progress and logging.
 */
static int run_with_log(const char **argv, const char *label,
			const char *done_label, const char *fail_label,
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

	prefix_len = strlen(label) + 2; /* "label: " */
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

			fprintf(stderr, "\r\033[K   @b{%s}: ", label);
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
		fprintf(stderr, " @g{*} @b{%s} (%s)\n",
			done_label, fmt_time(start, &tsb));
	} else {
		show_tail(logpath);
		fprintf(stderr,
			"\n @r{*} @b{%s (%s) -- last %d lines above, full log: %s}\n",
			fail_label, fmt_time(start, &tsb),
			TAIL_LINES, logpath);
	}

	sbuf_release(&tsb);
	sbuf_release(&partial);
	return rc;
}

int cmd_build(int argc, const char **argv)
{
	const char *build_dir = "build";
	struct sbuf path = SBUF_INIT;
	struct sbuf logpath = SBUF_INIT;
	int rc;

	struct option opts[] = {
		OPT_STRING('B', "build-dir", &build_dir, "path",
			   "build directory"),
		OPT_BOOL('v', "verbose", &verbose,
			 "show full build output"),
		OPT_END(),
	};

	parse_options(argc, argv, opts, build_usage);

	if (access("CMakeLists.txt", F_OK) != 0)
		die("no CMakeLists.txt found in current directory");

	/* Ensure log directory exists. */
	sbuf_addf(&logpath, "%s/log", build_dir);
	mkdir(build_dir, 0755);
	mkdir(logpath.buf, 0755);
	sbuf_reset(&logpath);

	/* Configure if not yet configured. */
	sbuf_addf(&path, "%s/CMakeCache.txt", build_dir);
	if (access(path.buf, F_OK) != 0) {
		const char *cmake_argv[] = {
			"cmake", "-G", "Ninja", "-B", build_dir, NULL
		};

		sbuf_addf(&logpath, "%s/log/configure.log", build_dir);

		if (verbose) {
			struct process proc = PROCESS_INIT;
			proc.argv = cmake_argv;
			rc = process_run(&proc);
		} else {
			rc = run_with_log(cmake_argv, "Configuring",
					  "Configured", "Configure failed",
					  logpath.buf);
		}

		if (rc) {
			sbuf_release(&path);
			sbuf_release(&logpath);
			return rc;
		}
	}
	sbuf_release(&path);

	/* Build. */
	const char *build_argv[] = {
		"cmake", "--build", build_dir, NULL
	};

	sbuf_reset(&logpath);
	sbuf_addf(&logpath, "%s/log/build.log", build_dir);

	if (verbose) {
		struct process proc = PROCESS_INIT;
		proc.argv = build_argv;
		rc = process_run(&proc);
	} else {
		rc = run_with_log(build_argv, "Building", "Built",
				  "Build failed", logpath.buf);
	}

	sbuf_release(&logpath);
	return rc;
}
