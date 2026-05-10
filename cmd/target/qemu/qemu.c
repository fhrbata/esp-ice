/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/qemu/qemu.c
 * @brief `ice target qemu` -- plumbing: run a pre-built flash image on
 * the Espressif QEMU fork.
 *
 * Inputs are explicit on the command line -- no project state is read.
 * The porcelain @c{ice qemu} fills argv from the active profile (chip,
 * merged flash image path, efuse path) and delegates to
 * @ref cmd_target_qemu.
 *
 * QEMU binary resolution: @b{--qemu-bin} > the chip's
 * @c qemu-system-xtensa / @c qemu-system-riscv32 on @c PATH.  No
 * auto-install here -- the porcelain handles that and either points at
 * the installed binary via @b{--qemu-bin} or relies on @c PATH already
 * carrying @c{~/.ice/tools/qemu-<arch>/<version>/qemu/bin} (set up by
 * @c setup_tool_env() when running inside an ice project).  When the
 * binary is missing this command fails with a hint pointing at
 * @c{ice tools install --tool qemu-xtensa|qemu-riscv32}.
 *
 * Quit hotkey is @c Ctrl-T+x (matching @c{ice monitor}): closes qemu's
 * stdin, sends @c SIGTERM, and leaves the alt-screen.  @c Ctrl-] is an
 * undocumented panic eject kept as a fallback.  PgUp / PgDn / Home /
 * End scroll the buffer.
 */
#include "ice.h"
#include "platform.h"
#include "sbuf.h"
#include "term.h"
#include "tui.h"
#include "vt100.h"

#include <signal.h>

/* clang-format off */
static const struct cmd_manual target_qemu_manual = {
	.name = "ice target qemu",
	.summary = "run a pre-built flash image on QEMU",

	.description =
	H_PARA("Launches the Espressif QEMU fork (@b{qemu-system-xtensa} for "
	       "ESP32 / ESP32-S3, @b{qemu-system-riscv32} for ESP32-C3) with "
	       "chip-specific machine arguments, the supplied merged flash "
	       "image as the MTD drive, and the supplied efuse blob.  The "
	       "emulated UART is wired through to the terminal.")
	H_PARA("Reads no project configuration.  Plumbing behind "
	       "@b{ice qemu}, which builds the merged flash image from the "
	       "active profile and fills @b{--chip} / @b{--flash-file} / "
	       "@b{--efuse-file} accordingly.")
	H_PARA("Output is rendered through the same vt100 / tui pipeline "
	       "as @b{ice target monitor}, so panic backtraces are decoded, "
	       "log levels are coloured, and PgUp / PgDn scroll history.  "
	       "Press @b{Ctrl-T x} to exit and shut down QEMU.")
	H_PARA("Supported chips: @b{esp32}, @b{esp32c3}, @b{esp32s3}.  "
	       "Other chips do not currently have an upstream QEMU "
	       "implementation in the Espressif fork."),

	.examples =
	H_EXAMPLE("ice target qemu --chip esp32 --flash-file factory.bin "
		  "--efuse-file efuse.bin")
	H_EXAMPLE("ice target qemu --chip esp32c3 --flash-file build/qemu_flash.bin "
		  "--efuse-file build/qemu_efuse.bin --gdb"),

	.extras =
	H_SECTION("KEY BINDINGS")
	H_ITEM("Ctrl-T h",      "Show command help.")
	H_ITEM("Ctrl-T x",      "Exit and shut down QEMU.")
	H_ITEM("Ctrl-T r",      "Reset the target (--debug only; routed through gdb).")
	H_ITEM("Ctrl-T Ctrl-T", "Send a literal Ctrl-T to the chip.")
	H_ITEM("Ctrl-T ?",      "Show pane (live mode) help.")
	H_ITEM("PgUp/PgDn",     "Scroll the buffer one page at a time.")
	H_ITEM("Home/End",      "Jump to the oldest line / resume tailing.")
	H_ITEM("Mouse wheel",   "Scroll the buffer; hold Shift to select for copy.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice qemu",
	       "Porcelain wrapper: builds the merged flash image and "
	       "resolves the qemu binary from the active profile."),
};
/* clang-format on */

static const char *opt_chip;
static const char *opt_flash_file;
static const char *opt_efuse_file;
static const char *opt_qemu_bin;
static const char *opt_elf;
static int opt_scrollback = 10000;
static int opt_no_tui;
static int opt_gdb;
static int opt_debug;
static const char *opt_gdb_bin;
/* Port the QEMU GDB stub listens on when @c --gdb / @c --debug is set.
 * Defaults to 3333 to match the idf.py / OpenOCD convention so existing
 * user muscle memory works; overridden via @c --gdb-port for users who
 * already have something on 3333 (e.g. OpenOCD for real hardware in a
 * neighbouring window). */
static int opt_gdb_port = 3333;

/* clang-format off */
static const struct option cmd_target_qemu_opts[] = {
	OPT_STRING(0, "chip", &opt_chip, "name",
		   "chip to emulate (esp32, esp32c3, esp32s3)", NULL),
	OPT_STRING(0, "flash-file", &opt_flash_file, "path",
		   "merged flash image to load as the MTD drive", NULL),
	OPT_STRING(0, "efuse-file", &opt_efuse_file, "path",
		   "efuse blob; written with the chip default if missing", NULL),
	OPT_STRING(0, "qemu-bin", &opt_qemu_bin, "path",
		   "qemu-system-* binary (default: search $PATH)", NULL),
	OPT_STRING(0, "elf", &opt_elf, "path",
		   "ELF file gdb loads symbols from (--debug only)", NULL),
	OPT_INT(0, "scrollback", &opt_scrollback, "lines",
		"scrollback buffer in lines (default: 10000)", NULL),
	OPT_BOOL(0, "no-tui", &opt_no_tui,
		 "dumb passthrough; no alt-screen, no scroll "
		 "(auto when stdout/stdin isn't a tty)"),
	OPT_BOOL('d', "gdb", &opt_gdb,
		 "wait for gdb on tcp::3333 before booting "
		 "(attach with the chip's gdb in another terminal)"),
	OPT_BOOL('D', "debug", &opt_debug,
		 "dual-pane: spawn the chip's gdb in one pane, UART in the "
		 "other (implies --gdb)"),
	OPT_STRING(0, "gdb-bin", &opt_gdb_bin, "path",
		   "gdb binary (default: chip-specific xtensa-* / riscv32-* "
		   "from PATH)", NULL),
	OPT_INT(0, "gdb-port", &opt_gdb_port, "port",
		"TCP port for the QEMU GDB stub (default: 3333)", NULL),
	OPT_END(),
};
/* clang-format on */

int cmd_target_qemu(int argc, const char **argv);

const struct cmd_desc cmd_target_qemu_desc = {
    .name = "qemu",
    .fn = cmd_target_qemu,
    .opts = cmd_target_qemu_opts,
    .manual = &target_qemu_manual,
};

