/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/qemu/qemu.c
 * @brief `ice qemu` -- porcelain wrapper around `ice target qemu`.
 *
 * Reads the active profile to:
 *
 *   - resolve the target chip (@c _project.chip);
 *   - build a single padded flash image at
 *     @c{<build>/qemu_flash.bin} from the profile's
 *     @c _project.flash-file entries (offset = path);
 *   - point at @c{<build>/qemu_efuse.bin} for the efuse blob (the
 *     plumbing writes the chip default the first time it sees the
 *     file missing, then honours user changes thereafter);
 *   - resolve the qemu binary, auto-installing
 *     @c qemu-xtensa / @c qemu-riscv32 from the project's IDF tools
 *     manifest if it isn't already under @c{~/.ice/tools/}.
 *
 * Then delegates to @ref cmd_target_qemu for the actual qemu spawn /
 * UI loop.  Same porcelain-mirrors-plumbing shape as @c{ice debug} ↔
 * @c{ice target debug}.
 */
#include "ice.h"
#include "json.h"
#include "platform.h"
#include "sbuf.h"

/* Plumbing entry point declared in cmd/target/qemu/qemu.c. */
int cmd_target_qemu(int argc, const char **argv);

int cmd_qemu(int argc, const char **argv);

static const char *opt_qemu_bin;
static const char *opt_flash_file;
static const char *opt_efuse_file;
static int opt_scrollback = 10000;
static int opt_no_tui;
static int opt_gdb;
static int opt_debug;
static const char *opt_gdb_bin;
static int opt_gdb_port = 3333;

/* clang-format off */
static const struct option cmd_qemu_opts[] = {
	OPT_STRING(0, "qemu-bin", &opt_qemu_bin, "path",
		   "qemu-system-* binary (default: installed under "
		   "~/.ice/tools/, else auto-install)", NULL),
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
	H_ITEM("Ctrl-T h",      "Show command help.")
	H_ITEM("Ctrl-T x",      "Exit and shut down QEMU.")
	H_ITEM("Ctrl-T r",      "Reset the target (--debug only; routed through gdb).")
	H_ITEM("Ctrl-T Ctrl-T", "Send a literal Ctrl-T to the chip.")
	H_ITEM("Ctrl-T ?",      "Show pane (live mode) help.")
	H_ITEM("PgUp/PgDn",     "Scroll the buffer one page at a time.")
	H_ITEM("Home/End",      "Jump to the oldest line / resume tailing.")
	H_ITEM("Mouse wheel",   "Scroll the buffer; hold Shift to select for copy.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice target qemu",
	       "Plumbing: takes chip / merged flash image / efuse blob "
	       "directly on the command line.")
	H_ITEM("ice monitor",
	       "Run the same UART pane against a real device.")
	H_ITEM("ice flash",
	       "Program the firmware to physical hardware instead."),
};
/* clang-format on */

const struct cmd_desc cmd_qemu_desc = {
    .name = "qemu",
    .fn = cmd_qemu,
    .opts = cmd_qemu_opts,
    .manual = &qemu_manual,
    .needs = PROJECT_BUILT,
};

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

/*
 * Resolve the qemu binary for @p chip.  Returns a malloc'd absolute
 * path (caller frees) or NULL if neither installed nor installable.
 *
 * Resolution: @b{--qemu-bin} > installed under
 * @c{~/.ice/tools/qemu-<arch>/<version>/qemu/bin/<prog>} >
 * auto-install via the project's IDF tools manifest.
 */
