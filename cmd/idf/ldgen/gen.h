/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gen.h
 * @brief Full-expansion linker-script generator.
 *
 * Takes parsed .lf fragment files and an entity database
 * (@ref sinfo_db), compiles rules, resolves every concrete
 * (archive, object, section) triple to a (target, flags) placement,
 * and emits per-target explicit listings.
 *
 * Design rationale: the Python ldgen emits rules (wildcards plus
 * EXCLUDE_FILE) and relies on the linker's first-match-wins to resolve
 * them.  The machinery for that (entity tree, basis chains,
 * significance, intermediate placements) accounts for most of its
 * 1700+ lines.  In C we can afford to enumerate the universe up-front
 * and emit facts -- explicit `*archive:object(section)` placements with
 * every section named exactly once -- eliminating the rule-resolution
 * machinery entirely.
 */
#ifndef LDGEN_GEN_H
#define LDGEN_GEN_H

#include <stdio.h>

#include "lf.h"
#include "sdkconfig.h"
#include "sinfo.h"

/* ---- Rules ------------------------------------------------------- */

enum gen_flag_kind {
	GF_KEEP,
	GF_ALIGN,
	GF_SORT,
	GF_SURROUND,
};

struct gen_flag {
	enum gen_flag_kind kind;
	int alignment;	   /**< ALIGN */
	int pre;	   /**< ALIGN, SURROUND: wrapper before group */
	int post;	   /**< ALIGN, SURROUND: wrapper after group */
	char *sort_first;  /**< SORT */
	char *sort_second; /**< SORT */
	char *symbol;	   /**< SURROUND */
};

/**
 * One compiled rule.  Rules are produced by cross-joining mapping
 * entries with scheme entries: each mapping entry binds an entity
 * pattern to a scheme, and each scheme entry binds a sections name to
 * a target, so the product yields N rules per mapping entry.
 *
 * Pattern fields are NULL for wildcards (so rule.matches("*") is
 * simply rule.archive == NULL).
 *
 * @p section_patterns stores the already-expanded section glob list
 * from the @c Sections fragment, e.g. `.text+` becomes `{.text,
 * .text.*}` (both strings appear here).
 */
struct gen_rule {
	char *archive; /**< NULL = "*" */
	char *object;  /**< NULL = "*" */
	char *symbol;  /**< NULL = object- or archive-level */
	char **section_patterns;
	int n_section_patterns;
	char *target;
	struct gen_flag *flags;
	int n_flags;
	int specificity;  /**< 0 wildcard, 1 arch, 2 obj, 3 sym */
	int source_order; /**< stable-sort tiebreak */
};

/* ---- Placements (the output of resolution) ----------------------- */

struct gen_placement {
	const char *archive; /**< borrowed from sinfo_db */
	const char *object;  /**< borrowed from sinfo_db */
	const char *section; /**< borrowed from sinfo_db */
	const char *target;  /**< borrowed from rule */
	int rule_idx;	     /**< index into gen_ctx.rules */
};

/* ---- Context ----------------------------------------------------- */

struct gen_ctx {
	struct gen_rule *rules;
	int n_rules;
	int alloc_rules;

	struct gen_placement *placements;
	int n_placements;
	int alloc_placements;

	/* intermediate state: sections/scheme fragments indexed by name */
	struct gen_sections *sections;
	int n_sections;
	int alloc_sections;

	struct gen_scheme *schemes;
	int n_schemes;
	int alloc_schemes;
};

/**
 * @brief Compile rules from a set of parsed fragment files.
 *
 * Conditional arms (`if`/`elif`/`else`) are evaluated against @p cfg;
 * pass NULL to skip all conditionals (useful only for self-contained
 * test fragments).
 */
void gen_compile(struct gen_ctx *ctx, struct lf_file **files, int n_files,
		 const struct sdkconfig *cfg);

/**
 * @brief Walk the entity DB, resolving every (archive, object, section)
 *        triple to the first matching rule.
 *
 * Sections not claimed by any rule are dropped (the linker's default
 * handling places them).
 */
void gen_resolve(struct gen_ctx *ctx, const struct sinfo_db *db);

/**
 * @brief Emit per-target explicit placements to @p out.
 *
 * Output is grouped by target, then by source-rule index within the
 * target, so flag wrappers (ALIGN/SURROUND pre/post) bracket the
 * entire run produced by a single rule.
 */
void gen_emit(FILE *out, const struct gen_ctx *ctx);

/**
 * @brief Emit placements for ONE target, indented by @p indent.
 *
 * Used by template fill: substitutes for a @c mapping[<target>] marker
 * in the linker-script template.  No @c [target] header -- the caller
 * has already positioned us inside the output section description.
 */
void gen_emit_target(FILE *out, const char *indent, const struct gen_ctx *ctx,
		     const char *target);

/**
 * @brief Fill a linker-script template.
 *
 * Reads @p template_path line by line.  Lines matching
 * <ws>@c mapping[<target>] are replaced by the placements for that
 * target, indented by <ws>.  All other lines pass through unchanged.
 *
 * A header comment is prepended so the output is identifiable.
 */
void gen_fill_template(FILE *out, const struct gen_ctx *ctx,
		       const char *template_path);

/**
 * @brief Emit a canonical (archive|object|section|target) dump.
 *
 * Sorted lexicographically.  This is the format used to diff against
 * the Python implementation's output.
 */
void gen_emit_canonical(FILE *out, const struct gen_ctx *ctx);

/** @brief Release all memory owned by @p ctx. */
void gen_free(struct gen_ctx *ctx);

#endif /* LDGEN_GEN_H */
