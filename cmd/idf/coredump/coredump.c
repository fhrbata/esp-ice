/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/coredump/coredump.c
 * @brief `ice idf coredump` -- native port of @b{esp_coredump} (info /
 * dbg modes).
 *
 * The full upstream pipeline is:
 *
 *   1. Read a core dump file from disk (one of three formats):
 *        - @b{elf} -- already an ELF core file, ready for GDB.
 *        - @b{raw} -- @c esp_core_dump_priv.h header + per-task TCB +
 *          memory segments + CRC32/SHA256 checksum.
 *        - @b{b64} -- a line-wrapped base64 encoding of @b{raw} (the
 *          format ESP-IDF prints over UART between the
 *          @c{===== CORE DUMP START =====} markers).
 *   2. Validate the checksum (raw / b64 only).
 *   3. Synthesise an ELF core file from the raw blob (PT_NOTE program
 *      headers carrying PRSTATUS for each task; PT_LOAD segments for
 *      the captured memory regions).
 *   4. Spawn @b{<prefix>gdb} with @c{--core=<elf>} and the program ELF,
 *      drive it via MI2 to produce per-task backtraces ("info_corefile"
 *      mode) or hand control to the user ("dbg_corefile" mode).
 *
 * This file is the first cut: it implements step 1 -- read the input,
 * detect the format, and (for @b{b64}) decode it to raw bytes -- and
 * exposes that via @b{--save-core} so end-to-end tests can verify the
 * decode against upstream.  Steps 2-4 land in follow-up commits.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base64.h"
#include "ice.h"
#include "loader.h"

