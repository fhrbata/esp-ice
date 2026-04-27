/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/monitor/monitor.c
 * @brief `ice target monitor` -- plumbing serial port monitor.
 *
 * Two user-visible modes:
 *
 *   - @b{normal}: pure passthrough.  Every keystroke goes to the
 *     serial port unchanged except @c Ctrl-T, which acts as a one-byte
 *     prefix for monitor commands (the next byte selects the action).
 *     This is the live state -- target REPLs, vim, readline-driven
 *     consoles all just work because we don't intercept their keys.
 *
 *   - @b{inspect}: enters via @c Ctrl-T+i and freezes the buffer at
 *     that moment via @ref tui_log_freeze.  Bytes keep arriving in the
 *     background but stay invisible until the user exits and re-enters.
 *     Inside inspect, the keyboard is rich -- @c /, @c n, @c N, arrows,
 *     @c j / @c k, @c g / @c G, @c PgUp / @c PgDn, @c Home / @c End all
 *     do the obvious things; @c Esc or @c q returns to normal.
 *
 * Help is per-mode (@c Ctrl-T+? in normal, @c ? in inspect) so users
 * see only the keys that are live for them right now.  @c Ctrl-] is an
 * undocumented panic-eject that exits from any state.
 *
 * Usage:
 *   ice target monitor [--port <dev>] [--chip <name>] [--baud <rate>]
 *                      [--no-reset] [--scrollback <lines>]
 */
#include "esf_port.h"
#include "ice.h"
#include "serial.h"
#include "tui.h"
#include "vt100.h"

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

/* Help-modal bodies, one per user-visible mode.  Caller-owned strings
 * referenced by @ref tui_info_init -- the static-literal lifetime is
 * fine because the widget only stores pointers into the body. */
static const char NORMAL_HELP_TEXT[] =
    "ice monitor - normal mode\n"
    "\n"
    "Normal mode is pure passthrough: every keystroke goes to the\n"
    "target except the Ctrl-T prefix.  Type Ctrl-T followed by:\n"
    "\n"
    "  ?              Show this help.\n"
    "  i              Enter inspect mode (freeze + explore the buffer).\n"
    "  r              Reset the target.\n"
    "  p              Reset into the bootloader.\n"
    "  x              Exit the monitor.\n"
    "  Ctrl-T         Send a literal Ctrl-T to the target.\n"
    "  Esc / anything Cancel the prefix without firing.\n";

static const char INSPECT_HELP_TEXT[] =
    "ice monitor - inspect mode\n"
    "\n"
    "Inspect mode shows a frozen snapshot of the buffer at the moment\n"
    "you entered.  New bytes keep arriving in the background but stay\n"
    "off the screen until you exit and re-enter.\n"
    "\n"
    "  /              Open the regex search prompt (PCRE2).\n"
    "  n / N          Jump to the next / previous match.\n"
    "  j / Down       Scroll one line down.\n"
    "  k / Up         Scroll one line up.\n"
    "  PgDn           Scroll one page down.\n"
    "  PgUp           Scroll one page up.\n"
    "  g / Home       Top of the snapshot.\n"
    "  G / End        Bottom of the snapshot.\n"
    "  ?              Show this help.\n"
    "  Esc / q        Exit inspect mode (back to normal).\n";

/*
 * Hard reset.  Pulses RESET asserted with BOOT deasserted so the chip
 * boots into the application.  Mirrors @c ice_reset_target in
 * @c esf_port.c -- both modem lines are driven together via
 * @c serial_set_dtr_rts so the USB-UART bridge sees one clean edge per
 * step rather than the intermediate (half-set) state two separate
 * ioctls would expose.
 */
static void monitor_reset(struct serial *s)
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
static void monitor_bootloader(struct serial *s)
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

/*
 * Mode flat enum.  Two user-visible modes (normal, inspect) plus a
 * couple of internal sub-states each (Ctrl-T prefix waiting, help
 * modal, search prompt).  @ref mode_is_inspect groups the inspect
 * family for the status bar's badge decision.
 */
