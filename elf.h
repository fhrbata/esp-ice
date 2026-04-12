/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf.h
 * @brief ELF object file section reader.
 *
 * Reads section headers from ELF object files (.o) in memory.
 * Both 32-bit and 64-bit ELF are supported, in either byte order
 * (little-endian or big-endian).
 *
 * The parser extracts section name, type, flags, and size for each
 * section header.  The SHT_NULL entry at index 0 is skipped.
 *
 * Section name pointers reference the ELF section header string table
 * directly within the input buffer -- the caller must keep the buffer
 * alive while using the returned section data.
 *
 * Usage:
 *   struct elf_sections secs;
 *   elf_read_sections(buf, len, &secs);
 *   for (int i = 0; i < secs.nr; i++)
 *       printf("%s (type=%u, size=%llu)\n",
 *              secs.s[i].name, secs.s[i].type,
 *              (unsigned long long)secs.s[i].size);
 *   elf_sections_release(&secs);
 *
 * Common section type values (sh_type):
 *   0  SHT_NULL      inactive / placeholder
 *   1  SHT_PROGBITS  program data (.text, .data, .rodata, etc.)
 *   2  SHT_SYMTAB    symbol table
 *   3  SHT_STRTAB    string table
 *   8  SHT_NOBITS    uninitialized data (.bss)
 *
 * Common section flag bits (sh_flags):
 *   0x1  SHF_WRITE      writable at runtime
 *   0x2  SHF_ALLOC      occupies memory at runtime
 *   0x4  SHF_EXECINSTR  contains executable instructions
 */
#ifndef ELF_H
#define ELF_H

#include <stddef.h>
#include <stdint.h>

/**
 * Parsed information from a single ELF section header.
 *
 * @p name points into the input buffer's section header string table
 * (not a copy); the caller must keep the buffer alive while using it.
 */
struct elf_section {
	const char *name;	/**< Section name (points into buffer). */
	uint32_t type;		/**< Section type (SHT_*). */
	uint64_t flags;		/**< Section flags (SHF_*). */
	uint64_t size;		/**< Section size in bytes. */
};

/**
 * Array of parsed ELF section headers.
 *
 * Allocated by elf_read_sections(); freed by elf_sections_release().
 */
struct elf_sections {
	struct elf_section *s;	/**< Array of section headers. */
	int nr;			/**< Number of entries. */
};

/**
 * @brief Read section headers from an ELF object file in memory.
 *
 * Parses the ELF header, locates the section header table and string
 * table, and populates @p out with one entry per section (excluding
 * the SHT_NULL entry at index 0).
 *
 * Dies on malformed ELF data.
 *
 * @param buf  ELF file contents (caller retains ownership)
 * @param len  length of @p buf
 * @param out  output structure (caller provides; filled in)
 */
void elf_read_sections(const void *buf, size_t len,
		       struct elf_sections *out);

/**
 * @brief Free memory allocated by elf_read_sections().
 *
 * After this call, out->s is NULL and out->nr is 0.
 */
void elf_sections_release(struct elf_sections *out);

#endif /* ELF_H */
