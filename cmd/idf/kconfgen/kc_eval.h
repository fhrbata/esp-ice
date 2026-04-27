/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_eval.h
 * @brief Kconfig symbol evaluation -- resolves parsed AST to concrete
 *        per-symbol values.
 *
 * Consumes a @p kc_ctx populated by kc_parse_file() and populates the
 * evaluation-state fields on each ksym / kmenu (effective_dep, rev_dep,
 * visible, cur_val, etc).  After kc_eval(), callers can inspect
 * @c ksym->cur_val and @c ksym->visible for every symbol in
 * @c ctx->symlist.
 *
 * Pipeline (matches cmd/idf/kconfgen/DESIGN.md section 5):
 *   1. menu ctx_dep       -- parent-menu AND own-dep, walked top-down.
 *   2. symbol effective_dep -- ctx_dep AND every KP_DEPENDS prop.
 *   3. rev_dep / weak_rev_dep -- accumulate `select` / `imply` targets.
 *   4. default application -- seeded inside the fixpoint loop.
 *   5. visibility + value fixpoint -- iterate until stable.
 *   6. range enforcement  -- clamp int/hex/float to applicable ranges.
 */
#ifndef KC_EVAL_H
#define KC_EVAL_H

#include "kc_ast.h"

struct kc_ctx;
struct kexpr;

/**
 * @brief Canonical "zero value" for a symbol type.
 *
 * Returns a pointer into static storage: @c "n" for KS_BOOL, @c "0"
 * for KS_INT, @c "0x0" for KS_HEX, @c "0.0" for KS_FLOAT, and @c ""
 * for KS_STRING / KS_UNKNOWN.  Callers that need ownership must
 * @c sbuf_strdup the result.
 *
 * Used as the fallback payload when a visible symbol has no
 * user-provided value and no @c default property fires.
 */
const char *kc_sym_type_default(enum ksym_type type);

/**
 * @brief Evaluate an expression to a boolean using current symbol
 *        values.
 *
 * Intended for post-@c kc_eval() callers (e.g. output writers) that need
 * to re-evaluate a property's @c if guard -- a @c prompt whose cond is
 * false should be treated as having no prompt, so the symbol drops out
 * of sdkconfig output.  Returns 1 for a NULL expression, matching the
 * internal "no guard means always true" convention.
 */
int kc_expr_bool(const struct kexpr *e);

/**
 * @brief Parse a boolean expression from a NUL-terminated string.
 *
 * Returns a heap-owned @ref kexpr tree equivalent to the expression
 * python kconfiglib's @c eval_string would produce.  Caller frees
 * with @ref kc_expr_free.
 *
 * @p ctx must already be through @ref kc_eval so identifier references
 * resolve against populated symbol values.  @p src_name appears in
 * diagnostic output; pass the source path or @c "<expr>".
 */
struct kexpr *kc_expr_parse_string(struct kc_ctx *ctx, const char *src,
				   const char *src_name);

/**
 * @brief Free a @ref kexpr tree returned from @ref kc_expr_parse_string.
 *
 * No-op on NULL.
 */
void kc_expr_free(struct kexpr *e);

/**
 * @brief Run the full evaluation pipeline on @p ctx.
 *
 * Builds the structural dep / rev-dep / choice-link state (one-shot)
 * and performs a first @ref kc_resolve.  Call once per context after
 * parsing and seeding user input.  Subsequent interactive re-resolves
 * (menuconfig) should go through @ref kc_resolve, which skips the
 * structural passes.  Dies on oscillating fixpoints (cap: 50 iterations).
 */
void kc_eval(struct kc_ctx *ctx);

/**
 * @brief Re-run the resolve passes (fixpoint + setter propagation).
 *
 * Must be preceded by exactly one @ref kc_eval so the structural
 * state (effective_dep / rev_dep / choice links) is populated.  Safe
 * to call repeatedly -- the per-resolve scratch state (set_rank,
 * default_applied, default_seeded) is reset from
 * @c user_default_seeded at the start of each invocation.  Dies on
 * oscillating fixpoints.
 */
void kc_resolve(struct kc_ctx *ctx);

/**
 * @brief Set a user-provided value on a named symbol.
 *
 * Seeds @c cur_val and sets @c user_set so the next @ref kc_eval /
 * @ref kc_resolve preserves @p val instead of applying defaults,
 * provided the symbol remains visible.  Clears any prior default-
 * seeded state (the user has taken over the symbol).  Silently
 * no-ops if @p name doesn't match a known symbol -- matches python
 * kconfgen's tolerant merge behaviour.
 *
 * Usable both before the initial @ref kc_eval (batch seeding from
 * --config / --defaults) and between successive @ref kc_resolve
 * calls (interactive menuconfig mutation).
 */
void kc_sym_set_user(struct kc_ctx *ctx, const char *name, const char *val);

/**
 * @brief Dump every symbol to stdout in a simple one-line-per-symbol
 *        format: @c NAME : TYPE = VALUE [visible] .
 *
 * Intended for debugging and for golden-file diffing against python
 * kconfgen's resolution.
 */
void kc_symbols_dump(const struct kc_ctx *ctx);

/**
 * @brief Locale-independent strtod.
 *
 * Python kconfgen parses numeric literals under the C locale so
 * @c "1.5" always means one-and-a-half regardless of the caller's
 * @c LC_NUMERIC.  Plain @c strtod() would misparse such a literal as
 * @c 1.0 under a decimal-comma locale (e.g. @c de_DE).  This helper
 * saves and restores @c LC_NUMERIC around the call.
 *
 * Not thread-safe on its own: @c setlocale is process-global.  Cconfig
 * is single-threaded today; if that ever changes, move to
 * @c uselocale / newlocale (POSIX 2008).
 */
double kc_strtod_c(const char *nptr, char **endptr);

#endif /* KC_EVAL_H */