/* ------------------------------------------------------------------ */
/*  Per-chip QEMU profile                                             */
/* ------------------------------------------------------------------ */

/*
 * Default efuse blobs taken verbatim from
 * @c tools/idf_py_actions/qemu_ext.py in @c esp-idf.  The chip
 * revisions encoded match what idf.py emits by default (esp32 rev 3,
 * esp32c3 / esp32s3 rev 0.3).  The blobs are mostly zeros with a few
 * non-zero bytes; designated initialisers keep the source compact.
 */
static const unsigned char default_efuse_esp32[124] = {
    [13] = 0x80, /* chip revision 3 */
    [21] = 0x10,
};

static const unsigned char default_efuse_esp32c3[1024] = {
    [38] = 0x0c, /* chip revision 0.3 */
};

static const unsigned char default_efuse_esp32s3[1024] = {
    [38] = 0x0c, /* chip revision 0.3 */
};

struct qemu_chip {
	const char *name;	     /* "esp32", "esp32c3", "esp32s3" */
	const char *qemu_prog;	     /* default binary name */
	const char *gdb_prog;	     /* matching gdb for the @c --gdb hint */
	const char *machine;	     /* -M argument */
	const char *memory;	     /* -m argument, NULL to omit */
	const char *boot_mode_strap; /* strap_mode value for forced download
				      * mode (efuse burn, etc.); unused in
				      * v1 -- kept here so we can wire it up
				      * the day a --download flag lands. */
	const unsigned char *default_efuse;
	size_t default_efuse_len;
};

static const struct qemu_chip qemu_chips[] = {
    {
	.name = "esp32",
	.qemu_prog = "qemu-system-xtensa",
	.gdb_prog = "xtensa-esp32-elf-gdb",
	.machine = "esp32",
	.memory = "4M",
	.boot_mode_strap = "driver=esp32.gpio,property=strap_mode,value=0x0f",
	.default_efuse = default_efuse_esp32,
	.default_efuse_len = sizeof default_efuse_esp32,
    },
    {
	.name = "esp32c3",
	.qemu_prog = "qemu-system-riscv32",
	.gdb_prog = "riscv32-esp-elf-gdb",
	.machine = "esp32c3",
	.memory = NULL,
	.boot_mode_strap = "driver=esp32c3.gpio,property=strap_mode,value=0x02",
	.default_efuse = default_efuse_esp32c3,
	.default_efuse_len = sizeof default_efuse_esp32c3,
    },
    {
	.name = "esp32s3",
	.qemu_prog = "qemu-system-xtensa",
	.gdb_prog = "xtensa-esp32s3-elf-gdb",
	.machine = "esp32s3",
	.memory = "32M",
	.boot_mode_strap = "driver=esp32s3.gpio,property=strap_mode,value=0x07",
	.default_efuse = default_efuse_esp32s3,
	.default_efuse_len = sizeof default_efuse_esp32s3,
    },
};

static const struct qemu_chip *find_chip(const char *name)
{
	if (!name)
		return NULL;
	for (size_t i = 0; i < sizeof qemu_chips / sizeof qemu_chips[0]; i++) {
		if (!strcmp(qemu_chips[i].name, name))
			return &qemu_chips[i];
	}
	return NULL;
}

/*
 * Write the chip's default efuse blob to @p path if it doesn't already
 * exist.  Honours an existing file so users can persist custom efuse
 * state across runs (the same way idf.py does).
 */
