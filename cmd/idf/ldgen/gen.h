/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gen.h
 * @brief Rule-driven linker-script generator.
 *
 * Pipeline:
 *
 *   gen_compile()  -- parse .lf fragments into rules.  One rule per
 *                     (mapping-entry x scheme-entry) cross-product.
 *   gen_resolve()  -- two-stage resolution:
 *                       (a) early-drop symbol rules whose archive or
 *                           object isn't in the entity DB;
 *                       (b) walk the DB and append every (archive,
 *                           object, section) triple to its
 *                           most-specific matching rule's @c matches;
 *                       (c) late-drop symbol rules left with empty
 *                           matches (function inlined / DCE'd);
 *                       (d) for every more-specific rule that overlaps
 *                           a less-specific basis in a *different*
 *                           target, append the rule's file pattern to
 *                           the basis's per-rule @c exclude_list -- so
 *                           the basis's glob-emitting line carries an
 *                           @c{EXCLUDE_FILE(...)} that keeps cross-
 *                           target leakage from depending on output-
 *                           section order in the template.
 *   gen_emit()     -- iterate rules per target, emit each rule's frame
 *                     (KEEP/SORT/ALIGN/SURROUND wrappers) plus a body
 *                     that lists every matched (archive, object) group
 *                     by literal section name.  Glob-emitting lines
 *                     (root catch-all, archive-fallback for unknown
 *                     archives) carry the rule's @c exclude_list so
 *                     more-specific carve-outs are honoured regardless
 *                     of template order.
 *
 * Design properties:
 *
 *  1. Rules drive emission.  A non-symbol rule with zero matches still
 *     emits its frame -- so SURROUND boundary symbols are always
 *     defined, even when no input section populates the surrounded
 *     area.  This is load-bearing for components like espcoredump that
 *     reference the boundary symbols unconditionally from C code.
 *     Symbol-specific rules with empty matches are dropped (with a
 *     warning): the underlying function/data was inlined or DCE'd, so
 *     there is nothing to surround anyway.
 *
 *  2. Section bodies are listed by literal name.  No globs in the
 *     output for sections from archives in the entity DB; each input
 *     section appears in exactly one input section description.  The
 *     linker has no ambiguity to resolve.
 *
 *  3. Cross-target placement is order-independent.  Each rule that
 *     narrows a less-specific basis in a different target contributes
 *     its file pattern to the basis's @c exclude_list, which the basis
 *     emits as @c{EXCLUDE_FILE(...)} on its glob-emitting line.  The
 *     template author can place output sections in any order.
 *
 *  4. Unknowns fall through.  Sections from archives not in the
 *     libraries-file (toolchain libs, prebuilt blobs) land in the root
 *     rule's catch-all glob.  The catch-all's @c exclude_list keeps
 *     cross-target carve-outs honest; sections from archives nobody
 *     named anywhere fall to the linker's orphan handling.
 *
 * See cmd/idf/ldgen/README.md for the full design rationale and
 * worked examples.
 */
#ifndef LDGEN_GEN_H
#define LDGEN_GEN_H

#include <stdio.h>

#include "lf.h"
struct kc_ctx;
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
 * One DB-confirmed (archive, object, section) triple matched by a rule.
 *
 * All three pointers are borrowed from @ref sinfo_db -- not freed by
 * @ref gen_free.  Populated by @ref gen_resolve as it walks the entity
 * DB and consumed by @ref gen_emit.
 */
struct gen_rule_match {
	const char *archive;
	const char *object;
	const char *section;
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
 * .text.*}` (both strings appear here).  These globs are used during
 * matching only; the emitted output uses literal section names from
 * @p matches.
 *
 * @p matches is filled by @ref gen_resolve: every (archive, object,
 * section) triple in the entity DB whose most-specific match is this
 * rule appends an entry here.  May be empty -- non-symbol rules still
 * emit their frame; symbol rules with empty matches get @p dropped.
 *
 * @p exclude_list accumulates space-separated @c{*<archive>} (or
 * @c{*<archive>:<object>.*}) entries -- one per more-specific rule
 * that narrows this rule but routes to a different target.  Used by
 * @ref gen_emit when this rule emits a glob (root catch-all, or
 * archive-fallback for an archive not in the DB) so that
 * @c{EXCLUDE_FILE(...)} keeps cross-target carve-outs honest.
 *
 * @p dropped marks symbol rules removed by @ref gen_resolve's early
 * drop (archive/object not in DB) or late drop (no section matched).
 * Dropped rules are skipped by emission.
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

	struct gen_rule_match *matches;
	int n_matches;
	int alloc_matches;

	char *exclude_list; /**< NULL or space-separated *<arch>[:<obj>.*] */
	int dropped;	    /**< symbol rule removed by gen_resolve */
};

/* ---- Context ----------------------------------------------------- */

struct gen_ctx {
	struct gen_rule *rules;
	int n_rules;
	int alloc_rules;

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
 * Conditional arms (`if`/`elif`/`else`) are evaluated against @p cfg
 * (a @ref kc_ctx already taken through @ref kc_parse_file and
 * @ref kc_eval).  Pass NULL to skip all conditionals (useful only for
 * self-contained test fragments).
 *
 * Rules are sorted by (specificity ASC, source_order ASC) so that the
 * resolution loop in @ref gen_resolve can walk the array backwards to
 * implement most-specific-wins selection.
 */
void gen_compile(struct gen_ctx *ctx, struct lf_file **files, int n_files,
		 const struct kc_ctx *cfg);

/**
 * @brief Resolve rules against the entity DB.
 *
 * Four sub-passes:
 *
 *   1. **Early drop.**  Symbol-specific rules whose archive or object
 *      isn't in @p db are marked dropped (with a warning) -- there is
 *      nothing the rule could ever apply to.
 *
 *   2. **Match attachment.**  For every (archive, object, section)
 *      triple in @p db, the first matching rule (specificity DESC,
 *      skipping dropped ones) gets the triple appended to its
 *      @c matches array.  Sections not claimed by any rule are
 *      silently dropped; they will land in the root rule's catch-all
 *      glob or hit the linker's orphan handling.
 *
 *   3. **Late drop.**  Symbol-specific rules whose patterns ended up
 *      matching no real section are marked dropped (with a warning).
 *      The function or data the rule named was inlined or DCE'd.
 *
 *   4. **Cross-target exclusion.**  Each surviving rule that narrows
 *      a less-specific basis routing to a different target
 *      contributes a @c{*<archive>} (or @c{*<archive>:<object>.*})
 *      pattern to the basis's per-rule @c exclude_list, which the
 *      basis's glob-emitting line carries as @c{EXCLUDE_FILE(...)}.
 */
void gen_resolve(struct gen_ctx *ctx, const struct sinfo_db *db);

/**
 * @brief Emit per-target explicit placements to @p out.
 *
 * Iterates rules, grouped by target in template-discovery order, and
 * for each rule emits its frame (SURROUND/ALIGN/SORT/KEEP wrappers)
 * around a body of literal-section listings per (archive, object).
 * Root rules also emit a `*(EXCLUDE_FILE(...) <patterns>)` catch-all.
 *
 * Output is bare per-target blocks prefixed with `[target]\n`.  Used
 * for debugging; production builds go through @ref gen_fill_template.
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
 * Walks every rule's @c matches and prints one line per triple,
 * sorted lexicographically.  This is the format used to diff against
 * the Python implementation's resolved output.
 */
void gen_emit_canonical(FILE *out, const struct gen_ctx *ctx);

/** @brief Release all memory owned by @p ctx. */
void gen_free(struct gen_ctx *ctx);

#endif /* LDGEN_GEN_H */