enum {
	MON_NORMAL = 0,
	MON_NORMAL_PREFIX, /* Ctrl-T pressed, waiting for next byte */
	MON_NORMAL_HELP,   /* tui_info modal showing normal-mode help */
	MON_INSPECT,
	MON_INSPECT_SEARCH, /* tui_prompt modal active */
	MON_INSPECT_HELP,   /* tui_info modal showing inspect-mode help */
};

static int mode_is_inspect(int mode)
{
	return mode == MON_INSPECT || mode == MON_INSPECT_SEARCH ||
	       mode == MON_INSPECT_HELP;
}

/*
 * Rebuild the title-bar status string.  Always called before render so
 * the badge, key hints, and search counter stay live as state changes.
 * Cheap: a few @c sbuf appends and one pointer swap.
 */
static void update_status(struct sbuf *status, struct tui_log *L,
			  const char *port, unsigned baud, int mode)
{
	sbuf_reset(status);
	sbuf_addf(status, "ice monitor: %s @ %u baud", port, baud);
	if (mode_is_inspect(mode)) {
		sbuf_addstr(status, "  \xe2\x80\xa2  [INSPECT]");
		sbuf_addstr(status, "  \xe2\x80\xa2  Esc=back  ?=help");
		if (tui_log_search_active(L)) {
			sbuf_addstr(status, "  \xe2\x80\xa2  search: ");
			sbuf_addstr(status, tui_log_search_pattern(L));
			int total = tui_log_search_total(L);
			int idx = tui_log_search_index(L);
			if (total <= 0)
				sbuf_addstr(status, " (no matches)");
			else if (idx > 0)
				sbuf_addf(status, " (%d/%d)", idx, total);
			else
				sbuf_addf(status, " (%d)", total);
		}
	} else {
		sbuf_addstr(status, "  \xe2\x80\xa2  [NORMAL]");
		sbuf_addstr(
		    status,
		    "  \xe2\x80\xa2  Ctrl-T:  ?=help  i=inspect  x=exit");
	}
	tui_log_set_status(L, status->buf);
}

/*
 * Keyword rules for lines that don't carry an ESP-IDF level prefix --
 * compile output, link errors, CMake noise streamed over the serial
 * port (or any other text the user happens to be tailing).  Keywords
 * are matched literally; the SGR strings mirror the @c
 * ice_default_color_rules table used by @c cmd/log/log.c and
 * @c progress.c so the colouring stays consistent across the project.
 */
static const struct {
	const char *kw;
	size_t klen;
	const char *sgr;
} mon_kw_rules[] = {
    {"fatal error:", 12, "1;31"},
    {"FAILED:", 7, "1;31"},
    {"error:", 6, "1;31"},
    {"warning:", 8, "1;33"},
    {"note:", 5, "36"},
    {"CMake Error", 11, "0;31"},
    {"CMake Warning", 13, "0;33"},
    {"undefined reference to", 22, "0;31"},
    {"multiple definition of", 22, "0;31"},
    {"In file included from", 21, "36"},
};

/*
 * Per-line decorator.  Two layers:
 *
 *   1. ESP-IDF level prefix: lines starting with @c E / @c W / @c I
 *      followed by a space get a whole-line overlay tinted to match
 *      the firmware's own colouring.  @c D / @c V (debug / verbose)
 *      are intentionally uncoloured to match the firmware's defaults.
 *      Lines that already begin with an ANSI escape are passed through
 *      untouched -- the firmware coloured them itself and we don't
 *      double-paint.
 *
 *   2. For lines without a level prefix, scan for the keyword rules
 *      above and emit one overlay per match.  This covers the case
 *      where users tail compile / link output through the same
 *      monitor session (or run a non-IDF firmware that still echoes
 *      tool output) and want @c error: / @c warning: / etc. to read
 *      consistently with the rest of @c ice's colouring.
 *
 * When the level layer fires we skip the keyword scan -- the level
 * colour already conveys severity, and stacking another keyword
 * highlight on top of a matching @c error: in an @c E line would
 * produce a redundant red-on-red region.
 *
 * Installed only when @c use_color is set, so the global
 * @c --no-color flag disables both layers without per-call branching.
 */
static int monitor_decorate(const char *line, size_t len, void *ctx,
			    struct tui_overlay *out, int max)
{
	(void)ctx;
	if (max < 1 || len == 0)
		return 0;

