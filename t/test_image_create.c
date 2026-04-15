/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for the elf2image.c engine that backs `ice image create`
 * (and its `ice image elf2image` alias).
 *
 * We synthesise a minimal ELF32-LE ET_EXEC file in memory with a few
 * PT_LOAD segments at canonical ESP32 addresses, then exercise
 * e2i_build and verify the structure of the output.  These tests do
 * not require esptool or any IDF toolchain; the companion .t wrapper
 * runs an additional byte-diff against `esptool elf2image` when
 * esptool is available on PATH.
 */
#include "elf2image.h"
#include "ice.h"
#include "tap.h"

static void le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/*
 * Build an ELF32-LE ET_EXEC buffer with N PT_LOADs.  Each PT_LOAD is
 * given an ascending block of `0xAA + seg_index` fill bytes to make
 * the output easy to eyeball.  The ELF header, program header table,
 * and all segment data are laid out contiguously at fixed offsets.
 */
struct fake_seg {
	uint32_t vaddr;
	uint32_t filesz;
};

static void build_elf(struct sbuf *sb, uint32_t entry,
		      const struct fake_seg *segs, int n)
{
	const uint32_t ehdr_size = 52;
	const uint32_t phdr_size = 32;
	uint32_t phoff = ehdr_size;
	uint32_t data_off = phoff + (uint32_t)n * phdr_size;

	size_t total = data_off;
	for (int i = 0; i < n; i++)
		total += segs[i].filesz;

	sbuf_grow(sb, total);
	memset(sb->buf, 0, total);
	uint8_t *buf = (uint8_t *)sb->buf;

	/* Ehdr */
	buf[0] = 0x7F;
	buf[1] = 'E';
	buf[2] = 'L';
	buf[3] = 'F';
	buf[4] = 1;		     /* ELFCLASS32 */
	buf[5] = 1;		     /* ELFDATA2LSB */
	buf[6] = 1;		     /* EV_CURRENT */
	le16(buf + 16, 2);	     /* e_type = ET_EXEC */
	le16(buf + 18, 0x5E);	     /* e_machine = Xtensa (arbitrary) */
	le32(buf + 20, 1);	     /* e_version */
	le32(buf + 24, entry);	     /* e_entry */
	le32(buf + 28, phoff);	     /* e_phoff */
	le32(buf + 32, 0);	     /* e_shoff (none) */
	le32(buf + 36, 0);	     /* e_flags */
	le16(buf + 40, 52);	     /* e_ehsize */
	le16(buf + 42, 32);	     /* e_phentsize */
	le16(buf + 44, (uint16_t)n); /* e_phnum */
	le16(buf + 46, 40);	     /* e_shentsize */
	le16(buf + 48, 0);	     /* e_shnum */
	le16(buf + 50, 0);	     /* e_shstrndx */

	/* Phdrs */
	uint32_t off = data_off;
	for (int i = 0; i < n; i++) {
		uint8_t *ph = buf + phoff + (uint32_t)i * phdr_size;
		le32(ph + 0, 1);	       /* p_type = PT_LOAD */
		le32(ph + 4, off);	       /* p_offset */
		le32(ph + 8, segs[i].vaddr);   /* p_vaddr */
		le32(ph + 12, segs[i].vaddr);  /* p_paddr */
		le32(ph + 16, segs[i].filesz); /* p_filesz */
		le32(ph + 20, segs[i].filesz); /* p_memsz */
		le32(ph + 24, 5);	       /* p_flags = R+X */
		le32(ph + 28, 4);	       /* p_align */
		for (uint32_t j = 0; j < segs[i].filesz; j++)
			buf[off + j] = (uint8_t)(0xAA + i);
		off += segs[i].filesz;
	}

	sbuf_setlen(sb, total);
}

