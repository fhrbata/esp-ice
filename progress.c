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
 * Runtime live-tail toggle: when stdin is a tty the read loop also
 * polls for keystrokes (raw mode with @c TERM_RAW_KEEP_SIG so Ctrl-C
 * still kills the child).  Pressing @b{Ctrl-v} flips between the
 * spinner and verbose-mirror mode -- the spinner is cleared on the
 * way in and child bytes stream straight to stdout; on the way back
 * out a newline is emitted and the spinner resumes on a fresh row.
 * The atexit handler in @c platform/{posix,win}/term.c restores the
 * cooked mode if a signal kills the process.
 *
 * Captured output lands at ~/.ice/logs/YYYYMMDD-HHMMSS-<slug>-<pid>.log
 * so every spawn produces a uniquely-named artefact regardless of which
 * command invoked the helper -- the PID disambiguates concurrent or
 * nested invocations that share a wall-clock second, and the
 * @c{<slug>-<pid>} suffix is also the typing-friendly identifier the
 * failure hint prints (`ice log <slug>-<pid>`).
 */
#include "ice.h"

#include <time.h>

#include "color_rules.h"
#include "progress.h"

/*
 * Fixed-width header written at file open (padded with spaces) and
 * rewritten in-place at file close.  First line of every log;
 * @b{ice log --all} parses it to summarise a run without reading the
 * whole body.  Width keeps it simple: slug + iso timestamp +
 * duration_ms + exit code fit comfortably in 128 bytes.
 */
#define LOG_HEADER_BYTES 128

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

/* After this much elapsed time we append a reassurance tag to the
 * spinner so a slow op (toolchain download, big clone, ...) doesn't
 * look stuck.  Short enough that only genuinely slow work trips it;
 * long enough that fast ops stay quiet. */
#define PROGRESS_SLOW_HINT_MS 5000
#define PROGRESS_SLOW_HINT " @[2]{[this can take a while]}"
/* Always-on cue when raw mode is active so the user knows the toggle
 * exists from the first frame.  Lowercase @c{v} on purpose -- @c{V}
 * reads as Shift-V and would mislead users into pressing the wrong
 * combo. */
#define PROGRESS_CTRLV_HINT " @[2]{[Ctrl-v shows live output]}"

static int progress_interactive(void) { return isatty(STDOUT_FILENO); }

static void format_elapsed(char *out, size_t cap, unsigned long long ms)
{
	unsigned secs = (unsigned)(ms / 1000ull);
	unsigned tenths = (unsigned)((ms % 1000ull) / 100ull);
	snprintf(out, cap, "%u.%us", secs, tenths);
}

/*
 * Resolve where this invocation's log file lives.  @c _project.log-dir
 * is populated by setup_project() once a CONFIGURED project is in
 * scope and points at @c <build>/.ice/logs; when it's unset (e.g.
 * @b{ice repo clone}, @b{ice tools install}, @b{ice init} before the
 * marker is written) the log falls back to ~/.ice/logs/ so early
 * failures still have a place to land.
 */
static void build_log_path(struct sbuf *out, const char *slug)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	const char *log_dir = config_get("_project.log-dir");

	if (log_dir && *log_dir)
		sbuf_addf(out, "%s/", log_dir);
	else
		sbuf_addf(out, "%s/logs/", ice_home());

	sbuf_addf(out, "%04d%02d%02d-%02d%02d%02d-%s-%d.log",
		  tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
		  tm->tm_min, tm->tm_sec, slug, self_pid());
}

/*
 * ISO-8601 local time, to the second, no offset -- cheap and readable
 * in @b{ice log --all}, which is the only consumer.
 */
static void format_iso_ts(char *out, size_t cap, time_t t)
{
	struct tm *tm = localtime(&t);

	snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d", tm->tm_year + 1900,
		 tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min,
		 tm->tm_sec);
}

/*
 * Render the header line for the log file, padded to exactly
 * LOG_HEADER_BYTES so it can be overwritten in place at close.  The
 * trailing '\n' is at the last byte so the padding looks clean when
 * someone runs `head -1` on a finalised log.
 */
static void format_log_header(char *out, const char *slug, const char *iso,
			      unsigned long long duration_ms, int rc)
{
	int n = snprintf(out, LOG_HEADER_BYTES,
			 "# ice-log: slug=%s start=%s duration_ms=%llu exit=%d",
			 slug, iso, duration_ms, rc);

	if (n < 0)
		n = 0;
	if (n > LOG_HEADER_BYTES - 1)
		n = LOG_HEADER_BYTES - 1;
	memset(out + n, ' ', (size_t)(LOG_HEADER_BYTES - 1 - n));
	out[LOG_HEADER_BYTES - 1] = '\n';
}

