/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/log/log.c
 * @brief The "ice log" subcommand -- view logs captured by
 *        process_run_progress.
 *
 * Each process_run_progress() call writes its full tee'd output to a
 * timestamped file under @c <build>/.ice/logs/ (or @c ~/.ice/logs/
 * when no project is in scope) and updates @c <log-dir>/last to
 * point at the newest one.  @b{ice log} is the reader:
 *
 *   ice log             — view the log pointed to by `last`.
 *   ice log -n N        — view the Nth-most-recent log.
 *   ice log --all       — one line per log with timings and exit
 *                         codes, oldest last.
 */
#include "ice.h"

#include "color_rules.h"

static int opt_n;
static int opt_all;

/* clang-format off */
static const struct cmd_manual log_manual = {
	.name = "ice log",
	.summary = "view logs from the last spawned tool runs",

	.description =
	H_PARA("Display the log captured by the most recent "
	       "@b{process_run_progress} invocation for the active "
	       "profile.  Logs live under @b{<build>/.ice/logs/} for "
	       "commands that run inside a configured project, and "
	       "under @b{~/.ice/logs/} for standalone commands "
	       "(@b{ice repo clone}, @b{ice tools install}, ...).")
	H_PARA("By default the newest log is colorised and paged.  "
	       "@b{-n <N>} selects the Nth most recent (1 = newest); "
	       "@b{--all} lists every log for the active profile with "
	       "start time, slug, exit code and duration."),

	.examples =
	H_EXAMPLE("ice log")
	H_EXAMPLE("ice log -n 2")
	H_EXAMPLE("ice log --all"),

	.extras =
	H_SECTION("ENVIRONMENT")
	H_ITEM("PAGER",
	       "Command used to page the log output.  Defaults to "
	       "@b{less -R} when available, otherwise writes directly "
	       "to stdout.  Set to an empty string to disable paging.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Writes a log (and flips the @b{built} marker on success)."),
};
/* clang-format on */

static const struct option cmd_log_opts[] = {
    OPT_INT('n', NULL, &opt_n, "N", "view the Nth most recent log (1 = newest)",
	    NULL),
    OPT_BOOL(0, "all", &opt_all, "list all logs for this profile"),
    OPT_END(),
};

int cmd_log(int argc, const char **argv);

const struct cmd_desc cmd_log_desc = {
    .name = "log",
    .fn = cmd_log,
    .opts = cmd_log_opts,
    .manual = &log_manual,
    .needs = PROJECT_CONFIGURED,
};

/*
 * Filter + collector: picks `.log` filenames from a directory listing.
 * Sorted lexicographically; since the timestamps in the filenames use
 * a zero-padded YYYYMMDD-HHMMSS prefix that sort is chronological,
 * oldest first.
 */
static int collect_log(const char *name, void *ud)
{
	size_t n = strlen(name);

	if (n > 4 && !strcmp(name + n - 4, ".log"))
		svec_push((struct svec *)ud, name);
	return 0;
}

static void collect_logs(const char *dir, struct svec *out)
{
	if (access(dir, F_OK) != 0)
		return;
	dir_foreach(dir, collect_log, out);
	svec_sort(out);
}

/*
 * Colorize the log content using the shared rule set and stream it
 * to stdout.  If stdout is a tty and core.pager is set, pager_start()
 * has already rewired fd 1 to the pager before we get here so the
 * plain fwrite naturally goes through @b{less} / @b{$PAGER}.
 */
static int display_log(const char *path)
{
	struct sbuf content = SBUF_INIT;
	struct sbuf colored = SBUF_INIT;
	struct sbuf expanded = SBUF_INIT;

	if (sbuf_read_file(&content, path) < 0)
		die_errno("cannot read '%s'", path);

	pager_start();

	if (use_color_for(stdout)) {
		color_text(&colored, content.buf, content.len,
			   ice_default_color_rules);
		expand_colors(&expanded, colored.buf, 1);
		fwrite(expanded.buf, 1, expanded.len, stdout);
	} else {
		fwrite(content.buf, 1, content.len, stdout);
	}

	sbuf_release(&expanded);
	sbuf_release(&colored);
	sbuf_release(&content);
	return 0;
}

/*
 * Resolve the path of the log selected by @p n (1-indexed from
 * newest).  @p n = 0 means "use the `last` pointer" which shortcuts
 * any directory enumeration.  Caller owns @p out and must sbuf_release
 * it; on success @p out holds the absolute log path.
 */
static void resolve_log_by_index(const char *log_dir, int n, struct sbuf *out)
{
	struct svec logs = SVEC_INIT;

	collect_logs(log_dir, &logs);
	if (!logs.nr) {
		svec_clear(&logs);
		die("no logs under @b{%s}", log_dir);
	}

	if (n < 1 || (size_t)n > logs.nr) {
		size_t total = logs.nr;

		svec_clear(&logs);
		die("log index %d out of range (have %zu)", n, total);
	}

	/* logs is oldest-first after sort; newest is index nr-1. */
	sbuf_addf(out, "%s/%s", log_dir, logs.v[logs.nr - (size_t)n]);
	svec_clear(&logs);
}