	if ((unsigned char)line[0] == 0x1b)
		return 0; /* firmware already coloured this line */

	if (len >= 2 && line[1] == ' ') {
		const char *sgr_open = NULL;
		switch (line[0]) {
		case 'E':
			sgr_open = "0;31";
			break;
		case 'W':
			sgr_open = "0;33";
			break;
		case 'I':
			sgr_open = "0;32";
			break;
		}
		if (sgr_open) {
			out[0].start = 0;
			out[0].end = len;
			out[0].sgr_open = sgr_open;
			out[0].sgr_close = "0";
			return 1;
		}
	}

	int n = 0;
	for (size_t r = 0;
	     r < sizeof(mon_kw_rules) / sizeof(mon_kw_rules[0]) && n < max;
	     r++) {
		size_t klen = mon_kw_rules[r].klen;
		if (klen == 0 || klen > len)
			continue;
		size_t end = len - klen;
		for (size_t i = 0; i <= end && n < max; i++) {
			if (memcmp(line + i, mon_kw_rules[r].kw, klen) != 0)
				continue;
			out[n].start = i;
			out[n].end = i + klen;
			out[n].sgr_open = mon_kw_rules[r].sgr;
			out[n].sgr_close = "0";
			n++;
			i += klen - 1; /* loop's i++ steps past the match */
		}
	}
	return n;
}

/*
 * Dispatch one keystroke arriving after a @c Ctrl-T prefix.  Returns
 * @c 1 if the monitor should exit, @c 0 otherwise.  Owns the side
 * effects: sets @c *mode to @c MON_NORMAL_HELP / @c MON_INSPECT for
 * commands that change mode; otherwise leaves @c *mode at @c
 * MON_NORMAL.  Initialising the help / inspect state (allocating the
 * @c tui_info, freezing the buffer) happens here so both call sites
 * (in-chunk dispatch and @c MON_NORMAL_PREFIX) get identical
 * behaviour.
 */
static int dispatch_normal_prefix(unsigned char k, struct serial *s,
				  struct tui_log *L, struct tui_info *help_info,
				  int cols, int rows, int *mode)
{
	switch (k) {
	case 'x':
	case 'X':
		return 1;
	case 0x12: /* Ctrl-R */
	case 'r':
	case 'R':
		monitor_reset(s);
		break;
	case 0x10: /* Ctrl-P */
	case 'p':
	case 'P':
		monitor_bootloader(s);
		break;
	case 0x08: /* Ctrl-H */
	case 'h':
	case 'H':
	case '?':
		tui_info_init(help_info, "ice monitor - normal mode keys",
			      NORMAL_HELP_TEXT);
		tui_info_resize(help_info, cols, rows);
		/* Freeze the log so the visible content stops changing
		 * while help is up.  Each redraw produces an identical
		 * frame (log + modal), so the terminal has nothing to
		 * flicker -- without this, fresh serial bytes would
		 * scroll the log behind the modal and the user sees a
		 * brief blink as the log paints before the modal
		 * repaints over it. */
		tui_log_freeze(L);
		*mode = MON_NORMAL_HELP;
		break;
	case 'i':
	case 'I':
		tui_log_freeze(L);
		*mode = MON_INSPECT;
		break;
	case 0x14: /* literal Ctrl-T */
		serial_write(s, &k, 1);
		break;
	case 0x1d: /* Ctrl-]: panic eject */
		return 1;
	default:
		/* Esc / any other byte cancels the prefix without firing.
		 * Including unknown keys in the catch-all keeps mistakes
		 * cheap -- the user just tries again. */
		break;
	}
	return 0;
}

/*
 * Inspect-mode keystroke dispatch.  Synthesises the right
 * @ref term_event and feeds it to @ref tui_log_on_event for
 * navigation; opens the search prompt; jumps the search cursor;
 * dismisses inspect.  Returns @c 1 to exit the monitor, @c 0
 * otherwise; sets @c *mode to @c MON_INSPECT_SEARCH /
 * @c MON_INSPECT_HELP / @c MON_NORMAL when the action transitions
 * to a different state.
 */
