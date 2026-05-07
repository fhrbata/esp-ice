/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/coredump/coredump.c
 * @brief The "ice coredump" porcelain -- profile-aware wrapper that
 * pulls a core dump straight off the chip's @c data/coredump
 * partition and analyses it.
 *
 * Concretely:
 *
 *   1. Read project config: @c _project.target / @c _project.elf /
 *      @c _project.idf-path.
 *   2. Connect to the chip via @c pt_target_connect().
 *   3. Load the partition table (from device), find the
 *      @c data/coredump entry.
 *   4. @c esp_loader_flash_read() the whole partition.
 *   5. Peek the first 32-bit word: @c tot_len from the IDF wire
 *      header.  Trim trailing 0xff padding to those bytes.
 *   6. Write the trimmed bytes to a temp file.
 *   7. Auto-detect the ROM ELF for the chip + revision via
 *      @c $IDF_PATH/components/esp_rom/roms.json (or the pre-v5.5
 *      fallback path), and the @c esp-rom-elfs tools.json package
 *      install dir.  Skipped silently if anything is missing or if
 *      the user passed @c{--rom-elf} explicitly.
 *   8. Hand off to @c cmd_idf_coredump() (a direct C call -- no
 *      subprocess) with @c{--core <tmp> --core-format raw [...]}
 *      plus the project's app ELF as the positional @c <prog>.
 *   9. Unlink the temp file.
 */
#include "cmd/idf/coredump/loader.h"
#include "cmd/target/partition/partition.h"
#include "ice.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Plumbing entry point: cmd/idf/coredump/coredump.c */
int cmd_idf_coredump(int argc, const char **argv);

int cmd_coredump(int argc, const char **argv);

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

static const char *opt_port;
static int opt_baud = 460800;
static int opt_no_verify;
static int opt_interactive;
static const char *opt_save_core;
static const char *opt_save_raw;
static const char *opt_rom_elf;

/* clang-format off */
static const struct cmd_manual coredump_manual = {
	.name = "ice coredump",
	.summary = "decode the chip's saved core dump (info / dbg)",

	.description =
	H_PARA("Reads the @b{data/coredump} partition off the chip, "
	       "trims it to the actual dump length, and hands it to "
	       "@b{ice idf coredump} together with the active "
	       "profile's app ELF.  Project-aware counterpart to "
	       "upstream @b{idf.py coredump-info}.")
	H_PARA("Project config drives almost everything: "
	       "@b{_project.target} (chip family), @b{_project.elf} "
	       "(app ELF for backtrace symbols), and "
	       "@b{_project.idf-path} (used to locate "
	       "@b{components/esp_rom/roms.json} for picking the right "
	       "ROM ELF for this chip's silicon revision).")
	H_PARA("ROM symbol resolution is automatic: ice consults "
	       "@b{roms.json} for the chip's @b{rev} (from the dump's "
	       "@c{chip_rev} field, encoded as "
	       "@b{major * 100 + minor}) and passes the matching ELF "
	       "from the @b{esp-rom-elfs} tools.json package via "
	       "@b{--rom-elf}.  Pass @b{--rom-elf PATH} explicitly to "
	       "override the auto-detection."),

	.examples =
	H_EXAMPLE("ice coredump")
	H_EXAMPLE("ice coredump --interactive")
	H_EXAMPLE("ice coredump --save-core dump.elf")
	H_EXAMPLE("ice coredump -p /dev/ttyUSB0 -b 921600"),

	.extras =
	H_SECTION("CONFIG")
	H_ITEM("_project.target",
	       "Chip name (@b{esp32}, @b{esp32c3}, ...).")
	H_ITEM("_project.elf",
	       "App ELF used for backtrace symbols.")
	H_ITEM("_project.idf-path",
	       "ESP-IDF path; used to locate @b{roms.json} for ROM ELF "
	       "auto-detection.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice idf coredump",
	       "Plumbing: decode an explicit @b{--core} file."),
};
/* clang-format on */

static const struct option cmd_coredump_opts[] = {
    OPT_STRING_CFG('p', "port", &opt_port, "path", "serial.port", "ESPPORT",
		   "serial port device path", NULL, serial_complete_port),
    OPT_INT_CFG('b', "baud", &opt_baud, "rate", "serial.baud", "ESPBAUD",
		"baud rate (default: 460800)", NULL, NULL),
    OPT_BOOL(0, "no-verify", &opt_no_verify,
	     "skip the trailing CRC32 / SHA256 checksum check"),
    OPT_BOOL(0, "interactive", &opt_interactive,
	     "drop into a gdb prompt instead of printing info+bt"),
    OPT_STRING(0, "save-core", &opt_save_core, "PATH",
	       "write a GDB-loadable ELF core file to PATH", NULL),
    OPT_STRING(0, "save-raw", &opt_save_raw, "PATH",
	       "write the raw decoded core image (post-b64) to PATH", NULL),
    OPT_STRING(0, "rom-elf", &opt_rom_elf, "PATH",
	       "ROM ELF override (skip auto-detection)", NULL),
    OPT_END(),
};

