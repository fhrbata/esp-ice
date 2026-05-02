/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/qemu/qemu.c
 * @brief `ice qemu` -- run the project's firmware on the Espressif QEMU
 *        fork.
 *
 * Reads the active profile to discover the target chip and the list of
 * flash files, builds a single padded flash image and a default efuse
 * blob in the build directory, resolves the @c qemu-system-xtensa /
 * @c qemu-system-riscv32 binary on @c PATH, spawns it with the chip-
 * specific machine arguments, and pumps the emulated UART through the
 * @ref vt100 + @ref tui_log pipeline so panic decode and ANSI rendering
 * work the same way @c{ice monitor} does for real hardware.
 *
 * Quit hotkey is @c Ctrl-T+x (matching @c{ice monitor}): closes qemu's
 * stdin, sends @c SIGTERM, and leaves the alt-screen.  @c Ctrl-] is an
 * undocumented panic eject kept as a fallback.  PgUp / PgDn / Home /
 * End scroll the buffer.
 *
 * Defaults to the simple foreground story (single tty, single pane).
 * The dual-pane @c{--debug} mode that drives @c gdb in a second pane
 * will land on top of this once the pty primitive is in place.
 */
#include "ice.h"
#include "json.h"
#include "monitor.h"
#include "platform.h"
#include "sbuf.h"
#include "term.h"
#include "tui.h"
#include "vt100.h"

#include <signal.h>

/* clang-format off */
static const struct cmd_manual qemu_manual = {
	.name = "ice qemu",
	.summary = "run the firmware on QEMU",

	.description =
	H_PARA("Builds a single padded flash image plus a default efuse "
	       "blob from the active build, then launches the Espressif "
	       "QEMU fork (@b{qemu-system-xtensa} for ESP32 / ESP32-S3, "
	       "@b{qemu-system-riscv32} for ESP32-C3) with the chip-"
	       "specific machine arguments and the emulated UART wired "
	       "through to your terminal.")
	H_PARA("Output is rendered through the same vt100 / tui pipeline "
	       "as @b{ice monitor}, so panic backtraces are decoded, log "
	       "levels are coloured, and PgUp / PgDn scroll history.  "
	       "Press @b{Ctrl-T x} to exit and shut down QEMU.")
	H_PARA("Supported targets: @b{esp32}, @b{esp32c3}, @b{esp32s3}.  "
	       "Other chips do not currently have an upstream QEMU "
	       "implementation in the Espressif fork."),

	.examples =
	H_EXAMPLE("ice qemu")
	H_EXAMPLE("ice qemu --flash-file build/factory.bin")
	H_EXAMPLE("ice qemu --qemu-bin /usr/local/bin/qemu-system-xtensa"),

	.extras =
	H_SECTION("KEY BINDINGS")
	H_ITEM("Ctrl-T x",      "Exit and shut down QEMU.")
	H_ITEM("Ctrl-T Ctrl-T", "Send a literal Ctrl-T to the chip.")
	H_ITEM("PgUp/PgDn",     "Scroll the buffer one page at a time.")
	H_ITEM("Home/End",      "Jump to the oldest line / resume tailing.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice monitor",
	       "Run the same UART pane against a real device.")
	H_ITEM("ice flash",
	       "Program the firmware to physical hardware instead."),
};
/* clang-format on */