static int dispatch_inspect_event(const struct term_event *ev,
				  struct tui_log *L,
				  struct tui_prompt *search_prompt,
				  struct tui_info *help_info, int cols,
				  int rows, int *mode)
{
	if (ev->key == 0x1d) /* Ctrl-]: panic eject */
		return 1;

	/* Translate vim-letter shortcuts to their term_key equivalents
	 * so the widget's on_event handles both keyboard styles
	 * uniformly. */
	int key = ev->key;
	if (key == 'j')
		key = TK_DOWN;
	else if (key == 'k')
		key = TK_UP;
	else if (key == 'g')
		key = TK_HOME;
	else if (key == 'G')
		key = TK_END;

	switch (key) {
	case TK_UP:
	case TK_DOWN:
	case TK_PGUP:
	case TK_PGDN:
	case TK_HOME:
	case TK_END: {
		struct term_event nav = {.key = key};
		tui_log_on_event(L, &nav);
		return 0;
	}
	case '/':
		tui_prompt_init(
		    search_prompt, "search:",
		    tui_log_search_pattern(L) ? tui_log_search_pattern(L) : "");
		tui_prompt_resize(search_prompt, cols, rows);
		*mode = MON_INSPECT_SEARCH;
		return 0;
	case 'n':
		tui_log_search_next(L);
		return 0;
	case 'N':
		tui_log_search_prev(L);
		return 0;
	case '?':
		tui_info_init(help_info, "ice monitor - inspect mode keys",
			      INSPECT_HELP_TEXT);
		tui_info_resize(help_info, cols, rows);
		*mode = MON_INSPECT_HELP;
		return 0;
	case TK_ESC:
	case 'q':
	case 'Q':
		tui_log_search_clear(L);
		tui_log_unfreeze(L);
		*mode = MON_NORMAL;
		return 0;
	default:
		return 0;
	}
}

