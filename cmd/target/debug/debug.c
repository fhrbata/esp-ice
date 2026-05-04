/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/debug/debug.c
 * @brief `ice target debug` -- plumbing OpenOCD + gdb dual-pane.
 *
 * Mirrors the dual-pane shape of @c{ice qemu --debug} for real
 * hardware: gdb in the top pane talking to OpenOCD's gdb stub on
 * @c tcp::3333, the chip's UART in the bottom pane.  Per-pane inspect
 * / scrollback / search / yank live on the @ref tui_log widget; the
 * cmd owns focus, quit, reset, and help.
 *
 * Inputs are explicit on the command line -- no project state read
 * here.  The porcelain @c{ice debug} fills argv from the active
 * profile (ELF path, chip, openocd args, gdb prefix, serial port +
 * baud) and delegates to @ref cmd_target_debug.
 *
 * OpenOCD binary resolution: @b{--openocd-bin} > installed under
 * @c{~/.ice/tools/openocd-esp32/<version>/} > auto-install via
 * @c{ice tools install --tool openocd-esp32}.  The auto-install path
 * needs an IDF tools manifest; without one (no project), the user has
 * to point at a system openocd via @b{--openocd-bin}.
 */
#include "esf_port.h"
#include "ice.h"
#include "platform.h"
#include "sbuf.h"
#include "serial.h"
#include "svec.h"
#include "term.h"
#include "tui.h"
#include "vt100.h"

#include <signal.h>

/* clang-format off */
static const struct cmd_manual target_debug_manual = {
	.name = "ice target debug",
	.summary = "OpenOCD + gdb dual-pane debug session",

	.description =
	H_PARA("Spawns an OpenOCD daemon (JTAG / built-in USB-JTAG -> gdb "
	       "stub on @b{tcp::3333} by default) and connects gdb to it in "
	       "the top pane while streaming the chip's UART through the "
	       "bottom pane.  The firmware is assumed to be already running "
	       "on the chip -- this command attaches; it does not flash.")
	H_PARA("Reads no project configuration.  Plumbing behind "
	       "@b{ice debug}, which fills the OpenOCD board file, ELF "
	       "path, chip, gdb binary and serial port from the active "
	       "profile.")
	H_PARA("Per-pane scrollback / regex search / OSC 52 yank live on "
	       "each pane's keymap (@b{Ctrl-T ?} on the focused pane).  "
	       "The cmd-level prefix (@b{Ctrl-T}) reserves @b{Tab} (focus), "
	       "@b{r} (reset target), @b{h} (help), and @b{x} (quit)."),

	.examples =
	H_EXAMPLE("ice target debug --port /dev/ttyUSB1 --elf build/app.elf "
		  "--chip esp32 --openocd-cmd \"-f board/esp32-wrover-kit-3.3v.cfg\"")
	H_EXAMPLE("ice target debug --openocd-bin /usr/bin/openocd "
		  "--gdb-bin xtensa-esp32-elf-gdb --elf build/app.elf "
		  "--port /dev/ttyUSB1 "
		  "--openocd-cmd \"-f board/esp32-wrover-kit-3.3v.cfg\""),

	.extras =
	H_SECTION("KEY BINDINGS")
	H_ITEM("Ctrl-T Tab",    "Toggle focus between gdb and UART panes.")
	H_ITEM("Ctrl-T r",      "Reset the target (@b{monitor reset halt}).")
	H_ITEM("Ctrl-T h",      "Show command help.")
	H_ITEM("Ctrl-T x",      "Exit and shut down OpenOCD + gdb.")
	H_ITEM("Ctrl-T Ctrl-T", "Send a literal Ctrl-T to the focused pane.")
	H_ITEM("Ctrl-T ?",      "Show pane (live mode) help.")
	H_ITEM("PgUp/PgDn",     "Scroll the focused pane one page.")
	H_ITEM("Mouse click",   "Switch focus to the clicked pane.")
	H_ITEM("Mouse wheel",   "Scroll the pane under the cursor.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice debug",
	       "Porcelain wrapper: resolves OpenOCD args, ELF, gdb, port "
	       "from the active profile."),
};
/* clang-format on */

static const char *opt_port;
static const char *opt_elf;
static const char *opt_chip;
static const char *opt_openocd_bin;
static const char *opt_openocd_cmd;
static const char *opt_gdb_bin;
static int opt_gdb_port = 3333;
static int opt_baud = 115200;
static int opt_no_reset;
static int opt_scrollback = 10000;