/* clang-format off */
static const struct cmd_manual idf_coredump_manual = {
	.name = "ice idf coredump",
	.summary = "decode and inspect ESP core dumps",

	.description =
	H_PARA("Native port of ESP-IDF's @b{espcoredump.py}.  Reads a "
	       "core dump captured from a crashed application and "
	       "prepares it for inspection (post-mortem backtrace, "
	       "register state, FreeRTOS task list, captured memory).")
	H_PARA("Three input formats are accepted, selected via "
	       "@b{--core-format}:")
	H_ITEM("@b{elf}",
	       "Already an ELF core file, ready for GDB.")
	H_ITEM("@b{raw}",
	       "Binary dump as written to the @b{coredump} flash "
	       "partition (@c{esp_core_dump_priv.h} header + TCBs + "
	       "memory segments + CRC32 / SHA256 checksum).")
	H_ITEM("@b{b64}",
	       "Line-wrapped base64 of the @b{raw} format -- the form "
	       "ESP-IDF emits over UART between the "
	       "@c{===== CORE DUMP START / END =====} markers.")
	H_PARA("@b{auto} (the default) sniffs the first bytes of the "
	       "input: an @c{\\x7fELF} prefix selects @b{elf}; a "
	       "recognised @c{tot_len} / @c{ver} pair selects @b{raw}; "
	       "anything else falls through to @b{b64}.")
	H_PARA("Input decode, header parse, trailing-checksum "
	       "verification, and ELF-corefile extraction are "
	       "implemented.  Per-task backtrace / register display "
	       "arrive in a follow-up commit.")
	H_PARA("By default (no save flag) the command prints a "
	       "structured summary of the on-wire header: dump version, "
	       "chip, total length, task / segment counts, and "
	       "@b{[verified]} or @b{[FAILED]} for the CRC32 / SHA256 "
	       "trailer.  Pass @b{--no-verify} to skip the checksum "
	       "check (e.g. when inspecting a deliberately-tampered "
	       "image).")
	H_PARA("Two save flags emit different artefacts:")
	H_ITEM("@b{--save-core PATH}",
	       "Write a GDB-loadable ELF core file.  For @b{ELF_*} "
	       "dumps the embedded ELF is extracted; for ELF input it "
	       "is copied through.  @b{BIN_V*} dumps require ELF "
	       "synthesis (next commit) and are reported as not yet "
	       "implemented today.")
	H_ITEM("@b{--save-raw PATH}",
	       "Write the raw decoded bytes (the @b{b64} wrapper "
	       "stripped if any).  Useful for piping into upstream "
	       "@b{espcoredump.py} until @b{--save-core} covers every "
	       "dump version.")
	H_PARA("Pass a positional @b{<prog>} (the user's app ELF, with "
	       "debug info) to spawn the chip-specific GDB on the "
	       "extracted core file and print @b{info threads} + "
	       "@b{thread apply all bt}.  GDB is resolved from PATH "
	       "(the @b{xtensa-esp32-elf-gdb} or @b{riscv32-esp-elf-gdb} "
	       "binary, depending on the chip); pass @b{--gdb PATH} to "
	       "override.  When @b{--save-core} is also set, GDB reads "
	       "from that path; otherwise the core ELF goes to a temp "
	       "file in @b{$TMPDIR} (POSIX) / @b{GetTempPath()} "
	       "(Windows) for the duration of the GDB run.")
	H_PARA("Pass @b{--rom-elf PATH} to also load ROM symbols, so "
	       "backtrace frames in @c{esp_rom_*} / @c{ets_*} code show "
	       "function names instead of bare hex addresses.  The "
	       "files ship in the @b{esp-rom-elfs} tools.json package "
	       "(filename pattern: @c{<target>_rev<N>_rom.elf}); "
	       "explicit-args policy applies, so the path is not "
	       "auto-discovered here -- a future @b{ice coredump} "
	       "porcelain will pick it up from the active profile.")
	H_PARA("Pass @b{--interactive} to drop into a gdb prompt "
	       "instead of running @b{info threads} + @b{thread apply "
	       "all bt} in batch mode.  Mirrors upstream's "
	       "@c{dbg_corefile} subcommand: @c{--batch} / @c{--quiet} "
	       "/ @c{--nh} are dropped so the user's @c{~/.gdbinit} is "
	       "honoured and the prompt stays alive; @c{--nx} is kept "
	       "so system / cwd gdbinit don't leak in."),

	.examples =
	H_EXAMPLE("ice idf coredump --core dump.b64")
	H_EXAMPLE("ice idf coredump --core dump.b64 --save-core dump.elf")
	H_EXAMPLE("ice idf coredump --core dump.b64 --save-core dump.elf "
		  "build/app.elf")
	H_EXAMPLE("ice idf coredump --core dump.b64 --interactive "
		  "build/app.elf")
	H_EXAMPLE("ice idf coredump --core dump.b64 --save-raw dump.bin")
	H_EXAMPLE("ice idf coredump --core dump.b64 --no-verify"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("python -m esp_coredump",
	       "Upstream Python tool that this command will eventually "
	       "replace end-to-end."),
};
/* clang-format on */

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

static const char *opt_core;
static const char *opt_core_format = "auto";
static const char *opt_save_core;
static const char *opt_save_raw;
static const char *opt_gdb;
static const char *opt_rom_elf;
static int opt_no_verify;
static int opt_interactive;

static void complete_core_format(void)
{
	complete_emit("auto", "sniff the input format from its prefix");
	complete_emit("elf", "ELF core file (ready for GDB)");
	complete_emit("raw", "binary core dump as in the flash partition");
	complete_emit("b64", "line-wrapped base64 of the raw format");
}

static const struct option cmd_idf_coredump_opts[] = {
    OPT_STRING('c', "core", &opt_core, "PATH",
	       "path to the input core dump file", NULL),
    OPT_STRING('t', "core-format", &opt_core_format, "FMT",
	       "input format: auto (default), elf, raw, b64",
	       complete_core_format),
    OPT_STRING(0, "save-core", &opt_save_core, "PATH",
	       "write a GDB-loadable ELF core file to PATH", NULL),
    OPT_STRING(0, "save-raw", &opt_save_raw, "PATH",
	       "write the raw decoded core image (post-b64) to PATH", NULL),
    OPT_BOOL(0, "no-verify", &opt_no_verify,
	     "skip the trailing CRC32 / SHA256 checksum check"),
    OPT_STRING(0, "gdb", &opt_gdb, "PATH",
	       "path to the gdb binary (default: chip-specific)", NULL),
    OPT_STRING(0, "rom-elf", &opt_rom_elf, "PATH",
	       "ELF with ROM symbols (passed to gdb as add-symbol-file)", NULL),
    OPT_BOOL(0, "interactive", &opt_interactive,
	     "drop into a gdb prompt instead of printing info+bt"),
    OPT_POSITIONAL("[<prog>]", NULL),
    OPT_END(),
};

int cmd_idf_coredump(int argc, const char **argv);

const struct cmd_desc cmd_idf_coredump_desc = {
    .name = "coredump",
    .fn = cmd_idf_coredump,
    .opts = cmd_idf_coredump_opts,
    .manual = &idf_coredump_manual,
};

/* ------------------------------------------------------------------ */
/* Format detection                                                    */
/* ------------------------------------------------------------------ */

enum core_format {
	CORE_FMT_ELF,
	CORE_FMT_RAW,
	CORE_FMT_B64,
};

static int parse_core_format(const char *s, enum core_format *out)
{
	if (!strcmp(s, "elf")) {
		*out = CORE_FMT_ELF;
		return 0;
	}
	if (!strcmp(s, "raw")) {
		*out = CORE_FMT_RAW;
		return 0;
	}
	if (!strcmp(s, "b64")) {
		*out = CORE_FMT_B64;
		return 0;
	}
	return -1;
}

/*
 * Mirror upstream @c get_core_file_format:
 *   - an @c{\x7fELF} prefix means the file is already an ELF core;
 *   - a known @c dump_ver in bytes 4..7 means a raw IDF core image;
 *   - otherwise assume the file is base64-wrapped.
 */
static enum core_format sniff_core_format(const struct sbuf *body)
{
	if (body->len >= 4 && memcmp(body->buf,
				     "\x7f"
				     "ELF",
				     4) == 0)
		return CORE_FMT_ELF;
	if (body->len >= 8) {
		const uint8_t *p = (const uint8_t *)body->buf;
		uint32_t version_word =
		    ((uint32_t)p[4]) | ((uint32_t)p[5] << 8) |
		    ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
		if (core_dump_ver_known(version_word & 0xffffu))
			return CORE_FMT_RAW;
	}
	return CORE_FMT_B64;
}

/* ------------------------------------------------------------------ */
/* b64 decode                                                          */
/* ------------------------------------------------------------------ */

/*
 * Upstream's @c ESPCoreDumpFileLoader._get_core_src reads the b64 file
 * line by line and decodes each line independently with
 * @c base64.standard_b64decode -- so any '=' padding in line N is
 * line-internal, not a stream-wide terminator.  Our @c base64_decode
 * stops at the first '=', so we must call it once per line.
 */
static int decode_b64_lines(const struct sbuf *body, struct sbuf *raw)
{
	const char *p = body->buf;
	const char *end = body->buf + body->len;

	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		size_t line_len = eol ? (size_t)(eol - p) : (size_t)(end - p);

		while (line_len &&
		       (p[line_len - 1] == '\r' || p[line_len - 1] == ' ' ||
			p[line_len - 1] == '\t'))
			line_len--;

		if (line_len > 0 && base64_decode(p, line_len, raw) < 0)
			return -1;

		if (!eol)
			break;
		p = eol + 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Info printer                                                        */
/* ------------------------------------------------------------------ */

static const char *fmt_name(enum core_format fmt)
{
	switch (fmt) {
	case CORE_FMT_ELF:
		return "elf";
	case CORE_FMT_RAW:
		return "raw";
	case CORE_FMT_B64:
		return "b64";
	}
	return "unknown";
}

/*
 * Print a structured summary for a parsed @b{raw} core image (or the
 * post-decode bytes of a @b{b64} image).  Mirrors the most useful
 * fields of upstream @c info_corefile's preamble; per-task backtrace
 * and register display arrive once we have the GDB driver.
 *
 * Returns 0 if (when @p verify is set) the trailing checksum matches,
 * -1 otherwise.  A bad checksum is reported in the printed summary
 * too.
 */
static int print_raw_info(const void *buf, size_t len,
			  const struct core_header *h, enum core_format src_fmt,
			  int verify, FILE *out)
{
	const char *ver_name;
	const char *chip_name;
	uint32_t dump_ver;
	uint32_t chip_ver;
	int verify_rc = 0;
	const char *verify_err = NULL;

	if (verify)
		verify_rc = core_validate(buf, len, h, &verify_err);

	dump_ver = CORE_DUMP_VER(h);
	chip_ver = CORE_CHIP_VER(h);
	ver_name = core_dump_ver_name(dump_ver);
	chip_name = core_chip_idf_name(chip_ver);

	fprintf(out, "core dump:\n");
	fprintf(out, "  format       %s\n", fmt_name(src_fmt));
	fprintf(out, "  version      %s (%u.%u, dump_ver=0x%04x)\n",
		ver_name ? ver_name : "?", (unsigned)CORE_DUMP_MAJOR(dump_ver),
		(unsigned)CORE_DUMP_MINOR(dump_ver), (unsigned)dump_ver);
	fprintf(out, "  data shape   %s\n",
		core_dump_ver_is_elf(dump_ver)
		    ? "embedded ELF core file"
		    : "binary (TCBs + memory segments)");
	fprintf(out, "  chip         %s (chip_ver=%u)\n",
		chip_name ? chip_name : "?", (unsigned)chip_ver);
	if (h->chip_rev != CORE_FIELD_ABSENT)
		fprintf(out, "  chip_rev     0x%04x\n", (unsigned)h->chip_rev);
	fprintf(out, "  total length %u bytes\n", (unsigned)h->tot_len);
	fprintf(out, "  header       %zu bytes\n", h->header_size);
	fprintf(out, "  checksum     %s (%zu bytes) %s\n",
		h->checksum_size == 32 ? "SHA256" : "CRC32", h->checksum_size,
		!verify		 ? "[skipped: --no-verify]"
		: verify_rc == 0 ? "[verified]"
				 : "[FAILED]");
	if (verify && verify_rc < 0)
		fprintf(out, "  status       checksum mismatch: %s\n",
			verify_err ? verify_err : "unknown");
	if (h->task_num != CORE_FIELD_ABSENT)
		fprintf(out, "  tasks        %u (TCB size %u)\n",
			(unsigned)h->task_num, (unsigned)h->tcbsz);
	if (h->segs_num != CORE_FIELD_ABSENT)
		fprintf(out, "  segments     %u\n", (unsigned)h->segs_num);

	return (verify && verify_rc < 0) ? -1 : 0;
}

/*
 * Fall-back when the wire header didn't parse: print whatever we
 * still have (input format + size) and the parser's error string.
 */
static void print_raw_unparsed(const void *buf, size_t len,
			       enum core_format src_fmt, const char *err,
			       FILE *out)
{
	(void)buf;
	fprintf(out, "core dump:\n");
	fprintf(out, "  format       %s\n", fmt_name(src_fmt));
	fprintf(out, "  size         %zu bytes\n", len);
	fprintf(out, "  status       header parse failed: %s\n",
		err ? err : "unknown error");
}

static void print_elf_info(const struct sbuf *body, FILE *out)
{
	fprintf(out, "core dump:\n");
	fprintf(out, "  format       elf\n");
	fprintf(out, "  size         %zu bytes\n", body->len);
	fprintf(out, "  note         already an ELF core file; pass "
		     "to <prefix>gdb directly until @b{ice idf coredump} "
		     "grows its own driver.\n");
}

/* ------------------------------------------------------------------ */
/* GDB runner                                                          */
/* ------------------------------------------------------------------ */

/*
 * Spawn @p gdb_prog on @p core_path loaded against @p prog.
 *
 * Default mode: @c --batch with a canned @c info @c threads + @c
 * thread @c apply @c all @c bt; gdb prints output and exits.
 *
 * @p interactive mode: drops @c --batch / @c --quiet / @c --nh and
 * the canned commands so gdb stays at its prompt with the user's
 * @c ~/.gdbinit honoured.  We still pass @c --nx to keep system /
 * cwd gdbinit out of the picture.  The temp file owned by the
 * caller stays valid for the whole interactive session.
 */
static int run_gdb(const char *gdb_prog, const char *core_path,
		   const char *prog, const char *rom_elf, int interactive)
{
	const char *argv[24];
	int n = 0;
	struct sbuf rom_cmd = SBUF_INIT;
	struct process proc = PROCESS_INIT;

	argv[n++] = gdb_prog;
	if (!interactive) {
		argv[n++] = "--batch";
		argv[n++] = "--quiet";
		argv[n++] = "--nh"; /* don't read ~/.gdbinit */
	}
	argv[n++] = "--nx"; /* don't read system / cwd .gdbinit */
	if (!interactive) {
		argv[n++] = "-ex";
		argv[n++] = "set pagination off";
		argv[n++] = "-ex";
		argv[n++] = "set print pretty on";
	}
	argv[n++] = "--core";
	argv[n++] = core_path;
	argv[n++] = prog;
	if (rom_elf) {
		sbuf_addf(&rom_cmd, "add-symbol-file %s", rom_elf);
		argv[n++] = "-ex";
		argv[n++] = rom_cmd.buf;
	}
	if (!interactive) {
		argv[n++] = "-ex";
		argv[n++] = "info threads";
		argv[n++] = "-ex";
		argv[n++] = "thread apply all bt";
	}
	argv[n] = NULL;

	proc.argv = argv;
	int rc = process_run(&proc);
	sbuf_release(&rom_cmd);
	if (rc < 0) {
		err("failed to spawn @b{%s}: not found in PATH? "
		    "(pass @b{--gdb /path/to/gdb})",
		    gdb_prog);
		return -1;
	}
	if (rc != 0) {
		err("@b{%s} exited with status %d", gdb_prog, rc);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int cmd_idf_coredump(int argc, const char **argv)
{
	struct sbuf input = SBUF_INIT;
	struct sbuf decoded = SBUF_INIT;
	enum core_format fmt;
	const void *out_data;
	size_t out_len;
	const char *prog = NULL;
	int rc = 0;

	argc = parse_options(argc, argv, &cmd_idf_coredump_desc);

	if (argc > 1)
		die("too many positional arguments "
		    "(expected at most one <prog>)");
	if (argc == 1)
		prog = argv[0];

	if (!opt_core || !*opt_core)
		die("--core <path> is required");

	if (opt_rom_elf && access(opt_rom_elf, F_OK) != 0)
		die_errno("--rom-elf '%s'", opt_rom_elf);

	if (sbuf_read_file(&input, opt_core) < 0)
		die_errno("cannot read '%s'", opt_core);

	if (!strcmp(opt_core_format, "auto")) {
		fmt = sniff_core_format(&input);
	} else if (parse_core_format(opt_core_format, &fmt) < 0) {
		die("unknown --core-format '%s' "
		    "(expected auto, elf, raw, b64)",
		    opt_core_format);
	}

	if (fmt == CORE_FMT_B64) {
		if (decode_b64_lines(&input, &decoded) < 0)
			die("invalid base64 in '%s'", opt_core);
		out_data = decoded.buf;
		out_len = decoded.len;
	} else {
		out_data = input.buf;
		out_len = input.len;
	}

	/*
	 * For non-ELF inputs, parse the IDF wire header up front so the
	 * info path and the --save-core path share a single result.
	 * A failure here doesn't abort -- we still print what we can
	 * and let --save-raw work on the raw bytes.
	 */
	struct core_header h;
	int have_header = 0;
	const char *header_err = NULL;
	if (fmt != CORE_FMT_ELF) {
		if (core_header_parse(out_data, out_len, &h, &header_err) == 0)
			have_header = 1;
	}

	if (fmt == CORE_FMT_ELF) {
		print_elf_info(&input, stdout);
	} else if (have_header) {
		if (print_raw_info(out_data, out_len, &h, fmt, !opt_no_verify,
				   stdout) < 0)
			rc = 1;
	} else {
		print_raw_unparsed(out_data, out_len, fmt, header_err, stdout);
		rc = 1;
	}

	if (opt_save_raw) {
		if (write_file_atomic(opt_save_raw, out_data, out_len) != 0) {
			err_errno("cannot write '%s'", opt_save_raw);
			rc = 1;
		}
	}

	/*
	 * Compute @c core_data / @c core_len once: the bytes that will
	 * land in the GDB-loadable ELF, regardless of whether they go
	 * to @c --save-core, to a temp file for @c run_gdb, or both.
	 */
	const void *core_data = NULL;
	size_t core_len = 0;
	int have_core = 0;

	if (opt_save_core || prog) {
		if (fmt == CORE_FMT_ELF) {
			core_data = out_data;
			core_len = out_len;
			have_core = 1;
		} else if (have_header &&
			   core_dump_ver_is_elf(CORE_DUMP_VER(&h))) {
			core_data = (const uint8_t *)out_data + h.header_size;
			core_len = out_len - h.header_size - h.checksum_size;
			have_core = 1;
		} else if (have_header) {
			err("BIN_V* dumps require ELF synthesis (not yet "
			    "implemented in @b{ice idf coredump}); pass "
			    "@b{--save-raw} to dump the raw bytes for now");
			rc = 1;
		} else {
			err("cannot extract core ELF: header parse failed "
			    "(%s)",
			    header_err ? header_err : "unknown");
			rc = 1;
		}
	}

	if (have_core && opt_save_core) {
		if (write_file_atomic(opt_save_core, core_data, core_len) !=
		    0) {
			err_errno("cannot write '%s'", opt_save_core);
			rc = 1;
		}
	}

	/*
	 * Spawn gdb if a program ELF was provided.  When @c --save-core
	 * is also set, reuse that path; otherwise allocate a temp file,
	 * write the core ELF there, and unlink it after gdb returns.
	 * Gated on rc==0 so we don't run gdb on a half-written /
	 * mismatched core file.
	 */
	struct sbuf temp_path = SBUF_INIT;
	if (prog && have_core && rc == 0) {
		const char *core_path = opt_save_core;

		if (!core_path) {
			int fd =
			    make_temp_file("ice-coredump-", "elf", &temp_path);
			if (fd < 0) {
				err_errno("cannot create temp file "
					  "for the GDB-loadable core "
					  "ELF");
				rc = 1;
			} else {
				const uint8_t *p = core_data;
				size_t remaining = core_len;

				while (remaining > 0) {
					ssize_t n = write(fd, p, remaining);
					if (n < 0) {
						if (errno == EINTR)
							continue;
						err_errno("write");
						rc = 1;
						break;
					}
					p += n;
					remaining -= (size_t)n;
				}
				close(fd);
				if (rc == 0)
					core_path = temp_path.buf;
			}
		}

		if (rc == 0) {
			/* Make sure our buffered info text reaches the
			 * terminal before gdb's output streams in. */
			fflush(stdout);

			const char *gdb_prog = opt_gdb;
			if (!gdb_prog) {
				uint32_t chip_ver =
				    have_header ? CORE_CHIP_VER(&h) : 0;
				gdb_prog = core_chip_gdb_prog(chip_ver);
				if (!gdb_prog) {
					err("cannot resolve gdb binary "
					    "for chip_ver=%u; pass "
					    "@b{--gdb PATH} explicitly",
					    (unsigned)chip_ver);
					rc = 1;
				}
			}
			if (gdb_prog &&
			    run_gdb(gdb_prog, core_path, prog, opt_rom_elf,
				    opt_interactive) < 0)
				rc = 1;
		}
	}

	if (temp_path.len) {
		unlink(temp_path.buf);
		sbuf_release(&temp_path);
	}

	sbuf_release(&decoded);
	sbuf_release(&input);
	return rc;
}