const struct cmd_desc cmd_coredump_desc = {
    .name = "coredump",
    .fn = cmd_coredump,
    .opts = cmd_coredump_opts,
    .manual = &coredump_manual,
    .needs = PROJECT_CONFIGURED,
};

/* ------------------------------------------------------------------ */
/* ROM ELF auto-detection                                              */
/* ------------------------------------------------------------------ */

static int read_roms_json(const char *idf_path, struct sbuf *body)
{
	struct sbuf path = SBUF_INIT;

	sbuf_addf(&path, "%s/components/esp_rom/roms.json", idf_path);
	if (sbuf_read_file(body, path.buf) >= 0) {
		sbuf_release(&path);
		return 0;
	}
	sbuf_reset(&path);
	sbuf_addf(&path, "%s/tools/idf_py_actions/roms.json", idf_path);
	if (sbuf_read_file(body, path.buf) >= 0) {
		sbuf_release(&path);
		return 0;
	}
	sbuf_release(&path);
	return -1;
}

/*
 * Find the @c rev entry in @p target_arr that exactly matches
 * @p chip_rev.  Returns the rev value on hit (always non-negative
 * because roms.json revs are non-negative) or -1 on miss.
 */
static int find_rom_rev(const struct json_value *target_arr, uint32_t chip_rev)
{
	int n = json_array_size(target_arr);
	for (int i = 0; i < n; i++) {
		struct json_value *entry = json_array_at(target_arr, i);
		struct json_value *rev_v = json_get(entry, "rev");
		if (!rev_v)
			continue;
		int rev = (int)json_as_number(rev_v);
		if (rev >= 0 && (uint32_t)rev == chip_rev)
			return rev;
	}
	return -1;
}

/*
 * Locate the ROM ELF for @p target at silicon @p chip_rev.  Reads
 * @p idf_path's @c roms.json to confirm the rev exists, then builds
 * a path under the @c esp-rom-elfs install dir (populated as
 * @c $ESP_ROM_ELF_DIR by @c setup_tool_env).
 *
 * Returns a malloc'd string on success (caller frees) or NULL if
 * the lookup failed at any step (no roms.json, no matching rev,
 * @c ESP_ROM_ELF_DIR unset, file not on disk).  The function is
 * deliberately silent on failure -- ROM auto-detection is best-
 * effort; gdb still works without it (just prints hex addresses
 * for ROM frames).
 */
static char *autodetect_rom_elf(const char *idf_path, const char *target,
				uint32_t chip_rev)
{
	if (!idf_path || !*idf_path)
		return NULL;

	struct sbuf body = SBUF_INIT;
	if (read_roms_json(idf_path, &body) < 0)
		return NULL;

	struct json_value *root = json_parse(body.buf, body.len);
	sbuf_release(&body);
	if (!root)
		return NULL;

	struct json_value *target_arr = json_get(root, target);
	int rev = target_arr ? find_rom_rev(target_arr, chip_rev) : -1;
	json_free(root);
	if (rev < 0)
		return NULL;

	/*
	 * @c setup_tool_env populates @c ESP_ROM_ELF_DIR from
	 * tools.json / install state.  Calling it just for the side
	 * effect is the minimal-code path; the alternative would be
	 * to expose @c preferred_installed_version() and replicate
	 * the install-dir logic here.
	 */
	setup_tool_env(idf_path);
	const char *rom_dir = getenv("ESP_ROM_ELF_DIR");
	if (!rom_dir || !*rom_dir)
		return NULL;

	struct sbuf path = SBUF_INIT;
	sbuf_addf(&path, "%s/%s_rev%d_rom.elf", rom_dir, target, rev);
	if (access(path.buf, F_OK) != 0) {
		sbuf_release(&path);
		return NULL;
	}
	return sbuf_detach(&path);
}

/* ------------------------------------------------------------------ */
/* Chip-side: read coredump partition, trim to tot_len                 */
/* ------------------------------------------------------------------ */

