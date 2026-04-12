/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf.c
 * @brief ELF object file section reader implementation.
 *
 * Parses ELF headers directly from a memory buffer without relying
 * on system <elf.h> or external tools like objdump.  Handles both
 * ELF32 and ELF64 formats in either byte order.
 *
 * Format reference: ELF specification (Tool Interface Standard),
 * https://refspecs.linuxfoundation.org/elf/elf.pdf
 *
 * ELF layout (relevant parts):
 *   e_ident[16]    identification (magic, class, data encoding)
 *   ELF header     type, machine, section header table location
 *   ...
 *   Section headers  array of shnum entries at offset shoff
 *   Shstrtab         string table holding section names
 */
#include "ice.h"

/* ELF identification indices. */
#define EI_MAG0    0
#define EI_MAG1    1
#define EI_MAG2    2
#define EI_MAG3    3
#define EI_CLASS   4
#define EI_DATA    5
#define EI_NIDENT  16

/* ELF class values. */
#define ELFCLASS32 1
#define ELFCLASS64 2

/* ELF data encoding values. */
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

/* Minimum ELF header sizes. */
#define ELF32_EHDR_SIZE 52
#define ELF64_EHDR_SIZE 64

/* Section header sizes. */
#define ELF32_SHDR_SIZE 40
#define ELF64_SHDR_SIZE 64

/**
 * Internal reader state -- tracks buffer, class, and byte order.
 */
struct elf_reader {
	const unsigned char *buf;
	size_t len;
	int ei_class;
	int ei_data;
};

/* ---- Endianness-aware read helpers -------------------------------- */

static uint16_t rd16(const struct elf_reader *r, size_t off)
{
	const unsigned char *p = r->buf + off;

	if (r->ei_data == ELFDATA2LSB)
		return (uint16_t)p[0] | (uint16_t)p[1] << 8;
	return (uint16_t)p[0] << 8 | (uint16_t)p[1];
}

