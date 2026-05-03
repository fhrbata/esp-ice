/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/monitor/monitor.c
 * @brief `ice target monitor` -- plumbing serial port monitor.
 *
 * Thin command-shaped wrapper around the reusable @ref monitor_session
 * abstraction: opens the serial port, owns the user's terminal, drives
 * one session full-screen until the user quits.  The actual UX
 * (vt100 + tui_log + level decoration + Ctrl-T prefix +
 * inspect/search/help) lives in @c monitor.c at the project root so
 * the same experience is available to @c{ice qemu}, @c{ice qemu
 * --debug}, and the planned @c{ice debug} without re-implementing it.
 *
 * Two user-visible modes:
 *
 *   - @b{normal}: pure passthrough.  Every keystroke goes to the
 *     serial port unchanged except @c Ctrl-T, which acts as a one-byte
 *     prefix for monitor commands (the next byte selects the action).
 *
 *   - @b{inspect}: enters via @c Ctrl-T+i and freezes the buffer.
 *     Bytes keep arriving in the background but stay invisible until
 *     the user exits.  Inside, vim-style navigation + regex search.
 *
 * Help is per-mode (@c Ctrl-T+? in normal, @c ? in inspect).  @c Ctrl-]
 * is an undocumented panic eject from any state.
 *
 * Usage:
 *   ice target monitor [--port <dev>] [--chip <name>] [--baud <rate>]
 *                      [--no-reset] [--scrollback <lines>]
 */
#include "monitor.h"
#include "esf_port.h"
#include "ice.h"
#include "serial.h"
#include "tui.h"

static const char *opt_port;
static const char *opt_chip;
static int opt_baud = 115200;
static int opt_no_reset;
static int opt_scrollback = 10000;
static int opt_no_tui;

/* clang-format off */
static const struct option cmd_target_monitor_opts[] = {
	OPT_STRING('p', "port", &opt_port, "dev",
		   "serial port device (auto-detected if omitted)",
		   serial_complete_port),
	OPT_STRING('c', "chip", &opt_chip, "name",
		   "expected chip name for port auto-detection (e.g. esp32c6)",
		   NULL),
	OPT_INT('b', "baud", &opt_baud, "rate",
		"baud rate (default: 115200)", NULL),
	OPT_BOOL(0, "no-reset", &opt_no_reset,
		 "open port without touching DTR/RTS"),
	OPT_INT(0, "scrollback", &opt_scrollback, "lines",
		"scrollback buffer in lines (default: 10000)", NULL),
	OPT_BOOL(0, "no-tui", &opt_no_tui,
		 "dumb passthrough; no alt-screen, no inspect mode "
		 "(auto when stdout/stdin isn't a tty)"),
	OPT_END(),
};

static const struct cmd_manual target_monitor_manual = {
	.name = "ice target monitor",
	.summary = "open a serial port for monitoring",

	.description =
	H_PARA("Low-level serial port monitor.  Opens @b{--port}, "
	       "optionally deasserts DTR/RTS to prevent an accidental "
	       "reset, then bridges the port to the terminal.  Two modes: "
	       "@b{normal} is pure passthrough so target-side editors and "
	       "REPLs work unchanged; @b{inspect} freezes the buffer at the "
	       "moment of entry and exposes a vim-like keymap for searching "
	       "and scrolling history while new bytes accumulate in the "
	       "background.")
	H_PARA("Lines matching the ESP-IDF log format -- a single letter "
	       "@b{E} / @b{W} / @b{I} followed by a space -- are tinted "
	       "red / yellow / green respectively.  Firmware that already "
	       "emits its own ANSI colours is passed through unchanged "
	       "(no double-paint).  Use @b{--no-color} on the @b{ice} "
	       "command to suppress the local tinting.")
	H_PARA("Whenever stdout or stdin is not a terminal -- piped output, "
	       "redirected to a file, captured by CI -- the monitor "
	       "automatically drops into a dumb passthrough loop: no "
	       "alt-screen, no inspect mode, no key bindings, just bytes "
	       "from the serial port to stdout (and stdin, if any, to the "
	       "port).  Use @b{--no-tui} to force the dumb path explicitly.")
	H_PARA("When @b{--port} is omitted the first available ESP device is "
	       "auto-detected.  Pass @b{--chip} to restrict the scan to a "
	       "specific chip family.")
	H_PARA("This command reads no project configuration.  It is the "
	       "plumbing behind @b{ice monitor}, which resolves the port "
	       "and baud rate from the project profile."),

	.examples =
	H_EXAMPLE("ice target monitor --port /dev/ttyUSB0")
	H_EXAMPLE("ice target monitor")
	H_EXAMPLE("ice target monitor --chip esp32c6")
	H_EXAMPLE("ice target monitor -p /dev/ttyACM0 -b 460800")
	H_EXAMPLE("ice target monitor -p /dev/ttyUSB0 --no-reset")
	H_EXAMPLE("ice target monitor -p /dev/ttyUSB0 --scrollback 50000")
	H_EXAMPLE("ice target monitor -p /dev/ttyUSB0 | tee session.log")
	H_EXAMPLE("ice target monitor -p /dev/ttyUSB0 --no-tui"),

	.extras =
	H_SECTION("NORMAL MODE (live passthrough)")
	H_ITEM("Ctrl-T x",   "Exit the monitor.")
	H_ITEM("Ctrl-T i",   "Enter inspect mode (freeze + explore the buffer).")
	H_ITEM("Ctrl-T r",   "Reset the target.")
	H_ITEM("Ctrl-T p",   "Reset into the bootloader.")
	H_ITEM("Ctrl-T ?",   "Show normal-mode help.")
	H_ITEM("Ctrl-T Ctrl-T", "Send a literal Ctrl-T to the target.")

	H_SECTION("INSPECT MODE (frozen snapshot)")
	H_ITEM("/",          "Open a regex search prompt (PCRE2).")
	H_ITEM("n / N",      "Jump to the next / previous match.")
	H_ITEM("j / Down",   "Scroll one line down.")
	H_ITEM("k / Up",     "Scroll one line up.")
	H_ITEM("PgUp/PgDn",  "Scroll one page.")
	H_ITEM("g / Home",   "Top of the snapshot.")
	H_ITEM("G / End",    "Bottom of the snapshot.")
	H_ITEM("?",          "Show inspect-mode help.")
	H_ITEM("Esc / q",    "Exit inspect (back to normal).")

	H_SECTION("SEE ALSO")
	H_ITEM("ice monitor",
	       "Porcelain wrapper: resolves port from project profile."),
};
/* clang-format on */

