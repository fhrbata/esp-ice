/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file memmap.h
 * @brief Hierarchical memory map for `ice idf size`.
 *
 * Mirrors the data model produced by `esp-idf-size`'s memorymap.get():
 * memory types -> sections -> archives -> object files -> symbols,
 * with diff fields populated by mm_diff().
 *
 * All names are owned (heap-duplicated on insertion) so that mm_unify()
 * and mm_diff() can introduce keys that don't trace back to the source
 * buffer.  Use mm_release() to free everything.
 *
 * Lookups are linear; counts at every level except symbols are tens at
 * most, and this tool is invoked once per build.
 */
#ifndef CMD_IDF_SIZE_MEMMAP_H
#define CMD_IDF_SIZE_MEMMAP_H

#include <stddef.h>
#include <stdint.h>

#include "elf.h"
#include "map.h"
#include "mem.h"

/* Forward declaration -- defined in size.c (CLI args). */
struct mm_args;

struct mm_symbol {
	char *name;	   /**< Owned. */
	char *abbrev_name; /**< Owned. */
	int64_t size;
	int64_t size_diff;
};

struct mm_object {
	char *name;
	char *abbrev_name;
	int64_t size;
	int64_t size_diff;
	struct mm_symbol **syms;
	int nr_syms, alloc_syms;
};

struct mm_archive {
	char *name;
	char *abbrev_name;
	int64_t size;
	int64_t size_diff;
	struct mm_object **objs;
	int nr_objs, alloc_objs;
};

struct mm_section {
	char *name;
	char *abbrev_name;
	int64_t size;
	int64_t size_diff;
	struct mm_archive **archives;
	int nr_archives, alloc_archives;
};

struct mm_type {
	char *name; /**< Display alias (e.g. "Flash Code"). */
	int64_t size;
	int64_t size_diff;
	int64_t used;
	int64_t used_diff;
	struct mm_section **sections;
	int nr_sections, alloc_sections;
};

struct memmap {
	char *target;
	char *target_diff;
	int64_t image_size;
	int64_t image_size_diff;
	char *project_path;
	char *project_path_diff;
	struct mm_type **types;
	int nr_types, alloc_types;
};

/**
 * @brief Build a memmap from a parsed linker map and (optional) ELF.
 *
 * @param out          memmap to populate (caller-owned, must be zeroed).
 * @param mf           parsed linker map file.
 * @param chip         chip info (ranges) -- pre-resolved by the caller.
 * @param target       chip name (e.g. "esp32s3"); used for memmap.target.
 * @param project_path absolute path used for memmap.project_path.
 * @param elf_secs     ELF section headers if available, else NULL.
 *                     Used to filter map sections by SHF_ALLOC.
 * @param elf_syms     ELF symbol table if available, else NULL.
 *                     Used to expand input sections into symbols and
 *                     to compute image_size more accurately.
 * @param load_symbols if 0, leaf "symbols" still get one synthetic
 *                     entry per input section (cheap path), without
 *                     using ELF symtab at all.  Matches upstream
 *                     load_symbols=False semantics.
 */
void mm_load(struct memmap *out, struct map_file *mf,
	     const struct chip_info *chip, const char *target,
	     const char *project_path, const struct elf_sections *elf_secs,
	     const struct elf_symbols *elf_syms, int load_symbols);

void mm_release(struct memmap *out);

/* Utility -- find a memory type by name (linear). */
struct mm_type *mm_find_type(const struct memmap *mm, const char *name);

/* ---- Mutating operations ------------------------------------------- */

/**
 * Merge @p ref into @p cur, populating *_diff = cur - ref throughout
 * the tree.  Items present only in @p ref are added with size = 0 and
 * size_diff < 0.  After this call, @p cur owns the merged tree and
 * @p ref is unchanged.
 */
void mm_diff(struct memmap *cur, const struct memmap *ref);

/**
 * Collapse leaf names by their abbreviated form.  After unify(),
 * sections with the same abbrev_name (e.g. ".dram0.bss" and ".dram1.bss"
 * both -> ".bss") are aggregated; archives, objects and symbols also
 * collapse to their basenames.
 */
void mm_unify(struct memmap *mm);

/** Drop memory types with used == 0 and sections with no archives. */
void mm_remove_unused(struct memmap *mm);

/** Zero the .size of every memory type whose name contains "flash". */
void mm_ignore_flash_size(struct memmap *mm);

/** Trim tree depth and remove unchanged entries (for diff). */
void mm_trim(struct memmap *mm, const struct mm_args *args);

/** Sort all levels by size (or size_diff) descending (or reverse). */
void mm_sort(struct memmap *mm, const struct mm_args *args);

/* ---- Helpers used by view builders --------------------------------- */

/** Section name -> abbreviated form (".iram0.text" -> ".text"). */
char *mm_abbrev_section(const char *name);

/** "<dir>/foo.a" -> "foo.a" (strdup). */
char *mm_basename(const char *path);

/** Find or create memory type. */
struct mm_type *mm_get_type(struct memmap *mm, const char *name);
struct mm_section *mm_type_get_section(struct mm_type *t, const char *name);
struct mm_archive *mm_section_get_archive(struct mm_section *s,
					  const char *name);
struct mm_object *mm_archive_get_object(struct mm_archive *a, const char *name);
struct mm_symbol *mm_object_get_symbol(struct mm_object *o, const char *name);

/**
 * Stable sort on a pointer array.  Insertion sort, O(n^2) but the
 * arrays we touch (memory types, sections, archives, ...) max out at
 * a few hundred entries, and stability is required so that tied
 * entries (e.g. zero-size cells) keep their memmap insertion order
 * -- matching Python's `sorted(..., reverse=...)`.
 *
 * The comparator follows qsort's contract: it gets pointers to the
 * array slots (i.e. void **), and returns negative / zero / positive.
 */
void mm_stable_sort(void **arr, int nr, int (*cmp)(const void *, const void *));

#endif /* CMD_IDF_SIZE_MEMMAP_H */