static uint32_t rd32(const struct elf_reader *r, size_t off)
{
	const unsigned char *p = r->buf + off;

	if (r->ei_data == ELFDATA2LSB)
		return (uint32_t)p[0] |
		       (uint32_t)p[1] << 8 |
		       (uint32_t)p[2] << 16 |
		       (uint32_t)p[3] << 24;
	return (uint32_t)p[0] << 24 |
	       (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 |
	       (uint32_t)p[3];
}

static uint64_t rd64(const struct elf_reader *r, size_t off)
{
	uint32_t lo, hi;

	if (r->ei_data == ELFDATA2LSB) {
		lo = rd32(r, off);
		hi = rd32(r, off + 4);
	} else {
		hi = rd32(r, off);
		lo = rd32(r, off + 4);
	}
	return (uint64_t)hi << 32 | lo;
}

/* ------------------------------------------------------------------ */

void elf_read_sections(const void *buf, size_t len,
		       struct elf_sections *out)
{
	struct elf_reader r;
	uint64_t shoff;
	uint32_t shnum, shstrndx, shentsize;
	uint64_t str_off, str_size;
	size_t str_hdr;
	const char *strtab;
	int alloc = 0;
	uint32_t i;

	r.buf = buf;
	r.len = len;

	out->s = NULL;
	out->nr = 0;

	/* Validate ELF magic: 0x7f 'E' 'L' 'F'. */
	if (len < EI_NIDENT ||
	    r.buf[EI_MAG0] != 0x7f ||
	    r.buf[EI_MAG1] != 'E' ||
	    r.buf[EI_MAG2] != 'L' ||
	    r.buf[EI_MAG3] != 'F')
		die("not an ELF file (bad magic)");

	r.ei_class = r.buf[EI_CLASS];
	r.ei_data = r.buf[EI_DATA];

	if (r.ei_class != ELFCLASS32 && r.ei_class != ELFCLASS64)
		die("ELF: unsupported class %d", r.ei_class);
	if (r.ei_data != ELFDATA2LSB && r.ei_data != ELFDATA2MSB)
		die("ELF: unsupported data encoding %d", r.ei_data);

	/*
	 * Read section header table location from the ELF header.
	 *
	 * ELF32: e_shoff at offset 32 (4 bytes), e_shentsize/shnum/shstrndx
	 *        at offsets 46/48/50 (2 bytes each).
	 * ELF64: e_shoff at offset 40 (8 bytes), e_shentsize/shnum/shstrndx
	 *        at offsets 58/60/62 (2 bytes each).
	 */
	if (r.ei_class == ELFCLASS32) {
		if (len < ELF32_EHDR_SIZE)
			die("ELF32 header truncated");
		shoff      = rd32(&r, 32);
		shentsize  = rd16(&r, 46);
		shnum      = rd16(&r, 48);
		shstrndx   = rd16(&r, 50);
	} else {
		if (len < ELF64_EHDR_SIZE)
			die("ELF64 header truncated");
		shoff      = rd64(&r, 40);
		shentsize  = rd16(&r, 58);
		shnum      = rd16(&r, 60);
		shstrndx   = rd16(&r, 62);
	}

	if (shnum == 0)
		return;

	if (shoff + (uint64_t)shnum * shentsize > len)
		die("ELF: section header table extends past end of file");

	if (shstrndx >= shnum)
		die("ELF: section string table index %u "
		    "out of range (shnum=%u)", shstrndx, shnum);

	/*
	 * Locate the section header string table (.shstrtab).
	 * Its offset and size are in the section header at index shstrndx.
	 *
	 * ELF32 section header: sh_offset at +16 (4 bytes), sh_size at +20.
	 * ELF64 section header: sh_offset at +24 (8 bytes), sh_size at +32.
	 */
	str_hdr = (size_t)(shoff + (uint64_t)shstrndx * shentsize);
	if (r.ei_class == ELFCLASS32) {
		str_off  = rd32(&r, str_hdr + 16);
		str_size = rd32(&r, str_hdr + 20);
	} else {
		str_off  = rd64(&r, str_hdr + 24);
		str_size = rd64(&r, str_hdr + 32);
	}

	if (str_off + str_size > len)
		die("ELF: section string table truncated");

	strtab = (const char *)r.buf + str_off;

	/*
	 * Parse each section header (skip index 0 which is SHT_NULL).
	 *
	 * ELF32 section header layout (40 bytes):
	 *   sh_name(4) sh_type(4) sh_flags(4) sh_addr(4)
	 *   sh_offset(4) sh_size(4) sh_link(4) sh_info(4)
	 *   sh_addralign(4) sh_entsize(4)
	 *
	 * ELF64 section header layout (64 bytes):
	 *   sh_name(4) sh_type(4) sh_flags(8) sh_addr(8)
	 *   sh_offset(8) sh_size(8) sh_link(4) sh_info(4)
	 *   sh_addralign(8) sh_entsize(8)
	 */
	for (i = 1; i < shnum; i++) {
		size_t hoff = (size_t)(shoff + (uint64_t)i * shentsize);
		uint32_t sh_name = rd32(&r, hoff);
		uint32_t sh_type = rd32(&r, hoff + 4);
		uint64_t sh_flags, sh_size;

		if (r.ei_class == ELFCLASS32) {
			sh_flags = rd32(&r, hoff + 8);
			sh_size  = rd32(&r, hoff + 20);
		} else {
			sh_flags = rd64(&r, hoff + 8);
			sh_size  = rd64(&r, hoff + 32);
		}

		ALLOC_GROW(out->s, out->nr + 1, alloc);
		out->s[out->nr].name = (sh_name < str_size)
			? strtab + sh_name
			: "";
		out->s[out->nr].type = sh_type;
		out->s[out->nr].flags = sh_flags;
		out->s[out->nr].size = sh_size;
		out->nr++;
	}
}

void elf_sections_release(struct elf_sections *out)
{
	free(out->s);
	out->s = NULL;
	out->nr = 0;
}