static int ensure_efuse_file(const char *path, const struct qemu_chip *chip)
{
	if (access(path, F_OK) == 0)
		return 0; /* already there */

	mkdirp_for_file(path);
	FILE *fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "ice target qemu: cannot write '%s': %s\n",
			path, strerror(errno));
		return -1;
	}
	if (fwrite(chip->default_efuse, 1, chip->default_efuse_len, fp) !=
	    chip->default_efuse_len) {
		fprintf(stderr, "ice target qemu: write error on '%s'\n", path);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  QEMU argv assembly                                                */
/* ------------------------------------------------------------------ */

static void push_argv(struct svec *v, const char *s) { svec_push(v, s); }

static void push_argvf(struct svec *v, const char *fmt, ...)
{
	struct sbuf b = SBUF_INIT;
	va_list ap;
	va_start(ap, fmt);
	sbuf_vaddf(&b, fmt, ap);
	va_end(ap);
	svec_push(v, b.buf);
	sbuf_release(&b);
}

/*
 * Assemble the QEMU command line.  Roughly mirrors the order in
 * @c qemu_ext.py: machine + memory, MTD flash drive, efuse drive, the
 * matching @c -global driver bindings, watchdog disable, the strap-
 * mode pin, networking, and finally @c{-serial stdio} for UART
 * passthrough plus @c{-nographic} to keep the SDL window from popping
 * up.
 */
static void build_qemu_argv(struct svec *v, const struct qemu_chip *chip,
			    const char *qemu_bin, const char *flash_path,
			    const char *efuse_path)
{
	push_argv(v, qemu_bin);
	push_argv(v, "-M");
	push_argv(v, chip->machine);
	if (chip->memory) {
		push_argv(v, "-m");
		push_argv(v, chip->memory);
	}

	push_argv(v, "-drive");
	push_argvf(v, "file=%s,if=mtd,format=raw", flash_path);

	push_argv(v, "-drive");
	push_argvf(v, "file=%s,if=none,format=raw,id=efuse", efuse_path);

	push_argv(v, "-global");
	push_argvf(v, "driver=nvram.%s.efuse,property=drive,value=efuse",
		   chip->name);

	push_argv(v, "-global");
	push_argvf(v, "driver=timer.%s.timg,property=wdt_disable,value=true",
		   chip->name);

	/* No strap_mode override for normal boot: qemu's default puts the
	 * chip in @c SPI_FAST_FLASH_BOOT, which is what we want.  The
	 * @c boot_mode_strap field on @ref qemu_chip is the value we'd
	 * pass to FORCE download mode (chip->boot_mode_strap), reserved
	 * for a future @c{--download} flag. */
	(void)chip->boot_mode_strap;

	push_argv(v, "-nic");
	push_argv(v, "user,model=open_eth");

	/* @c -nographic is convenient but secretly implies @c{-serial
	 * mon:stdio}, which collides with the explicit @c{-serial stdio}
	 * below ("cannot use stdio by multiple character devices").
	 * Spell out the pieces individually: no display, no QEMU monitor,
	 * stdio dedicated to the emulated UART. */
	push_argv(v, "-display");
	push_argv(v, "none");
	push_argv(v, "-monitor");
	push_argv(v, "none");
	push_argv(v, "-serial");
	push_argv(v, "stdio");

	if (opt_gdb) {
		/* @c -S halts the virtual CPU at reset until gdb attaches
		 * and continues -- the user gets to set breakpoints before
		 * the first instruction runs. */
		push_argv(v, "-gdb");
		push_argvf(v, "tcp::%d", opt_gdb_port);
		push_argv(v, "-S");
	}
}

/* ------------------------------------------------------------------ */
/*  Dumb fallback (no-tui)                                            */
/* ------------------------------------------------------------------ */

/*
 * Plumbing path used when stdout/stdin is not a tty (CI, piped output)
 * or @b{--no-tui} is set.  Pipes qemu's output to our stdout one read
 * at a time and forwards any incoming stdin bytes to qemu's stdin.
 * No vt100, no scrollback, no key bindings -- by design.
 */
static int run_dumb(struct process *proc)
{
	for (;;) {
		uint8_t buf[4096];
		ssize_t n = pipe_read_timed(proc->out, buf, sizeof buf, 30);
		if (n < 0)
			break; /* EOF */
		if (n > 0) {
			if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
				break;
			fflush(stdout);
		}

		/* Forward stdin without modification.  The buffer is small
		 * (well under PIPE_BUF), so a single write(2) is atomic and
		 * either completes fully or fails -- no retry loop needed. */
		ssize_t k = term_read(buf, sizeof buf, 0);
		if (k > 0) {
			if (write(proc->in, buf, (size_t)k) < 0)
				break;
		}
	}
	return process_finish(proc);
}

/* ------------------------------------------------------------------ */
/*  vt100 + tui_log loop                                              */
/* ------------------------------------------------------------------ */

/*
 * Per-line decorator: tints ESP-IDF level prefixes (E / W / I) so log
 * output is readable at a glance even before the firmware emits its own
 * ANSI.  Matches the colour palette in cmd/target/monitor/monitor.c so
 * users see the same look across "monitor" and "qemu".
 */
static int decorate_idf_level(const char *line, size_t len, void *ctx,
			      struct tui_overlay *out, int max)
{
	(void)ctx;
	if (max < 1 || len < 2)
		return 0;
	const char *sgr = NULL;
	switch (line[0]) {
	case 'E':
		sgr = "31";
		break; /* red */
	case 'W':
		sgr = "33";
		break; /* yellow */
	case 'I':
		sgr = "32";
		break; /* green */
	default:
		return 0;
	}
	if (line[1] != ' ' || (line[2] != '(' && line[2] != '['))
		return 0;
	out[0].start = 0;
	out[0].end = len;
	out[0].sgr_open = sgr;
	out[0].sgr_close = "0";
	return 1;
}

/*
 * Cmd-level help for the single-pane qemu view.  Documents the
 * orchestrator-level shortcuts (the widget's keymap is reachable via
 * @c Ctrl-T+? on the pane).
 */
static const char QEMU_HELP_TITLE[] = "ice target qemu";

static const char QEMU_HELP_TEXT[] =
    "Runs a pre-built flash image on the Espressif QEMU fork and pipes\n"
    "the emulated UART through the pane.  Output is rendered the same\n"
    "way @b{ice target monitor} renders a real chip's UART (panic\n"
    "decode, log-level colouring, scrollback / search / yank).\n"
    "\n"
    "Use Ctrl-T as the prefix for command shortcuts:\n"
    "\n"
    "  Ctrl-T h       Show this help.\n"
    "  Ctrl-T x       Quit (terminates QEMU).\n"
    "  Ctrl-T Ctrl-T  Send a literal Ctrl-T to the chip.\n"
    "\n"
    "Per-pane scrollback, regex search, and clipboard yank are\n"
    "reachable through the pane's own keymap -- press Ctrl-T+? on\n"
    "the pane (or scroll with PgUp / mouse wheel to auto-enter\n"
    "inspect) to see those.\n";

static int run_tui(struct process *proc, const char *chip_name)
{
	int rc = term_raw_enter(TERM_RAW_MOUSE | TERM_RAW_BRACKETED_PASTE);
	if (rc < 0)
		die("cannot set terminal to raw mode: %s", strerror(-rc));
	term_screen_enter();

	int cols = 80, rows = 24;
	term_size(&cols, &rows);

	/*
	 * Layout: row 1 is the cmd's own top status bar (cmd-prefix
	 * shortcuts: @c h=help, @c x=quit).  Rows 2..rows belong to
	 * @ref tui_log, which paints its own bottom status row with
	 * widget-prefix shortcuts.  Mirrors the dual-pane debug view
	 * so the single-pane and dual-pane views feel consistent.
	 */
	struct tui_log L;
	tui_log_init(&L, opt_scrollback, TUI_LOG_STATUS_BOTTOM, 0, NULL);
	if (use_color)
		tui_log_set_decorator(&L, decorate_idf_level, NULL);
	tui_log_set_origin(&L, 1, 2);
	tui_log_resize(&L, cols, rows > 1 ? rows - 1 : rows);

	/* Inner-terminal vt100: body is cols-1 wide (scrollbar) by
	 * rows-2 tall (cmd top bar + widget bottom bar each eat one
	 * row). */
	int inner_cols = cols > 1 ? cols - 1 : cols;
	int inner_rows = rows > 2 ? rows - 2 : 1;
	struct vt100 *V = vt100_new(inner_rows, inner_cols);
	tui_log_set_grid(&L, V);

	struct tui_info help_info;
	memset(&help_info, 0, sizeof(help_info));
	int help_open = 0;
	int help_freeze = 0;

	int in_prefix = 0;
	int quit = 0;

	{
		struct sbuf frame = SBUF_INIT;
		char status_buf[256];
		snprintf(status_buf, sizeof status_buf,
			 "ice target qemu: %s%s  \xe2\x80\xa2  "
			 "Ctrl-T:  h=help  x=quit",
			 chip_name,
			 opt_gdb ? "  \xe2\x80\xa2  [GDB attached, halted]"
				 : "");
		struct tui_rect status_r = {.x = 1, .y = 1, .w = cols, .h = 1};
		tui_status_bar(&frame, &status_r, status_buf, "1;37;44");
		tui_log_render(&frame, &L);
		tui_flush(&frame);
	}

	while (!quit) {
		uint8_t buf[4096];
		int dirty = 0;

		if (term_resize_pending()) {
			term_size(&cols, &rows);
			tui_log_resize(&L, cols, rows > 1 ? rows - 1 : rows);
			int new_inner_cols = cols > 1 ? cols - 1 : cols;
			int new_inner_rows = rows > 2 ? rows - 2 : 1;
			vt100_resize(V, new_inner_rows, new_inner_cols);
			if (help_open)
				tui_info_resize(&help_info, cols, rows);
			dirty = 1;
		}

		ssize_t n = pipe_read_timed(proc->out, buf, sizeof buf, 30);
		if (n < 0) {
			/* qemu exited / pipe closed -- leave the loop and
			 * surface whatever exit code it had. */
			break;
		}
		if (n > 0) {
			vt100_input(V, buf, (size_t)n);
			struct sbuf *r = vt100_reply(V);
			if (r->len) {
				/* DSR replies and the like need to go back
				 * to the chip; reply payloads are tiny
				 * (atomic under PIPE_BUF) so a single
				 * write(2) is enough.  Ignore errors -- the
				 * next iteration will see EOF on the read
				 * side and bail. */
				(void)write(proc->in, r->buf, r->len);
				sbuf_reset(r);
			}
			if (!tui_log_is_frozen(&L))
				tui_log_pull_from_vt100(&L, V);
			dirty = 1;
		}

		struct term_event ev;
		int got = term_read_event(&ev, 0);
		if (got > 0 && ev.key != TK_NONE) {
			if (ev.key == TK_RESIZE) {
				tui_log_resize(&L, ev.cols,
					       ev.rows > 1 ? ev.rows - 1
							   : ev.rows);
				int ic = ev.cols > 1 ? ev.cols - 1 : ev.cols;
				int ir = ev.rows > 2 ? ev.rows - 2 : 1;
				vt100_resize(V, ir, ic);
				cols = ev.cols;
				rows = ev.rows;
				if (help_open)
					tui_info_resize(&help_info, cols, rows);
				dirty = 1;
			} else if (ev.key == 0x1d) {
				/* Ctrl-]: undocumented panic eject, kept
				 * as a fallback the way @b{ice monitor}
				 * does. */
				quit = 1;
			} else if (help_open) {
				/* Cmd help modal owns the input.  Any key the
				 * modal consumes (Esc / Enter / q / nav)
				 * dismisses it and unfreezes the log if we
				 * froze it on the way in. */
				if (tui_info_on_event(&help_info, &ev)) {
					tui_info_release(&help_info);
					help_open = 0;
					if (help_freeze) {
						tui_log_unfreeze(&L);
						help_freeze = 0;
					}
				}
				dirty = 1;
			} else if (in_prefix) {
				/* One key after Ctrl-T.  Cmd reserves x = quit,
				 * h = cmd help, Ctrl-T = literal Ctrl-T to the
				 * chip; any other key is handed to the widget
				 * as Ctrl-T + key so widget shortcuts (i, ?,
				 * y, ...) reach it from live mode. */
				in_prefix = 0;
				if (ev.key == 'x' || ev.key == 'X') {
					quit = 1;
				} else if (ev.key == 'h' || ev.key == 'H') {
					tui_info_init(&help_info,
						      QEMU_HELP_TITLE,
						      QEMU_HELP_TEXT);
					tui_info_resize(&help_info, cols, rows);
					if (!L.inspect_active) {
						tui_log_freeze(&L);
						help_freeze = 1;
					}
					help_open = 1;
				} else if (ev.key == 0x14) {
					uint8_t k = 0x14;
					if (write(proc->in, &k, 1) < 0)
						quit = 1;
				} else {
					struct term_event pfx = {.key = 0x14};
					tui_log_on_event(&L, &pfx);
					tui_log_on_event(&L, &ev);
				}
				dirty = 1;
			} else if (ev.key == 0x14) {
				/* Claim Ctrl-T at the cmd layer BEFORE the
				 * widget sees it.  If we passed it down, the
				 * widget's own prefix_pending would latch on
				 * the user's plain Ctrl-T and the cmd would
				 * never see x / Ctrl-T-Ctrl-T come through.
				 * The widget only ever gets Ctrl-T when the
				 * cmd explicitly forwards a non-reserved
				 * combination above. */
				in_prefix = 1;
				dirty = 1;
			} else if (tui_log_on_event(&L, &ev)) {
				/* Log consumed it (auto-enter, scroll,
				 * search, help, yank, ...).  Host
				 * forwards nothing. */
				dirty = 1;
			} else if (ev.key < 0x100) {
				/* Plain printable / C0 control: forward
				 * to qemu's UART RX. */
				uint8_t k = (uint8_t)ev.key;
				if (write(proc->in, &k, 1) < 0)
					quit = 1;
			} else {
				/* Named navigation key (arrows, F-keys,
				 * ...): re-emit the canonical xterm
				 * sequence so a remote shell sees the
				 * same byte pattern the user typed. */
				const char *seq = term_key_to_xterm_seq(ev.key);
				if (seq &&
				    write(proc->in, seq, strlen(seq)) < 0)
					quit = 1;
			}
		}

		if (dirty) {
			struct sbuf frame = SBUF_INIT;
			char status_buf[256];
			const char *prefix_hint =
			    in_prefix ? "  \xe2\x80\xa2  [Ctrl-T] prefix" : "";
			snprintf(status_buf, sizeof status_buf,
				 "ice target qemu: %s%s%s  \xe2\x80\xa2  "
				 "Ctrl-T:  h=help  x=quit",
				 chip_name,
				 opt_gdb ? "  \xe2\x80\xa2  [GDB attached, "
					   "halted]"
					 : "",
				 prefix_hint);
			struct tui_rect status_r = {
			    .x = 1, .y = 1, .w = cols, .h = 1};
			tui_status_bar(&frame, &status_r, status_buf,
				       "1;37;44");
			tui_log_render(&frame, &L);
			if (help_open)
				tui_info_render(&frame, &help_info);
			tui_flush(&frame);
		}
	}

	if (help_open)
		tui_info_release(&help_info);
	tui_log_release(&L);
	vt100_free(V);
	term_screen_leave();
	term_raw_leave();

	/* Ask qemu to terminate (no-op if it already exited), then reap.
	 * On POSIX this is plain kill(2); on Windows the platform.h shim
	 * maps to TerminateProcess via the pid_t-cast HANDLE that
	 * process_start stored.  If we exited the loop because qemu died
	 * on its own (quit still 0 -- exec failed, internal fault, ...),
	 * surface its exit code so ice fails non-zero.  When the user
	 * pressed Ctrl-T x (quit==1) we asked qemu to terminate, so the
	 * SIGTERM-induced exit is wrapped as a clean 0. */
	kill(proc->pid, SIGTERM);
	int exit_code = process_finish(proc);
	return quit ? 0 : exit_code;
}

/* ------------------------------------------------------------------ */
/*  --debug: dual-pane gdb + UART                                     */
/* ------------------------------------------------------------------ */

/*
 * One pane's worth of bookkeeping: a vt100 grid feeding a tui_log,
 * with the rect that says where on the screen the log lives.  The
 * tui_log carries its own inspect-mode state so each pane can be
 * frozen and explored independently -- see @c L.inspect_active.
 */
struct dpane {
	struct vt100 *V;
	struct tui_log L;
	struct tui_rect rect; /* outer rect including any chrome */
};

/*
 * Lay out the screen for the dual-pane debug view.  Top row is the
 * status bar (carries the focus / inspect / key-hint chrome -- same
 * style as the single-pane header so the two views feel consistent);
 * the body is split evenly between gdb (top half) and UART (bottom
 * half) with a one-row divider in between.  Stores the rects on the
 * @ref dpane structs and resizes their tui_log + vt100 accordingly.
 */
static void debug_layout(int rows, int cols, struct dpane *gdb_p,
			 struct dpane *uart_p, struct tui_rect *status_r,
			 struct tui_rect *divider_r)
{
	struct tui_rect screen = {.x = 1, .y = 1, .w = cols, .h = rows};
	struct tui_rect rest1, body;

	/* status row at the top */
	tui_rect_split_h(&screen, status_r, &rest1, 1);
	/* split the body evenly with a 1-row divider in between */
	int gdb_h = rest1.h / 2;
	tui_rect_split_h(&rest1, &gdb_p->rect, &body, gdb_h);
	tui_rect_split_h(&body, divider_r, &uart_p->rect, 1);

	/* Apply the rects to the log widgets and resize the vt100 grids
	 * so the chip / gdb get a faithful column count.  The grid is
	 * one column narrower than the pane to leave room for the
	 * scrollbar tui_log_render reserves on the right edge, and one
	 * row shorter when the pane carries a status bar (status_pos =
	 * BOTTOM here) so the grid maps 1:1 to the body region -- gdb's
	 * cursor lands on its prompt row, not on the status row beneath
	 * it. */
	tui_log_set_origin(&gdb_p->L, gdb_p->rect.x, gdb_p->rect.y);
	tui_log_resize(&gdb_p->L, gdb_p->rect.w, gdb_p->rect.h);
	int gdb_inner_w = gdb_p->rect.w > 1 ? gdb_p->rect.w - 1 : gdb_p->rect.w;
	int gdb_inner_h = gdb_p->rect.h > 1 ? gdb_p->rect.h - 1 : 1;
	vt100_resize(gdb_p->V, gdb_inner_h, gdb_inner_w);

	tui_log_set_origin(&uart_p->L, uart_p->rect.x, uart_p->rect.y);
	tui_log_resize(&uart_p->L, uart_p->rect.w, uart_p->rect.h);
	int uart_inner_w =
	    uart_p->rect.w > 1 ? uart_p->rect.w - 1 : uart_p->rect.w;
	int uart_inner_h = uart_p->rect.h > 1 ? uart_p->rect.h - 1 : 1;
	vt100_resize(uart_p->V, uart_inner_h, uart_inner_w);

	/* The inspect search prompt / help modal sized along with the
	 * log itself, by tui_log_resize -- so each pane's modal sits
	 * inside its own rect rather than spanning the screen.  No
	 * extra plumbing here.  cols / rows are unused now. */
	(void)cols;
	(void)rows;
}

/*
 * Read available bytes from @p fd, feed them through @p V, drain V's
 * device-bound reply back to @p write_fd, and pull scrolled-off rows
 * into @p L.  Returns 1 if the visible frame might have changed (so
 * the caller should re-render), 0 if nothing happened, -1 on EOF or
 * read error (the child has gone away).
 */
static int pump_byte_source(int fd, int write_fd, struct vt100 *V,
			    struct tui_log *L, unsigned timeout_ms)
{
	uint8_t buf[4096];
	ssize_t n = pipe_read_timed(fd, buf, sizeof buf, timeout_ms);
	if (n < 0)
		return -1;
	if (n == 0)
		return 0;
	vt100_input(V, buf, (size_t)n);
	struct sbuf *r = vt100_reply(V);
	if (r->len) {
		(void)write(write_fd, r->buf, r->len);
		sbuf_reset(r);
	}
	if (!tui_log_is_frozen(L))
		tui_log_pull_from_vt100(L, V);
	return 1;
}

/*
 * Paint one row of @c {U+2500} (light horizontal) characters across
 * the rect at row @p row_y, columns @p row_x..row_x+row_w-1.  Used to
 * draw the divider line between the two panes.  SGR is reset before
 * and after so the line doesn't carry stale styles.
 */
static void draw_hrule(struct sbuf *out, int row_y, int row_x, int row_w)
{
	if (row_w <= 0)
		return;
	sbuf_addf(out, "\x1b[%d;%dH\x1b[2m", row_y, row_x);
	for (int i = 0; i < row_w; i++)
		sbuf_addstr(out, "\xe2\x94\x80"); /* U+2500 */
	sbuf_addstr(out, "\x1b[0m");
}

/*
 * Cmd-level help for the dual-pane debug view.  Documents the
 * shortcuts the cmd reserves and explains what the screen is
 * showing.  Per-pane scrollback / search / yank live on the
 * widget's own help (Ctrl-T+? on the focused pane).
 */
static const char DEBUG_HELP_TITLE[] = "ice target qemu --debug";

static const char DEBUG_HELP_TEXT[] =
    "Runs the firmware on QEMU under a GDB stub and splits the\n"
    "screen into two panes:\n"
    "\n"
    "  Top    gdb attached to the QEMU stub on tcp::<port>.\n"
    "  Bottom The chip's UART output.\n"
    "\n"
    "Use Ctrl-T as the prefix for command shortcuts:\n"
    "\n"
    "  Ctrl-T Tab     Switch focus between gdb and UART.\n"
    "  Ctrl-T x       Quit (terminates QEMU and gdb).\n"
    "  Ctrl-T r       Reset the target (via gdb's monitor\n"
    "                 system_reset; the reset command appears\n"
    "                 in the gdb pane).\n"
    "  Ctrl-T h       Show this help.\n"
    "  Ctrl-T Ctrl-T  Send a literal Ctrl-T to the focused pane.\n"
    "\n"
    "A mouse click on a pane also switches focus.\n"
    "\n"
    "Per-pane scrollback, regex search, and clipboard yank are\n"
    "reachable through each pane's own keymap -- press Ctrl-T+?\n"
    "on the focused pane (or scroll with PgUp / mouse wheel to\n"
    "auto-enter inspect) to see those.\n";

/*
 * Drive the dual-pane view: gdb pane (top), UART pane (bottom),
 * status bar (identity + focus + mode + key hints, mirroring the
 * single-pane header) and a one-row divider.  The user types in
 * whichever pane has focus -- @c Ctrl-T is the prefix:
 *
 *   Ctrl-T Tab     Toggle focus.
 *   Ctrl-T x       Quit.
 *   Ctrl-T r       Reset the target (via gdb's monitor system_reset).
 *   Ctrl-T h       Cmd-level help (orchestrator description).
 *   Ctrl-T Ctrl-T  Send a literal Ctrl-T to the focused pane.
 *
 * @c Ctrl-] is an undocumented panic eject kept as a fallback (matches
 * @c{ice monitor} convention).
 */
static int run_debug(struct process *qemu_proc, const struct qemu_chip *chip,
		     const char *gdb_bin, const char *elf)
{
	int rc = term_raw_enter(TERM_RAW_MOUSE | TERM_RAW_BRACKETED_PASTE);
	if (rc < 0)
		die("cannot set terminal to raw mode: %s", strerror(-rc));
	term_screen_enter();

	int cols = 80, rows = 24;
	term_size(&cols, &rows);

	/* Per-pane scrollback + vt100 grid.  Set origin to (1,1)
	 * temporarily; debug_layout overwrites it once we've sized the
	 * rectangles. */
	struct dpane gdb_p = {0}, uart_p = {0};
	gdb_p.V = vt100_new(24, 80);
	uart_p.V = vt100_new(24, 80);
	/* Per-pane self-rendered status bar at the bottom of each pane,
	 * tagged with the pane's name so the user can tell at a glance
	 * which pane is in inspect mode.  No @c TUI_LOG_SHOW_EXIT --
	 * cmd's own top-level status row already advertises Ctrl-T+x. */
	tui_log_init(&gdb_p.L, opt_scrollback, TUI_LOG_STATUS_BOTTOM, 0, "gdb");
	tui_log_init(&uart_p.L, opt_scrollback, TUI_LOG_STATUS_BOTTOM, 0,
		     "UART");
	if (use_color)
		tui_log_set_decorator(&uart_p.L, decorate_idf_level, NULL);
	tui_log_set_grid(&gdb_p.L, gdb_p.V);
	tui_log_set_grid(&uart_p.L, uart_p.V);

	struct tui_rect status_r, divider_r;
	debug_layout(rows, cols, &gdb_p, &uart_p, &status_r, &divider_r);

	/* Spawn gdb in a pty pre-loaded with target remote :3333 and the
	 * ELF (when one was passed) so symbols / source resolve.  The pty
	 * is sized to the gdb pane so readline wrapping matches what
	 * we'll render. */
	struct sbuf gdb_remote = SBUF_INIT;
	sbuf_addf(&gdb_remote, "target remote :%d", opt_gdb_port);
	/* @c -q skips the boilerplate splash; @c{set pagination off}
	 * stops gdb's pager from blocking on "Press RETURN" mid-stream
	 * (we own the scrolling, not gdb).  @c{set confirm off} keeps
	 * shutdown / quit clean.  ELF is the last positional and may be
	 * NULL -- without it gdb works in symbol-less mode (raw addresses
	 * only). */
	const char *gdb_argv[] = {gdb_bin, "-q",
				  "-ex",   "set pagination off",
				  "-ex",   "set confirm off",
				  "-ex",   gdb_remote.buf,
				  elf,	   NULL};
	if (!elf)
		gdb_argv[8] = NULL;
	struct process gdb_proc = PROCESS_INIT;
	gdb_proc.argv = gdb_argv;
	gdb_proc.use_pty = 1;
	gdb_proc.pty_rows = gdb_p.rect.h;
	gdb_proc.pty_cols = gdb_p.rect.w;

	if (process_start(&gdb_proc) < 0) {
		term_screen_leave();
		term_raw_leave();
		fprintf(stderr,
			"ice target qemu --debug: cannot launch %s\n"
			"  Pass --gdb-bin or add the chip's gdb to PATH.\n",
			gdb_bin);
		sbuf_release(&gdb_remote);
		tui_log_release(&gdb_p.L);
		tui_log_release(&uart_p.L);
		vt100_free(gdb_p.V);
		vt100_free(uart_p.V);
		return 1;
	}

	int focus = 0; /* 0 = gdb pane, 1 = UART pane */
	int in_prefix = 0;
	int quit = 0;

	/* Cmd-level help modal (Ctrl-T+h).  Distinct from the per-pane
	 * widget help -- this one documents the orchestrator: dual-pane
	 * layout + cmd shortcuts.  When up we route events to the modal
	 * and freeze any pane that wasn't already inspecting so the
	 * background stays still under the overlay. */
	struct tui_info help_info;
	memset(&help_info, 0, sizeof(help_info));
	int help_open = 0;
	int gdb_freeze_for_help = 0;
	int uart_freeze_for_help = 0;

	while (!quit) {
		int dirty = 0;

		if (term_resize_pending()) {
			term_size(&cols, &rows);
			debug_layout(rows, cols, &gdb_p, &uart_p, &status_r,
				     &divider_r);
			pty_resize(&gdb_proc, gdb_p.rect.h, gdb_p.rect.w);
			if (help_open)
				tui_info_resize(&help_info, cols, rows);
			dirty = 1;
		}

		/* gdb pty is the most latency-sensitive (user prompts),
		 * give it the timeout slot. */
		int rg = pump_byte_source(gdb_proc.out, gdb_proc.in, gdb_p.V,
					  &gdb_p.L, 30);
		if (rg < 0) {
			/* gdb exited; we can keep showing UART but the user
			 * probably wants out. */
			break;
		}
		if (rg > 0)
			dirty = 1;

		int ru = pump_byte_source(qemu_proc->out, qemu_proc->in,
					  uart_p.V, &uart_p.L, 0);
		if (ru < 0) {
			/* qemu died; same logic. */
			break;
		}
		if (ru > 0)
			dirty = 1;

		struct term_event ev;
		int got = term_read_event(&ev, 0);
		if (got > 0 && ev.key != TK_NONE) {
			if (ev.key == TK_RESIZE) {
				cols = ev.cols;
				rows = ev.rows;
				debug_layout(rows, cols, &gdb_p, &uart_p,
					     &status_r, &divider_r);
				pty_resize(&gdb_proc, gdb_p.rect.h,
					   gdb_p.rect.w);
				dirty = 1;
			} else if (ev.key == 0x1d) { /* Ctrl-] panic */
				quit = 1;
			} else if (help_open) {
				/* Cmd help modal owns the input.  Any key
				 * the modal consumes (Esc / Enter / q / nav)
				 * dismisses it and unfreezes any panes we
				 * froze on the way in. */
				if (tui_info_on_event(&help_info, &ev)) {
					tui_info_release(&help_info);
					help_open = 0;
					if (gdb_freeze_for_help) {
						tui_log_unfreeze(&gdb_p.L);
						gdb_freeze_for_help = 0;
					}
					if (uart_freeze_for_help) {
						tui_log_unfreeze(&uart_p.L);
						uart_freeze_for_help = 0;
					}
				}
				dirty = 1;
			} else if (ev.key == TK_MOUSE_PRESS) {
				/* Click to switch focus.  Inspect state is
				 * left untouched -- if the user is in
				 * inspect on one pane and clicks the
				 * other, focus visually moves but the
				 * keyboard still goes to the inspecting
				 * pane until they Esc out.  Clicks on
				 * chrome (status bar, divider) are
				 * ignored. */
				int row = ev.rows;
				if (row >= gdb_p.rect.y &&
				    row < gdb_p.rect.y + gdb_p.rect.h)
					focus = 0;
				else if (row >= uart_p.rect.y &&
					 row < uart_p.rect.y + uart_p.rect.h)
					focus = 1;
				dirty = 1;
			} else {
				/* Routing: keyboard always goes to the
				 * focused pane.  Inspect is per-pane and
				 * independent -- Tab while the other pane
				 * is in inspect leaves that pane frozen
				 * visually but doesn't drag the keyboard
				 * with it; focus and control stay in
				 * sync.  Mouse wheel takes the pane under
				 * the cursor regardless of focus, so the
				 * user can scroll either pane with the
				 * mouse without first switching. */
				struct dpane *target;
				if (ev.key == TK_WHEEL_UP ||
				    ev.key == TK_WHEEL_DOWN) {
					int row = ev.rows;
					if (row >= gdb_p.rect.y &&
					    row < gdb_p.rect.y + gdb_p.rect.h)
						target = &gdb_p;
					else if (row >= uart_p.rect.y &&
						 row < uart_p.rect.y +
							   uart_p.rect.h)
						target = &uart_p;
					else
						target = focus == 0 ? &gdb_p
								    : &uart_p;
				} else {
					target = focus == 0 ? &gdb_p : &uart_p;
				}

				/* Cmd-level prefix has to be checked BEFORE
				 * we hand the event to the widget, otherwise
				 * the widget's own @c prefix_pending latch
				 * fires on the user's plain @c Ctrl-T and the
				 * cmd never sees its reserved set (Tab / x /
				 * h) come through. */
				if (in_prefix) {
					/* Cmd reserves Tab (focus), x/q
					 * (quit), h (cmd help), and literal
					 * Ctrl-T (forwarded to the focused
					 * pane's fd).  Anything else is
					 * forwarded as Ctrl-T + key to the
					 * focused widget so widget shortcuts
					 * (i, ?, y, ...) work from live mode
					 * without first scrolling into
					 * inspect. */
					in_prefix = 0;
					if (ev.key == TK_TAB) {
						focus = !focus;
					} else if (ev.key == 'x' ||
						   ev.key == 'q') {
						quit = 1;
					} else if (ev.key == 0x14) {
						uint8_t k = 0x14;
						int wfd = focus == 0
							      ? gdb_proc.in
							      : qemu_proc->in;
						(void)write(wfd, &k, 1);
					} else if (ev.key == 'r' ||
						   ev.key == 'R') {
						/* Reset the QEMU target via
						 * gdb's monitor passthrough.
						 * The leading 0x03 (Ctrl-C)
						 * interrupts a running
						 * inferior so the reset
						 * command lands at a (gdb)
						 * prompt; if gdb was already
						 * idle the 0x03 is harmless.
						 * Result lands in the gdb
						 * pane like any other gdb
						 * output. */
						const char cmd[] =
						    "\x03monitor system_reset"
						    "\n";
						(void)write(gdb_proc.in, cmd,
							    sizeof cmd - 1);
					} else if (ev.key == 'h' ||
						   ev.key == 'H') {
						/* Cmd-level help: layout
						 * description + cmd shortcut
						 * reference.  Freeze any pane
						 * that isn't already frozen
						 * for inspect so the modal's
						 * background stays still. */
						tui_info_init(&help_info,
							      DEBUG_HELP_TITLE,
							      DEBUG_HELP_TEXT);
						tui_info_resize(&help_info,
								cols, rows);
						if (!gdb_p.L.inspect_active) {
							tui_log_freeze(
							    &gdb_p.L);
							gdb_freeze_for_help = 1;
						}
						if (!uart_p.L.inspect_active) {
							tui_log_freeze(
							    &uart_p.L);
							uart_freeze_for_help =
							    1;
						}
						help_open = 1;
					} else {
						struct term_event pfx = {
						    .key = 0x14};
						tui_log_on_event(&target->L,
								 &pfx);
						tui_log_on_event(&target->L,
								 &ev);
					}
					dirty = 1;
				} else if (ev.key == 0x14) {
					/* Ctrl-T -> prefix.  We claim it
					 * here so the widget never latches
					 * its own prefix on a plain user
					 * Ctrl-T (it only sees Ctrl-T when
					 * the cmd explicitly forwards a
					 * non-reserved combo). */
					in_prefix = 1;
					dirty = 1;
				} else if (tui_log_on_event(&target->L, &ev)) {
					/* Widget consumed (auto-enter,
					 * scroll, in-inspect command...). */
					dirty = 1;
				} else if (ev.key < 0x100) {
					uint8_t k = (uint8_t)ev.key;
					int wfd = focus == 0 ? gdb_proc.in
							     : qemu_proc->in;
					(void)write(wfd, &k, 1);
				} else {
					/* Named key (arrow, F-key, ...):
					 * forward as its xterm sequence
					 * so gdb / shell history works. */
					const char *seq =
					    term_key_to_xterm_seq(ev.key);
					int wfd = focus == 0 ? gdb_proc.in
							     : qemu_proc->in;
					if (seq)
						(void)write(wfd, seq,
							    strlen(seq));
				}
			}
		}

		if (dirty) {
			struct sbuf frame = SBUF_INIT;
			char status_buf[256];

			/* Cmd top status bar: identity + focus + cmd
			 * shortcuts only.  Per-pane state ([INSPECT] +
			 * search counter) lives on each pane's own
			 * bottom status row -- this top bar is the
			 * orchestrator's, not any one pane's, and
			 * stays clean. */
			const char *focus_name = focus == 0 ? "gdb" : "UART";
			const char *prefix_hint =
			    in_prefix ? "  \xe2\x80\xa2  [Ctrl-T] prefix" : "";
			snprintf(
			    status_buf, sizeof status_buf,
			    "ice target qemu: %s  \xe2\x80\xa2  Focus: [%s]%s"
			    "  \xe2\x80\xa2  "
			    "Ctrl-T:  Tab=switch  h=help  r=reset  x=quit",
			    chip->name, focus_name, prefix_hint);
			tui_status_bar(&frame, &status_r, status_buf,
				       "1;37;44");

			/* Render unfocused pane first so the focused one's
			 * cursor placement wins at the end of the frame. */
			if (focus == 0) {
				tui_log_render(&frame, &uart_p.L);
				tui_log_render(&frame, &gdb_p.L);
			} else {
				tui_log_render(&frame, &gdb_p.L);
				tui_log_render(&frame, &uart_p.L);
			}
			draw_hrule(&frame, divider_r.y, divider_r.x,
				   divider_r.w);

			/* Pane-scoped modals (search prompt, per-pane help)
			 * are painted by tui_log_render itself.  The cmd-
			 * level help modal sits on top of everything when
			 * up; render it last so it overrides any cursor
			 * placement / content underneath. */
			if (help_open)
				tui_info_render(&frame, &help_info);

			/* Place the terminal cursor inside the focused pane
			 * iff that pane is in live mode -- a frozen pane
			 * shouldn't show the live grid's cursor (its own
			 * tui_log_render already handles that), and when
			 * the cmd help modal is up the modal owns the
			 * cursor.  Whether the OTHER pane is in inspect is
			 * irrelevant: focus and keyboard are in sync now,
			 * so the focused pane's live cursor is the right
			 * thing to show. */
			const struct dpane *fp = focus == 0 ? &gdb_p : &uart_p;
			if (!fp->L.inspect_active && !help_open) {
				if (vt100_cursor_visible(fp->V)) {
					int row = fp->rect.y +
						  vt100_cursor_row(fp->V);
					int col = fp->rect.x +
						  vt100_cursor_col(fp->V);
					sbuf_addf(&frame,
						  "\x1b[%d;%dH\x1b[?25h", row,
						  col);
				} else {
					sbuf_addstr(&frame, "\x1b[?25l");
				}
			}

			tui_flush(&frame);
		}
	}

	/* ---- teardown ---- */
	term_screen_leave();
	term_raw_leave();

	/* Detach gdb politely first (it will close the GDB-RSP session
	 * and unpause qemu's CPU on the way out -- but we'll kill qemu
	 * anyway).  Closing the master sends EOF to gdb's stdin; gdb
	 * exits cleanly on EOF. */
	close(gdb_proc.in);
	gdb_proc.in = -1;
	gdb_proc.out = -1;
	process_finish(&gdb_proc);

	kill(qemu_proc->pid, SIGTERM);
	int qemu_exit = process_finish(qemu_proc);

	if (help_open)
		tui_info_release(&help_info);
	tui_log_release(&gdb_p.L);
	tui_log_release(&uart_p.L);
	vt100_free(gdb_p.V);
	vt100_free(uart_p.V);
	sbuf_release(&gdb_remote);
	/* User-quit (Ctrl-] / Ctrl-T q): treat the SIGTERM-induced exit
	 * as a clean 0.  Otherwise qemu died on its own -- propagate. */
	return quit ? 0 : qemu_exit;
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

int cmd_target_qemu(int argc, const char **argv)
{
	/* Reset option globals for repeat invocations (the porcelain
	 * calls us in-process). */
	opt_chip = NULL;
	opt_flash_file = NULL;
	opt_efuse_file = NULL;
	opt_qemu_bin = NULL;
	opt_elf = NULL;
	opt_scrollback = 10000;
	opt_no_tui = 0;
	opt_gdb = 0;
	opt_debug = 0;
	opt_gdb_bin = NULL;
	opt_gdb_port = 3333;

	argc = parse_options(argc, argv, &cmd_target_qemu_desc);
	if (argc > 0)
		die("too many arguments");

	if (!opt_chip || !*opt_chip)
		die("ice target qemu: --chip is required");
	if (!opt_flash_file || !*opt_flash_file)
		die("ice target qemu: --flash-file is required");
	if (!opt_efuse_file || !*opt_efuse_file)
		die("ice target qemu: --efuse-file is required");
	if (access(opt_flash_file, R_OK) != 0)
		die("ice target qemu: cannot read flash image '%s': %s",
		    opt_flash_file, strerror(errno));
	if (opt_scrollback < 1)
		die("--scrollback must be at least 1");

	/* --debug folds in the gdb stub (-gdb tcp::3333 -S) plus the
	 * dual-pane UI; -d / --gdb on its own is the two-terminal flow. */
	if (opt_debug)
		opt_gdb = 1;

	const struct qemu_chip *chip = find_chip(opt_chip);
	if (!chip) {
		fprintf(
		    stderr,
		    "ice target qemu: chip '%s' is not supported by QEMU yet.\n"
		    "  Supported: esp32, esp32c3, esp32s3.\n",
		    opt_chip);
		return 1;
	}

	/* ---- ensure efuse blob exists ---- */
	if (ensure_efuse_file(opt_efuse_file, chip) < 0)
		return 1;

	/* ---- qemu binary ---- */
	/* Resolution: @b{--qemu-bin} > the chip's @c qemu_prog on @c PATH.
	 * We don't walk @c{~/.ice/tools/} ourselves -- when the porcelain
	 * runs us inside an ice project, @c setup_tool_env() has already
	 * prepended the active profile's tool dirs to @c PATH, so the
	 * installed qemu shows up via a normal @c PATH lookup.  When the
	 * user runs us directly with no project (or hasn't installed
	 * qemu via @c{ice tools install}), they have to either supply
	 * @b{--qemu-bin} or have qemu on the system @c PATH. */
	const char *qemu_bin = opt_qemu_bin ? opt_qemu_bin : chip->qemu_prog;

	/* ---- build argv ---- */
	/* svec_push keeps a NULL sentinel after the last entry, so the
	 * vector is already correctly terminated for argv-style consumers
	 * like process_start. */
	struct svec qemu_argv = SVEC_INIT;
	build_qemu_argv(&qemu_argv, chip, qemu_bin, opt_flash_file,
			opt_efuse_file);

	/* ---- spawn qemu ---- */
	struct process proc = PROCESS_INIT;
	proc.argv = (const char **)qemu_argv.v;
	proc.pipe_in = 1;
	proc.pipe_out = 1;
	proc.merge_err = 1;

	if (process_start(&proc) < 0) {
		fprintf(stderr,
			"ice target qemu: cannot launch %s\n"
			"  Install via @b{ice tools install --tool %s},\n"
			"  pass @b{--qemu-bin <path>}, or add the binary to "
			"PATH.\n",
			qemu_bin,
			!strcmp(chip->qemu_prog, "qemu-system-xtensa")
			    ? "qemu-xtensa"
			    : "qemu-riscv32");
		svec_clear(&qemu_argv);
		return 1;
	}

	/* Tell the user how to attach gdb in another terminal.  Printed
	 * before screen_enter so the message stays in the user's terminal
	 * scrollback (alt-screen output is discarded on screen_leave).
	 * The chip is halted at reset until gdb connects and continues.
	 * Skipped under --debug: that mode owns the dual-pane UI and is
	 * already showing the gdb prompt directly. */
	if (opt_gdb && !opt_debug) {
		fprintf(stderr,
			"@y{ice target qemu: chip halted, waiting for gdb on "
			"tcp::%d}\n"
			"  In another terminal:\n"
			"    %s -ex \"target remote :%d\"\n",
			opt_gdb_port, chip->gdb_prog, opt_gdb_port);
	}

	int interactive =
	    !opt_no_tui && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
	int rc;
	if (opt_debug) {
		if (!interactive)
			die("ice target qemu --debug requires an interactive "
			    "terminal");
		const char *gdb_bin =
		    opt_gdb_bin ? opt_gdb_bin : chip->gdb_prog;
		rc = run_debug(&proc, chip, gdb_bin, opt_elf);
	} else if (interactive) {
		rc = run_tui(&proc, chip->name);
	} else {
		rc = run_dumb(&proc);
	}

	svec_clear(&qemu_argv);
	return rc;
}
