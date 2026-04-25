/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf.c
 * @brief ELF object file section and segment reader implementation.
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
 *   ELF header     type, machine, section & program header table
 *                  locations, entry point
 *   ...
 *   Program headers  array of phnum entries at offset phoff
 *                    (used by elf_read_segments)
 *   Section headers  array of shnum entries at offset shoff
 *                    (used by elf_read_sections)
 *   Shstrtab         string table holding section names
 */
#include "ice.h"

/* ELF identification indices. */
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_NIDENT 16

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

/* Program header sizes. */
#define ELF32_PHDR_SIZE 32
#define ELF64_PHDR_SIZE 56

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
		return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | (uint32_t)p[3];
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

/*
 * Validate the ELF identification bytes and the minimum Ehdr size,
 * then fill in @p r so the caller can read the rest of the header.
 *
 * Dies on any inconsistency.  Shared between elf_read_sections and
 * elf_read_segments: every consumer of an ELF buffer needs the same
 * preamble, so extracting it here keeps the two readers short and
 * prevents the validation rules from drifting apart.
 */
static void elf_reader_init(struct elf_reader *r, const void *buf, size_t len)
{
	r->buf = buf;
	r->len = len;

	/* Validate ELF magic: 0x7f 'E' 'L' 'F'. */
	if (len < EI_NIDENT || r->buf[EI_MAG0] != 0x7f ||
	    r->buf[EI_MAG1] != 'E' || r->buf[EI_MAG2] != 'L' ||
	    r->buf[EI_MAG3] != 'F')
		die("not an ELF file (bad magic)");

	r->ei_class = r->buf[EI_CLASS];
	r->ei_data = r->buf[EI_DATA];

	if (r->ei_class != ELFCLASS32 && r->ei_class != ELFCLASS64)
		die("ELF: unsupported class %d", r->ei_class);
	if (r->ei_data != ELFDATA2LSB && r->ei_data != ELFDATA2MSB)
		die("ELF: unsupported data encoding %d", r->ei_data);

	if (r->ei_class == ELFCLASS32 && len < ELF32_EHDR_SIZE)
		die("ELF32 header truncated");
	if (r->ei_class == ELFCLASS64 && len < ELF64_EHDR_SIZE)
		die("ELF64 header truncated");
}

/* ------------------------------------------------------------------ */