/*
 * Dumb passthrough loop.  Engaged when stdout / stdin isn't a tty (CI,
 * piped capture, redirected output) or when the user explicitly passes
 * @b{--no-tui}.  No alt screen, no raw mode, no widgets -- just drain
 * serial to stdout and forward whatever stdin produces back to the
 * port.  Stdin EOF (the typical pipe-source case: @c echo @c "x" @c |
 * @c ice @c monitor) latches a flag so we keep draining serial output
 * even after the input source has closed.  Exits cleanly when the
 * serial port itself returns an error (port unplug) or when a signal
 * tears the process down.
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
	unsigned char buf[1024];
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

	rc = term_raw_enter();
	if (rc) {
		serial_close(s);
		die("cannot set terminal to raw mode: %s", strerror(-rc));
	}
	term_screen_enter();

	int cols = 80, rows = 24;
	term_size(&cols, &rows);

	struct tui_log L;
	tui_log_init(&L, opt_scrollback);
	if (use_color)
		tui_log_set_decorator(&L, monitor_decorate, NULL);
	tui_log_resize(&L, cols, rows);

	int mode = MON_NORMAL;
	struct sbuf status = SBUF_INIT;
	struct tui_prompt search_prompt;
	struct tui_info help_info;
	memset(&search_prompt, 0, sizeof(search_prompt));
	memset(&help_info, 0, sizeof(help_info));

	update_status(&status, &L, opt_port, baud, mode);

	/*
	 * Inner-terminal vt100: sized to the visible body region so the
	 * chip's column probes (linenoise's `\x1b[999C\x1b[6n`) and
	 * autowrap math match what gets painted on-screen.  Body is
	 * cols-1 wide (scrollbar) by rows-1 tall (status); no footer.
	 */
	int inner_cols = cols > 1 ? cols - 1 : cols;
	int inner_rows = rows > 1 ? rows - 1 : rows;
	struct vt100 *V = vt100_new(inner_rows, inner_cols);
	tui_log_set_grid(&L, V);
	{
		struct sbuf frame = SBUF_INIT;
		tui_log_render(&frame, &L);
		tui_flush(&frame);
	}

	serial_flush_input(s);

	for (;;) {
		ssize_t n;
		int dirty = 0;

		if (term_resize_pending()) {
			term_size(&cols, &rows);
			tui_log_resize(&L, cols, rows);
			int new_inner_cols = cols > 1 ? cols - 1 : cols;
			int new_inner_rows = rows > 1 ? rows - 1 : rows;
			vt100_resize(V, new_inner_rows, new_inner_cols);
			if (mode == MON_INSPECT_SEARCH)
				tui_prompt_resize(&search_prompt, cols, rows);
			if (mode == MON_NORMAL_HELP || mode == MON_INSPECT_HELP)
				tui_info_resize(&help_info, cols, rows);
			dirty = 1;
		}

		/* Drain serial regardless of mode so the device-side
		 * buffer doesn't overflow while the user is reading help,
		 * typing into the search prompt, or paused in inspect.
		 * Modal modes (help, inspect, search) all freeze the log
		 * via @ref tui_log_freeze before transitioning, so these
		 * bytes accumulate in the ring without changing the
		 * visible frame -- the redraw below paints an identical
		 * picture and the terminal has nothing to flicker. */
		n = serial_read(s, buf, sizeof(buf), 30);
		if (n < 0)
			break;
		if (n > 0) {
			vt100_input(V, buf, (size_t)n);
			struct sbuf *r = vt100_reply(V);
			if (r->len) {
				serial_write(s, r->buf, r->len);
				sbuf_reset(r);
			}
			if (tui_log_is_frozen(&L)) {
				/* Log is frozen (help or inspect modal is up).
				 * Drain scrolled-off rows so V's queue doesn't
				 * grow without bound, but don't push them into
				 * the ring: that would evict pre-ceiling lines
				 * and change the frozen background on every
				 * frame, causing the modal to blink.  No dirty
				 * flag either -- nothing visible changed. */
				vt100_drain_scrolled(V);
			} else {
				tui_log_pull_from_vt100(&L, V);
				dirty = 1;
			}
		}

		switch (mode) {
		case MON_NORMAL: {
			n = term_read(buf, sizeof(buf), 0);
			if (n < 0)
				goto break_loop;
			size_t pass_start = 0;
			size_t in = (size_t)n;
			size_t i;
			int stop_chunk = 0;
			for (i = 0; i < in && !stop_chunk;) {
				if (buf[i] == 0x1d) { /* Ctrl-] */
					if (i > pass_start)
						serial_write(s,
							     buf + pass_start,
							     i - pass_start);
					goto done;
				}
				if (buf[i] == 0x14) { /* Ctrl-T */
					if (i > pass_start)
						serial_write(s,
							     buf + pass_start,
							     i - pass_start);
					i++; /* consume Ctrl-T */
					if (i < in) {
						/* Dispatch immediately --
						 * keeps "Ctrl-T x" snappy
						 * for users who type the
						 * pair quickly. */
						unsigned char k = buf[i];
						i++;
						pass_start = i;
						if (dispatch_normal_prefix(
							k, s, &L, &help_info,
							cols, rows, &mode))
							goto done;
						dirty = 1;
						stop_chunk = 1;
						break;
					}
					/* No follow-on byte yet -- park in
					 * MON_NORMAL_PREFIX and pick up the
					 * next keystroke on the next read. */
					mode = MON_NORMAL_PREFIX;
					pass_start = i;
					dirty = 1;
					stop_chunk = 1;
					break;
				}
				i++;
			}
			if (!stop_chunk && in > pass_start)
				serial_write(s, buf + pass_start,
					     in - pass_start);
			break;
		}

		case MON_NORMAL_PREFIX: {
			n = term_read(buf, sizeof(buf), 0);
			if (n < 0)
				goto break_loop;
			if (n > 0) {
				unsigned char k = buf[0];
				/* Trailing bytes in the same read are
				 * dropped -- one prefix-dispatch per
				 * Ctrl-T so a paste-typed multi-byte
				 * burst doesn't trigger a chain. */
				if (dispatch_normal_prefix(k, s, &L, &help_info,
							   cols, rows, &mode))
					goto done;
				if (mode == MON_NORMAL_PREFIX)
					mode = MON_NORMAL;
				dirty = 1;
			}
			break;
		}

		case MON_NORMAL_HELP: {
			struct term_event ev;
			int got = term_read_event(&ev, 0);
			if (got > 0 && ev.key != TK_NONE) {
				if (ev.key == TK_RESIZE) {
					tui_log_resize(&L, ev.cols, ev.rows);
					tui_info_resize(&help_info, ev.cols,
							ev.rows);
					dirty = 1;
				} else if (ev.key == 0x1d) {
					goto done;
				} else if (tui_info_on_event(&help_info, &ev)) {
					tui_info_release(&help_info);
					/* Pair with the freeze in
					 * dispatch_normal_prefix -- snap
					 * back to live tail so the user
					 * sees whatever streamed in while
					 * help was up. */
					tui_log_unfreeze(&L);
					mode = MON_NORMAL;
					dirty = 1;
				} else {
					dirty = 1;
				}
			}
			break;
		}

		case MON_INSPECT: {
			struct term_event ev;
			int got = term_read_event(&ev, 0);
			if (got > 0 && ev.key != TK_NONE) {
				if (ev.key == TK_RESIZE) {
					tui_log_resize(&L, ev.cols, ev.rows);
					dirty = 1;
				} else {
					if (dispatch_inspect_event(
						&ev, &L, &search_prompt,
						&help_info, cols, rows, &mode))
						goto done;
					dirty = 1;
				}
			}
			break;
		}

		case MON_INSPECT_SEARCH: {
			struct term_event ev;
			int got = term_read_event(&ev, 0);
			if (got > 0 && ev.key != TK_NONE) {
				if (ev.key == TK_RESIZE) {
					tui_log_resize(&L, ev.cols, ev.rows);
					tui_prompt_resize(&search_prompt,
							  ev.cols, ev.rows);
					dirty = 1;
				} else if (ev.key == 0x1d) {
					goto done;
				} else {
					int r = tui_prompt_on_event(
					    &search_prompt, &ev);
					if (r == 1) {
						/* Enter: compile + apply.
						 * On compile failure keep
						 * the prompt up with an
						 * error title so the user
						 * can fix without losing
						 * what they had. */
						int rc2 = tui_log_search_set(
						    &L, search_prompt.buf);
						if (rc2 < 0) {
							search_prompt.title =
							    "invalid regex - "
							    "search:";
						} else {
							mode = MON_INSPECT;
						}
					} else if (r == -1) {
						/* Esc: cancel without
						 * disturbing any
						 * previously-applied
						 * pattern. */
						mode = MON_INSPECT;
					}
					dirty = 1;
				}
			}
			break;
		}

		case MON_INSPECT_HELP: {
			struct term_event ev;
			int got = term_read_event(&ev, 0);
			if (got > 0 && ev.key != TK_NONE) {
				if (ev.key == TK_RESIZE) {
					tui_log_resize(&L, ev.cols, ev.rows);
					tui_info_resize(&help_info, ev.cols,
							ev.rows);
					dirty = 1;
				} else if (ev.key == 0x1d) {
					goto done;
				} else if (tui_info_on_event(&help_info, &ev)) {
					tui_info_release(&help_info);
					mode = MON_INSPECT;
					dirty = 1;
				} else {
					dirty = 1;
				}
			}
			break;
		}
		}

		if (dirty) {
			update_status(&status, &L, opt_port, baud, mode);
			/*
			 * Compose log + any active modal into a single
			 * frame and flush once.  The log is frozen for
			 * every mode that shows a modal (inspect freezes
			 * on entry; normal-mode help freezes via the
			 * Ctrl-T+? dispatch and unfreezes on dismiss),
			 * so consecutive frames are byte-identical and
			 * the terminal has nothing to flicker even when
			 * serial bytes are streaming into the ring
			 * behind the modal.
			 */
			struct sbuf frame = SBUF_INIT;
			tui_log_render(&frame, &L);
			if (mode == MON_INSPECT_SEARCH)
				tui_prompt_render(&frame, &search_prompt);
			else if (mode == MON_NORMAL_HELP ||
				 mode == MON_INSPECT_HELP)
				tui_info_render(&frame, &help_info);
			tui_flush(&frame);
		}
	}
break_loop:;

done:
	if (mode == MON_NORMAL_HELP || mode == MON_INSPECT_HELP)
		tui_info_release(&help_info);
	tui_log_release(&L);
	vt100_free(V);
	sbuf_release(&status);
	term_screen_leave();
	term_raw_leave();
	serial_close(s);
	free(autoport);
	fprintf(stderr, "--- exit ---" EOL);
	return 0;
}