/* clang-format off */
static const struct option cmd_target_debug_opts[] = {
	OPT_STRING('p', "port", &opt_port, "dev",
		   "serial port for the chip's UART (auto-detected if omitted)",
		   serial_complete_port),
	OPT_STRING(0, "elf", &opt_elf, "path",
		   "ELF file gdb loads symbols from", NULL),
	OPT_STRING(0, "chip", &opt_chip, "name",
		   "chip name; picks the default gdb binary "
		   "(esp32, esp32c3, ...)", NULL),
	OPT_STRING(0, "openocd-bin", &opt_openocd_bin, "path",
		   "OpenOCD binary (default: installed under ~/.ice/tools/, "
		   "else auto-install)", NULL),
	OPT_STRING(0, "openocd-cmd", &opt_openocd_cmd, "args",
		   "OpenOCD command-line tail "
		   "(e.g. \"-f board/esp32-wrover-kit-3.3v.cfg\")", NULL),
	OPT_STRING(0, "gdb-bin", &opt_gdb_bin, "path",
		   "gdb binary (default: chip-specific xtensa-* / riscv32-*)",
		   NULL),
	OPT_INT(0, "gdb-port", &opt_gdb_port, "port",
		"TCP port for the OpenOCD gdb stub (default: 3333)", NULL),
	OPT_INT('b', "baud", &opt_baud, "rate",
		"UART baud rate (default: 115200)", NULL),
	OPT_BOOL(0, "no-reset", &opt_no_reset,
		 "open the UART without touching DTR/RTS"),
	OPT_INT(0, "scrollback", &opt_scrollback, "lines",
		"per-pane scrollback in lines (default: 10000)", NULL),
	OPT_END(),
};
/* clang-format on */

int cmd_target_debug(int argc, const char **argv);

const struct cmd_desc cmd_target_debug_desc = {
    .name = "debug",
    .fn = cmd_target_debug,
    .opts = cmd_target_debug_opts,
    .manual = &target_debug_manual,
};

/* ------------------------------------------------------------------ */
/*  Per-chip gdb defaults (only consulted when --gdb-bin is omitted)  */
/* ------------------------------------------------------------------ */

struct debug_chip {
	const char *name;     /* IDF target name */
	const char *gdb_pkg;  /* ice tools package for the gdb */
	const char *gdb_prog; /* binary name inside the package */
};

static const struct debug_chip debug_chips[] = {
    {"esp32", "xtensa-esp-elf-gdb", "xtensa-esp32-elf-gdb"},
    {"esp32s2", "xtensa-esp-elf-gdb", "xtensa-esp32s2-elf-gdb"},
    {"esp32s3", "xtensa-esp-elf-gdb", "xtensa-esp32s3-elf-gdb"},
    {"esp32c2", "riscv32-esp-elf-gdb", "riscv32-esp-elf-gdb"},
    {"esp32c3", "riscv32-esp-elf-gdb", "riscv32-esp-elf-gdb"},
    {"esp32c5", "riscv32-esp-elf-gdb", "riscv32-esp-elf-gdb"},
    {"esp32c6", "riscv32-esp-elf-gdb", "riscv32-esp-elf-gdb"},
    {"esp32c61", "riscv32-esp-elf-gdb", "riscv32-esp-elf-gdb"},
    {"esp32h2", "riscv32-esp-elf-gdb", "riscv32-esp-elf-gdb"},
    {"esp32h21", "riscv32-esp-elf-gdb", "riscv32-esp-elf-gdb"},
    {"esp32h4", "riscv32-esp-elf-gdb", "riscv32-esp-elf-gdb"},
    {"esp32p4", "riscv32-esp-elf-gdb", "riscv32-esp-elf-gdb"},
};