int cmd_target_monitor(int argc, const char **argv);

const struct cmd_desc cmd_target_monitor_desc = {
    .name = "monitor",
    .fn = cmd_target_monitor,
    .opts = cmd_target_monitor_opts,
    .manual = &target_monitor_manual,
};

/*
 * Reset the target without entering bootloader.  Sequence matches
 * @c ice_target_reset in @c esf_port.c -- both modem lines are driven
 * together via @c serial_set_dtr_rts so the USB-UART bridge sees one
 * clean edge per step rather than the intermediate (half-set) state
 * two separate ioctls would expose.
 */
static void serial_target_reset(struct serial *s)
{
	serial_set_dtr_rts(s, 0, 1); /* BOOT high, RESET low */
	delay_ms(100);
	serial_set_dtr_rts(s, 0, 0); /* BOOT high, RESET high */
}

/*
 * Reset into the serial bootloader.  Holds BOOT (GPIO0) low while
 * pulsing RESET so the ROM samples @c BOOT==low at reset release and
 * lands in download mode.  Sequence is the UART path of
 * @c ice_enter_bootloader in @c esf_port.c (UnixTightReset).  USB
 * JTAG Serial chips need a different sequence we don't auto-detect
 * here -- those users still go through @c ice @c flash.
 */
static void serial_target_bootloader(struct serial *s)
{
	serial_set_dtr_rts(s, 0, 0); /* idle */
	serial_set_dtr_rts(s, 1, 1); /* through (1,1) avoids 0,0->0,1 glitch */
	serial_set_dtr_rts(s, 0, 1); /* BOOT high, RESET low */
	delay_ms(100);
	serial_set_dtr_rts(s, 1, 0); /* BOOT low, RESET high -> bootloader */
	delay_ms(50);
	serial_set_dtr_rts(s, 0, 0); /* release */
	serial_flush_input(s);	     /* drop ROM banner noise */
}

/* Trampolines so the monitor session can call into the serial helpers
 * via the @c on_reset / @c on_bootloader callbacks. */
static void cb_reset(void *ctx) { serial_target_reset((struct serial *)ctx); }
static void cb_bootloader(void *ctx)
{
	serial_target_bootloader((struct serial *)ctx);
}

/*
 * Dumb passthrough when stdout/stdin isn't a tty (CI, redirection,
 * capture) or @c --no-tui is set explicitly.  Pure bytes from the
 * serial port to @c stdout and -- as long as stdin stays alive --
 * any @c stdin bytes back to the port.  No vt100, no scrollback, no
 * key bindings; this is the path that lets @c{ice target monitor |
 * tee session.log} just work.
 */
static int monitor_run_dumb(struct serial *s)
{
	unsigned char buf[1024];
	int stdin_dead = 0;
	for (;;) {
		ssize_t n = serial_read(s, buf, sizeof(buf), 30);
		if (n < 0)
			return 0;
		if (n > 0) {
			fwrite(buf, 1, (size_t)n, stdout);
			fflush(stdout);
		}
		if (!stdin_dead) {
			n = term_read(buf, sizeof(buf), 0);
			if (n < 0)
				stdin_dead = 1;
			else if (n > 0)
				serial_write(s, buf, (size_t)n);
		}
	}
}

