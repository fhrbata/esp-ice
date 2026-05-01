/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file map.h
 * @brief GCC/LD linker map file parser.
 *
 * Parses the text output of `ld --Map` (GCC linker map files) from a
 * mutable memory buffer.  Extracts memory regions from the "Memory
 * Configuration" section and output/input sections from the "Linker
 * script and memory map" section.
 *
 * The parser works in-place: it NUL-terminates lines and tokens directly
 * in the source buffer, and all string fields in the output structures
 * are pointers back into that buffer.  The caller must keep the buffer
 * alive while the parsed result is in use.
 *
 * Usage:
 *   struct sbuf sb = SBUF_INIT;
 *   sbuf_read_file(&sb, "project.map");
 *
 *   struct map_file mf;
 *   map_read(sb.buf, sb.len, &mf);
 *   for (int i = 0; i < mf.nr_sections; i++)
 *       printf("%s\n", mf.sections[i].name);
 *   map_release(&mf);
 *   sbuf_release(&sb);
 *
 * Map file sections parsed:
 *
 *   Memory Configuration        -- memory region definitions
 *   Linker script and memory map -- output/input section layout
 *
 * Format reference:
 *   https://sourceware.org/binutils/docs/ld/
 */
#ifndef MAP_H
#define MAP_H

#include <stddef.h>
#include <stdint.h>

/**
 * A memory region from the "Memory Configuration" section.
 *
 * String fields point into the source buffer passed to map_read().
 */
struct map_region {
	const char *name;  /**< Region name (e.g. "IRAM0_0_seg"). */
	uint64_t origin;   /**< Starting address. */
	uint64_t length;   /**< Size in bytes. */
	const char *attrs; /**< Attributes string (may be ""). */
};

/**
 * An input section within a linker output section.
 *
 * Represents a single object file's contribution to an output section.
 * String fields point into the source buffer passed to map_read().
 */
struct map_input {
	const char *name;    /**< Section name (e.g. ".text", "COMMON"). */
	uint64_t address;    /**< Load address. */
	uint64_t size;	     /**< Size in bytes (0 for overlapping sections). */
	const char *archive; /**< Archive path, or "(exe)" for direct links. */
	const char *object;  /**< Object file name. */
	uint64_t fill;	     /**< Padding/fill bytes following this section. */
};

/**
 * An output section from the "Linker script and memory map" section.
 *
 * Contains an array of input sections that compose this output section.
 * String fields point into the source buffer; the inputs array is
 * heap-allocated and freed by map_release().
 */
struct map_section {
	const char *name;	  /**< Section name (e.g. ".iram0.text"). */
	uint64_t address;	  /**< Starting address. */
	uint64_t size;		  /**< Total size in bytes. */
	struct map_input *inputs; /**< Array of input sections. */
	int nr_inputs;		  /**< Number of input sections. */
};

/**
 * One reference of a symbol from the Cross Reference Table.
 *
 * Strings point into the source buffer.
 */
struct map_xref_ref {
	const char *archive; /**< Archive path, or "(exe)". */
	const char *object;  /**< Object file name. */
};

/**
 * One entry in the Cross Reference Table.
 *
 * The first reference (refs[0]) is the symbol's defining location;
 * the remaining entries are referencing locations.  The original
 * linker output groups these under the symbol name in document order,
 * which the C side preserves.
 */
struct map_xref {
	const char *symbol;	   /**< Symbol name (points into buffer). */
	struct map_xref_ref *refs; /**< Heap array; refs[0] is the def. */
	int nr_refs;		   /**< Number of entries in refs. */
	int alloc_refs;		   /**< Capacity of refs. */
};

/**
 * Complete parsed linker map file.
 *
 * Populated by map_read(); freed by map_release().
 */
struct map_file {
	const char *target; /**< Chip target (e.g. "esp32s3"), or NULL. */
	struct map_region *regions;   /**< Array of memory regions. */
	int nr_regions;		      /**< Number of memory regions. */
	struct map_section *sections; /**< Array of output sections. */
	int nr_sections;	      /**< Number of output sections. */
	struct map_xref *xrefs;	      /**< Cross Reference Table entries. */
	int nr_xrefs; /**< Number of xref entries (0 if absent). */
};

/**
 * @brief Parse a GCC/LD linker map file from a mutable memory buffer.
 *
 * Reads the "Memory Configuration" and "Linker script and memory map"
 * sections from the map file and populates @p out.
 *
 * The buffer is modified in-place (newlines and whitespace replaced with
 * NUL bytes).  String fields in the output point into the buffer, so the
 * caller must keep it alive while using the result.
 *
 * Dies on malformed input.
 *
 * @param buf  map file contents (modified in-place; caller retains ownership)
 * @param len  length of @p buf
 * @param out  output structure (caller provides; filled in)
 */
void map_read(char *buf, size_t len, struct map_file *out);

/**
 * @brief Free arrays allocated by map_read().
 *
 * Frees the regions, sections, and input arrays.  Does not free the
 * source buffer (that is the caller's responsibility).
 *
 * After this call, all arrays are NULL and counts are 0.
 */
void map_release(struct map_file *out);

#endif /* MAP_H */
