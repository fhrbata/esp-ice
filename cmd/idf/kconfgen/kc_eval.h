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

struct kc_ctx;
struct kexpr;

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
 * @brief Run the full evaluation pipeline on @p ctx.
 *
 * Safe to call at most once per context (currently).  Dies on
 * oscillating fixpoints (cap: 50 iterations).
 */
void kc_eval(struct kc_ctx *ctx);

/**
 * @brief Set a user-provided value on a named symbol.
 *
 * Call between kc_parse_file() and kc_eval().  Marks the symbol as
 * @c user_set so kc_eval preserves @p val instead of applying
 * defaults, provided the symbol remains visible.  Silently no-ops if
 * @p name doesn't match a known symbol -- matches python kconfgen's
 * tolerant merge behaviour.
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

#endif /* KC_EVAL_H */
