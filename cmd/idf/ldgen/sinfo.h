/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sinfo.h
 * @brief Entity database built from objdump-style sections.info files.
 *
 * ldgen consumes a "sections.info" file (the concatenated output of
 * `objdump -h` over every archive in the build) to learn which
 * sections exist in each object of each archive.  This is the same
 * input format ESP-IDF's Python ldgen uses, which lets us diff the
 * two implementations against identical inputs.
 *
 * The format looks like:
 *
 *   In archive libfoo.a:
 *
 *   a.o:     file format elf32-xtensa-le
 *
 *   Sections:
 *   Idx Name          Size     ...
 *     0 .text.f1      00000010 ...
 *                     CONTENTS, ALLOC, LOAD, RELOC, READONLY, CODE
 *     1 .text.f2      00000010 ...
 *                     ...
 *
 * Multiple archives may appear in sequence.
 */
#ifndef LDGEN_SINFO_H
#define LDGEN_SINFO_H

#include <stddef.h>

struct sinfo_object {
	char *name;	 /**< e.g. "croutine.c.obj", "a.o" */
	char **sections; /**< section names (heap-allocated each) */
	int n_sections;
	int alloc_sections;
};

struct sinfo_archive {
	char *name; /**< e.g. "libfreertos.a" (basename only) */
	struct sinfo_object *objs;
	int n_objs;
	int alloc_objs;
};

struct sinfo_db {
	struct sinfo_archive *archives;
	int n_archives;
	int alloc_archives;
};

/**
 * @brief Parse a sections.info file and add its contents to @p db.
 *
 * May be called multiple times to merge several sections.info files
 * into one database.
 *
 * Dies on parse errors.
 */
void sinfo_load(struct sinfo_db *db, const char *path);

/**
 * @brief Load an archive (.a) by walking members and reading their
 *        ELF section headers directly.
 *
 * Uses ar.c + elf.c to build the same (archive, object, sections)
 * mapping that sinfo_load() extracts from an objdump dump -- no
 * external tool required.
 *
 * Dies on I/O or parse errors.
 */
void sinfo_load_archive(struct sinfo_db *db, const char *path);

/** @brief Release all memory owned by @p db. */
void sinfo_free(struct sinfo_db *db);

#endif /* LDGEN_SINFO_H */