static const struct debug_chip *find_debug_chip(const char *name)
{
	if (!name)
		return NULL;
	for (size_t i = 0; i < sizeof debug_chips / sizeof debug_chips[0];
	     i++) {
		if (!strcmp(debug_chips[i].name, name))
			return &debug_chips[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/*  Tool resolution: ~/.ice/tools/<package>/<version>/                */
/*  Mirrors cmd/qemu/qemu.c:resolve_tool literally.                   */
/* ------------------------------------------------------------------ */

struct pick_version_ctx {
	struct sbuf *out;
};

static int pick_version_cb(const char *name, void *ud)
{
	struct pick_version_ctx *ctx = ud;
	if (name[0] == '.')
		return 0;
	if (ctx->out->len == 0 || strcmp(name, ctx->out->buf) > 0) {
		sbuf_reset(ctx->out);
		sbuf_addstr(ctx->out, name);
	}
	return 0;
}

/*
 * Resolve a tool binary by walking @c{ice_home()/tools/<package>/} for
 * the latest installed version and joining the export sub-path.  Returns
 * a malloc'd absolute path on success (caller frees), NULL if no
 * matching install is found.  Verifies the binary is executable.
 *
 * If @p version_out is non-NULL, it receives the chosen version name
 * (caller frees) -- handy for building OPENOCD_SCRIPTS at the same
 * version.
 */
static char *resolve_tool(const char *package, const char *subpath,
			  char **version_out)
{
	struct sbuf tool_dir = SBUF_INIT;
	struct sbuf version = SBUF_INIT;
	struct sbuf full = SBUF_INIT;
	char *result = NULL;
	struct pick_version_ctx ctx = {.out = &version};

	sbuf_addf(&tool_dir, "%s/tools/%s", ice_home(), package);
	if (!is_directory(tool_dir.buf))
		goto done;

	dir_foreach(tool_dir.buf, pick_version_cb, &ctx);
	if (version.len == 0)
		goto done;

	sbuf_addf(&full, "%s/%s/%s", tool_dir.buf, version.buf, subpath);
	if (access(full.buf, X_OK) != 0)
		goto done;

	result = sbuf_strdup(full.buf);
	if (version_out) {
		*version_out = sbuf_strdup(version.buf);
	}

done:
	sbuf_release(&full);
	sbuf_release(&version);
	sbuf_release(&tool_dir);
	return result;
}

/*
 * Resolve OpenOCD: @b{--openocd-bin} > installed under
 * @c{~/.ice/tools/openocd-esp32/<version>/} > auto-install.
 *
 * On success returns the absolute path (malloc'd; caller frees).  The
 * version directory (used to compute @c OPENOCD_SCRIPTS) is also
 * returned via @p version_out when non-NULL and we resolved through
 * @c ~/.ice/tools/.  When the user supplies @b{--openocd-bin} we leave
 * @c *version_out as @c NULL (that openocd is presumed to know where
 * its own scripts live).
 */
static char *resolve_openocd(char **version_out)
{
	if (opt_openocd_bin)
		return sbuf_strdup(opt_openocd_bin);

	const char *pkg = "openocd-esp32";
	const char *sub = "openocd-esp32/bin/openocd";

	char *bin = resolve_tool(pkg, sub, version_out);
	if (bin)
		return bin;

	/* Auto-install path: needs the project's IDF manifest.  Without
	 * one (no project, or running purely from --openocd-bin) the
	 * caller already short-circuited above. */
	const char *idf_path = config_get("_project.idf-path");
	if (!idf_path || !*idf_path)
		die("ice target debug: openocd not installed and no "
		    "@b{_project.idf-path} for auto-install; pass "
		    "@b{--openocd-bin} or run @b{ice init} first");

	struct sbuf manifest = SBUF_INIT;
	struct svec icmd = SVEC_INIT;
	struct process iproc = PROCESS_INIT;
	const char *exe = process_exe();
	int irc;

	sbuf_addf(&manifest, "%s/tools/tools.json", idf_path);
	if (access(manifest.buf, F_OK) != 0)
		die("ice target debug: cannot install openocd, '%s' not found",
		    manifest.buf);

	svec_push(&icmd, exe ? exe : "ice");
	svec_push(&icmd, "tools");
	svec_push(&icmd, "install");
	svec_push(&icmd, "--tool");
	svec_push(&icmd, pkg);
	svec_push(&icmd, manifest.buf);
	iproc.argv = icmd.v;

	irc = process_run_progress(&iproc, "Installing openocd-esp32",
				   "openocd-install", NULL);
	svec_clear(&icmd);
	sbuf_release(&manifest);
	if (irc != 0)
		die("ice target debug: openocd install failed (exit %d)", irc);

	bin = resolve_tool(pkg, sub, version_out);
	if (!bin)
		die("ice target debug: '%s' install completed but openocd not "
		    "found under @b{~/.ice/tools/%s/<version>/}",
		    pkg, pkg);
	return bin;
}

/* ------------------------------------------------------------------ */
/*  Resolve gdb binary                                                */
/* ------------------------------------------------------------------ */

/*
 * @b{--gdb-bin} > installed under @c{~/.ice/tools/<pkg>/<version>/} >
 * bare program name on @c PATH.  Same shape as @c run_debug in
 * @c cmd/qemu/qemu.c so the two paths feel consistent.
 */
static char *resolve_gdb(const struct debug_chip *dc)
{
	if (opt_gdb_bin)
		return sbuf_strdup(opt_gdb_bin);
	if (!dc)
		die("ice target debug: --chip is required to pick a gdb "
		    "binary (or pass --gdb-bin)");

	struct sbuf sub = SBUF_INIT;
	sbuf_addf(&sub, "%s/bin/%s", dc->gdb_pkg, dc->gdb_prog);
	char *path = resolve_tool(dc->gdb_pkg, sub.buf, NULL);
	sbuf_release(&sub);
	return path ? path : sbuf_strdup(dc->gdb_prog);
}

/* ------------------------------------------------------------------ */
/*  OpenOCD launcher                                                  */
/* ------------------------------------------------------------------ */

/*
 * Spawn OpenOCD on a pty and tail-poll it for the @c{Listening on
 * port \d+ for gdb connections} line that means the gdb stub is up.
 * Mirrors esp-idf's own cadence (5 x 500ms) -- if OpenOCD exits or
 * stays silent past the budget we tear down and report.
 *
 * Pty (rather than pipe) for two reasons: the kernel's line
 * discipline applies @c ONLCR so OpenOCD's bare-LF log lines render
 * correctly when fed straight into the gdb pane's vt100 (a pipe
 * would scroll-without-CR and stair-step the output), and libc
 * line-buffers stdout on a tty so log lines flush per @c \n instead
 * of waiting for a 4KB block to fill.
 *
 * Splits @p openocd_cmd in-place on whitespace via @ref sbuf_split; the
 * caller must hold the storage live until @ref process_finish runs.
 *
 * Returns 0 on success (OpenOCD is up and listening); -1 on failure.
 * On success the openocd output captured during the wait is left at the
 * head of the pty so the orchestrator's gdb pane carries the same
 * "Open On-Chip Debugger ..." banner the user would see in a regular
 * openocd terminal.
 */
static int spawn_openocd(struct process *proc, const char *openocd_bin,
			 const char *openocd_version, char *openocd_cmd_buf)
{
	struct svec argv = SVEC_INIT;
	const char *env_kv[2] = {NULL, NULL};
	struct sbuf scripts_env = SBUF_INIT;

	svec_push(&argv, openocd_bin);

	/* OpenOCD command tail -- whitespace-split.  Cap at a sensible
	 * upper bound; sbuf_split returns at most this many tokens, with
	 * the last token absorbing any remainder. */
	if (openocd_cmd_buf && *openocd_cmd_buf) {
		char *toks[32];
		int n = sbuf_split(openocd_cmd_buf, toks, 32);
		for (int i = 0; i < n; i++)
			svec_push(&argv, toks[i]);
	}

	proc->argv = argv.v;
	proc->use_pty = 1;

	/* Point OpenOCD at its own scripts dir if we resolved through
	 * ~/.ice/tools/.  The Espressif build's compiled-in default is
	 * relative to the binary so this is belt-and-suspenders -- but
	 * cheap and removes any chance of the daemon picking up a
	 * neighbouring system install's scripts. */
	if (openocd_version) {
		sbuf_addf(&scripts_env, "OPENOCD_SCRIPTS=%s/tools/%s/%s/%s",
			  ice_home(), "openocd-esp32", openocd_version,
			  "openocd-esp32/share/openocd/scripts");
		env_kv[0] = scripts_env.buf;
		proc->env = env_kv;
	}

	if (process_start(proc) < 0) {
		fprintf(stderr,
			"ice target debug: cannot launch %s\n"
			"  Pass --openocd-bin or run "
			"@b{ice tools install --tool openocd-esp32}.\n",
			openocd_bin);
		svec_clear(&argv);
		sbuf_release(&scripts_env);
		return -1;
	}

	/* Tail-poll for the listen banner.  500ms timeouts x 20 covers
	 * ~10s -- enough for OpenOCD to probe the JTAG adapter and bring
	 * the gdb stub up.  Stream what we read to stderr in real time so
	 * the user sees the banner / probe progress before the alt screen
	 * takes over (and so failure diagnostics are visible without
	 * digging in scrollback). */
	struct sbuf prelog = SBUF_INIT;
	int ready = 0;

	for (int attempt = 0; attempt < 20; attempt++) {
		uint8_t buf[2048];
		ssize_t n = pipe_read_timed(proc->out, buf, sizeof buf, 500);
		if (n < 0)
			break; /* EOF -- daemon died */
		if (n > 0) {
			fwrite(buf, 1, (size_t)n, stderr);
			fflush(stderr);
			sbuf_add(&prelog, buf, (size_t)n);
			/* sbuf is NUL-terminated and OpenOCD's output is
			 * text, so strstr is safe and portable (memmem is
			 * a GNU extension we don't depend on elsewhere). */
			if (strstr(prelog.buf, "Listening on port ")) {
				ready = 1;
				break;
			}
		}
	}

	if (!ready) {
		fprintf(stderr,
			"ice target debug: openocd did not announce a gdb "
			"port within ~10s\n");
		kill(proc->pid, SIGTERM);
		process_finish(proc);
		svec_clear(&argv);
		sbuf_release(&prelog);
		sbuf_release(&scripts_env);
		return -1;
	}

	sbuf_release(&prelog);
	/* argv strings are owned by svec; the process struct stores
	 * proc->argv as a borrow.  Clearing svec here is safe: the child
	 * has already exec'd, so its argv copy is independent. */
	svec_clear(&argv);
	sbuf_release(&scripts_env);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Per-pane bookkeeping (mirrors cmd/qemu/qemu.c)                    */
/* ------------------------------------------------------------------ */

struct dpane {
	struct vt100 *V;
	struct tui_log L;
	struct tui_rect rect;
};

static void debug_layout(int rows, int cols, struct dpane *gdb_p,
			 struct dpane *uart_p, struct tui_rect *status_r,
			 struct tui_rect *divider_r)
{
	struct tui_rect screen = {.x = 1, .y = 1, .w = cols, .h = rows};
	struct tui_rect rest1, body;

	tui_rect_split_h(&screen, status_r, &rest1, 1);
	int gdb_h = rest1.h / 2;
	tui_rect_split_h(&rest1, &gdb_p->rect, &body, gdb_h);
	tui_rect_split_h(&body, divider_r, &uart_p->rect, 1);

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

	(void)cols;
	(void)rows;
}

/*
 * Decorate ESP-IDF level prefixes (E / W / I) -- same colour palette as
 * @c cmd/qemu/qemu.c and @c cmd/target/monitor/monitor.c so log output
 * looks the same regardless of which command is showing it.
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
		break;
	case 'W':
		sgr = "33";
		break;
	case 'I':
		sgr = "32";
		break;
	default:
		return 0;
	}
	if (line[1] != ' ' || (len > 2 && line[2] != '(' && line[2] != '['))
		return 0;
	out[0].start = 0;
	out[0].end = len;
	out[0].sgr_open = sgr;
	out[0].sgr_close = "0";
	return 1;
}

/*
 * Read available bytes from @p fd, feed them through @p V, drain V's
 * device-bound reply back to @p write_fd, and pull scrolled-off rows
 * into @p L.  Returns 1 on visible change, 0 on idle, -1 on EOF / read
 * error.  Same shape as @c pump_byte_source in @c cmd/qemu/qemu.c.
 */
static int pump_pipe(int fd, int write_fd, struct vt100 *V, struct tui_log *L,
		     unsigned timeout_ms)
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
 * Same as @ref pump_pipe but reads from a @ref serial port, replies via
 * @ref serial_write, and treats @c serial_read 0 (timeout) as idle.
 * Returns 1 on visible change, 0 on idle, -1 on serial error.
 */
static int pump_serial(struct serial *s, struct vt100 *V, struct tui_log *L,
		       unsigned timeout_ms)
{
	uint8_t buf[4096];
	ssize_t n = serial_read(s, buf, sizeof buf, timeout_ms);
	if (n < 0)
		return -1;
	if (n == 0)
		return 0;
	vt100_input(V, buf, (size_t)n);
	struct sbuf *r = vt100_reply(V);
	if (r->len) {
		serial_write(s, r->buf, r->len);
		sbuf_reset(r);
	}
	if (!tui_log_is_frozen(L))
		tui_log_pull_from_vt100(L, V);
	return 1;
}

static void draw_hrule(struct sbuf *out, int row_y, int row_x, int row_w)
{
	if (row_w <= 0)
		return;
	sbuf_addf(out, "\x1b[%d;%dH\x1b[2m", row_y, row_x);
	for (int i = 0; i < row_w; i++)
		sbuf_addstr(out, "\xe2\x94\x80");
	sbuf_addstr(out, "\x1b[0m");
}

/* ------------------------------------------------------------------ */
/*  Cmd-level help modal                                              */
/* ------------------------------------------------------------------ */

static const char DEBUG_HELP_TITLE[] = "ice target debug";

static const char DEBUG_HELP_TEXT[] =
    "Spawns an OpenOCD daemon (JTAG -> gdb stub on tcp::<port>) and\n"
    "splits the screen into two panes:\n"
    "\n"
    "  Top    gdb attached to the OpenOCD stub.\n"
    "  Bottom The chip's UART output.\n"
    "\n"
    "Use Ctrl-T as the prefix for command shortcuts:\n"
    "\n"
    "  Ctrl-T Tab     Switch focus between gdb and UART.\n"
    "  Ctrl-T x       Quit (terminates OpenOCD and gdb).\n"
    "  Ctrl-T r       Reset the target (via gdb's monitor reset\n"
    "                 halt; the reset command appears in the gdb\n"
    "                 pane and leaves the CPU halted -- continue\n"
    "                 with @b{c} to resume).\n"
    "  Ctrl-T h       Show this help.\n"
    "  Ctrl-T Ctrl-T  Send a literal Ctrl-T to the focused pane.\n"
    "\n"
    "A mouse click on a pane also switches focus.\n"
    "\n"
    "Per-pane scrollback, regex search, and clipboard yank are\n"
    "reachable through each pane's own keymap -- press Ctrl-T+?\n"
    "on the focused pane (or scroll with PgUp / mouse wheel to\n"
    "auto-enter inspect) to see those.\n";

/* ------------------------------------------------------------------ */
/*  Dual-pane orchestrator                                            */
/* ------------------------------------------------------------------ */

static int run_debug(struct process *oocd_proc, const char *gdb_bin,
		     const char *elf, struct serial *uart_s,
		     const char *port_label, unsigned baud,
		     const char *chip_label)
{
	int rc = term_raw_enter(TERM_RAW_MOUSE | TERM_RAW_BRACKETED_PASTE);
	if (rc < 0)
		die("cannot set terminal to raw mode: %s", strerror(-rc));
	term_screen_enter();

	int cols = 80, rows = 24;
	term_size(&cols, &rows);

	struct dpane gdb_p = {0}, uart_p = {0};
	gdb_p.V = vt100_new(24, 80);
	uart_p.V = vt100_new(24, 80);
	tui_log_init(&gdb_p.L, opt_scrollback, TUI_LOG_STATUS_BOTTOM, 0, "gdb");
	tui_log_init(&uart_p.L, opt_scrollback, TUI_LOG_STATUS_BOTTOM, 0,
		     "UART");
	if (use_color)
		tui_log_set_decorator(&uart_p.L, decorate_idf_level, NULL);
	tui_log_set_grid(&gdb_p.L, gdb_p.V);
	tui_log_set_grid(&uart_p.L, uart_p.V);

	struct tui_rect status_r, divider_r;
	debug_layout(rows, cols, &gdb_p, &uart_p, &status_r, &divider_r);

	/* gdb in a pty pre-loaded with target remote :<port> + ELF. */
	struct sbuf gdb_remote = SBUF_INIT;
	sbuf_addf(&gdb_remote, "target remote :%d", opt_gdb_port);
	const char *gdb_argv[] = {gdb_bin, "-q",
				  "-ex",   "set pagination off",
				  "-ex",   "set confirm off",
				  "-ex",   gdb_remote.buf,
				  elf,	   NULL};
	struct process gdb_proc = PROCESS_INIT;
	gdb_proc.argv = gdb_argv;
	gdb_proc.use_pty = 1;
	gdb_proc.pty_rows = gdb_p.rect.h;
	gdb_proc.pty_cols = gdb_p.rect.w;

	if (process_start(&gdb_proc) < 0) {
		term_screen_leave();
		term_raw_leave();
		fprintf(stderr,
			"ice target debug: cannot launch %s\n"
			"  Pass --gdb-bin or add the chip's gdb to PATH.\n",
			gdb_bin);
		sbuf_release(&gdb_remote);
		tui_log_release(&gdb_p.L);
		tui_log_release(&uart_p.L);
		vt100_free(gdb_p.V);
		vt100_free(uart_p.V);
		return 1;
	}

	int focus = 0;
	int in_prefix = 0;
	int quit = 0;

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

		/* gdb pty is the latency-sensitive one (user prompts);
		 * give it the timeout slot.  OpenOCD's stdout (banner +
		 * "Info" lines) interleaves into the gdb pane via the
		 * second pump call below -- we don't open a third pane
		 * for it, the user reads it as gdb context. */
		int rg =
		    pump_pipe(gdb_proc.out, gdb_proc.in, gdb_p.V, &gdb_p.L, 30);
		if (rg < 0)
			break;
		if (rg > 0)
			dirty = 1;

		int ro = pump_pipe(oocd_proc->out, -1, gdb_p.V, &gdb_p.L, 0);
		if (ro > 0)
			dirty = 1;
		/* OpenOCD pipe EOF is non-fatal here: if the daemon dies
		 * gdb will surface the broken connection on the next
		 * remote command.  Quit only on gdb EOF (above) or user
		 * action. */

		int ru = pump_serial(uart_s, uart_p.V, &uart_p.L, 0);
		if (ru < 0)
			break;
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
				if (help_open)
					tui_info_resize(&help_info, cols, rows);
				dirty = 1;
			} else if (ev.key == 0x1d) { /* Ctrl-] panic */
				quit = 1;
			} else if (help_open) {
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
				int row = ev.rows;
				if (row >= gdb_p.rect.y &&
				    row < gdb_p.rect.y + gdb_p.rect.h)
					focus = 0;
				else if (row >= uart_p.rect.y &&
					 row < uart_p.rect.y + uart_p.rect.h)
					focus = 1;
				dirty = 1;
			} else {
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

				if (in_prefix) {
					in_prefix = 0;
					if (ev.key == TK_TAB) {
						focus = !focus;
					} else if (ev.key == 'x' ||
						   ev.key == 'q') {
						quit = 1;
					} else if (ev.key == 0x14) {
						/* Literal Ctrl-T: gdb pane gets
						 * it via pty stdin, UART pane
						 * gets it via the serial port.
						 */
						uint8_t k = 0x14;
						if (focus == 0)
							(void)write(gdb_proc.in,
								    &k, 1);
						else
							serial_write(uart_s, &k,
								     1);
					} else if (ev.key == 'r' ||
						   ev.key == 'R') {
						/* Reset via gdb's monitor
						 * passthrough.  Leading 0x03
						 * (Ctrl-C) interrupts a running
						 * inferior so the reset command
						 * lands at a (gdb) prompt; if
						 * gdb was idle the 0x03 is
						 * harmless.  OpenOCD's command
						 * is "monitor reset halt"; the
						 * CPU stays halted at reset
						 * until the user types
						 * @b{continue}. */
						const char cmd[] =
						    "\x03monitor reset halt\n";
						(void)write(gdb_proc.in, cmd,
							    sizeof cmd - 1);
					} else if (ev.key == 'h' ||
						   ev.key == 'H') {
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
					in_prefix = 1;
					dirty = 1;
				} else if (tui_log_on_event(&target->L, &ev)) {
					dirty = 1;
				} else if (ev.key < 0x100) {
					uint8_t k = (uint8_t)ev.key;
					if (focus == 0)
						(void)write(gdb_proc.in, &k, 1);
					else
						serial_write(uart_s, &k, 1);
				} else {
					const char *seq =
					    term_key_to_xterm_seq(ev.key);
					if (seq) {
						size_t slen = strlen(seq);
						if (focus == 0)
							(void)write(gdb_proc.in,
								    seq, slen);
						else
							serial_write(uart_s,
								     seq, slen);
					}
				}
			}
		}

		if (dirty) {
			struct sbuf frame = SBUF_INIT;
			char status_buf[256];
			const char *focus_name = focus == 0 ? "gdb" : "UART";
			const char *prefix_hint =
			    in_prefix ? "  \xe2\x80\xa2  [Ctrl-T] prefix" : "";
			snprintf(status_buf, sizeof status_buf,
				 "ice debug: %s @ %s %u baud  \xe2\x80\xa2  "
				 "Focus: [%s]%s  \xe2\x80\xa2  "
				 "Ctrl-T:  Tab=switch  h=help  r=reset  x=quit",
				 chip_label, port_label, baud, focus_name,
				 prefix_hint);
			tui_status_bar(&frame, &status_r, status_buf,
				       "1;37;44");

			if (focus == 0) {
				tui_log_render(&frame, &uart_p.L);
				tui_log_render(&frame, &gdb_p.L);
			} else {
				tui_log_render(&frame, &gdb_p.L);
				tui_log_render(&frame, &uart_p.L);
			}
			draw_hrule(&frame, divider_r.y, divider_r.x,
				   divider_r.w);

			if (help_open)
				tui_info_render(&frame, &help_info);

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

	term_screen_leave();
	term_raw_leave();

	close(gdb_proc.in);
	gdb_proc.in = -1;
	gdb_proc.out = -1;
	process_finish(&gdb_proc);

	kill(oocd_proc->pid, SIGTERM);
	process_finish(oocd_proc);

	if (help_open)
		tui_info_release(&help_info);
	tui_log_release(&gdb_p.L);
	tui_log_release(&uart_p.L);
	vt100_free(gdb_p.V);
	vt100_free(uart_p.V);
	sbuf_release(&gdb_remote);
	return quit ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

int cmd_target_debug(int argc, const char **argv)
{
	opt_port = NULL;
	opt_elf = NULL;
	opt_chip = NULL;
	opt_openocd_bin = NULL;
	opt_openocd_cmd = NULL;
	opt_gdb_bin = NULL;
	opt_gdb_port = 3333;
	opt_baud = 115200;
	opt_no_reset = 0;
	opt_scrollback = 10000;

	argc = parse_options(argc, argv, &cmd_target_debug_desc);
	if (argc > 0)
		die("unexpected argument '%s'", argv[0]);

	if (!opt_elf || !*opt_elf)
		die("ice target debug: --elf is required");
	if (access(opt_elf, R_OK) != 0)
		die("ice target debug: cannot read elf '%s': %s", opt_elf,
		    strerror(errno));
	if (!opt_openocd_cmd || !*opt_openocd_cmd)
		die("ice target debug: --openocd-cmd is required (e.g. "
		    "\"-f board/esp32-wrover-kit-3.3v.cfg\")");
	if (opt_scrollback < 1)
		die("--scrollback must be at least 1");
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		die("ice target debug requires an interactive terminal");

	const struct debug_chip *dc = find_debug_chip(opt_chip);

	/* ---- resolve openocd ---- */
	char *openocd_version = NULL;
	char *openocd_bin = resolve_openocd(&openocd_version);

	/* ---- resolve gdb ---- */
	char *gdb_bin = resolve_gdb(dc);

	/* ---- resolve port ---- */
	char *autoport = NULL;
	if (!opt_port) {
		enum ice_chip scan_chip = ice_chip_from_idf_name(opt_chip);
		autoport = esf_find_esp_port(scan_chip);
		if (!autoport)
			die("ice target debug: no ESP device found; use "
			    "--port to specify a port explicitly");
		opt_port = autoport;
	}

	/* ---- open serial ---- */
	struct serial *s;
	int rc = serial_open(&s, opt_port);
	if (rc)
		die("cannot open %s: %s", opt_port, strerror(-rc));

	if (!opt_no_reset) {
		serial_set_dtr(s, 0);
		serial_set_rts(s, 0);
	}

	rc = serial_set_baud(s, (unsigned)opt_baud);
	if (rc) {
		serial_close(s);
		die("cannot set baud rate %u on %s: %s", (unsigned)opt_baud,
		    opt_port, strerror(-rc));
	}

	/* ---- spawn openocd (banner / probe shows in the user's terminal,
	 * before the alt screen takes over) ---- */
	/* sbuf_split mutates the buffer in place; pass an owned copy so
	 * we don't trip the const-qualifier on opt_openocd_cmd. */
	char *oocd_cmd_owned = sbuf_strdup(opt_openocd_cmd);
	struct process oocd_proc = PROCESS_INIT;

	fprintf(stderr, "Starting openocd ...\n");
	if (spawn_openocd(&oocd_proc, openocd_bin, openocd_version,
			  oocd_cmd_owned) < 0) {
		free(oocd_cmd_owned);
		serial_close(s);
		free(autoport);
		free(gdb_bin);
		free(openocd_bin);
		free(openocd_version);
		return 1;
	}

	/* ---- run dual-pane ---- */
	const char *chip_label = opt_chip ? opt_chip : "?";
	int run_rc = run_debug(&oocd_proc, gdb_bin, opt_elf, s, opt_port,
			       (unsigned)opt_baud, chip_label);

	free(oocd_cmd_owned);
	serial_close(s);
	free(autoport);
	free(gdb_bin);
	free(openocd_bin);
	free(openocd_version);
	return run_rc;
}