int main(void)
{
	struct sbuf elf = SBUF_INIT;
	struct sbuf img = SBUF_INIT;

	/* One DROM, one DRAM, one IROM on ESP32. */
	struct fake_seg segs[] = {
	    {0x3F400000u, 256}, /* DROM */
	    {0x3FFB0000u, 128}, /* DRAM (BYTE_ACCESSIBLE range on esp32) */
	    {0x400D0000u, 512}, /* IROM */
	};
	build_elf(&elf, 0x400D0080u, segs, 3);

	struct e2i_config cfg = E2I_CONFIG_DEFAULT();
	cfg.flash_mode = "dio";
	cfg.flash_freq = "40m";
	cfg.flash_size = "2MB";

	e2i_build(elf.buf, elf.len, BIN_CHIP_ESP32, &cfg, &img);

	/* Basic structural checks on the generated image. */
	tap_check(img.len > 24);
	tap_check((uint8_t)img.buf[0] == 0xE9);
	tap_done("image starts with esp_image magic 0xE9");

	uint8_t nsegs = (uint8_t)img.buf[1];
	tap_check(nsegs >= 3 && nsegs <= 16);
	tap_done("image declares between 3 and 16 segments "
		 "(pads may be inserted)");

	tap_check((uint8_t)img.buf[2] == 2); /* flash_mode dio */
	uint8_t size_freq = (uint8_t)img.buf[3];
	tap_check((size_freq & 0xF0) == 0x10); /* 2MB */
	tap_check((size_freq & 0x0F) == 0x00); /* 40m */
	tap_done("flash_mode/size/freq bytes encoded correctly");

	uint32_t entry = (uint32_t)(uint8_t)img.buf[4] |
			 (uint32_t)(uint8_t)img.buf[5] << 8 |
			 (uint32_t)(uint8_t)img.buf[6] << 16 |
			 (uint32_t)(uint8_t)img.buf[7] << 24;
	tap_check(entry == 0x400D0080u);
	tap_done("entry point matches the ELF's e_entry");

	/* Extended header: chip_id at bytes 8+4..8+5 */
	uint16_t chip_id = (uint16_t)(uint8_t)img.buf[12] |
			   (uint16_t)(uint8_t)img.buf[13] << 8;
	tap_check(chip_id == 0);
	tap_done("chip_id == 0 for ESP32");

	/* Append-digest flag is byte 15 of extended header
	 * (absolute file offset 8 + 15 = 23). */
	tap_check((uint8_t)img.buf[23] == 1);
	tap_done("append_digest flag set (default config)");

	/* Checksum byte is the last byte of a 16-aligned chunk, and
	 * the SHA-256 digest occupies the final 32 bytes. */
	tap_check((img.len - 32) % 16 == 0);
	tap_done("image length - digest is 16-byte aligned");

	/* Re-running with the same config should be deterministic. */
	struct sbuf img2 = SBUF_INIT;
	e2i_build(elf.buf, elf.len, BIN_CHIP_ESP32, &cfg, &img2);
	tap_check(img.len == img2.len);
	tap_check(memcmp(img.buf, img2.buf, img.len) == 0);
	tap_done("e2i_build is deterministic for a fixed ELF + config");

	sbuf_release(&img2);

	/* Flipping append_sha256 off drops 32 bytes. */
	struct sbuf img3 = SBUF_INIT;
	cfg.append_sha256 = false;
	e2i_build(elf.buf, elf.len, BIN_CHIP_ESP32, &cfg, &img3);
	tap_check(img3.len + 32 == img.len);
	tap_check((uint8_t)img3.buf[23] == 0); /* digest flag cleared */
	tap_done("append_sha256=false removes the final 32-byte digest");

	sbuf_release(&img3);

	/* Chip-by-name: spot-check a few. */
	tap_check(bin_chip_by_name("esp32") == BIN_CHIP_ESP32);
	tap_check(bin_chip_by_name("esp32c3") == BIN_CHIP_ESP32C3);
	tap_check(bin_chip_by_name("esp32p4") == BIN_CHIP_ESP32P4);
	tap_check(bin_chip_by_name("nope") == BIN_CHIP_MAX);
	tap_check(bin_chip_by_name(NULL) == BIN_CHIP_MAX);
	tap_done("bin_chip_by_name resolves known chips and rejects unknown");

	tap_check(strcmp(bin_chip_name(BIN_CHIP_ESP32C6), "esp32c6") == 0);
	tap_done("bin_chip_name round-trips");

	const char *const *names = bin_chip_names();
	int listed = 0;
	for (; *names != NULL; names++)
		listed++;
	tap_check(listed == BIN_CHIP_MAX);
	tap_done("bin_chip_names lists every supported chip");

	sbuf_release(&img);
	sbuf_release(&elf);
	return tap_result();
}
