/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/coredump/loader.h
 * @brief Wire-format parser for the IDF core-dump header.
 *
 * The header format is defined by
 * @c{components/espcoredump/include_core_dump/esp_core_dump_priv.h}.
 * Eight versions exist in the field today; their layouts differ only
 * in which trailing fields are present, so this module presents a
 * single @c struct core_header that captures the union of fields.
 */
#ifndef CMD_IDF_COREDUMP_LOADER_H
#define CMD_IDF_COREDUMP_LOADER_H

#include <stddef.h>
#include <stdint.h>

/*
 * The 32-bit @c version word is split into two halves on the wire:
 *
 *   bits  0..15 -- dump_ver  (split into major:8 / minor:8)
 *   bits 16..31 -- chip_ver  (the value of the silicon's
 *                             @c esp_chip_info_id_t enum)
 *
 * @c major identifies the *shape* of the data section that follows
 * the header.  @c major == 0 means the data is a raw "binary" dump
 * (BIN_V*); @c major == 1 means the data is itself an ELF core file
 * (ELF_*) wrapped only by the IDF header + checksum.
 */
#define CORE_DUMP_VER(h) ((uint16_t)((h)->version_word & 0xffffu))
#define CORE_CHIP_VER(h) ((uint16_t)(((h)->version_word >> 16) & 0xffffu))
#define CORE_DUMP_MAJOR(v) (((v) >> 8) & 0xffu)
#define CORE_DUMP_MINOR(v) ((v) & 0xffu)

/*
 * Known dump_ver values.  Symbolic names mirror upstream
 * @c EspCoreDumpLoader; numeric values are
 * (major << 8) | minor as produced by @c make_dump_ver().
 */
#define CORE_DUMP_BIN_V1 0x0001u	  /* major=0, minor=1 */
#define CORE_DUMP_BIN_V2 0x0002u	  /* major=0, minor=2 */
#define CORE_DUMP_BIN_V2_1 0x0003u	  /* major=0, minor=3 */
#define CORE_DUMP_ELF_CRC32_V2 0x0100u	  /* major=1, minor=0 */
#define CORE_DUMP_ELF_SHA256_V2 0x0101u	  /* major=1, minor=1 */
#define CORE_DUMP_ELF_CRC32_V2_1 0x0102u  /* major=1, minor=2 */
#define CORE_DUMP_ELF_SHA256_V2_1 0x0103u /* major=1, minor=3 */
#define CORE_DUMP_ELF_SHA256_V2_2 0x0104u /* major=1, minor=4 */

/*
 * Sentinel value used in @c struct core_header for fields that
 * the parsed wire header did not carry (e.g. @c chip_rev in BIN_V1).
 */
#define CORE_FIELD_ABSENT ((uint32_t)0xffffffffu)

struct core_header {
	/*
	 * Verbatim 32-bit word from the wire (offset 4).  Split into
	 * dump_ver / chip_ver via the macros above.
	 */
	uint32_t version_word;

	/*
	 * Total length of the on-wire image in bytes (header + data +
	 * checksum).  Always present.
	 */
	uint32_t tot_len;

	/*
	 * task_num, tcbsz, segs_num, chip_rev are set to
	 * CORE_FIELD_ABSENT when the version's wire header doesn't
	 * carry that field.  V2_2 -- the most recent format -- only
	 * carries tot_len, version_word, chip_rev, having moved task /
	 * segment metadata into ELF NOTE sections inside the data.
	 */
	uint32_t task_num;
	uint32_t tcbsz;
	uint32_t segs_num;
	uint32_t chip_rev;

	/*
	 * Number of bytes consumed from the front of the buffer for
	 * this header, and from the back for the trailing checksum.
	 * The data section is everything in between.
	 */
	size_t header_size;
	size_t checksum_size; /* 4 = CRC32, 32 = SHA256 */
};

/*
 * Returns 1 if @p dump_ver matches one of the eight known versions,
 * 0 otherwise.
 */
int core_dump_ver_known(uint32_t dump_ver);

/*
 * Returns 1 if @p dump_ver is an ELF-shaped dump (data section is an
 * embedded ELF core file), 0 if it's a BIN-shaped dump (data section
 * is raw TCBs + memory segments that need synthesis).
 */
int core_dump_ver_is_elf(uint32_t dump_ver);

/*
 * Symbolic name for @p dump_ver: "BIN_V1", "ELF_SHA256_V2_2", etc.
 * Returns NULL for unknown values.
 */
const char *core_dump_ver_name(uint32_t dump_ver);

/*
 * IDF target name ("esp32", "esp32c3", ...) for an
 * @c esp_chip_info_id_t value.  Returns NULL for unknown chips.
 */
const char *core_chip_idf_name(uint32_t chip_ver);

/*
 * Default GDB binary name for a given chip.  Mirrors upstream
 * @c esp_coredump.coredump.EspCoreDump.get_gdb_path:
 *   - Xtensa chips (esp32 / s2 / s3) all use @c xtensa-esp32-elf-gdb
 *     (per upstream's note that @c xtensa-esp32s2-elf-gdb misbehaves
 *     for core-file consumption);
 *   - RISC-V chips all use @c riscv32-esp-elf-gdb.
 *
 * Returns NULL for unknown chips.  The caller is expected to find
 * the binary in PATH (no absolute-path resolution here).
 */
const char *core_chip_gdb_prog(uint32_t chip_ver);

/*
 * Parse the wire header of a raw core image.  The data starts at
 * @p buf; @p len is the total length of the image including header
 * and checksum.
 *
 * On success returns 0 and fills @p out.  On failure returns -1 and
 * sets @p *err to a static error string.  Failure modes: buffer too
 * short, unknown dump_ver, tot_len doesn't match @p len, header
 * larger than tot_len.
 */
int core_header_parse(const void *buf, size_t len, struct core_header *out,
		      const char **err);

/*
 * Verify the trailing checksum of a core image.
 *
 * @p buf and @p len are the same bytes that were handed to
 * @c core_header_parse; @p h is the resulting parsed header.  The
 * function computes a CRC-32 (when @c h->checksum_size is 4) or
 * SHA-256 (when 32) over @c buf[0..len - checksum_size] and compares
 * it against @c buf[len - checksum_size..len].
 *
 * The checksum coverage rule above is an invariant of the wire
 * format and matches upstream @c esp_coredump for V2 / V2_1 / V2_2
 * dumps (the three variants seen on real hardware).  V1's upstream
 * validator has a known padding quirk; we don't replicate it -- a
 * V1 image that fails here is by definition not a valid V2-style
 * blob, and the caller can pass @c --no-verify to inspect anyway.
 *
 * Returns 0 on match, -1 with @p err set on mismatch or if the image
 * is too short for the declared checksum.
 */
int core_validate(const void *buf, size_t len, const struct core_header *h,
		  const char **err);

#endif /* CMD_IDF_COREDUMP_LOADER_H */