static const char *opt_qemu_bin;
static const char *opt_flash_file;
static const char *opt_efuse_file;
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
static const struct option cmd_qemu_opts[] = {
	OPT_STRING(0, "qemu-bin", &opt_qemu_bin, "path",
		   "qemu-system-* binary (default: search $PATH)", NULL),
	OPT_STRING(0, "flash-file", &opt_flash_file, "path",
		   "use this pre-built merged flash image instead of building one",
		   NULL),
	OPT_STRING(0, "efuse-file", &opt_efuse_file, "path",
		   "use this efuse blob instead of writing a chip default",
		   NULL),
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

int cmd_qemu(int argc, const char **argv);

const struct cmd_desc cmd_qemu_desc = {
    .name = "qemu",
    .fn = cmd_qemu,
    .opts = cmd_qemu_opts,
    .manual = &qemu_manual,
    .needs = PROJECT_BUILT,
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

/* ------------------------------------------------------------------ */
/*  Tool resolution: ~/.ice/tools/<package>/<version>/                */
/* ------------------------------------------------------------------ */

/*
 * Pick a "best" version directory under @p tool_dir.  Newest-by-name
 * is good enough for v1: ice tools install names directories like
 * @c{esp_develop_9.2.2_20250817} so lexicographic order gets us the
 * latest installed.  Returns the chosen name in @p out (caller-owned)
 * or 0 if @p tool_dir is empty or absent.
 */
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
 * the latest installed version and joining the export sub-path.  The
 * @p subpath argument is the relative path inside the version dir
 * (e.g. @c{"qemu/bin/qemu-system-xtensa"}); for Espressif's qemu fork
 * this matches the tools.json @c export_paths entry.
 *
 * Returns a malloc'd absolute path on success (caller frees), NULL if
 * no matching install is found.  Verifies the binary is actually
 * executable before returning.
 */
static char *resolve_tool(const char *package, const char *subpath)
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

done:
	sbuf_release(&full);
	sbuf_release(&version);
	sbuf_release(&tool_dir);
	return result;
}

/* ------------------------------------------------------------------ */
/*  Flash-image building                                              */
/* ------------------------------------------------------------------ */

/*
 * Parse an IDF-style flash size token ("1MB", "16KB", "0x400000",
 * raw bytes) into a byte count.  Mirrors the parser in
 * @c cmd/image/merge/merge.c so the two stay aligned; reproduced
 * locally because that one is @c static.
 */
static size_t parse_flash_size(const char *s)
{
	char *end;
	unsigned long n;

	errno = 0;
	n = strtoul(s, &end, 0);
	if (errno != 0 || end == s)
		die("invalid flash size '%s'", s);
	if (*end == '\0')
		return (size_t)n;
	if ((end[0] == 'M' || end[0] == 'm') &&
	    (end[1] == 'B' || end[1] == 'b') && end[2] == '\0')
		return (size_t)n * 1024u * 1024u;
	if ((end[0] == 'K' || end[0] == 'k') &&
	    (end[1] == 'B' || end[1] == 'b') && end[2] == '\0')
		return (size_t)n * 1024u;
	die("invalid flash size '%s' (expected e.g. 4MB, 16KB)", s);
	return 0;
}

/*
 * Read @c flasher_args.json's @c flash_settings.flash_size field.  Falls
 * back to "4MB" when the file is missing or the field absent so users
 * with non-IDF builds still get a working default.
 */
static size_t read_flash_size(const char *build_dir)
{
	struct sbuf path = SBUF_INIT;
	struct sbuf buf = SBUF_INIT;
	size_t result = (size_t)4 * 1024 * 1024;

	sbuf_addf(&path, "%s/flasher_args.json", build_dir);
	if (sbuf_read_file(&buf, path.buf) < 0)
		goto done;

	struct json_value *root = json_parse(buf.buf, buf.len);
	if (!root)
		goto done;

	const char *fs = json_as_string(
	    json_get(json_get(root, "flash_settings"), "flash_size"));
	if (fs && *fs)
		result = parse_flash_size(fs);

	json_free(root);

done:
	sbuf_release(&buf);
	sbuf_release(&path);
	return result;
}

/*
 * Build the merged flash image at @p out_path by collecting every
 * @c _project.flash-file entry (offset=path) and placing each at its
 * offset in a buffer padded to @p flash_size with 0xff (the flash-chip
 * erase state).  Same shape as @c cmd_image_merge but runs in-process,
 * since calling that command would mean re-parsing argv.
 */
static int build_flash_image(const char *out_path, size_t flash_size)
{
	struct config_entry **flash_files;
	int n_files = config_get_all("_project.flash-file", &flash_files);
	if (n_files <= 0) {
		fprintf(stderr,
			"ice qemu: no flash files in flasher_args.json\n"
			"  Run 'ice build' first to generate build "
			"artifacts.\n");
		free(flash_files);
		return -1;
	}

	uint8_t *out = malloc(flash_size);
	if (!out)
		die_errno("malloc(%zu)", flash_size);
	memset(out, 0xff, flash_size);

	int rc = 0;
	for (int i = 0; i < n_files; i++) {
		const char *entry = flash_files[i]->value; /* "offset=path" */
		const char *eq = strchr(entry, '=');
		if (!eq) {
			fprintf(stderr,
				"ice qemu: malformed flash-file entry "
				"'%s'\n",
				entry);
			rc = -1;
			goto out;
		}
		size_t off_len = (size_t)(eq - entry);
		char *off_str = malloc(off_len + 1);
		if (!off_str)
			die_errno("malloc");
		memcpy(off_str, entry, off_len);
		off_str[off_len] = '\0';
		char *end;
		errno = 0;
		unsigned long off = strtoul(off_str, &end, 0);
		/* Reject empty offset explicitly: strtoul("") returns 0 with
		 * errno==0 and *end=='\0', so the parser thinks it's a valid
		 * 0 and we'd cheerfully drop the app at offset 0, clobbering
		 * the bootloader.  An empty offset means @c flasher_args.json
		 * was generated without a partition-table lookup -- usually
		 * because the parttool.py compat call into ice failed -- and
		 * we'd rather fail loudly than write a corrupted flash. */
		if (off_len == 0 || end == off_str || errno != 0 ||
		    *end != '\0') {
			fprintf(
			    stderr,
			    "ice qemu: %s offset in '%s'\n"
			    "  flasher_args.json may be missing the app "
			    "partition offset.  Re-run 'ice build' after "
			    "making sure the partition table parses cleanly.\n",
			    off_len == 0 ? "missing" : "bad", entry);
			free(off_str);
			rc = -1;
			goto out;
		}
		free(off_str);

		struct sbuf data = SBUF_INIT;
		if (sbuf_read_file(&data, eq + 1) < 0) {
			fprintf(stderr, "ice qemu: cannot read '%s': %s\n",
				eq + 1, strerror(errno));
			rc = -1;
			goto out;
		}
		if ((size_t)off + data.len > flash_size) {
			fprintf(stderr,
				"ice qemu: '%s' at 0x%lx extends past flash "
				"size 0x%zx\n",
				eq + 1, off, flash_size);
			sbuf_release(&data);
			rc = -1;
			goto out;
		}
		memcpy(out + off, data.buf, data.len);
		sbuf_release(&data);
	}

	mkdirp_for_file(out_path);
	FILE *fp = fopen(out_path, "wb");
	if (!fp) {
		fprintf(stderr, "ice qemu: cannot write '%s': %s\n", out_path,
			strerror(errno));
		rc = -1;
		goto out;
	}
	if (fwrite(out, 1, flash_size, fp) != flash_size) {
		fprintf(stderr, "ice qemu: write error on '%s'\n", out_path);
		fclose(fp);
		rc = -1;
		goto out;
	}
	fclose(fp);

out:
	free(out);
	free(flash_files);
	return rc;
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
		fprintf(stderr, "ice qemu: cannot write '%s': %s\n", path,
			strerror(errno));
		return -1;
	}
	if (fwrite(chip->default_efuse, 1, chip->default_efuse_len, fp) !=
	    chip->default_efuse_len) {
		fprintf(stderr, "ice qemu: write error on '%s'\n", path);
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

static int run_tui(struct process *proc, const char *chip_name)
{
	int rc = term_raw_enter(0);
	if (rc < 0)
		die("cannot set terminal to raw mode: %s", strerror(-rc));
	term_screen_enter();

	int cols = 80, rows = 24;
	term_size(&cols, &rows);

	/* Compose the source label the @ref monitor_session shows in its
	 * status bar.  qemu has no DTR/RTS, so the @c on_reset and
	 * @c on_bootloader callbacks stay NULL -- the session treats
	 * Ctrl-T+r and Ctrl-T+p as silent prefix-cancels. */
	struct sbuf label = SBUF_INIT;
	sbuf_addf(&label, "ice qemu: %s", chip_name);
	if (opt_gdb)
		sbuf_addf(&label, "  \xe2\x80\xa2  [GDB :%d, halted]",
			  opt_gdb_port);

	struct monitor_config cfg = {
	    .scrollback = opt_scrollback,
	    .origin_x = 1,
	    .origin_y = 1,
	    .width = cols,
	    .height = rows,
	    .source_label = label.buf,
	};
	struct monitor_session *m = monitor_new(&cfg);
	sbuf_release(&label);
	if (!m) {
		term_screen_leave();
		term_raw_leave();
		die("monitor_new: out of memory");
	}

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

		uint8_t buf[4096];
		ssize_t n = pipe_read_timed(proc->out, buf, sizeof buf, 30);
		if (n < 0) {
			/* qemu exited / pipe closed -- leave the loop and
			 * surface whatever exit code it had. */
			break;
		}
		if (n > 0)
			monitor_feed_chip(m, buf, (size_t)n);

		struct term_event ev;
		int got = term_read_event(&ev, 0);
		if (got > 0 && ev.key != TK_NONE)
			monitor_feed_event(m, &ev);

		struct sbuf *tx = monitor_chip_tx(m);
		if (tx->len) {
			/* qemu's stdin is a pipe; payloads are atomic under
			 * PIPE_BUF.  Errors mean the child is gone -- next
			 * read iteration will see EOF and bail. */
			(void)write(proc->in, tx->buf, tx->len);
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

	int quit = monitor_should_quit(m);
	monitor_release(m);
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
 * with the rect that says where on the screen the log lives.  Used
 * symmetrically for the gdb pane and the UART pane.
 */
struct dpane {
	struct vt100 *V;
	struct tui_log L;
	struct tui_rect rect; /* outer rect including any chrome */
};

/*
 * Lay out the screen for the dual-pane debug view.  Top row is the
 * status bar; bottom row is the key-binding hint; the body is split
 * evenly between gdb (top half) and UART (bottom half) with a
 * one-row divider in between.  Sizes the gdb pane's tui_log + vt100
 * directly (gdb is rendered in-house through the @ref dpane struct);
 * for the UART pane, returns the rect via @p uart_r so the caller
 * can hand it to @ref monitor_set_rect on the embedded
 * @ref monitor_session.
 */
static void debug_layout(int rows, int cols, struct dpane *gdb_p,
			 struct tui_rect *uart_r, struct tui_rect *status_r,
			 struct tui_rect *divider_r, struct tui_rect *footer_r)
{
	struct tui_rect screen = {.x = 1, .y = 1, .w = cols, .h = rows};
	struct tui_rect rest1, body, after_status;

	/* status row at the top */
	tui_rect_split_h(&screen, status_r, &after_status, 1);
	/* footer row at the bottom */
	tui_rect_split_h(&after_status, &rest1, footer_r,
			 after_status.h > 1 ? after_status.h - 1 : 0);
	/* split the body evenly with a 1-row divider in between */
	int gdb_h = rest1.h / 2;
	tui_rect_split_h(&rest1, &gdb_p->rect, &body, gdb_h);
	tui_rect_split_h(&body, divider_r, uart_r, 1);

	/* Apply the gdb rect to its log widget and resize the vt100 grid
	 * so gdb gets a faithful column count.  The grid is one column
	 * narrower than the pane to leave room for the scrollbar
	 * tui_log_render reserves on the right edge. */
	tui_log_set_origin(&gdb_p->L, gdb_p->rect.x, gdb_p->rect.y);
	tui_log_resize(&gdb_p->L, gdb_p->rect.w, gdb_p->rect.h);
	int gdb_inner_w = gdb_p->rect.w > 1 ? gdb_p->rect.w - 1 : gdb_p->rect.w;
	int gdb_inner_h = gdb_p->rect.h > 0 ? gdb_p->rect.h : 1;
	vt100_resize(gdb_p->V, gdb_inner_h, gdb_inner_w);
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

static void draw_status_bar(struct sbuf *out, const struct tui_rect *r,
			    const char *text, const char *sgr)
{
	if (r->h < 1 || r->w < 1)
		return;
	sbuf_addf(out, "\x1b[%d;%dH\x1b[%sm ", r->y, r->x, sgr);
	int w = r->w - 1;
	int len = (int)strlen(text);
	if (len > w)
		len = w;
	sbuf_add(out, text, (size_t)len);
	for (int i = len; i < w; i++)
		sbuf_addch(out, ' ');
	sbuf_addstr(out, "\x1b[0m");
}

/*
 * Drive the dual-pane view: gdb pane (top), UART pane (bottom),
 * status bar, key-binding hint footer, and a one-row divider.  The
 * user types in whichever pane has focus -- @c Ctrl-T is the prefix:
 *
 *   Ctrl-T Tab     Toggle focus.
 *   Ctrl-T x       Quit.
 *   Ctrl-T Ctrl-T  Send a literal Ctrl-T to the focused pane.
 *
 * @c Ctrl-] is an undocumented panic eject kept as a fallback (matches
 * @c{ice monitor} convention).
 */
static int run_debug(struct process *qemu_proc, const struct qemu_chip *chip)
{
	/* Same resolution rule as the qemu binary above: --gdb-bin >
	 * installed under ice_home/tools/ > PATH.  Different package
	 * name and a different layout under <version>/ -- the chip-
	 * specific binaries live in <package>/bin/. */
	char *gdb_bin_owned = NULL;
	const char *gdb_bin = opt_gdb_bin;
	if (!gdb_bin) {
		const char *pkg =
		    !strcmp(chip->qemu_prog, "qemu-system-riscv32")
			? "riscv32-esp-elf-gdb"
			: "xtensa-esp-elf-gdb";
		struct sbuf sub = SBUF_INIT;
		sbuf_addf(&sub, "%s/bin/%s", pkg, chip->gdb_prog);
		gdb_bin_owned = resolve_tool(pkg, sub.buf);
		sbuf_release(&sub);
		gdb_bin = gdb_bin_owned ? gdb_bin_owned : chip->gdb_prog;
	}
	const char *elf = config_get("_project.elf");
	if (!elf || !*elf)
		die("ice qemu --debug: no ELF resolved (run 'ice build' "
		    "first)");

	int rc = term_raw_enter(0);
	if (rc < 0)
		die("cannot set terminal to raw mode: %s", strerror(-rc));
	term_screen_enter();

	int cols = 80, rows = 24;
	term_size(&cols, &rows);

	/* gdb pane: in-house dpane (vt100 + tui_log), rendered the way
	 * we always have.  No level decoration -- gdb produces its own
	 * ANSI for prompts and command output.  Origin is overwritten by
	 * debug_layout below once we've split the screen. */
	struct dpane gdb_p = {0};
	gdb_p.V = vt100_new(24, 80);
	tui_log_init(&gdb_p.L, opt_scrollback);
	tui_log_set_grid(&gdb_p.L, gdb_p.V);

	struct tui_rect uart_r, status_r, divider_r, footer_r;
	debug_layout(rows, cols, &gdb_p, &uart_r, &status_r, &divider_r,
		     &footer_r);

	/* UART pane: a @ref monitor_session sized to its slice of the
	 * screen.  Same widget @c{ice target monitor} drives standalone,
	 * embedded -- so PgUp/PgDn navigation, Ctrl-T+i inspect mode,
	 * regex search, level coloring all work the same way here.
	 * Reset / bootloader callbacks stay NULL because qemu has no
	 * DTR/RTS to twiddle.  source_label on its own (no extra prefix)
	 * gives a clean per-pane status bar inside the pane. */
	struct sbuf uart_label = SBUF_INIT;
	sbuf_addf(&uart_label, "ice qemu: %s", chip->name);
	struct monitor_config mcfg = {
	    .scrollback = opt_scrollback,
	    .origin_x = uart_r.x,
	    .origin_y = uart_r.y,
	    .width = uart_r.w,
	    .height = uart_r.h,
	    .source_label = uart_label.buf,
	};
	struct monitor_session *uart_m = monitor_new(&mcfg);
	sbuf_release(&uart_label);
	if (!uart_m) {
		term_screen_leave();
		term_raw_leave();
		tui_log_release(&gdb_p.L);
		vt100_free(gdb_p.V);
		die("monitor_new: out of memory");
	}

	/* Spawn gdb in a pty pre-loaded with target remote :3333 + ELF.
	 * The pty is sized to the gdb pane so readline wrapping matches
	 * what we'll render. */
	struct sbuf gdb_remote = SBUF_INIT;
	sbuf_addf(&gdb_remote, "target remote :%d", opt_gdb_port);
	/* @c -q skips the boilerplate splash; @c{set pagination off}
	 * stops gdb's pager from blocking on "Press RETURN" mid-stream
	 * (we own the scrolling, not gdb).  @c{set confirm off} keeps
	 * shutdown / quit clean. */
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
			"ice qemu --debug: cannot launch %s\n"
			"  Pass --gdb-bin or add the chip's gdb to PATH.\n",
			gdb_bin);
		sbuf_release(&gdb_remote);
		monitor_release(uart_m);
		tui_log_release(&gdb_p.L);
		vt100_free(gdb_p.V);
		free(gdb_bin_owned);
		return 1;
	}

	int focus = 0; /* 0 = gdb pane, 1 = UART pane */
	int in_prefix = 0;
	int quit = 0;

	while (!quit) {
		int dirty = 0;

		if (term_resize_pending()) {
			term_size(&cols, &rows);
			debug_layout(rows, cols, &gdb_p, &uart_r, &status_r,
				     &divider_r, &footer_r);
			pty_resize(&gdb_proc, gdb_p.rect.h, gdb_p.rect.w);
			monitor_set_rect(uart_m, uart_r.x, uart_r.y, uart_r.w,
					 uart_r.h);
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

		/* UART pane: feed bytes from qemu's pipe into the embedded
		 * monitor session and drain its chip-TX queue back to qemu.
		 * Non-blocking read -- gdb's pump above already paid the
		 * timeout cost for the loop iteration. */
		uint8_t ubuf[4096];
		ssize_t un =
		    pipe_read_timed(qemu_proc->out, ubuf, sizeof ubuf, 0);
		if (un < 0) {
			/* qemu died -- same logic as gdb above. */
			break;
		}
		if (un > 0) {
			monitor_feed_chip(uart_m, ubuf, (size_t)un);
		}
		struct sbuf *utx = monitor_chip_tx(uart_m);
		if (utx->len) {
			(void)write(qemu_proc->in, utx->buf, utx->len);
			sbuf_reset(utx);
		}
		if (monitor_dirty(uart_m))
			dirty = 1;
		if (monitor_should_quit(uart_m)) {
			/* User pressed Ctrl-T+x inside the UART pane (only
			 * possible after Ctrl-T Ctrl-T to bypass the outer
			 * prefix).  Treat as a clean quit. */
			quit = 1;
			break;
		}

		struct term_event ev;
		int got = term_read_event(&ev, 0);
		if (got > 0 && ev.key != TK_NONE) {
			if (ev.key == TK_RESIZE) {
				cols = ev.cols;
				rows = ev.rows;
				debug_layout(rows, cols, &gdb_p, &uart_r,
					     &status_r, &divider_r, &footer_r);
				pty_resize(&gdb_proc, gdb_p.rect.h,
					   gdb_p.rect.w);
				monitor_set_rect(uart_m, uart_r.x, uart_r.y,
						 uart_r.w, uart_r.h);
				dirty = 1;
			} else if (ev.key == 0x1d) { /* Ctrl-] panic */
				quit = 1;
			} else if (in_prefix) {
				in_prefix = 0;
				if (ev.key == TK_TAB) {
					focus = !focus;
				} else if (ev.key == 'x' || ev.key == 'q') {
					quit = 1;
				} else if (ev.key == 0x14) {
					/* Ctrl-T Ctrl-T: literal Ctrl-T to
					 * the focused child.  For the UART
					 * side that's a feed_event so the
					 * session sees it as its own prefix
					 * entry; gdb gets the raw byte over
					 * its pty. */
					if (focus == 0) {
						uint8_t k = 0x14;
						(void)write(gdb_proc.in, &k, 1);
					} else {
						struct term_event ctrl_t = {
						    .key = 0x14};
						monitor_feed_event(uart_m,
								   &ctrl_t);
					}
				}
				dirty = 1;
			} else if (ev.key == 0x14) { /* Ctrl-T -> prefix */
				in_prefix = 1;
				dirty = 1;
			} else if (focus == 0 && ev.key < 0x100) {
				uint8_t k = (uint8_t)ev.key;
				(void)write(gdb_proc.in, &k, 1);
			} else if (focus == 1) {
				monitor_feed_event(uart_m, &ev);
			}
		}

		if (dirty) {
			struct sbuf frame = SBUF_INIT;
			char status_buf[256];

			snprintf(status_buf, sizeof status_buf,
				 "ice qemu --debug: %s @ :%d  "
				 "\xe2\x80\xa2  %s",
				 chip->name, opt_gdb_port,
				 in_prefix ? "[Ctrl-T] prefix" : "");
			draw_status_bar(&frame, &status_r, status_buf,
					"1;37;44");

			/* Render unfocused pane first so the focused one's
			 * cursor placement wins at the end of the frame.
			 * UART pane is a monitor_session; it draws its own
			 * status bar at the top of its rect, which slots
			 * naturally below the outer status row. */
			if (focus == 0) {
				monitor_render(&frame, uart_m);
				tui_log_render(&frame, &gdb_p.L);
			} else {
				tui_log_render(&frame, &gdb_p.L);
				monitor_render(&frame, uart_m);
			}
			monitor_clear_dirty(uart_m);
			draw_hrule(&frame, divider_r.y, divider_r.x,
				   divider_r.w);

			char hint_buf[256];
			snprintf(hint_buf, sizeof hint_buf,
				 "Focus: [%s]   Ctrl-T:  Tab=switch  "
				 "x=quit",
				 focus == 0 ? "gdb" : "UART");
			draw_status_bar(&frame, &footer_r, hint_buf, "37;44");

			/* Place the terminal cursor inside the focused pane.
			 * Each pane's render emits its own cursor positioning,
			 * but the divider hrule and footer status bar drawn
			 * after them advance the terminal cursor to the end
			 * of the footer text -- so the user sees no cursor
			 * in gdb's prompt or the UART prompt.  Recompute and
			 * re-emit. */
			if (focus == 0) {
				if (vt100_cursor_visible(gdb_p.V)) {
					int row = gdb_p.rect.y +
						  vt100_cursor_row(gdb_p.V);
					int col = gdb_p.rect.x +
						  vt100_cursor_col(gdb_p.V);
					sbuf_addf(&frame,
						  "\x1b[%d;%dH\x1b[?25h", row,
						  col);
				} else {
					sbuf_addstr(&frame, "\x1b[?25l");
				}
			} else {
				int row, col;
				if (monitor_cursor(uart_m, &row, &col)) {
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

	monitor_release(uart_m);
	tui_log_release(&gdb_p.L);
	vt100_free(gdb_p.V);
	sbuf_release(&gdb_remote);
	free(gdb_bin_owned);
	/* User-quit (Ctrl-] / Ctrl-T q): treat the SIGTERM-induced exit
	 * as a clean 0.  Otherwise qemu died on its own -- propagate. */
	return quit ? 0 : qemu_exit;
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

int cmd_qemu(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_qemu_desc);
	if (argc > 0)
		die("too many arguments");

	/* --debug folds in the gdb stub (-gdb tcp::3333 -S) plus the
	 * dual-pane UI; -d / --gdb on its own is the two-terminal flow. */
	if (opt_debug)
		opt_gdb = 1;

	const char *target = config_get("_project.target");
	const char *chip_name = config_get("_project.chip");
	if (!chip_name)
		chip_name =
		    target; /* fall back to target on non-IDF projects */

	if (target && !strcmp(target, "linux"))
		die("ice qemu: linux host build has nothing to emulate "
		    "(use 'ice monitor' to run the binary directly)");

	const struct qemu_chip *chip = find_chip(chip_name);
	if (!chip) {
		fprintf(stderr,
			"ice qemu: chip '%s' is not supported by QEMU yet.\n"
			"  Supported: esp32, esp32c3, esp32s3.\n",
			chip_name ? chip_name : "(unknown)");
		return 1;
	}

	const char *build_dir = config_get("_project.build-dir");
	if (!build_dir)
		die("ice qemu: no build directory in profile (run 'ice "
		    "build')");

	/* ---- flash image ---- */
	struct sbuf flash_path = SBUF_INIT;
	if (opt_flash_file) {
		sbuf_addstr(&flash_path, opt_flash_file);
	} else {
		sbuf_addf(&flash_path, "%s/qemu_flash.bin", build_dir);
		size_t flash_size = read_flash_size(build_dir);
		if (build_flash_image(flash_path.buf, flash_size) < 0) {
			sbuf_release(&flash_path);
			return 1;
		}
	}

	/* ---- efuse blob ---- */
	struct sbuf efuse_path = SBUF_INIT;
	if (opt_efuse_file) {
		sbuf_addstr(&efuse_path, opt_efuse_file);
		if (access(efuse_path.buf, F_OK) != 0) {
			fprintf(stderr, "ice qemu: efuse file '%s' not found\n",
				efuse_path.buf);
			sbuf_release(&flash_path);
			sbuf_release(&efuse_path);
			return 1;
		}
	} else {
		sbuf_addf(&efuse_path, "%s/qemu_efuse.bin", build_dir);
		if (ensure_efuse_file(efuse_path.buf, chip) < 0) {
			sbuf_release(&flash_path);
			sbuf_release(&efuse_path);
			return 1;
		}
	}

	/* ---- qemu binary ---- */
	/* Resolution order: --qemu-bin > installed under ice_home/tools/.
	 * The installed-tools lookup matches the layout that ice tools
	 * install / esp-idf's idf_tools.py both produce
	 * ("<version>/qemu/bin/<binary>").  If the tool isn't installed
	 * yet we run @b{ice tools install --tool <pkg>} against the
	 * project's IDF manifest -- qemu is @c install:on_request, so a
	 * fresh @b{ice init} never grabs it -- and re-resolve. */
	char *qemu_bin_owned = NULL;
	const char *qemu_bin = opt_qemu_bin;
	if (!qemu_bin) {
		const char *pkg = !strcmp(chip->qemu_prog, "qemu-system-xtensa")
				      ? "qemu-xtensa"
				      : "qemu-riscv32";
		struct sbuf sub = SBUF_INIT;
		sbuf_addf(&sub, "qemu/bin/%s", chip->qemu_prog);
		qemu_bin_owned = resolve_tool(pkg, sub.buf);

		if (!qemu_bin_owned) {
			const char *idf_path = config_get("_project.idf-path");
			struct sbuf manifest = SBUF_INIT;
			struct svec icmd = SVEC_INIT;
			struct process iproc = PROCESS_INIT;
			const char *exe = process_exe();
			int irc;

			if (!idf_path || !*idf_path)
				die("ice qemu: no @b{_project.idf-path}; "
				    "run @b{ice init} first");
			sbuf_addf(&manifest, "%s/tools/tools.json", idf_path);
			if (access(manifest.buf, F_OK) != 0)
				die("ice qemu: cannot install qemu, "
				    "'%s' not found",
				    manifest.buf);

			svec_push(&icmd, exe ? exe : "ice");
			svec_push(&icmd, "tools");
			svec_push(&icmd, "install");
			svec_push(&icmd, "--tool");
			svec_push(&icmd, pkg);
			svec_push(&icmd, manifest.buf);
			iproc.argv = icmd.v;

			irc = process_run_progress(&iproc, "Installing qemu",
						   "qemu-install", NULL);
			svec_clear(&icmd);
			sbuf_release(&manifest);
			if (irc != 0) {
				sbuf_release(&sub);
				sbuf_release(&flash_path);
				sbuf_release(&efuse_path);
				return irc;
			}

			qemu_bin_owned = resolve_tool(pkg, sub.buf);
			if (!qemu_bin_owned)
				die("ice qemu: '%s' install completed but "
				    "'%s' not found under "
				    "@b{~/.ice/tools/%s/<version>/}",
				    pkg, chip->qemu_prog, pkg);
		}
		sbuf_release(&sub);
		qemu_bin = qemu_bin_owned;
	}

	/* ---- build argv ---- */
	/* svec_push keeps a NULL sentinel after the last entry, so the
	 * vector is already correctly terminated for argv-style consumers
	 * like process_start. */
	struct svec qemu_argv = SVEC_INIT;
	build_qemu_argv(&qemu_argv, chip, qemu_bin, flash_path.buf,
			efuse_path.buf);

	/* ---- spawn qemu ---- */
	struct process proc = PROCESS_INIT;
	proc.argv = (const char **)qemu_argv.v;
	proc.pipe_in = 1;
	proc.pipe_out = 1;
	proc.merge_err = 1;

	if (process_start(&proc) < 0) {
		fprintf(stderr,
			"ice qemu: cannot launch %s\n"
			"  Install it from the Espressif fork: "
			"https://github.com/espressif/qemu\n",
			qemu_bin);
		svec_clear(&qemu_argv);
		sbuf_release(&flash_path);
		sbuf_release(&efuse_path);
		return 1;
	}

	/* Tell the user how to attach gdb in another terminal.  Printed
	 * before screen_enter so the message stays in the user's terminal
	 * scrollback (alt-screen output is discarded on screen_leave).
	 * The chip is halted at reset until gdb connects and continues.
	 * Skipped under --debug: that mode owns the dual-pane UI and is
	 * already showing the gdb prompt directly. */
	if (opt_gdb && !opt_debug) {
		const char *elf = config_get("_project.elf");
		fprintf(stderr,
			"@y{ice qemu: chip halted, waiting for gdb on "
			"tcp::%d}\n"
			"  In another terminal:\n"
			"    %s%s%s -ex \"target remote :%d\"\n",
			opt_gdb_port, chip->gdb_prog, elf && *elf ? " " : "",
			elf && *elf ? elf : "", opt_gdb_port);
	}

	int interactive =
	    !opt_no_tui && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
	int rc;
	if (opt_debug) {
		if (!interactive)
			die("ice qemu --debug requires an interactive "
			    "terminal");
		rc = run_debug(&proc, chip);
	} else if (interactive) {
		rc = run_tui(&proc, chip->name);
	} else {
		rc = run_dumb(&proc);
	}

	svec_clear(&qemu_argv);
	sbuf_release(&flash_path);
	sbuf_release(&efuse_path);
	free(qemu_bin_owned);
	return rc;
}