int cmd_target_monitor(int argc, const char **argv)
{
	opt_port = NULL;
	opt_chip = NULL;
	opt_baud = 115200;
	opt_no_reset = 0;
	opt_scrollback = 10000;
	opt_no_tui = 0;

	argc = parse_options(argc, argv, &cmd_target_monitor_desc);
	if (argc > 0)
		die("unexpected argument '%s'", argv[0]);

	if (opt_scrollback < 1)
		die("--scrollback must be at least 1");

	/* ---- resolve port (auto-detect if omitted) ---- */
	char *autoport = NULL;
	if (!opt_port) {
		enum ice_chip scan_chip = ice_chip_from_idf_name(opt_chip);
		autoport = esf_find_esp_port(scan_chip);
		if (!autoport)
			die("no ESP device found; use --port to specify a port "
			    "explicitly");
		opt_port = autoport;
	}

	unsigned baud = (unsigned)opt_baud;
	struct serial *s;
	int rc;

	rc = serial_open(&s, opt_port);
	if (rc)
		die("cannot open %s: %s", opt_port, strerror(-rc));

	if (!opt_no_reset) {
		serial_set_dtr(s, 0);
		serial_set_rts(s, 0);
	}

	rc = serial_set_baud(s, baud);
	if (rc) {
		serial_close(s);
		die("cannot set baud rate %u on %s: %s", baud, opt_port,
		    strerror(-rc));
	}

	/* Dumb passthrough when the user asked for it explicitly or when
	 * either side isn't a tty (CI, redirection, capture).  We need a
	 * tty on both ends because the TUI relies on raw-mode input and
	 * alt-screen output; falling back gracefully is friendlier than
	 * dying with -ENOTTY out of @ref term_raw_enter. */
	if (opt_no_tui || !isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO)) {
		int dumb_rc = monitor_run_dumb(s);
		serial_close(s);
		free(autoport);
		return dumb_rc;
	}

	rc = term_raw_enter(0);
	if (rc) {
		serial_close(s);
		die("cannot set terminal to raw mode: %s", strerror(-rc));
	}
	term_screen_enter();

	int cols = 80, rows = 24;
	term_size(&cols, &rows);

	struct sbuf label = SBUF_INIT;
	sbuf_addf(&label, "ice monitor: %s @ %u baud", opt_port, baud);

	struct monitor_config cfg = {
	    .scrollback = opt_scrollback,
	    .origin_x = 1,
	    .origin_y = 1,
	    .width = cols,
	    .height = rows,
	    .source_label = label.buf,
	    .on_reset = cb_reset,
	    .on_bootloader = cb_bootloader,
	    .cb_ctx = s,
	};
	struct monitor_session *m = monitor_new(&cfg);
	sbuf_release(&label);
	if (!m) {
		term_screen_leave();
		term_raw_leave();
		serial_close(s);
		free(autoport);
		die("monitor_new: out of memory");
	}

	serial_flush_input(s);

	/* Initial render so the user sees the status bar before the first
	 * byte arrives.  After this the dirty flag governs redraws. */
	{
		struct sbuf frame = SBUF_INIT;
		monitor_render(&frame, m);
		tui_flush(&frame);
		sbuf_release(&frame);
		monitor_clear_dirty(m);
	}

	while (!monitor_should_quit(m)) {
		if (term_resize_pending()) {
			term_size(&cols, &rows);
			monitor_set_rect(m, 1, 1, cols, rows);
		}

		/* Drain whatever the chip just produced; serial_read uses
		 * a 30 ms timeout so the loop ticks even when the chip is
		 * silent (lets the resize check fire without a busy-spin). */
		unsigned char buf[1024];
		ssize_t n = serial_read(s, buf, sizeof(buf), 30);
		if (n < 0)
			break;
		if (n > 0)
			monitor_feed_chip(m, buf, (size_t)n);

		/* One key event per pass keeps the prefix-then-key cadence
		 * snappy without monopolising the loop on a paste burst.
		 * term_read_event covers both raw bytes (for normal
		 * passthrough) and the parsed keys inspect / search / help
		 * need; the session decides what applies. */
		struct term_event ev;
		int got = term_read_event(&ev, 0);
		if (got > 0 && ev.key != TK_NONE)
			monitor_feed_event(m, &ev);

		struct sbuf *tx = monitor_chip_tx(m);
		if (tx->len) {
			serial_write(s, (unsigned char *)tx->buf, tx->len);
			sbuf_reset(tx);
		}

		if (monitor_dirty(m)) {
			struct sbuf frame = SBUF_INIT;
			monitor_render(&frame, m);
			tui_flush(&frame);
			sbuf_release(&frame);
			monitor_clear_dirty(m);
		}
	}

	monitor_release(m);
	term_screen_leave();
	term_raw_leave();
	serial_close(s);
	free(autoport);
	fprintf(stderr, "--- exit ---" EOL);
	return 0;
}