/*
 * Parse the "# ice-log: slug=<slug> start=<iso> duration_ms=<ms> exit=<rc>"
 * header written by process_run_progress().  Missing fields default
 * to placeholder values so older logs without the header still
 * render something useful in --all output.
 */
static void parse_log_header(const char *path, struct sbuf *slug,
			     struct sbuf *start,
			     unsigned long long *duration_ms, int *exit_code)
{
	FILE *fp;
	char line[256];
	const char *p;

	sbuf_reset(slug);
	sbuf_reset(start);
	*duration_ms = 0;
	*exit_code = -1;

	fp = fopen(path, "rb");
	if (!fp)
		return;
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return;
	}
	fclose(fp);

	if (strncmp(line, "# ice-log:", 10) != 0)
		return;

	p = strstr(line, "slug=");
	if (p) {
		p += 5;
		while (*p && *p != ' ' && *p != '\n')
			sbuf_addch(slug, *p++);
	}
	p = strstr(line, "start=");
	if (p) {
		p += 6;
		while (*p && *p != ' ' && *p != '\n')
			sbuf_addch(start, *p++);
	}
	p = strstr(line, "duration_ms=");
	if (p) {
		char *end;
		unsigned long long v = strtoull(p + 12, &end, 10);

		if (end != p + 12)
			*duration_ms = v;
	}
	p = strstr(line, "exit=");
	if (p) {
		char *end;
		long v = strtol(p + 5, &end, 10);

		if (end != p + 5)
			*exit_code = (int)v;
	}
}

static int list_all(const char *log_dir)
{
	struct svec logs = SVEC_INIT;

	collect_logs(log_dir, &logs);
	if (!logs.nr) {
		fprintf(stderr, "No logs under @b{%s}.\n", log_dir);
		svec_clear(&logs);
		return 0;
	}

	pager_start();

	/* Newest first to match what a user expects from "show all". */
	for (size_t i = logs.nr; i > 0; i--) {
		const char *name = logs.v[i - 1];
		struct sbuf full = SBUF_INIT;
		struct sbuf slug = SBUF_INIT;
		struct sbuf start = SBUF_INIT;
		unsigned long long duration_ms = 0;
		int exit_code = -1;
		unsigned secs;
		unsigned tenths;

		sbuf_addf(&full, "%s/%s", log_dir, name);
		parse_log_header(full.buf, &slug, &start, &duration_ms,
				 &exit_code);

		secs = (unsigned)(duration_ms / 1000ull);
		tenths = (unsigned)((duration_ms % 1000ull) / 100ull);

		if (start.len && slug.len && exit_code >= 0) {
			printf("%-19s  %-14s  exit %-4d  %u.%us\n", start.buf,
			       slug.buf, exit_code, secs, tenths);
		} else {
			/* Header missing or malformed -- show the filename
			 * so the user can still locate the file. */
			printf("%s\n", name);
		}

		sbuf_release(&full);
		sbuf_release(&slug);
		sbuf_release(&start);
	}

	svec_clear(&logs);
	return 0;
}

static int show_last(const char *log_dir)
{
	struct sbuf last_path = SBUF_INIT;
	struct sbuf target = SBUF_INIT;
	int rc;

	sbuf_addf(&last_path, "%s/last", log_dir);
	if (sbuf_read_file(&target, last_path.buf) < 0) {
		sbuf_release(&last_path);
		sbuf_release(&target);
		die("no last-log pointer at @b{%s}", log_dir);
	}
	sbuf_rtrim(&target);

	if (!target.len)
		die("last-log pointer at @b{%s} is empty", last_path.buf);

	rc = display_log(target.buf);

	sbuf_release(&last_path);
	sbuf_release(&target);
	return rc;
}

int cmd_log(int argc, const char **argv)
{
	const char *log_dir;
	int rc;

	argc = parse_options(argc, argv, &cmd_log_desc);
	if (argc > 0)
		die("too many arguments");
	if (opt_n && opt_all)
		die("-n and --all are mutually exclusive");
	if (opt_n < 0)
		die("-n argument must be >= 1");

	log_dir = config_get("_project.log-dir");
	if (!log_dir || !*log_dir)
		die("no log directory for active profile");

	if (opt_all)
		return list_all(log_dir);

	if (opt_n) {
		struct sbuf path = SBUF_INIT;

		resolve_log_by_index(log_dir, opt_n, &path);
		rc = display_log(path.buf);
		sbuf_release(&path);
		return rc;
	}

	return show_last(log_dir);
}