static void progress_draw(const char *msg, int frame, unsigned long long start,
			  int raw)
{
	unsigned long long elapsed_ms = mono_ms() - start;
	char elapsed[32];
	const char *slow_hint = "";
	const char *ctrlv_hint = "";

	format_elapsed(elapsed, sizeof elapsed, elapsed_ms);
	if (elapsed_ms >= PROGRESS_SLOW_HINT_MS)
		slow_hint = PROGRESS_SLOW_HINT;
	if (raw)
		ctrlv_hint = PROGRESS_CTRLV_HINT;
	/* \r returns to column 0; the line only grows in length as
	 * seconds tick up (and the slow-hint clause appears once across
	 * the threshold), so no explicit clear sequence is needed
	 * between redraws.  Slow hint goes BEFORE the Ctrl-v hint so
	 * the latter's position is stable -- "[this can take a while]"
	 * is inserted in front, the Ctrl-v hint just shifts right.
	 * Colors go through the @c{} / @[2]{} tokens so they degrade
	 * to plain text on non-tty output. */
	fprintf(stdout, "\r@c{%s} %s (%s)%s%s",
		spinner_frames[frame % SPINNER_NFRAMES], msg, elapsed,
		slow_hint, ctrlv_hint);
	fflush(stdout);
}

static void progress_clear(void)
{
	/* ANSI [K erases from cursor to end of line without advancing,
	 * which is both correct and immune to terminal-width wrapping.
	 * The legacy Windows console writer only parses SGR sequences,
	 * so there fall back to a bounded space pad sized to fit any
	 * realistic terminal without wrapping above the final line. */
	if (use_vt)
		fputs("\r\x1b[K", stdout);
	else
		fputs("\r                                                      "
		      "                          \r",
		      stdout);
	fflush(stdout);
}

/*
 * On failure (non-verbose): re-read the captured log, pass it through
 * @p on_fail (or use the whole log if NULL), colorize the result with
 * the default rule table, and write it to stderr.  Silently skip if
 * the log can't be read or the filter leaves nothing to show.
 */
static void dump_failure_log(const char *log_path, progress_fail_cb on_fail)
{
	struct sbuf content = SBUF_INIT;
	struct sbuf filtered = SBUF_INIT;
	struct sbuf colored = SBUF_INIT;

	if (sbuf_read_file(&content, log_path) < 0 || content.len == 0)
		goto done;

	if (on_fail)
		on_fail(&filtered, content.buf, content.len);
	else
		sbuf_add(&filtered, content.buf, content.len);

	if (filtered.len == 0)
		goto done;

	color_text(&colored, filtered.buf, filtered.len,
		   ice_default_color_rules);

	fputs("\n", stderr);
	fputs(colored.buf, stderr);
	if (filtered.buf[filtered.len - 1] != '\n')
		fputs("\n", stderr);

done:
	sbuf_release(&colored);
	sbuf_release(&filtered);
	sbuf_release(&content);
}

