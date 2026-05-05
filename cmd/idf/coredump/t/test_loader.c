/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for cmd/idf/coredump/loader.c.
 *
 * Synthesise a wire header for each version variant (V1, V2, V2_1,
 * V2_2) plus the V2 / V2_2 SHA256 sub-variants, parse, and assert
 * that fields present-or-absent match the version's documented
 * layout.  Also covers the failure modes: short buffer, unknown
 * dump_ver, tot_len mismatch.
 */
#include "cmd/idf/coredump/loader.h"
#include "ice.h"
#include "tap.h"
#include "vendor/sha256/sha256.h"

#include <stdint.h>
#include <string.h>
#include <zlib.h>

static void wr_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* Pack a version word: high 16 bits = chip_ver, low 16 bits = dump_ver. */
static uint32_t mkver(uint32_t chip_ver, uint32_t dump_ver)
{
	return ((chip_ver & 0xffffu) << 16) | (dump_ver & 0xffffu);
}

int main(void)
{
	/* ---- core_dump_ver_known ---- */
	{
		tap_check(core_dump_ver_known(CORE_DUMP_BIN_V1));
		tap_check(core_dump_ver_known(CORE_DUMP_BIN_V2));
		tap_check(core_dump_ver_known(CORE_DUMP_BIN_V2_1));
		tap_check(core_dump_ver_known(CORE_DUMP_ELF_CRC32_V2));
		tap_check(core_dump_ver_known(CORE_DUMP_ELF_SHA256_V2));
		tap_check(core_dump_ver_known(CORE_DUMP_ELF_CRC32_V2_1));
		tap_check(core_dump_ver_known(CORE_DUMP_ELF_SHA256_V2_1));
		tap_check(core_dump_ver_known(CORE_DUMP_ELF_SHA256_V2_2));
		tap_check(!core_dump_ver_known(0xdead));
		tap_done("known dump_vers are recognised; unknown is rejected");
	}

	/* ---- core_dump_ver_is_elf ---- */
	{
		tap_check(!core_dump_ver_is_elf(CORE_DUMP_BIN_V1));
		tap_check(!core_dump_ver_is_elf(CORE_DUMP_BIN_V2));
		tap_check(!core_dump_ver_is_elf(CORE_DUMP_BIN_V2_1));
		tap_check(core_dump_ver_is_elf(CORE_DUMP_ELF_CRC32_V2));
		tap_check(core_dump_ver_is_elf(CORE_DUMP_ELF_SHA256_V2));
		tap_check(core_dump_ver_is_elf(CORE_DUMP_ELF_CRC32_V2_1));
		tap_check(core_dump_ver_is_elf(CORE_DUMP_ELF_SHA256_V2_1));
		tap_check(core_dump_ver_is_elf(CORE_DUMP_ELF_SHA256_V2_2));
		tap_done("major>=1 means ELF-shaped");
	}

	/* ---- core_dump_ver_name ---- */
	{
		tap_check(strcmp(core_dump_ver_name(CORE_DUMP_BIN_V1),
				 "BIN_V1") == 0);
		tap_check(strcmp(core_dump_ver_name(CORE_DUMP_ELF_SHA256_V2_2),
				 "ELF_SHA256_V2_2") == 0);
		tap_check(core_dump_ver_name(0xdead) == NULL);
		tap_done("dump_ver names");
	}

	/* ---- core_chip_idf_name ---- */
	{
		tap_check(strcmp(core_chip_idf_name(0), "esp32") == 0);
		tap_check(strcmp(core_chip_idf_name(5), "esp32c3") == 0);
		tap_check(strcmp(core_chip_idf_name(13), "esp32c6") == 0);
		tap_check(strcmp(core_chip_idf_name(32), "esp32s31") == 0);
		tap_check(core_chip_idf_name(99) == NULL);
		tap_done("chip names");
	}

	/* ---- core_chip_gdb_prog: Xtensa/RISC-V buckets ---- */
	{
		/* Xtensa: esp32, esp32s2, esp32s3 -- all share the
		 * esp32 gdb per upstream's get_gdb_path. */
		tap_check(
		    strcmp(core_chip_gdb_prog(0), "xtensa-esp32-elf-gdb") == 0);
		tap_check(
		    strcmp(core_chip_gdb_prog(2), "xtensa-esp32-elf-gdb") == 0);
		tap_check(
		    strcmp(core_chip_gdb_prog(9), "xtensa-esp32-elf-gdb") == 0);
		/* RISC-V: every other supported chip. */
		tap_check(
		    strcmp(core_chip_gdb_prog(5), "riscv32-esp-elf-gdb") == 0);
		tap_check(
		    strcmp(core_chip_gdb_prog(13), "riscv32-esp-elf-gdb") == 0);
		tap_check(
		    strcmp(core_chip_gdb_prog(32), "riscv32-esp-elf-gdb") == 0);
		tap_check(core_chip_gdb_prog(99) == NULL);
		tap_done("chip -> gdb-prog: Xtensa share esp32-gdb, RISC-V "
			 "share esp-gdb");
	}

	/* ---- V1 (4-word) header parse ---- */
	{
		uint8_t buf[16 + 8 + 4]; /* 16 header + 8 data + 4 crc */
		struct core_header h;
		const char *err = NULL;

		memset(buf, 0xaa, sizeof(buf));
		wr_le32(buf + 0, sizeof(buf));		      /* tot_len */
		wr_le32(buf + 4, mkver(0, CORE_DUMP_BIN_V1)); /* chip=esp32 */
		wr_le32(buf + 8, 7);			      /* task_num */
		wr_le32(buf + 12, 196);			      /* tcbsz */

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == 0);
		tap_check(CORE_DUMP_VER(&h) == CORE_DUMP_BIN_V1);
		tap_check(CORE_CHIP_VER(&h) == 0);
		tap_check(h.tot_len == sizeof(buf));
		tap_check(h.task_num == 7);
		tap_check(h.tcbsz == 196);
		tap_check(h.segs_num == CORE_FIELD_ABSENT);
		tap_check(h.chip_rev == CORE_FIELD_ABSENT);
		tap_check(h.header_size == 16);
		tap_check(h.checksum_size == 4);
		tap_done("V1 (4-word) header parses; segs_num/chip_rev absent");
	}

	/* ---- V2 (5-word) header parse, esp32c3 ---- */
	{
		uint8_t buf[20 + 8 + 4];
		struct core_header h;
		const char *err = NULL;

		memset(buf, 0xaa, sizeof(buf));
		wr_le32(buf + 0, sizeof(buf));
		wr_le32(buf + 4, mkver(5, CORE_DUMP_BIN_V2));
		wr_le32(buf + 8, 4);
		wr_le32(buf + 12, 196);
		wr_le32(buf + 16, 3); /* segs_num */

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == 0);
		tap_check(CORE_CHIP_VER(&h) == 5);
		tap_check(h.task_num == 4);
		tap_check(h.tcbsz == 196);
		tap_check(h.segs_num == 3);
		tap_check(h.chip_rev == CORE_FIELD_ABSENT);
		tap_check(h.header_size == 20);
		tap_check(h.checksum_size == 4);
		tap_done("V2 (5-word) header parses; chip_rev absent");
	}

	/* ---- V2_1 (6-word) header parse with chip_rev ---- */
	{
		uint8_t buf[24 + 8 + 4];
		struct core_header h;
		const char *err = NULL;

		memset(buf, 0xaa, sizeof(buf));
		wr_le32(buf + 0, sizeof(buf));
		wr_le32(buf + 4, mkver(9, CORE_DUMP_BIN_V2_1));
		wr_le32(buf + 8, 4);
		wr_le32(buf + 12, 196);
		wr_le32(buf + 16, 3);
		wr_le32(buf + 20, 0x103); /* chip_rev */

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == 0);
		tap_check(CORE_CHIP_VER(&h) == 9);
		tap_check(h.chip_rev == 0x103);
		tap_check(h.header_size == 24);
		tap_done("V2_1 (6-word) header carries chip_rev");
	}

	/* ---- V2_2 (3-word, ELF_SHA256_V2_2) header parse ---- */
	{
		uint8_t buf[12 + 4 + 32]; /* 12 header + 4 data + 32 sha256 */
		struct core_header h;
		const char *err = NULL;

		memset(buf, 0xaa, sizeof(buf));
		wr_le32(buf + 0, sizeof(buf));
		wr_le32(buf + 4, mkver(13, CORE_DUMP_ELF_SHA256_V2_2));
		wr_le32(buf + 8, 0x200); /* chip_rev */

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == 0);
		tap_check(CORE_CHIP_VER(&h) == 13);
		tap_check(h.task_num == CORE_FIELD_ABSENT);
		tap_check(h.tcbsz == CORE_FIELD_ABSENT);
		tap_check(h.segs_num == CORE_FIELD_ABSENT);
		tap_check(h.chip_rev == 0x200);
		tap_check(h.header_size == 12);
		tap_check(h.checksum_size == 32);
		tap_done("V2_2 (3-word) header parses; "
			 "task/tcb/segs absent, SHA256 checksum");
	}

	/* ---- ELF_SHA256_V2 picks SHA256 checksum size on a V2 header ---- */
	{
		uint8_t buf[20 + 8 + 32];
		struct core_header h;
		const char *err = NULL;

		memset(buf, 0xaa, sizeof(buf));
		wr_le32(buf + 0, sizeof(buf));
		wr_le32(buf + 4, mkver(0, CORE_DUMP_ELF_SHA256_V2));
		wr_le32(buf + 8, 1);
		wr_le32(buf + 12, 196);
		wr_le32(buf + 16, 0);

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == 0);
		tap_check(h.checksum_size == 32);
		tap_check(h.header_size == 20);
		tap_done(
		    "ELF_SHA256_V2 picks SHA256 checksum on V2 header shape");
	}

	/* ---- failure: buffer too short ---- */
	{
		uint8_t buf[4] = {0};
		struct core_header h;
		const char *err = NULL;

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == -1);
		tap_check(err != NULL);
		tap_done("buffer < 8 bytes is rejected");
	}

	/* ---- failure: unknown dump_ver ---- */
	{
		uint8_t buf[20] = {0};
		struct core_header h;
		const char *err = NULL;

		wr_le32(buf + 0, sizeof(buf));
		wr_le32(buf + 4, mkver(0, 0xbeef)); /* not in the table */

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == -1);
		tap_check(err != NULL && strstr(err, "unknown") != NULL);
		tap_done("unknown dump_ver is rejected");
	}

	/* ---- failure: tot_len doesn't match buffer length ---- */
	{
		uint8_t buf[24] = {0}; /* on-disk size 24 */
		struct core_header h;
		const char *err = NULL;

		wr_le32(buf + 0, 999); /* but header claims 999 */
		wr_le32(buf + 4, mkver(0, CORE_DUMP_BIN_V1));
		wr_le32(buf + 8, 0);
		wr_le32(buf + 12, 0);

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == -1);
		tap_check(err != NULL);
		tap_done("tot_len mismatch is rejected");
	}

	/* ------------------------------------------------------------ */
	/* core_validate                                                 */
	/* ------------------------------------------------------------ */

	/* Valid V2 image with a correct CRC32 over [header || data]. */
	{
		uint8_t buf[20 + 8 + 4];
		struct core_header h;
		const char *err = NULL;
		uint32_t crc;

		memset(buf, 0xaa, sizeof(buf));
		wr_le32(buf + 0, sizeof(buf));
		wr_le32(buf + 4, mkver(0, CORE_DUMP_BIN_V2));
		wr_le32(buf + 8, 1);
		wr_le32(buf + 12, 196);
		wr_le32(buf + 16, 0);
		/* data section is the trailing 0xaa bytes from memset. */
		crc = (uint32_t)crc32(0, (const Bytef *)buf, 20 + 8);
		wr_le32(buf + 28, crc);

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == 0);
		tap_check(core_validate(buf, sizeof(buf), &h, &err) == 0);
		tap_done("V2 image with valid CRC32 verifies");
	}

	/* Tamper one byte in the data section -> CRC mismatch. */
	{
		uint8_t buf[20 + 8 + 4];
		struct core_header h;
		const char *err = NULL;
		uint32_t crc;

		memset(buf, 0xaa, sizeof(buf));
		wr_le32(buf + 0, sizeof(buf));
		wr_le32(buf + 4, mkver(0, CORE_DUMP_BIN_V2));
		wr_le32(buf + 8, 1);
		wr_le32(buf + 12, 196);
		wr_le32(buf + 16, 0);
		crc = (uint32_t)crc32(0, (const Bytef *)buf, 20 + 8);
		wr_le32(buf + 28, crc);

		buf[24] ^= 0xff; /* flip a byte in the data area */

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == 0);
		tap_check(core_validate(buf, sizeof(buf), &h, &err) == -1);
		tap_check(err != NULL && strstr(err, "CRC32") != NULL);
		tap_done("CRC32 mismatch is reported when data is tampered");
	}

	/* Valid V2_2 image with a correct SHA256. */
	{
		uint8_t buf[12 + 16 + 32];
		struct core_header h;
		const char *err = NULL;
		SHA256_CTX ctx;

		memset(buf, 0x55, sizeof(buf));
		wr_le32(buf + 0, sizeof(buf));
		wr_le32(buf + 4, mkver(13, CORE_DUMP_ELF_SHA256_V2_2));
		wr_le32(buf + 8, 0x100);
		sha256_init(&ctx);
		sha256_update(&ctx, buf, 12 + 16);
		sha256_final(&ctx, buf + 12 + 16);

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == 0);
		tap_check(core_validate(buf, sizeof(buf), &h, &err) == 0);
		tap_done("V2_2 image with valid SHA256 verifies");
	}

	/* Tamper one byte -> SHA256 mismatch. */
	{
		uint8_t buf[12 + 16 + 32];
		struct core_header h;
		const char *err = NULL;
		SHA256_CTX ctx;

		memset(buf, 0x55, sizeof(buf));
		wr_le32(buf + 0, sizeof(buf));
		wr_le32(buf + 4, mkver(13, CORE_DUMP_ELF_SHA256_V2_2));
		wr_le32(buf + 8, 0x100);
		sha256_init(&ctx);
		sha256_update(&ctx, buf, 12 + 16);
		sha256_final(&ctx, buf + 12 + 16);

		buf[20] ^= 0xff;

		tap_check(core_header_parse(buf, sizeof(buf), &h, &err) == 0);
		tap_check(core_validate(buf, sizeof(buf), &h, &err) == -1);
		tap_check(err != NULL && strstr(err, "SHA256") != NULL);
		tap_done("SHA256 mismatch is reported when data is tampered");
	}

	tap_result();
	return 0;
}
