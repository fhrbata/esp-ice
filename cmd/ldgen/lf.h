/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lf.h
 * @brief Linker fragment (.lf) parser -- types and public API.
 *
 * Parses ESP-IDF linker fragment files into an AST that preserves the
 * full conditional structure.  The grammar is an LL(1) transcription of
 * the pyparsing definitions in tools/ldgen/ldgen/fragments.py.
 *
 * Three fragment types are supported:
 *
 *   [sections:name]  -- lists of input sections (.text+, .rodata+, ...)
 *   [scheme:name]    -- maps section lists to linker targets
 *   [mapping:name]   -- attaches schemes to object files / symbols
 *
 * Conditionals (if/elif/else evaluated against sdkconfig) can appear
 * both inside entry blocks and at the top level wrapping entire
 * fragments.  They are stored in the AST and evaluated in a later pass.
 *
 * See cmd/ldgen/README.md for the formal grammar.
 */
#ifndef LF_H
#define LF_H

/* ------------------------------------------------------------------ */
/*  Entry-level AST                                                   */
/* ------------------------------------------------------------------ */

/* ---- Flags (mapping entries only) ------------------------------- */

enum lf_flag_kind {
	LF_FLAG_KEEP,
	LF_FLAG_ALIGN,
	LF_FLAG_SORT,
	LF_FLAG_SURROUND,
};

/**
 * A single flag attached to a section->target override.
 *
 * KEEP()                        kind=KEEP
 * ALIGN(n [,pre] [,post])      kind=ALIGN, alignment, pre, post
 * SORT([key [,key]])            kind=SORT, sort_first, sort_second
 * SURROUND(sym)                 kind=SURROUND, symbol
 */
struct lf_flag {
	enum lf_flag_kind kind;
	int alignment;		/**< ALIGN: alignment value */
	int pre;		/**< ALIGN: align before (default true) */
	int post;		/**< ALIGN: align after (default false) */
	char *sort_first;	/**< SORT: first key (NULL = default) */
	char *sort_second;	/**< SORT: second key (NULL = none) */
	char *symbol;		/**< SURROUND: symbol name */
};

/**
 * One section->target override with attached flags.
 *
 * Produced by the ';' syntax on mapping entries:
 *   obj (scheme);
 *       sections -> target KEEP() SURROUND(sym),
 *       sections2 -> target2 ALIGN(4)
 */
struct lf_flag_item {
	char *sections;		/**< sections reference */
	char *target;		/**< target name */
	struct lf_flag *flags;
	int n_flags;
};

/* ---- Entries ---------------------------------------------------- */

/**
 * A single entry in an entries block.
 *
 * Which fields are populated depends on the containing fragment:
 *
 *   sections:   name only             (".text+", "COMMON")
 *   scheme:     name + target         ("text" -> "flash_text")
 *   mapping:    name + target + scheme ("obj":"sym" ("noflash"))
 *               name = "*" for wildcard; target = symbol or NULL
 *   archive:    name only             ("libfoo.a", "*")
 *
 * For mapping entries with flags (';' syntax), @p flag_items holds the
 * section->target overrides.
 *
 * All strings are owned (heap-allocated) and freed by lf_file_free().
 */
struct lf_entry {
	char *name;
	char *target;
	char *scheme;
	struct lf_flag_item *flag_items;  /**< mapping: flag overrides (NULL otherwise) */
	int n_flag_items;
};

/**
 * One arm of an if/elif/else conditional block.
 *
 * @p expr is the raw condition text (e.g. "CONFIG_FOO = y"), or NULL
 * for the else branch.  @p stmts is the body of the branch.
 */
struct lf_branch {
	char *expr;
	struct lf_stmt *stmts;
	int n_stmts;
};

/**
 * A statement inside an entries block: either a plain entry or a
 * conditional that contains more statements.
 *
 * When @p is_cond is false, use @c u.entry.
 * When @p is_cond is true,  use @c u.cond.
 */
struct lf_stmt {
	int is_cond;
	union {
		struct lf_entry entry;
		struct {
			struct lf_branch *branches;
			int n_branches;
		} cond;
	} u;
};

/* ------------------------------------------------------------------ */
/*  Fragment-level AST                                                */
/* ------------------------------------------------------------------ */

enum lf_frag_kind {
	LF_SECTIONS,
	LF_SCHEME,
	LF_MAPPING,
	LF_FRAG_COND,
};

/**
 * One arm of a fragment-level conditional.
 *
 * Same structure as lf_branch but the body holds fragments, not
 * entry-level statements.
 */
struct lf_frag_branch {
	char *expr;
	struct lf_frag *frags;
	int n_frags;
};

/**
 * A parsed fragment.
 *
 * Use @p kind to decide which union member to read:
 *
 *   LF_SECTIONS  -- u.sec:  section name list
 *   LF_SCHEME    -- u.sch:  sections-to-target mapping
 *   LF_MAPPING   -- u.map:  entity-to-scheme binding with archive
 *   LF_FRAG_COND -- u.cond: fragment-level conditional
 *
 * For LF_MAPPING, the @c archive field is a statement list (usually a
 * single entry, but may be a conditional that resolves to one value).
 */
struct lf_frag {
	enum lf_frag_kind kind;
	union {
		struct {
			char *name;
			struct lf_stmt *stmts;
			int n;
		} sec;
		struct {
			char *name;
			struct lf_stmt *stmts;
			int n;
		} sch;
		struct {
			char *name;
			struct lf_stmt *archive;
			int n_archive;
			struct lf_stmt *entries;
			int n_entries;
		} map;
		struct {
			struct lf_frag_branch *branches;
			int n;
		} cond;
	} u;
};

/** A parsed fragment file -- the top-level AST node. */
struct lf_file {
	char *path;
	struct lf_frag *frags;
	int n_frags;
};

/* ------------------------------------------------------------------ */
/*  API                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Parse a fragment file from a NUL-terminated buffer.
 *
 * Calls die() on syntax errors.
 *
 * @param src   file contents (not modified; caller keeps ownership)
 * @param path  file path (used in error messages only)
 * @return      heap-allocated AST; free with lf_file_free()
 */
struct lf_file *lf_parse(const char *src, const char *path);

/**
 * @brief Free all memory owned by a parsed file, including itself.
 */
void lf_file_free(struct lf_file *f);

/**
 * @brief Dump a parsed file to stdout for debugging.
 */
void lf_file_dump(const struct lf_file *f);

#endif /* LF_H */
