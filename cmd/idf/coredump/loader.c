/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/coredump/loader.c
 * @brief Core-dump wire-format parser.
 *
 * Mirrors @c esp_coredump.corefile.loader.EspCoreDumpLoader._load_core_src:
 * read the leading 16 bytes as a V1 header to get the version word,
 * then -- based on the recognised dump_ver -- re-read the appropriate
 * variant header (V1 / V2 / V2_1 / V2_2) and record the trailing
 * checksum size.
 */
#include "loader.h"

#include "ice.h"
#include "vendor/sha256/sha256.h"

#include <string.h>
#include <zlib.h>

/* ------------------------------------------------------------------ */
/* Wire-format helpers                                                 */
/* ------------------------------------------------------------------ */

static uint32_t rd_le32(const uint8_t *p)
{
	return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Per-version traits                                                  */
/* ------------------------------------------------------------------ */

/*
 * For each known dump_ver, what's the on-wire header size (in u32
 * words) and the checksum size?  V1 is 4 words, V2 5 words, V2_1
 * 6 words, V2_2 3 words.  ELF_CRC32_* uses CRC32 (4B); ELF_SHA256_*
 * uses SHA256 (32B); BIN_V* always uses CRC32.
 */
struct ver_traits {
	uint32_t dump_ver;
	uint8_t header_words; /* number of u32s in the header */
	uint8_t checksum_size;
	const char *name;
};

static const struct ver_traits VER_TABLE[] = {
    {CORE_DUMP_BIN_V1, 4, 4, "BIN_V1"},
    {CORE_DUMP_BIN_V2, 5, 4, "BIN_V2"},
    {CORE_DUMP_BIN_V2_1, 6, 4, "BIN_V2_1"},
    {CORE_DUMP_ELF_CRC32_V2, 5, 4, "ELF_CRC32_V2"},
    {CORE_DUMP_ELF_SHA256_V2, 5, 32, "ELF_SHA256_V2"},
    {CORE_DUMP_ELF_CRC32_V2_1, 6, 4, "ELF_CRC32_V2_1"},
    {CORE_DUMP_ELF_SHA256_V2_1, 6, 32, "ELF_SHA256_V2_1"},
    {CORE_DUMP_ELF_SHA256_V2_2, 3, 32, "ELF_SHA256_V2_2"},
};

#define VER_TABLE_LEN (sizeof(VER_TABLE) / sizeof(VER_TABLE[0]))

static const struct ver_traits *ver_lookup(uint32_t dump_ver)
{
	for (size_t i = 0; i < VER_TABLE_LEN; i++) {
		if (VER_TABLE[i].dump_ver == dump_ver)
			return &VER_TABLE[i];
	}
	return NULL;
}

int core_dump_ver_known(uint32_t dump_ver)
{
	return ver_lookup(dump_ver) != NULL;
}

int core_dump_ver_is_elf(uint32_t dump_ver)
{
	return CORE_DUMP_MAJOR(dump_ver) >= 1;
}

const char *core_dump_ver_name(uint32_t dump_ver)
{
	const struct ver_traits *v = ver_lookup(dump_ver);
	return v ? v->name : NULL;
}

/* ------------------------------------------------------------------ */
/* Chip table                                                          */
/* ------------------------------------------------------------------ */

/*
 * @c esp_chip_info_id_t values from
 * @c components/esp_hw_support/include/esp_chip_info.h.  Kept here
 * (rather than in root @c chip.[ch]) because @c enum ice_chip is a
 * different, internal enumeration -- see CONTRIBUTING.md's "command-
 * specific specialisation stays in the command".
 */
/*
 * @c gdb_prog mirrors upstream's @c get_gdb_path: all Xtensa cores go
 * through @c xtensa-esp32-elf-gdb (S2's own gdb has a known bug; S3
 * is bucketed with the others in upstream too), all RISC-V cores
 * through @c riscv32-esp-elf-gdb.
 */
#define GDB_XT "xtensa-esp32-elf-gdb"
#define GDB_RV "riscv32-esp-elf-gdb"

static const struct {
	uint32_t chip_ver;
	const char *idf_name;
	const char *gdb_prog;
} CHIP_TABLE[] = {
    {0, "esp32", GDB_XT},     {2, "esp32s2", GDB_XT},
    {5, "esp32c3", GDB_RV},   {9, "esp32s3", GDB_XT},
    {12, "esp32c2", GDB_RV},  {13, "esp32c6", GDB_RV},
    {16, "esp32h2", GDB_RV},  {18, "esp32p4", GDB_RV},
    {20, "esp32c61", GDB_RV}, {23, "esp32c5", GDB_RV},
    {25, "esp32h21", GDB_RV}, {28, "esp32h4", GDB_RV},
    {32, "esp32s31", GDB_RV},
};

#define CHIP_TABLE_LEN (sizeof(CHIP_TABLE) / sizeof(CHIP_TABLE[0]))

const char *core_chip_idf_name(uint32_t chip_ver)
{
	for (size_t i = 0; i < CHIP_TABLE_LEN; i++) {
		if (CHIP_TABLE[i].chip_ver == chip_ver)
			return CHIP_TABLE[i].idf_name;
	}
	return NULL;
}

const char *core_chip_gdb_prog(uint32_t chip_ver)
{
	for (size_t i = 0; i < CHIP_TABLE_LEN; i++) {
		if (CHIP_TABLE[i].chip_ver == chip_ver)
			return CHIP_TABLE[i].gdb_prog;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Header parsing                                                      */
/* ------------------------------------------------------------------ */

int core_header_parse(const void *buf, size_t len, struct core_header *out,
		      const char **err)
{
	const uint8_t *p = buf;
	uint32_t tot_len;
	uint32_t version_word;
	const struct ver_traits *traits;
	size_t header_size;

	if (len < 8) {
		*err = "core image truncated (header < 8 bytes)";
		return -1;
	}

	tot_len = rd_le32(p + 0);
	version_word = rd_le32(p + 4);

	traits = ver_lookup(version_word & 0xffffu);
	if (!traits) {
		*err = "unknown core dump version";
		return -1;
	}

	header_size = (size_t)traits->header_words * 4u;
	if (len < header_size) {
		*err = "core image truncated (less than header_size)";
		return -1;
	}
	if (tot_len < header_size + traits->checksum_size) {
		*err = "tot_len smaller than header + checksum";
		return -1;
	}
	if (tot_len != len) {
		*err = "tot_len does not match the input file size";
		return -1;
	}

	memset(out, 0, sizeof(*out));
	out->version_word = version_word;
	out->tot_len = tot_len;
	out->header_size = header_size;
	out->checksum_size = traits->checksum_size;
	out->task_num = CORE_FIELD_ABSENT;
	out->tcbsz = CORE_FIELD_ABSENT;
	out->segs_num = CORE_FIELD_ABSENT;
	out->chip_rev = CORE_FIELD_ABSENT;

	/*
	 * Field layout per version:
	 *
	 *   V1   (4 words): tot_len, ver, task_num, tcbsz
	 *   V2   (5 words): + segs_num
	 *   V2_1 (6 words): + chip_rev
	 *   V2_2 (3 words): tot_len, ver, chip_rev      <-- different shape
	 *
	 * V2_2 (3 words) is the odd one: task_num, tcbsz, segs_num all
	 * moved into ELF NOTE sections in the embedded ELF, so they
	 * are not in the wire header at all.
	 */
	if (traits->header_words == 3) {
		/* V2_2: tot_len, ver, chip_rev */
		out->chip_rev = rd_le32(p + 8);
	} else {
		/* V1 / V2 / V2_1 share the first four words. */
		out->task_num = rd_le32(p + 8);
		out->tcbsz = rd_le32(p + 12);
		if (traits->header_words >= 5)
			out->segs_num = rd_le32(p + 16);
		if (traits->header_words >= 6)
			out->chip_rev = rd_le32(p + 20);
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Checksum validation                                                 */
/* ------------------------------------------------------------------ */

int core_validate(const void *buf, size_t len, const struct core_header *h,
		  const char **err)
{
	const uint8_t *p = buf;
	const uint8_t *trailer;
	size_t covered;

	if (len < h->checksum_size) {
		*err = "image shorter than declared checksum";
		return -1;
	}
	covered = len - h->checksum_size;
	trailer = p + covered;

	if (h->checksum_size == 4) {
		uint32_t expected =
		    ((uint32_t)trailer[0]) | ((uint32_t)trailer[1] << 8) |
		    ((uint32_t)trailer[2] << 16) | ((uint32_t)trailer[3] << 24);
		uint32_t got =
		    (uint32_t)crc32(0, (const Bytef *)p, (uInt)covered);
		if (got != expected) {
			*err = "CRC32 mismatch";
			return -1;
		}
		return 0;
	}

	if (h->checksum_size == 32) {
		SHA256_CTX ctx;
		uint8_t digest[32];

		sha256_init(&ctx);
		sha256_update(&ctx, p, covered);
		sha256_final(&ctx, digest);
		if (memcmp(digest, trailer, 32) != 0) {
			*err = "SHA256 mismatch";
			return -1;
		}
		return 0;
	}

	*err = "unsupported checksum size";
	return -1;
}