static uint32_t rd_le32(const uint8_t *p)
{
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Connect to the chip, find the @c data/coredump partition, read it
 * into @p part_out (caller frees), set @p tot_len_out to the dump's
 * declared @c tot_len.  Returns 0 on success, -1 on any failure
 * (diagnostic already printed via err()).
 */
static int read_coredump_partition(const char *port, int baud,
				   uint8_t **part_out, uint32_t *tot_len_out,
				   char **autoport_out)
{
	struct pt_entry entries[PT_MAX_ENTRIES];
	int count = 0;
	struct pt_target_selector sel = {.type = "data", .subtype = "coredump"};
	const struct pt_entry *e;
	esp_loader_t loader;
	esf_port_t sport;
	uint8_t *buf = NULL;
	esp_loader_error_t lerr;
	int rc = -1;

	pt_target_reset_opts();

	if (pt_target_connect(&loader, &sport, port, baud, autoport_out) != 0)
		goto out;

	if (pt_target_load(entries, &count, &loader, 0x8000) != 0)
		goto out;

	e = pt_target_select(entries, count, &sel, &loader);
	if (!e)
		goto out;

	if (e->size < 16) {
		err("coredump partition is implausibly small (%u bytes)",
		    e->size);
		goto out;
	}

	buf = malloc(e->size);
	if (!buf) {
		err_errno("malloc %u bytes", e->size);
		goto out;
	}

	printf("Reading coredump partition (0x%x bytes at 0x%x)...\n", e->size,
	       e->offset);
	fflush(stdout);

	lerr = esp_loader_flash_read(&loader, buf, e->offset, e->size);
	if (lerr != ESP_LOADER_SUCCESS) {
		err("flash_read failed (esp-loader err %d)", lerr);
		goto out;
	}

	uint32_t tot_len = rd_le32(buf);

	/*
	 * An erased partition (no dump captured) reads as all 0xff,
	 * so @c tot_len == 0xffffffff.  Anything outside [16, e->size]
	 * is also bogus.
	 */
	if (tot_len == 0xffffffffu) {
		err("no core dump on the chip: coredump partition is "
		    "erased.  The application either has not crashed "
		    "since the partition was last erased, or core-dump "
		    "to flash is disabled in sdkconfig "
		    "(@b{CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH}).");
		goto out;
	}
	if (tot_len < 16 || tot_len > e->size) {
		err("coredump partition has implausible @b{tot_len} = %u "
		    "(partition is %u bytes); contents may be corrupted",
		    tot_len, e->size);
		goto out;
	}

	*part_out = buf;
	*tot_len_out = tot_len;
	buf = NULL;
	rc = 0;
out:
	free(buf);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int cmd_coredump(int argc, const char **argv)
{
	uint8_t *part_buf = NULL;
	uint32_t tot_len = 0;
	char *autoport = NULL;
	char *rom_elf_auto = NULL;
	struct sbuf temp_path = SBUF_INIT;
	int rc = 1;

	argc = parse_options(argc, argv, &cmd_coredump_desc);
	if (argc > 0)
		die("too many arguments");

	const char *target = config_get("_project.target");
	const char *prog = config_get("_project.elf");
	const char *idf_path = config_get("_project.idf-path");

	if (!target || !*target)
		die("@b{_project.target} is unset; run @b{ice init} first");

	if (read_coredump_partition(opt_port, opt_baud, &part_buf, &tot_len,
				    &autoport) != 0)
		goto out;

	/*
	 * Write the trimmed dump to a temp file so the plumbing can
	 * read it via @c{--core <path> --core-format raw}.  Unlinked
	 * after @c cmd_idf_coredump returns.
	 */
	{
		int fd = make_temp_file("ice-coredump-", "bin", &temp_path);
		if (fd < 0) {
			err_errno("cannot create temp file");
			goto out;
		}
		const uint8_t *p = part_buf;
		size_t remaining = tot_len;
		while (remaining > 0) {
			ssize_t n = write(fd, p, remaining);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				err_errno("write");
				close(fd);
				goto out;
			}
			p += n;
			remaining -= (size_t)n;
		}
		close(fd);
	}

	/* Parse the header to learn chip_rev for ROM ELF auto-detection. */
	struct core_header h;
	const char *parse_err = NULL;
	int have_chip_rev = 0;
	if (core_header_parse(part_buf, tot_len, &h, &parse_err) == 0 &&
	    h.chip_rev != CORE_FIELD_ABSENT)
		have_chip_rev = 1;

	if (!opt_rom_elf && have_chip_rev)
		rom_elf_auto = autodetect_rom_elf(idf_path, target, h.chip_rev);
	const char *rom_elf = opt_rom_elf ? opt_rom_elf : rom_elf_auto;

	/* Build argv for the plumbing and call it directly. */
	const char *iargv[24];
	int n = 0;
	char baud_str[32];
	(void)baud_str;
	iargv[n++] = "ice idf coredump";
	iargv[n++] = "--core";
	iargv[n++] = temp_path.buf;
	iargv[n++] = "--core-format";
	iargv[n++] = "raw";
	if (opt_no_verify)
		iargv[n++] = "--no-verify";
	if (opt_interactive)
		iargv[n++] = "--interactive";
	if (opt_save_core) {
		iargv[n++] = "--save-core";
		iargv[n++] = opt_save_core;
	}
	if (opt_save_raw) {
		iargv[n++] = "--save-raw";
		iargv[n++] = opt_save_raw;
	}
	if (rom_elf) {
		iargv[n++] = "--rom-elf";
		iargv[n++] = rom_elf;
	}
	if (prog && *prog)
		iargv[n++] = prog;
	iargv[n] = NULL;

	rc = cmd_idf_coredump(n, iargv);

out:
	if (temp_path.len) {
		unlink(temp_path.buf);
		sbuf_release(&temp_path);
	}
	free(part_buf);
	free(rom_elf_auto);
	free(autoport);
	return rc;
}
