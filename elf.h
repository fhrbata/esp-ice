/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf.h
 * @brief ELF object file section and segment readers.
 *
 * Reads section headers (linker view) and program headers (loader
 * view) from ELF files in memory.  Both 32-bit and 64-bit ELF are
 * supported, in either byte order (little-endian or big-endian).
 *
 * Section vs. segment:
 *   - Sections describe the file as the linker sees it: named regions
 *     like .text/.data/.rodata with types and flags.  Use
 *     elf_read_sections() for linker-style work (ldgen, size, etc.).
 *   - Segments (program headers) describe the file as the loader sees
 *     it: ranges of virtual address + file offset + size + flags that
 *     get mapped into memory.  Use elf_read_segments() for image
 *     construction (elf2image) or anything that walks PT_LOAD entries.
 *
 * Both readers share the same buffer-input convention: pointers into
 * the result point into the caller's buffer (not copies), so the
 * caller must keep the buffer alive while using the returned data.
 *
 * --- Sections ---
 *
 * The parser extracts section name, type, flags, and size for each
 * section header.  The SHT_NULL entry at index 0 is skipped.
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
 *
 * --- Segments ---
 *
 * The parser extracts the program header table as-is (every entry,
 * including PT_NULL and non-PT_LOAD entries); filtering is left to
 * the caller.  The Ehdr's e_entry field is returned alongside, since
 * most consumers of program headers also need it for the image
 * header.
 *
 * Usage:
 *   struct elf_segments segs;
 *   elf_read_segments(buf, len, &segs);
 *   for (int i = 0; i < segs.nr; i++)
 *       if (segs.s[i].type == PT_LOAD && segs.s[i].filesz > 0)
 *           ... place segs.s[i] at its vaddr ...
 *   elf_segments_release(&segs);
 *
 * Common program header type values (p_type):
 *   0  PT_NULL      unused
 *   1  PT_LOAD      loadable segment (what image builders care about)
 *   2  PT_DYNAMIC   dynamic linking info (irrelevant for ESP images)
 *   4  PT_NOTE      auxiliary info
 *
 * Common program header flag bits (p_flags):
 *   0x1  PF_X  executable
 *   0x2  PF_W  writable
 *   0x4  PF_R  readable
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
	const char *name; /**< Section name (points into buffer). */
	uint32_t type;	  /**< Section type (SHT_*). */
	uint64_t flags;	  /**< Section flags (SHF_*). */
	uint64_t addr;	  /**< Virtual address at runtime (sh_addr). */
	uint64_t offset;  /**< Byte offset of section data in the file. */
	uint64_t size;	  /**< Section size in bytes. */
};

/**
 * Array of parsed ELF section headers.
 *
 * Allocated by elf_read_sections(); freed by elf_sections_release().
 */
struct elf_sections {
	struct elf_section *s; /**< Array of section headers. */
	int nr;		       /**< Number of entries. */
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
void elf_read_sections(const void *buf, size_t len, struct elf_sections *out);

/**
 * @brief Free memory allocated by elf_read_sections().
 *
 * After this call, out->s is NULL and out->nr is 0.
 */
void elf_sections_release(struct elf_sections *out);

/**
 * Parsed information from a single ELF program header.
 *
 * All fields are widened to 64 bits so the same struct carries both
 * ELF32 and ELF64 results without truncation.  For ELF32 inputs the
 * upper 32 bits are always zero.
 */
struct elf_segment {
	uint32_t type;	 /**< Segment type (PT_*). */
	uint64_t offset; /**< Byte offset of segment data in the file. */
	uint64_t vaddr;	 /**< Virtual address at runtime. */
	uint64_t paddr;	 /**< Physical / load address. */
	uint64_t filesz; /**< Size of segment data in the file. */
	uint64_t memsz;	 /**< Size of segment at runtime (>= filesz). */
	uint32_t flags;	 /**< Permission flags (PF_*). */
};

/**
 * Array of parsed ELF program headers plus the Ehdr's entry point.
 *
 * Allocated by elf_read_segments(); freed by elf_segments_release().
 * All entries are preserved (no filtering by type); callers interested
 * only in PT_LOAD should filter themselves.
 */
struct elf_segments {
	struct elf_segment *s; /**< Array of program headers. */
	int nr;		       /**< Number of entries. */
	uint64_t entry;	       /**< Entry point (Ehdr e_entry). */
};

/**
 * @brief Read program headers from an ELF executable in memory.
 *
 * Parses the ELF header, locates the program header table, and
 * populates @p out with one entry per program header.  The Ehdr's
 * entry point is stored in out->entry for convenience, since image
 * builders typically need it alongside the segment list.
 *
 * Dies on malformed ELF data.  A missing program header table
 * (phnum == 0) is not an error: out->nr is set to 0 and out->entry
 * is still populated.
 *
 * @param buf  ELF file contents (caller retains ownership)
 * @param len  length of @p buf
 * @param out  output structure (caller provides; filled in)
 */
void elf_read_segments(const void *buf, size_t len, struct elf_segments *out);

/**
 * @brief Free memory allocated by elf_read_segments().
 *
 * After this call, out->s is NULL, out->nr is 0, and out->entry is 0.
 */
void elf_segments_release(struct elf_segments *out);

/* ELF symbol type values (st_info & 0xf). */
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STT_COMMON 5
#define STT_TLS 6

/* ELF symbol bind values (st_info >> 4). */
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2

/* Special section indices (st_shndx). */
#define SHN_UNDEF 0
#define SHN_ABS 0xFFF1
#define SHN_COMMON 0xFFF2

/* Section types relevant to symbol tables. */
#define SHT_SYMTAB 2
#define SHT_PROGBITS 1
#define SHT_NOBITS 8

/* Section flags. */
#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4

/**
 * Parsed information from an ELF symbol-table entry.
 *
 * @p name points into the string table associated with the symbol's
 * SHT_SYMTAB section (not a copy).  The caller must keep the input
 * buffer alive while using the returned data.
 */
struct elf_symbol {
	const char *name; /**< Symbol name (points into buffer). */
	uint64_t value;	  /**< st_value (load address for defined syms). */
	uint64_t size;	  /**< st_size in bytes. */
	uint16_t shndx;	  /**< st_shndx (section index, or SHN_*). */
	uint8_t type;	  /**< STT_* (low nibble of st_info). */
	uint8_t bind;	  /**< STB_* (high nibble of st_info). */
};

/**
 * Array of parsed ELF symbols.
 *
 * Allocated by elf_read_symbols(); freed by elf_symbols_release().
 */
struct elf_symbols {
	struct elf_symbol *s; /**< Array of symbol entries. */
	int nr;		      /**< Number of entries (excludes the SHN_UNDEF
			       * sentinel at symtab index 0). */
};

/**
 * @brief Read symbols from the (single) SHT_SYMTAB section of an ELF
 *        executable in memory.
 *
 * Object files / executables produced by GCC contain a single static
 * symbol table -- the dynamic symbol table SHT_DYNSYM is not used in
 * the workflows ice serves.  This reader locates that one SHT_SYMTAB
 * and decodes every entry except the index-0 sentinel.
 *
 * Dies on malformed ELF data.  If no SHT_SYMTAB section is present,
 * out->nr is set to 0 and out->s to NULL (not an error).
 *
 * @param buf  ELF file contents (caller retains ownership)
 * @param len  length of @p buf
 * @param out  output structure (caller provides; filled in)
 */
void elf_read_symbols(const void *buf, size_t len, struct elf_symbols *out);

/** Free memory allocated by elf_read_symbols(). */
void elf_symbols_release(struct elf_symbols *out);

#endif /* ELF_H */
