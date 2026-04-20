/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file progress.c
 * @brief Tee-with-progress wrapper around process_start/_finish.
 *
 * Sits on top of the portable process API in platform.h and the timed
 * pipe-reader in pipe_read_timed().  Everything here is plain C -- no
 * #ifdef -- because the OS differences are already absorbed by those
 * primitives.
 *
 * Interactive display uses a 10-frame braille spinner (the same set
 * popularised by ora / yarn / vite) with an elapsed-time counter, and
 * closes with a green check-mark ✓ or red cross ✗.  Spinner glyphs
 * and mark glyphs are written as raw UTF-8 byte sequences so the
 * source file stays strict ASCII -- same convention used for the
 * box-drawing characters in term.h.
 *
 * Captured output lands at ~/.ice/logs/YYYYMMDD-HHMMSS-<slug>.log so
 * every spawn produces a uniquely-named artefact regardless of which
 * command invoked the helper.  The path is printed via hint() when
 * the child exits non-zero.
 */
#include "ice.h"

#include <time.h>

#include "progress.h"

/* ⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏ -- U+280B .. U+280F braille patterns. */
static const char *const spinner_frames[] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
    "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
    "\xe2\xa0\x87", "\xe2\xa0\x8f",
};
#define SPINNER_NFRAMES (sizeof spinner_frames / sizeof spinner_frames[0])

#define CHECK_MARK "\xe2\x9c\x93" /* ✓ U+2713 */
#define CROSS_MARK "\xe2\x9c\x97" /* ✗ U+2717 */

/* How long pipe_read_timed waits per iteration before ticking the
 * spinner.  100 ms gives a ~10 Hz animation -- fast enough to feel
 * live, slow enough to be cheap. */
#define PROGRESS_POLL_MS 100

static int progress_interactive(void) { return isatty(STDOUT_FILENO); }

static void format_elapsed(char *out, size_t cap, unsigned long long ms)
{
	unsigned secs = (unsigned)(ms / 1000ull);
	unsigned tenths = (unsigned)((ms % 1000ull) / 100ull);
	snprintf(out, cap, "%u.%us", secs, tenths);
}

static void build_log_path(struct sbuf *out, const char *slug)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	sbuf_addf(out, "%s/logs/%04d%02d%02d-%02d%02d%02d-%s.log", ice_home(),
		  tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
		  tm->tm_min, tm->tm_sec, slug);
}

static void progress_draw(const char *msg, int frame, unsigned long long start)
{
	char elapsed[32];

	format_elapsed(elapsed, sizeof elapsed, mono_ms() - start);
	/* \r returns to column 0; the line only grows in length as
	 * seconds tick up, so no explicit clear sequence is needed
	 * between redraws.  Colors go through the @c{} token so they
	 * degrade to plain text on non-tty output. */
	fprintf(stdout, "\r@c{%s} %s (%s)",
		spinner_frames[frame % SPINNER_NFRAMES], msg, elapsed);
	fflush(stdout);
}

static void progress_clear(void)
{
	/* Generous space padding erases the spinner line on all
	 * terminals without relying on ANSI [K -- the legacy Windows
	 * console writer does not parse non-SGR escapes. */
	fputs("\r                                                              "
	      "                  \r",
	      stdout);
	fflush(stdout);
}

int process_run_progress(struct process *proc, const char *msg,
			 const char *slug)
{
	struct sbuf log_path = SBUF_INIT;
	FILE *log;
	char buf[4096];
	char elapsed[32];
	ssize_t n;
	int rc;
	int frame = 0;
	int interactive;
	int verbose = global_verbose;
	unsigned long long start;

	build_log_path(&log_path, slug);

	if (mkdirp_for_file(log_path.buf) < 0) {
		err_errno("mkdirp: '%s'", log_path.buf);
		sbuf_release(&log_path);
		return -1;
	}

	log = fopen(log_path.buf, "wb");
	if (!log) {
		err_errno("fopen: '%s'", log_path.buf);
		sbuf_release(&log_path);
		return -1;
	}

	proc->pipe_out = 1;
	proc->pipe_err = 0;
	proc->merge_err = 1;

	start = mono_ms();
	if (process_start(proc)) {
		fclose(log);
		sbuf_release(&log_path);
		return -1;
	}

	interactive = progress_interactive();

	if (!verbose && interactive)
		progress_draw(msg, frame, start);

	for (;;) {
		n = pipe_read_timed(proc->out, buf, sizeof buf,
				    PROGRESS_POLL_MS);
		if (n < 0)
			break;
		if (n == 0) {
			if (!verbose && interactive)
				progress_draw(msg, ++frame, start);
			continue;
		}
		fwrite(buf, 1, (size_t)n, log);
		if (verbose) {
			fwrite(buf, 1, (size_t)n, stdout);
			fflush(stdout);
		}
	}

	rc = process_finish(proc);
	fclose(log);

	if (!verbose && interactive)
		progress_clear();

	format_elapsed(elapsed, sizeof elapsed, mono_ms() - start);
	if (rc == 0) {
		printf("@g{" CHECK_MARK "} %s @g{done}. (%s)\n", msg, elapsed);
	} else {
		printf("@r{" CROSS_MARK "} %s @r{failed}. (%s)\n", msg,
		       elapsed);
		/* hint() writes to (unbuffered) stderr; flush stdout
		 * first so the "failed" headline appears above it. */
		fflush(stdout);
		hint("see %s for details", log_path.buf);
	}

	sbuf_release(&log_path);
	return rc;
}
