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
 * Quit hotkey is @c Ctrl-]: closes qemu's stdin, sends @c SIGTERM, and
 * leaves the alt-screen.  PgUp / PgDn / Home / End scroll the buffer.
 *
 * Defaults to the simple foreground story (single tty, single pane).
 * The dual-pane @c{--debug} mode that drives @c gdb in a second pane
 * will land on top of this once the pty primitive is in place.
 */
#include "ice.h"
#include "json.h"
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
	       "Press @b{Ctrl-]} to exit and shut down QEMU.")
	H_PARA("Supported targets: @b{esp32}, @b{esp32c3}, @b{esp32s3}.  "
	       "Other chips do not currently have an upstream QEMU "
	       "implementation in the Espressif fork."),

	.examples =
	H_EXAMPLE("ice qemu")
	H_EXAMPLE("ice qemu --flash-file build/factory.bin")
	H_EXAMPLE("ice qemu --qemu-bin /usr/local/bin/qemu-system-xtensa"),

	.extras =
	H_SECTION("KEY BINDINGS")
	H_ITEM("Ctrl-]",     "Exit and shut down QEMU.")
	H_ITEM("PgUp/PgDn",  "Scroll the buffer one page at a time.")
	H_ITEM("Home/End",   "Jump to the oldest line / resume tailing.")

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
	.machine = "esp32",
	.memory = "4M",
	.boot_mode_strap = "driver=esp32.gpio,property=strap_mode,value=0x0f",
	.default_efuse = default_efuse_esp32,
	.default_efuse_len = sizeof default_efuse_esp32,
    },
    {
	.name = "esp32c3",
	.qemu_prog = "qemu-system-riscv32",
	.machine = "esp32c3",
	.memory = NULL,
	.boot_mode_strap = "driver=esp32c3.gpio,property=strap_mode,value=0x02",
	.default_efuse = default_efuse_esp32c3,
	.default_efuse_len = sizeof default_efuse_esp32c3,
    },
    {
	.name = "esp32s3",
	.qemu_prog = "qemu-system-xtensa",
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
	size_t result = 4u * 1024u * 1024u;

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
		if (errno != 0 || *end != '\0') {
			fprintf(stderr, "ice qemu: bad offset '%s' in '%s'\n",
				off_str, entry);
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

static int run_tui(struct process *proc, const char *qemu_bin,
		   const char *chip_name)
{
	int rc = term_raw_enter(0);
	if (rc < 0)
		die("cannot set terminal to raw mode: %s", strerror(-rc));
	term_screen_enter();

	int cols = 80, rows = 24;
	term_size(&cols, &rows);

	struct tui_log L;
	tui_log_init(&L, opt_scrollback);
	if (use_color)
		tui_log_set_decorator(&L, decorate_idf_level, NULL);
	tui_log_resize(&L, cols, rows);

	struct sbuf status = SBUF_INIT;
	sbuf_addf(&status, "ice qemu: %s @ %s  \xe2\x80\xa2  Ctrl-] exit",
		  chip_name, qemu_bin);
	tui_log_set_status(&L, status.buf);

	/* Inner-terminal vt100: same scaling rule as cmd/target/monitor.
	 * Body is cols-1 wide (scrollbar) by rows-1 tall (status). */
	int inner_cols = cols > 1 ? cols - 1 : cols;
	int inner_rows = rows > 1 ? rows - 1 : rows;
	struct vt100 *V = vt100_new(inner_rows, inner_cols);
	tui_log_set_grid(&L, V);

	{
		struct sbuf frame = SBUF_INIT;
		tui_log_render(&frame, &L);
		tui_flush(&frame);
	}

	int quit = 0;
	while (!quit) {
		uint8_t buf[4096];
		int dirty = 0;

		if (term_resize_pending()) {
			term_size(&cols, &rows);
			tui_log_resize(&L, cols, rows);
			int new_inner_cols = cols > 1 ? cols - 1 : cols;
			int new_inner_rows = rows > 1 ? rows - 1 : rows;
			vt100_resize(V, new_inner_rows, new_inner_cols);
			dirty = 1;
		}

		ssize_t n = pipe_read_timed(proc->out, buf, sizeof buf, 30);
		if (n < 0) {
			/* qemu exited / pipe closed -- leave the loop and
			 * surface whatever exit code it had. */
			quit = 1;
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
				tui_log_resize(&L, ev.cols, ev.rows);
				int ic = ev.cols > 1 ? ev.cols - 1 : ev.cols;
				int ir = ev.rows > 1 ? ev.rows - 1 : ev.rows;
				vt100_resize(V, ir, ic);
				cols = ev.cols;
				rows = ev.rows;
				dirty = 1;
			} else if (ev.key == 0x1d) { /* Ctrl-] */
				quit = 1;
			} else if (ev.key == TK_PGUP || ev.key == TK_PGDN ||
				   ev.key == TK_HOME || ev.key == TK_END ||
				   ev.key == TK_UP || ev.key == TK_DOWN) {
				if (tui_log_on_event(&L, &ev))
					dirty = 1;
			} else if (ev.key < 0x100) {
				/* Plain printable / C0 control: forward to
				 * qemu's UART RX.  Single-byte writes to a
				 * pipe are always atomic. */
				uint8_t k = (uint8_t)ev.key;
				if (write(proc->in, &k, 1) < 0) {
					quit = 1;
				}
			}
		}

		if (dirty) {
			struct sbuf frame = SBUF_INIT;
			tui_log_render(&frame, &L);
			tui_flush(&frame);
		}
	}

	tui_log_release(&L);
	vt100_free(V);
	sbuf_release(&status);
	term_screen_leave();
	term_raw_leave();

	/* Ask qemu to terminate, then reap.  On POSIX this is plain
	 * kill(2); on Windows the platform.h shim maps to TerminateProcess
	 * via the pid_t-cast HANDLE that process_start stored. */
	kill(proc->pid, SIGTERM);
	int exit_code = process_finish(proc);
	(void)exit_code;
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

int cmd_qemu(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_qemu_desc);
	if (argc > 0)
		die("too many arguments");

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
	const char *qemu_bin = opt_qemu_bin ? opt_qemu_bin : chip->qemu_prog;

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

	int interactive =
	    !opt_no_tui && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
	int rc;
	if (interactive)
		rc = run_tui(&proc, qemu_bin, chip->name);
	else
		rc = run_dumb(&proc);

	svec_clear(&qemu_argv);
	sbuf_release(&flash_path);
	sbuf_release(&efuse_path);
	return rc;
}