void elf_read_sections(const void *buf, size_t len, struct elf_sections *out)
{
	struct elf_reader r;
	uint64_t shoff;
	uint32_t shnum, shstrndx, shentsize;
	uint64_t str_off, str_size;
	size_t str_hdr;
	const char *strtab;
	int alloc = 0;
	uint32_t i;

	out->s = NULL;
	out->nr = 0;

	elf_reader_init(&r, buf, len);

	/*
	 * Read section header table location from the ELF header.
	 *
	 * ELF32: e_shoff at offset 32 (4 bytes), e_shentsize/shnum/shstrndx
	 *        at offsets 46/48/50 (2 bytes each).
	 * ELF64: e_shoff at offset 40 (8 bytes), e_shentsize/shnum/shstrndx
	 *        at offsets 58/60/62 (2 bytes each).
	 */
	if (r.ei_class == ELFCLASS32) {
		shoff = rd32(&r, 32);
		shentsize = rd16(&r, 46);
		shnum = rd16(&r, 48);
		shstrndx = rd16(&r, 50);
	} else {
		shoff = rd64(&r, 40);
		shentsize = rd16(&r, 58);
		shnum = rd16(&r, 60);
		shstrndx = rd16(&r, 62);
	}

	if (shnum == 0)
		return;

	if (shoff + (uint64_t)shnum * shentsize > len)
		die("ELF: section header table extends past end of file");

	if (shstrndx >= shnum)
		die("ELF: section string table index %u "
		    "out of range (shnum=%u)",
		    shstrndx, shnum);

	/*
	 * Locate the section header string table (.shstrtab).
	 * Its offset and size are in the section header at index shstrndx.
	 *
	 * ELF32 section header: sh_offset at +16 (4 bytes), sh_size at +20.
	 * ELF64 section header: sh_offset at +24 (8 bytes), sh_size at +32.
	 */
	str_hdr = (size_t)(shoff + (uint64_t)shstrndx * shentsize);
	if (r.ei_class == ELFCLASS32) {
		str_off = rd32(&r, str_hdr + 16);
		str_size = rd32(&r, str_hdr + 20);
	} else {
		str_off = rd64(&r, str_hdr + 24);
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
		uint64_t sh_flags, sh_addr, sh_offset, sh_size;

		if (r.ei_class == ELFCLASS32) {
			sh_flags = rd32(&r, hoff + 8);
			sh_addr = rd32(&r, hoff + 12);
			sh_offset = rd32(&r, hoff + 16);
			sh_size = rd32(&r, hoff + 20);
		} else {
			sh_flags = rd64(&r, hoff + 8);
			sh_addr = rd64(&r, hoff + 16);
			sh_offset = rd64(&r, hoff + 24);
			sh_size = rd64(&r, hoff + 32);
		}

		ALLOC_GROW(out->s, out->nr + 1, alloc);
		out->s[out->nr].name =
		    (sh_name < str_size) ? strtab + sh_name : "";
		out->s[out->nr].type = sh_type;
		out->s[out->nr].flags = sh_flags;
		out->s[out->nr].addr = sh_addr;
		out->s[out->nr].offset = sh_offset;
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

/* ------------------------------------------------------------------ */

void elf_read_segments(const void *buf, size_t len, struct elf_segments *out)
{
	struct elf_reader r;
	uint64_t phoff;
	uint32_t phnum, phentsize;
	int alloc = 0;
	uint32_t i;

	out->s = NULL;
	out->nr = 0;
	out->entry = 0;

	elf_reader_init(&r, buf, len);

	/*
	 * Read the entry point and the program header table location
	 * from the ELF header.
	 *
	 * ELF32: e_entry at offset 24 (4 bytes), e_phoff at 28 (4 bytes),
	 *        e_phentsize at 42, e_phnum at 44 (both 2 bytes).
	 * ELF64: e_entry at offset 24 (8 bytes), e_phoff at 32 (8 bytes),
	 *        e_phentsize at 54, e_phnum at 56 (both 2 bytes).
	 */
	if (r.ei_class == ELFCLASS32) {
		out->entry = rd32(&r, 24);
		phoff = rd32(&r, 28);
		phentsize = rd16(&r, 42);
		phnum = rd16(&r, 44);
	} else {
		out->entry = rd64(&r, 24);
		phoff = rd64(&r, 32);
		phentsize = rd16(&r, 54);
		phnum = rd16(&r, 56);
	}

	if (phnum == 0)
		return;

	if (phentsize <
	    (r.ei_class == ELFCLASS32 ? ELF32_PHDR_SIZE : ELF64_PHDR_SIZE))
		die("ELF: program header entry size %u too small", phentsize);

	if (phoff + (uint64_t)phnum * phentsize > len)
		die("ELF: program header table extends past end of file");

	/*
	 * Parse each program header.
	 *
	 * ELF32 Phdr layout (32 bytes):
	 *   p_type(4) p_offset(4) p_vaddr(4) p_paddr(4)
	 *   p_filesz(4) p_memsz(4) p_flags(4) p_align(4)
	 *
	 * ELF64 Phdr layout (56 bytes):
	 *   p_type(4) p_flags(4) p_offset(8) p_vaddr(8) p_paddr(8)
	 *   p_filesz(8) p_memsz(8) p_align(8)
	 *
	 * Note that p_flags moves position between ELF32 and ELF64 --
	 * it's at the end in ELF32 but right after p_type in ELF64 so
	 * the struct stays naturally 8-byte aligned.  Widths change too.
	 */
	for (i = 0; i < phnum; i++) {
		size_t hoff = (size_t)(phoff + (uint64_t)i * phentsize);
		uint32_t p_type = rd32(&r, hoff);
		uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz;
		uint32_t p_flags;

		if (r.ei_class == ELFCLASS32) {
			p_offset = rd32(&r, hoff + 4);
			p_vaddr = rd32(&r, hoff + 8);
			p_paddr = rd32(&r, hoff + 12);
			p_filesz = rd32(&r, hoff + 16);
			p_memsz = rd32(&r, hoff + 20);
			p_flags = rd32(&r, hoff + 24);
		} else {
			p_flags = rd32(&r, hoff + 4);
			p_offset = rd64(&r, hoff + 8);
			p_vaddr = rd64(&r, hoff + 16);
			p_paddr = rd64(&r, hoff + 24);
			p_filesz = rd64(&r, hoff + 32);
			p_memsz = rd64(&r, hoff + 40);
		}

		ALLOC_GROW(out->s, out->nr + 1, alloc);
		out->s[out->nr].type = p_type;
		out->s[out->nr].offset = p_offset;
		out->s[out->nr].vaddr = p_vaddr;
		out->s[out->nr].paddr = p_paddr;
		out->s[out->nr].filesz = p_filesz;
		out->s[out->nr].memsz = p_memsz;
		out->s[out->nr].flags = p_flags;
		out->nr++;
	}
}

void elf_segments_release(struct elf_segments *out)
{
	free(out->s);
	out->s = NULL;
	out->nr = 0;
	out->entry = 0;
}