static char *resolve_qemu(const char *chip_qemu_prog)
{
	if (opt_qemu_bin)
		return sbuf_strdup(opt_qemu_bin);

	const char *pkg = !strcmp(chip_qemu_prog, "qemu-system-xtensa")
			      ? "qemu-xtensa"
			      : "qemu-riscv32";
	struct sbuf sub = SBUF_INIT;
	sbuf_addf(&sub, "qemu/bin/%s", chip_qemu_prog);

	char *bin = resolve_tool(pkg, sub.buf);
	if (bin) {
		sbuf_release(&sub);
		return bin;
	}

	/* Not installed -- try to install from the project's IDF
	 * tools.json.  qemu is @c install:on_request, so a fresh
	 * @b{ice init} never grabs it; fetch it now on first run. */
	const char *idf_path = config_get("_project.idf-path");
	if (!idf_path || !*idf_path)
		die("ice qemu: no @b{_project.idf-path}; "
		    "run @b{ice init} first");

	struct sbuf manifest = SBUF_INIT;
	sbuf_addf(&manifest, "%s/tools/tools.json", idf_path);
	if (access(manifest.buf, F_OK) != 0)
		die("ice qemu: cannot install qemu, '%s' not found",
		    manifest.buf);

	struct svec icmd = SVEC_INIT;
	struct process iproc = PROCESS_INIT;
	const char *exe = process_exe();

	svec_push(&icmd, exe ? exe : "ice");
	svec_push(&icmd, "tools");
	svec_push(&icmd, "install");
	svec_push(&icmd, "--tool");
	svec_push(&icmd, pkg);
	svec_push(&icmd, manifest.buf);
	iproc.argv = icmd.v;

	int irc = process_run_progress(&iproc, "Installing qemu",
				       "qemu-install", NULL);
	svec_clear(&icmd);
	sbuf_release(&manifest);
	if (irc != 0) {
		sbuf_release(&sub);
		die("ice qemu: qemu install failed (exit %d)", irc);
	}

	bin = resolve_tool(pkg, sub.buf);
	sbuf_release(&sub);
	if (!bin)
		die("ice qemu: '%s' install completed but '%s' not found "
		    "under @b{~/.ice/tools/%s/<version>/}",
		    pkg, chip_qemu_prog, pkg);
	return bin;
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

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

/*
 * The chip → qemu-system-* mapping is duplicated here only so the
 * porcelain knows which tool package to install (@c qemu-xtensa vs
 * @c qemu-riscv32).  The full per-chip table (machine, memory,
 * efuse blob, etc.) lives in the plumbing.
 */
static const char *qemu_prog_for_chip(const char *chip)
{
	if (!chip)
		return NULL;
	if (!strcmp(chip, "esp32") || !strcmp(chip, "esp32s3"))
		return "qemu-system-xtensa";
	if (!strcmp(chip, "esp32c3"))
		return "qemu-system-riscv32";
	return NULL;
}

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

	const char *qemu_prog = qemu_prog_for_chip(chip_name);
	if (!qemu_prog) {
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
	}

	/* ---- qemu binary (resolves under ~/.ice/tools/, auto-installs
	 * if missing) ---- */
	char *qemu_bin = resolve_qemu(qemu_prog);

	/* ---- build argv for ice target qemu ---- */
	char scrollback_str[32];
	char gdb_port_str[32];
	snprintf(scrollback_str, sizeof scrollback_str, "%d", opt_scrollback);
	snprintf(gdb_port_str, sizeof gdb_port_str, "%d", opt_gdb_port);

	const char *qargv[24];
	int fa = 0;
	qargv[fa++] = "ice target qemu";
	qargv[fa++] = "--chip";
	qargv[fa++] = chip_name;
	qargv[fa++] = "--flash-file";
	qargv[fa++] = flash_path.buf;
	qargv[fa++] = "--efuse-file";
	qargv[fa++] = efuse_path.buf;
	qargv[fa++] = "--qemu-bin";
	qargv[fa++] = qemu_bin;
	qargv[fa++] = "--scrollback";
	qargv[fa++] = scrollback_str;
	qargv[fa++] = "--gdb-port";
	qargv[fa++] = gdb_port_str;
	if (opt_no_tui)
		qargv[fa++] = "--no-tui";
	if (opt_gdb)
		qargv[fa++] = "--gdb";
	if (opt_debug)
		qargv[fa++] = "--debug";
	if (opt_gdb_bin) {
		qargv[fa++] = "--gdb-bin";
		qargv[fa++] = opt_gdb_bin;
	}
	/* ELF is only meaningful under --debug (gdb pane); the plumbing
	 * accepts it always but ignores it otherwise.  Forward whatever
	 * the project resolved -- gdb without symbols is degraded enough
	 * to be worth always passing when we have one. */
	const char *elf = config_get("_project.elf");
	if (elf && *elf) {
		qargv[fa++] = "--elf";
		qargv[fa++] = elf;
	}
	qargv[fa] = NULL;

	int rc = cmd_target_qemu(fa, qargv);

	free(qemu_bin);
	sbuf_release(&flash_path);
	sbuf_release(&efuse_path);
	return rc;
}