int process_run_progress(struct process *proc, const char *msg,
			 const char *slug, progress_fail_cb on_fail)
{
	struct sbuf log_path = SBUF_INIT;
	char header[LOG_HEADER_BYTES];
	char iso[32];
	FILE *log = NULL;
	char buf[4096];
	char elapsed[32];
	time_t start_wall;
	ssize_t n;
	int rc;
	int frame = 0;
	int interactive = progress_interactive();
	/* Wrapped child: parent owns the log file and the user-facing
	 * output.  Force verbose so the read loop mirrors child bytes to
	 * stdout, where the parent's pipe captures them.
	 *
	 * Non-interactive (piped, redirected, CI): no spinner is
	 * possible, so default to verbose -- silence is the wrong
	 * default when there's no progress UI to fall back to.  Users
	 * who really want quiet can redirect stderr (where the spinner
	 * would have lived) and parse the captured log themselves. */
	int wrapped = global_wrapped;
	int verbose = (wrapped || !interactive) ? 1 : global_verbose;
	int raw_active = 0;
	unsigned long long start;
	unsigned long long duration_ms;

	if (!wrapped) {
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

		/*
		 * Reserve the header's bytes at file start; real values get
		 * written back at close once duration and exit are known.  A
		 * newline at the last byte keeps `head -1` output tidy even
		 * if the run abends before we rewrite in place.
		 */
		memset(header, ' ', LOG_HEADER_BYTES);
		header[LOG_HEADER_BYTES - 1] = '\n';
		fwrite(header, 1, LOG_HEADER_BYTES, log);
	}

	proc->pipe_out = 1;
	proc->pipe_err = 0;
	proc->merge_err = 1;

	start_wall = time(NULL);
	if (!wrapped)
		format_iso_ts(iso, sizeof iso, start_wall);

	start = mono_ms();
	if (process_start(proc)) {
		if (log)
			fclose(log);
		sbuf_release(&log_path);
		return -1;
	}

	/* Enter raw mode so we can poll for the @b{Ctrl-v} verbose
	 * toggle while the child runs.  Skip when already verbose
	 * (nothing to toggle to), wrapped (parent owns user-facing
	 * output), or non-interactive (no tty / no stdin to listen
	 * on).  TERM_RAW_KEEP_SIG keeps Ctrl-C signal generation so
	 * the user can still abort. */
	if (interactive && !verbose && !wrapped) {
		if (term_raw_enter(TERM_RAW_KEEP_SIG) == 0)
			raw_active = 1;
	}

	if (!verbose && interactive)
		progress_draw(msg, frame, start, raw_active);

	for (;;) {
		/* Drain any pending key events before sleeping in
		 * pipe_read_timed -- a buffered Ctrl-v should flip
		 * verbose mode on the next iteration without waiting
		 * another 100 ms tick.  Multiple presses queued up in
		 * one tick collapse to the last state, which matches
		 * what the user just asked for. */
		if (raw_active) {
			struct term_event ev;
			while (term_read_event(&ev, 0) == 1) {
				if (ev.key != TK_CTRL('v'))
					continue;
				verbose = !verbose;
				if (!interactive)
					continue;
				if (verbose) {
					/* Spinner row → live tail: wipe
					 * the spinner so the first child
					 * bytes start at column 0. */
					progress_clear();
				} else {
					/* Live tail → spinner: child output
					 * may have left the cursor mid-line
					 * with no trailing newline.  Start
					 * a fresh row before progress_draw
					 * lays the spinner down. */
					fputs("\n", stdout);
					fflush(stdout);
					progress_draw(msg, frame, start,
						      raw_active);
				}
			}
		}

		n = pipe_read_timed(proc->out, buf, sizeof buf,
				    PROGRESS_POLL_MS);
		if (n < 0)
			break;
		if (n == 0) {
			if (!verbose && interactive)
				progress_draw(msg, ++frame, start, raw_active);
			continue;
		}
		if (log)
			fwrite(buf, 1, (size_t)n, log);
		if (verbose) {
			fwrite(buf, 1, (size_t)n, stdout);
			fflush(stdout);
		}
	}

	if (raw_active)
		term_raw_leave();

	rc = process_finish(proc);
	duration_ms = mono_ms() - start;

	if (log) {
		/*
		 * Rewrite the reserved header in place with the real values.
		 * Fixed width keeps the seek simple: formatted output is
		 * truncated to LOG_HEADER_BYTES - 1 and space-padded, and
		 * the trailing newline stays at the same offset.
		 */
		format_log_header(header, slug, iso, duration_ms, rc);
		fseek(log, 0, SEEK_SET);
		fwrite(header, 1, LOG_HEADER_BYTES, log);
		fclose(log);
	}

	if (!verbose && interactive)
		progress_clear();

	format_elapsed(elapsed, sizeof elapsed, duration_ms);
	if (rc == 0) {
		printf("@g{" CHECK_MARK "} %s @g{done}. (%s)\n", msg, elapsed);
	} else {
		printf("@r{" CROSS_MARK "} %s @r{failed}. (%s)\n", msg,
		       elapsed);
		/* hint() and the dump write to (unbuffered) stderr; flush
		 * stdout first so the "failed" headline appears above
		 * them. */
		fflush(stdout);
		/* Wrapped: the parent will dump its own captured log and
		 * print the user-facing hint -- the child stays quiet. */
		if (!wrapped) {
			/* Verbose mode already streamed everything live, so
			 * skip the dump to avoid printing the same output
			 * twice. */
			if (!verbose)
				dump_failure_log(log_path.buf, on_fail);
			/*
			 * Scan the captured log against ESP-IDF's hints.yml
			 * and emit HINT: lines for any matching rule.  Gated
			 * on the same "configured project" check as `ice log`
			 * below -- outside a project we have no idf-path to
			 * find the rules file.  --no-hints / core.no-hints
			 * opts out.  Format matches ESP-IDF's
			 * yellow_print("HINT: ...") convention.
			 */
			if (!global_no_hints &&
			    config_has("_project.configured")) {
				const char *idf =
				    config_get("_project.idf-path");
				if (idf && *idf) {
					struct sbuf hints_yml = SBUF_INIT;
					struct svec hints = SVEC_INIT;
					sbuf_addf(&hints_yml,
						  "%s/tools/idf_py_actions/"
						  "hints.yml",
						  idf);
					hints_scan(hints_yml.buf, log_path.buf,
						   &hints);
					for (size_t i = 0; i < hints.nr; i++)
						printf("@y{HINT: %s}\n",
						       hints.v[i]);
					fflush(stdout);
					svec_clear(&hints);
					sbuf_release(&hints_yml);
				}
			}
			if (config_has("_project.configured"))
				hint("see %s for details, or run "
				     "@b{ice log %s-%d}",
				     log_path.buf, slug, self_pid());
			else
				hint("see %s for details", log_path.buf);
		}
	}

	sbuf_release(&log_path);
	return rc;
}
