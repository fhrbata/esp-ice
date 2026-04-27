/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_io.c
 * @brief sdkconfig output writers (config, header, cmake, json,
 *        json_menus, savedefconfig).
 *
 * Input parsing lives in kc_confread.c; deprecated-rename tables in
 * kc_rename.c.  This file holds the six format writers and the
 * emission filters they share -- has_prompt / should_emit /
 * walk_config -- because those predicates are load-bearing for
 * matching python kconfgen byte-for-byte and do not cleanly pair
 * with any one format.
 *
 * Further split of the writers is deferred: they share enough helpers
 * (emit_quoted, is_pseudo_sym, menu_visible_if_chain_ok, has_prompt,
 * emit_worthy_no_prompt, should_emit, walk_config, reset_emit_seen,
 * and the emit_deprecated_* family) that untangling them demands more
 * refactoring than a file move -- tracked as follow-up.
 */
#include "kc_io.h"
#include "ice.h"
#include "kc_ast.h"
#include "kc_eval.h"
#include "kc_private.h"

/* ================================================================== */
/*  Writer                                                            */
/* ================================================================== */

/*
 * Append a string value to @p out in sdkconfig quoted-string form.
 * Backslash-escapes '"' and '\\'; leaves other bytes verbatim.  (We
 * don't re-encode '\n' / '\t' because ESP-IDF doesn't use them in
 * config string values; matches python kconfgen's behaviour.)
 */
static void emit_quoted(struct sbuf *out, const char *s)
{
	sbuf_addch(out, '"');
	for (const char *p = s; *p; p++) {
		if (*p == '"' || *p == '\\')
			sbuf_addch(out, '\\');
		sbuf_addch(out, *p);
	}
	sbuf_addch(out, '"');
}

/*
 * True for symbols that should never appear in a CONFIG_ line: the
 * lexer-interned barewords (y, n, m, numeric literals) and choice
 * parents (whose "value" is synthetic, matching python kconfgen).
 */
static int is_pseudo_sym(const struct ksym *s)
{
	if (!s->name || !s->name[0])
		return 1;
	if (!strcmp(s->name, "y") || !strcmp(s->name, "n") ||
	    !strcmp(s->name, "m") || !strcmp(s->name, "yes") ||
	    !strcmp(s->name, "no"))
		return 1;
	if (s->type == KS_UNKNOWN && !s->props)
		return 1;
	if (s->is_choice)
		return 1;
	return 0;
}

/*
 * Render a hex symbol's stored string value with a `0x` prefix,
 * preserving the case of the source-level default.  Python's header
 * writer keeps the stored case (uppercase hex stays uppercase; a
 * bare `0` gets a `0x` added but stays `0x0`), while its cmake
 * writer parses the value and re-emits it in lowercase -- see
 * emit_hex_for_cmake.
 *
 * Empty input produces no output.  The caller handles the enclosing
 * quoting or `#define` syntax.
 */
static void emit_hex_for_header(struct sbuf *out, const char *val)
{
	if (!val || !*val)
		return;
	if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
		sbuf_addstr(out, val);
		return;
	}
	sbuf_addstr(out, "0x");
	sbuf_addstr(out, val);
}

/*
 * Render a hex value for sdkconfig.cmake: python parses the number
 * and formats with `%x`, so the digits come out lowercase and
 * without leading zeros (`0x0` instead of `0x0000`).  Non-numeric
 * inputs fall through to emit_hex_for_header's preserve-case path.
 */
static void emit_hex_for_cmake(struct sbuf *out, const char *val)
{
	if (!val || !*val)
		return;

	const char *p = val;
	int base = 10;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
	}
	char *end = NULL;
	unsigned long long v = strtoull(p, &end, base);
	if (!end || end == p || *end) {
		emit_hex_for_header(out, val);
		return;
	}
	sbuf_addf(out, "0x%llx", v);
}

/*
 * True when @p s has at least one KP_PROMPT property whose @c if guard
 * currently evaluates true.  Python kconfgen emits only user-facing
 * symbols (those with a visible prompt) -- "hidden" internal bool/int
 * flags that track derived state but have no prompt are not written to
 * any of the generated artefacts.
 *
 * Respecting the per-prompt @c cond is load-bearing for the idiom
 * `bool "Label" if FEATURE_FLAG` that ESP-IDF uses in a few places
 * (e.g. BOOTLOADER_LOG_VERSION_2's `bool "V2" if LOG_VERSION_2`).
 * When FEATURE_FLAG is false the prompt is effectively absent, so the
 * symbol behaves like a promptless bool and we should apply the
 * no-prompt emit rules (bool: emit only when "y").
 */
/*
 * Walk @p m's ancestor chain and return 0 if any enclosing menu's
 * @c `visible if` expression currently evaluates false.  Python folds
 * a parent menu's @c visible_if into the prompts of every child
 * symbol/choice, so a `config X` declared inside
 * `menu ... visible if 0` becomes prompt-invisible and stops emitting
 * `# CONFIG_X is not set` lines.  Ice keeps @c visible_if on the
 * menu node and replays that gate here at emit time.
 */
static int menu_visible_if_chain_ok(const struct kmenu *m)
{
	for (; m; m = m->parent) {
		if (m->visible_if && !kc_expr_bool(m->visible_if))
			return 0;
	}
	return 1;
}

static int has_prompt(const struct ksym *s)
{
	/* An unconditional prompt still counts as hidden when the
	 * enclosing menu chain has a `visible if 0` gate.  Check it once
	 * up front -- a given sym has a single declaration menu, so the
	 * chain doesn't vary across its KP_PROMPT props. */
	if (!menu_visible_if_chain_ok(s->decl_menu))
		return 0;
	for (const struct kprop *p = s->props; p; p = p->next) {
		if (p->kind != KP_PROMPT)
			continue;
		if (!p->cond || kc_expr_bool(p->cond))
			return 1;
	}
	return 0;
}

/*
 * Pick-filter for prompt-less symbols: emit when python would.
 *
 *   - Bool: only if the current value is "y".  A no-prompt bool that
 *     resolves to "n" is never emitted (it would just be a CONFIG_X
 *     is not set line with no way for the user to have influenced it).
 *   - int / hex / string / float: emit only when a @c default actually
 *     fired on the visible symbol.  Having @c default lines declared
 *     isn't enough -- see emit_worthy_no_prompt for the reasoning.
 */
static int emit_worthy_no_prompt(const struct ksym *s)
{
	if (s->type == KS_BOOL)
		return s->cur_val && !strcmp(s->cur_val, "y");
	/*
	 * int / hex / string / float without a prompt: emit only when
	 * the symbol is visible AND has a meaningful value -- either
	 * the user set it via --config / --defaults, or at least one
	 * of its @c default properties actually fired (@c
	 * default_applied tracked during fixpoint).  Merely having a
	 * @c default line declared isn't enough: HAL_LOG_LEVEL declares
	 * a full `default N if FLAG_N` table but none match when the
	 * enclosing choice is hidden, leaving the value at the type's
	 * zero.  Python kconfgen drops those; mirror that so we don't
	 * emit spurious `=0` lines for promptless derived ints.
	 */
	return s->visible && (s->default_applied || s->user_set);
}

/*
 * Final per-symbol filter applied by every writer.
 *
 * Python kconfgen emits a symbol when ANY of these hold:
 *
 *   (a) it's visible (has a prompt whose full depends-on chain
 *       evaluates true);
 *   (b) the user explicitly set it via @c --config / @c --defaults
 *       (so round-tripping never drops user-typed entries);
 *   (c) it's a derived / internal flag with a non-trivial computed
 *       value -- a no-prompt bool that evaluates to "y", or a no-
 *       prompt int/hex/string with a non-zero non-empty value.
 *
 * Choice parents, the lexer-interned barewords, and @c option env
 * symbols are always skipped.
 */
static int should_emit(const struct ksym *s)
{
	if (is_pseudo_sym(s))
		return 0;
	/*
	 * Python kconfgen does NOT treat `user_set` as an emit
	 * override: a line like `# CONFIG_X is not set` in the input
	 * sdkconfig marks X as user-set to "n", but "n" is the bool
	 * zero default, so the symbol is effectively unchanged and
	 * python drops it unless it would also be emitted by visibility
	 * / prompt-less-with-value.  Mirror that rule so full-sdkconfig
	 * round-trips don't accumulate hidden `is not set` lines.
	 */
	int has_p = has_prompt(s);
	if (has_p && s->visible)
		return 1;
	if (!has_p && emit_worthy_no_prompt(s))
		return 1;
	return 0;
}

/*
 * Walk the menu tree pre-order and invoke @p cb on every symbol that
 * backs a KM_SYM / KM_CHOICE menu node -- i.e. the declaration order
 * python kconfgen uses for its output files.  Filtering out
 * pseudo-syms, env-options, etc. is the caller's job.
 */
typedef void (*sym_cb)(struct sbuf *out, const struct ksym *s, void *ud);
static void walk_syms(const struct kmenu *m, struct sbuf *out, sym_cb cb,
		      void *ud)
{
	if (!m)
		return;
	if ((m->kind == KM_SYM || m->kind == KM_CHOICE) && m->sym)
		cb(out, m->sym, ud);
	for (const struct kmenu *c = m->children; c; c = c->next)
		walk_syms(c, out, cb, ud);
}

/*
 * Clear the @c emit_seen dedup flag on every symbol before a walk.
 * A Kconfig symbol can back multiple KM_SYM nodes when the source
 * has `config X` declared in several blocks with `# ignore:
 * multiple-definition` (ESP-IDF uses this e.g. in
 * components/esp_wifi/remote/Kconfig.soc_wifi_caps.in).  Without
 * dedup the symbol gets emitted N times.  Python kconfgen iterates
 * the symbol table directly so it naturally emits once.
 */
static void reset_emit_seen(const struct kc_ctx *ctx)
{
	for (size_t i = 0; i < ctx->symlist.nr; i++) {
		struct ksym *s =
		    smap_get((struct smap *)&ctx->symtab, ctx->symlist.v[i]);
		if (s)
			s->emit_seen = 0;
	}
}

/*
 * Emit one symbol in the canonical sdkconfig form.
 *
 *   bool  = y    -> CONFIG_X=y
 *   bool  = n    -> # CONFIG_X is not set
 *   int          -> CONFIG_X=<decimal>
 *   hex          -> CONFIG_X=0x...   (0x prefix preserved)
 *   string       -> CONFIG_X="..."   (escaped)
 *   float        -> CONFIG_X=<as-stored>
 *   UNKNOWN      -> CONFIG_X=<as-stored>  (still emitted so nothing
 *                                          is silently lost)
 */
static void emit_symbol(struct sbuf *out, const struct ksym *s)
{
	const char *val = s->cur_val ? s->cur_val : "";

	/*
	 * ESP-IDF extension: tag lines whose value came from a
	 * built-in default (as opposed to being set by the user via
	 * --config / --defaults) with a "# default:" pragma, so tools
	 * that diff sdkconfig can distinguish user-modified settings
	 * from untouched ones.  Matches python kconfgen's behaviour.
	 */
	if (!s->user_set)
		sbuf_addstr(out, "# default:\n");

	if (s->type == KS_BOOL) {
		if (!strcmp(val, "y")) {
			sbuf_addf(out, "%s%s=y\n", KC_CONFIG_PREFIX, s->name);
		} else {
			sbuf_addf(out, "# %s%s is not set\n", KC_CONFIG_PREFIX,
				  s->name);
		}
		return;
	}

	if (s->type == KS_STRING) {
		sbuf_addf(out, "%s%s=", KC_CONFIG_PREFIX, s->name);
		emit_quoted(out, val);
		sbuf_addch(out, '\n');
		return;
	}

	/* int / hex / float / unknown: emit raw.  For hex, default to
	 * "0x0" if the stored value happens to be missing a prefix. */
	sbuf_addf(out, "%s%s=%s\n", KC_CONFIG_PREFIX, s->name, val);
}

/*
 * Decide whether @p m's prompt markers should appear in the config
 * output.  For a menu this requires its ctx_dep (parent menu-chain
 * AND own `depends on`) to be true, AND any `visible if` guard on the
 * menu itself to be true -- python only prints `# Menu Title` /
 * `# end of Menu Title` markers around menus the user could see in
 * menuconfig.  For a comment node we additionally require its own @p
 * dep to be true (comments inside `visible if 0` or `depends on 0`
 * chains get skipped).
 */
static int menu_emit_visible(const struct kmenu *m)
{
	if (!m)
		return 0;
	if (m->ctx_dep && !kc_expr_bool(m->ctx_dep))
		return 0;
	if (m->visible_if && !kc_expr_bool(m->visible_if))
		return 0;
	if (m->dep && !kc_expr_bool(m->dep))
		return 0;
	return 1;
}

/*
 * True when @p out's last emitted line starts with "# end of ".
 * Walks backward to the previous newline and checks the prefix.
 * Used to decide whether to insert a blank line between a close
 * marker and a following sibling (which may be a symbol or another
 * menu / comment block).
 */
static int tail_is_end_marker(const struct sbuf *out)
{
	if (out->len < 2 || out->buf[out->len - 1] != '\n')
		return 0;
	size_t i = out->len - 1;
	while (i > 0 && out->buf[i - 1] != '\n')
		i--;
	const char *line = out->buf + i;
	size_t llen = out->len - 1 - i;
	static const char prefix[] = "# end of ";
	return llen >= sizeof(prefix) - 1 &&
	       !memcmp(line, prefix, sizeof(prefix) - 1);
}

/*
 * Config-format menu walker: emits python's `#\n# Title\n#\n ... # end
 * of Title\n` markers around each visible menu and `#\n# Comment\n#\n`
 * comment-node blocks, plus the actual symbol lines inside.  KM_CHOICE,
 * KM_IF, KM_ROOT are transparent -- they group symbols but contribute
 * no label to sdkconfig.  A menu whose own visibility is false still
 * gets recursed into (its symbols' values compute independently via
 * their own depends-on chains), but the `# Menu Title` markers are
 * suppressed -- matches python's "hidden menu, visible children" case
 * like `menu "SoC Settings" ... visible if 0`.
 *
 * Blank-line shaping: python puts one blank between any post-menu-
 * close sibling and the close itself.  Consecutive `# end of`
 * closes stack with no blanks.  We implement that by always emitting
 * `# end of X\n` without a trailing blank and inserting the blank
 * lazily right before the next non-close sibling (menu open, comment
 * open, or symbol).
 */
static void walk_config(const struct kmenu *m, struct sbuf *out)
{
	if (!m)
		return;

	if (m->kind == KM_MENU && m->prompt) {
		int vis = menu_emit_visible(m);
		if (vis) {
			if (out->len < 2 || out->buf[out->len - 1] != '\n' ||
			    out->buf[out->len - 2] != '\n')
				sbuf_addch(out, '\n');
			sbuf_addf(out, "#\n# %s\n#\n", m->prompt);
		}
		for (const struct kmenu *c = m->children; c; c = c->next)
			walk_config(c, out);
		if (vis)
			sbuf_addf(out, "# end of %s\n", m->prompt);
		return;
	}

	if (m->kind == KM_COMMENT && m->prompt) {
		if (menu_emit_visible(m)) {
			if (out->len < 2 || out->buf[out->len - 1] != '\n' ||
			    out->buf[out->len - 2] != '\n')
				sbuf_addch(out, '\n');
			sbuf_addf(out, "#\n# %s\n#\n", m->prompt);
		}
		return;
	}

	if ((m->kind == KM_SYM || m->kind == KM_CHOICE) && m->sym &&
	    !m->sym->emit_seen && should_emit(m->sym)) {
		m->sym->emit_seen = 1;
		/* A symbol line right after a menu close gets a blank line
		 * separator, so `# end of X` doesn't butt up against the
		 * next CONFIG_.  Nested closes have already stacked without
		 * blanks; this runs only at the first non-close sibling. */
		if (tail_is_end_marker(out))
			sbuf_addch(out, '\n');
		emit_symbol(out, m->sym);
	}

	for (const struct kmenu *c = m->children; c; c = c->next)
		walk_config(c, out);
}

/*
 * Emit one deprecated-alias line for rename @p r whose NEW target is
 * @p new_sym.  Bool renames expand to `CONFIG_OLD=y` or
 * `# CONFIG_OLD is not set` (flipped when the rename is inverted);
 * string renames preserve quoting; int / hex / float fall through to
 * `CONFIG_OLD=<value>`.
 */
static void emit_deprecated_alias(struct sbuf *out, const struct kc_rename *r,
				  const struct ksym *new_sym)
{
	const char *val = new_sym->cur_val ? new_sym->cur_val : "";

	if (new_sym->type == KS_BOOL) {
		int y = !strcmp(val, "y");
		if (r->invert)
			y = !y;
		if (y)
			sbuf_addf(out, "%s%s=y\n", KC_CONFIG_PREFIX,
				  r->old_name);
		else
			sbuf_addf(out, "# %s%s is not set\n", KC_CONFIG_PREFIX,
				  r->old_name);
		return;
	}
	if (new_sym->type == KS_STRING) {
		sbuf_addf(out, "%s%s=", KC_CONFIG_PREFIX, r->old_name);
		emit_quoted(out, val);
		sbuf_addch(out, '\n');
		return;
	}
	sbuf_addf(out, "%s%s=%s\n", KC_CONFIG_PREFIX, r->old_name, val);
}

struct dep_emit_ctx {
	const struct kc_ctx *ctx;
};

static void emit_deprecated_for_sym(struct sbuf *out, const struct ksym *s,
				    void *ud)
{
	const struct dep_emit_ctx *dctx = ud;
	/* Emit an OLD alias only when the NEW sym is itself emitted --
	 * otherwise the OLD entry would reference a CONFIG python also
	 * skipped and break diff-based tooling.  Matches python kconfgen's
	 * filter. */
	if (!should_emit(s))
		return;
	/* No dedup across menu nodes: a symbol declared twice via
	 * `# ignore: multiple-definition` (e.g. ESP_WIFI_STATIC_RX_BUFFER_NUM
	 * in esp_wifi/Kconfig and esp_wifi/remote/Kconfig.wifi_is_remote.in)
	 * shows up in @c config.node_iter() twice and python emits each
	 * node's deprecated aliases separately.  Mirror that. */
	for (size_t i = 0; i < dctx->ctx->n_renames; i++) {
		const struct kc_rename *r = &dctx->ctx->renames[i];
		if (strcmp(r->new_name, s->name) != 0)
			continue;
		emit_deprecated_alias(out, r, s);
	}
}

/*
 * Emit the `# Deprecated options for backward compatibility` block
 * that python kconfgen appends after the main body.  Python's
 * @c deprecated_config_contents iterates the node tree in declaration
 * order and, for each symbol that has one or more rename aliases,
 * emits all of its aliases in rename-file registration order.  The
 * ordering differs from a naive rename-list walk because the @em new
 * symbols live scattered across components whose Kconfig files appear
 * at varying tree positions; ESP-IDF relies on that menu-tree order
 * for stable sdkconfig diffs across release branches.
 */
static void emit_deprecated_config(struct sbuf *out, const struct kc_ctx *ctx)
{
	if (ctx->no_deprecated || !ctx->n_renames)
		return;

	sbuf_addstr(out, "\n"
			 "# Deprecated options for backward compatibility\n");

	reset_emit_seen(ctx);
	struct dep_emit_ctx dctx = {.ctx = ctx};
	walk_syms(ctx->root, out, emit_deprecated_for_sym, &dctx);

	sbuf_addstr(out, "# End of deprecated options\n");
}

/*
 * Emit the deprecated-aliases block in a C header form:
 *
 *   /\* List of deprecated options *\/
 *   #define CONFIG_OLD CONFIG_NEW
 *
 * Only non-inverting renames produce an alias -- inversion can't be
 * expressed as a simple #define.
 */
/*
 * The C header only gets an alias line for a deprecated symbol if
 * the NEW symbol would itself emit a @c #define -- i.e. bool-y, or
 * any non-bool type with a non-empty value.  bool-n NEW symbols are
 * absent from the header; emitting `#define CONFIG_OLD CONFIG_NEW`
 * against an undefined CONFIG_NEW would expand to an empty token
 * list and poison client code.
 */
static int header_defines_sym(const struct ksym *s)
{
	if (!s)
		return 0;
	const char *val = s->cur_val ? s->cur_val : "";
	if (s->type == KS_BOOL)
		return !strcmp(val, "y");
	return 1; /* int / hex / float / string / unknown */
}

static int rename_old_cmp(const void *a, const void *b)
{
	const struct kc_rename *ra = *(const struct kc_rename *const *)a;
	const struct kc_rename *rb = *(const struct kc_rename *const *)b;
	return strcmp(ra->old_name, rb->old_name);
}

static void emit_deprecated_header(struct sbuf *out, const struct kc_ctx *ctx)
{
	if (ctx->no_deprecated || !ctx->n_renames)
		return;

	/*
	 * Python kconfgen sorts the header deprecated-alias block
	 * alphabetically by old name (unlike the sdkconfig and cmake
	 * blocks, which keep declaration order).  Mirror that here so
	 * build-system diff tools don't trip over a reordered header.
	 */
	const struct kc_rename **sorted =
	    calloc(ctx->n_renames, sizeof(*sorted));
	if (!sorted)
		die_errno("calloc");
	size_t n = 0;
	for (size_t i = 0; i < ctx->n_renames; i++) {
		const struct kc_rename *r = &ctx->renames[i];
		if (r->invert)
			continue; /* can't express inversion as #define */
		struct ksym *new_sym = smap_get(&ctx->symtab, r->new_name);
		if (!new_sym || !should_emit(new_sym))
			continue;
		if (!header_defines_sym(new_sym))
			continue;
		sorted[n++] = r;
	}
	qsort(sorted, n, sizeof(*sorted), rename_old_cmp);

	sbuf_addstr(out, "\n/* List of deprecated options */\n");
	for (size_t i = 0; i < n; i++) {
		const struct kc_rename *r = sorted[i];
		sbuf_addf(out, "#define %s%s %s%s\n", KC_CONFIG_PREFIX,
			  r->old_name, KC_CONFIG_PREFIX, r->new_name);
	}
	free(sorted);
}

void kc_write_config(const struct kc_ctx *ctx, const char *path)
{
	struct sbuf out = SBUF_INIT;
	reset_emit_seen(ctx);

	/*
	 * Python kconfgen stamps the ESP-IDF release (`$IDF_VERSION`
	 * from the env) into the preamble.  When unset it leaves the
	 * double-space gap where the version would go, so we replicate
	 * that exact behaviour.
	 */
	const char *ver =
	    (ctx->idf_version && *ctx->idf_version) ? ctx->idf_version : "";
	sbuf_addstr(&out, "#\n"
			  "# Automatically generated file. DO NOT EDIT.\n");
	sbuf_addf(&out,
		  "# Espressif IoT Development Framework (ESP-IDF) %s "
		  "Project Configuration\n",
		  ver);
	sbuf_addstr(&out, "#\n");

	walk_config(ctx->root, &out);
	emit_deprecated_config(&out, ctx);

	if (write_file_atomic(path, out.buf, out.len) < 0)
		die_errno("cannot write '%s'", path);

	sbuf_release(&out);
}

/* ================================================================== */
/*  Header writer (sdkconfig.h)                                       */
/* ================================================================== */

/* Emit a #define line for the symbol in C-header form.  bool-n is
 * deliberately omitted (matching python kconfgen) rather than
 * #undef'd: absence of a define lets user code test with
 * `#if defined(CONFIG_X)`. */
static void emit_header_symbol(struct sbuf *out, const struct ksym *s)
{
	const char *val = s->cur_val ? s->cur_val : "";

	if (s->type == KS_BOOL) {
		if (!strcmp(val, "y"))
			sbuf_addf(out, "#define %s%s 1\n", KC_CONFIG_PREFIX,
				  s->name);
		return;
	}
	if (s->type == KS_STRING) {
		sbuf_addf(out, "#define %s%s ", KC_CONFIG_PREFIX, s->name);
		emit_quoted(out, val);
		sbuf_addch(out, '\n');
		return;
	}
	if (s->type == KS_HEX) {
		sbuf_addf(out, "#define %s%s ", KC_CONFIG_PREFIX, s->name);
		emit_hex_for_header(out, val);
		sbuf_addch(out, '\n');
		return;
	}
	/* int / float / unknown: emit raw. */
	sbuf_addf(out, "#define %s%s %s\n", KC_CONFIG_PREFIX, s->name, val);
}

static void cb_header(struct sbuf *out, const struct ksym *s, void *ud)
{
	(void)ud;
	if (s->emit_seen)
		return;
	if (!should_emit(s))
		return;
	((struct ksym *)s)->emit_seen = 1;
	emit_header_symbol(out, s);
}

void kc_write_header(const struct kc_ctx *ctx, const char *path)
{
	struct sbuf out = SBUF_INIT;
	reset_emit_seen(ctx);

	const char *ver =
	    (ctx->idf_version && *ctx->idf_version) ? ctx->idf_version : "";
	sbuf_addstr(&out, "/*\n"
			  " * Automatically generated file. DO NOT EDIT.\n");
	sbuf_addf(&out,
		  " * Espressif IoT Development Framework (ESP-IDF) %s "
		  "Configuration Header\n",
		  ver);
	sbuf_addstr(&out, " */\n"
			  "#pragma once\n");

	walk_syms(ctx->root, &out, cb_header, NULL);
	emit_deprecated_header(&out, ctx);

	if (write_file_atomic(path, out.buf, out.len) < 0)
		die_errno("cannot write '%s'", path);

	sbuf_release(&out);
}

/* ================================================================== */
/*  Minimal / savedefconfig writer                                    */
/* ================================================================== */

/*
 * Compute what the symbol's Kconfig-default value would be, given the
 * current evaluator state.  Returns an owned string the caller frees.
 *
 * Walks the declaration-order property list, picks the first KP_DEFAULT
 * whose @c if condition evaluates true, and returns its literal raw (no
 * hex normalisation -- matches the evaluator's @c default_value path and
 * python's @c str_value which keeps `default 0` / `default 33` verbatim
 * for hex symbols).  When no default fires we fall back to the type's
 * zero value so the comparison against @c cur_val cleanly identifies
 * any non-default state.
 */
static char *compute_kconfig_default(const struct ksym *s)
{
	for (const struct kprop *p = s->props; p; p = p->next) {
		if (p->kind != KP_DEFAULT)
			continue;
		if (p->cond && !kc_expr_bool(p->cond))
			continue;
		if (!p->expr)
			continue;
		if (s->type == KS_BOOL)
			return sbuf_strdup(kc_expr_bool(p->expr) ? "y" : "n");
		const char *raw = NULL;
		if (p->expr->op == KE_LITERAL)
			raw = p->expr->str;
		else if (p->expr->op == KE_SYMREF && p->expr->sym &&
			 p->expr->sym->cur_val)
			raw = p->expr->sym->cur_val;
		if (!raw)
			raw = "";
		return sbuf_strdup(raw);
	}
	return sbuf_strdup(kc_sym_type_default(s->type));
}

/*
 * Emit @p s as a savedefconfig line: `CONFIG_X=value`.  Bool-n is
 * written explicitly rather than the `# ... is not set` form the full
 * config uses -- matches python kconfgen's @c normalize_unset=True
 * post-processing on the minimal config.
 */
static void emit_symbol_min(struct sbuf *out, const struct ksym *s)
{
	const char *val = s->cur_val ? s->cur_val : "";

	if (s->type == KS_BOOL) {
		sbuf_addf(out, "%s%s=%s\n", KC_CONFIG_PREFIX, s->name,
			  strcmp(val, "y") == 0 ? "y" : "n");
		return;
	}
	if (s->type == KS_STRING) {
		sbuf_addf(out, "%s%s=", KC_CONFIG_PREFIX, s->name);
		emit_quoted(out, val);
		sbuf_addch(out, '\n');
		return;
	}
	sbuf_addf(out, "%s%s=%s\n", KC_CONFIG_PREFIX, s->name, val);
}

static int min_config_should_emit(const struct ksym *s)
{
	if (is_pseudo_sym(s))
		return 0;
	if (!should_emit(s))
		return 0;
	char *def = compute_kconfig_default(s);
	const char *cur = s->cur_val ? s->cur_val : "";
	int differs = strcmp(cur, def) != 0;
	free(def);
	return differs;
}

/*
 * With ESP_IDF_KCONFIG_MIN_LABELS=1 set, bracket groups of
 * non-default symbols with their owning menu's prompt.  Deferred-label
 * shape: a `# Menu Name` marker is only emitted just before the first
 * minimal symbol inside that menu; `# end of Menu Name` is only
 * emitted when the menu actually produced content.
 */
struct min_walk {
	struct sbuf *out;
	/* Pending labels stack: each entry is a menu prompt waiting to
	 * be written if/when a minimal symbol fires inside it. */
	const struct kmenu **pending;
	size_t n_pending;
	size_t alloc_pending;
	/* Stack of menus whose `# Menu` marker has already been written,
	 * used to pair them with `# end of` markers on scope exit. */
	const struct kmenu **opened;
	size_t n_opened;
	size_t alloc_opened;
	int after_end;
};

static void min_flush_pending(struct min_walk *w)
{
	for (size_t i = 0; i < w->n_pending; i++) {
		const struct kmenu *m = w->pending[i];
		if (w->out->len && w->out->buf[w->out->len - 1] == '\n' &&
		    (w->out->len < 2 || w->out->buf[w->out->len - 2] != '\n'))
			sbuf_addch(w->out, '\n');
		sbuf_addf(w->out, "# %s\n", m->prompt);
		ALLOC_GROW(w->opened, w->n_opened + 1, w->alloc_opened);
		w->opened[w->n_opened++] = m;
	}
	w->n_pending = 0;
	w->after_end = 0;
}

static void min_walk_tree(const struct kmenu *m, struct min_walk *w,
			  int with_labels)
{
	if (!m)
		return;

	int pushed_pending = 0;
	if (with_labels && m->kind == KM_MENU && m->prompt) {
		ALLOC_GROW(w->pending, w->n_pending + 1, w->alloc_pending);
		w->pending[w->n_pending++] = m;
		pushed_pending = 1;
	}

	if (m->kind == KM_SYM && m->sym && min_config_should_emit(m->sym) &&
	    !m->sym->emit_seen) {
		if (with_labels)
			min_flush_pending(w);
		((struct ksym *)m->sym)->emit_seen = 1;
		emit_symbol_min(w->out, m->sym);
	}

	for (const struct kmenu *c = m->children; c; c = c->next)
		min_walk_tree(c, w, with_labels);

	if (with_labels && m->kind == KM_MENU && m->prompt) {
		if (pushed_pending && w->n_pending &&
		    w->pending[w->n_pending - 1] == m) {
			/* No symbols fired inside this menu; drop the
			 * pending label without emitting anything. */
			w->n_pending--;
		} else if (w->n_opened && w->opened[w->n_opened - 1] == m) {
			w->n_opened--;
			sbuf_addf(w->out, "# end of %s\n", m->prompt);
			w->after_end = 1;
		}
	}
}

void kc_write_min_config(const struct kc_ctx *ctx, const char *path)
{
	struct sbuf out = SBUF_INIT;
	reset_emit_seen(ctx);

	const char *ver =
	    (ctx->idf_version && *ctx->idf_version) ? ctx->idf_version : "";
	sbuf_addstr(&out,
		    "# This file was generated using idf.py save-defconfig or "
		    "menuconfig [D] key. It can be edited manually.\n");
	sbuf_addf(&out,
		  "# Espressif IoT Development Framework (ESP-IDF) %s Project "
		  "Minimal Configuration\n",
		  ver);
	sbuf_addstr(&out, "#\n");

	/*
	 * Prepend @c CONFIG_IDF_TARGET when the build target differs from
	 * the python default `esp32`.  The target assignment is what lets
	 * a checked-in @c sdkconfig.defaults round-trip across chip-family
	 * workspaces without the user redefining the target each time.
	 */
	struct ksym *target = smap_get(&ctx->symtab, "IDF_TARGET");
	if (target && target->cur_val && target->type == KS_STRING &&
	    strcmp(target->cur_val, "esp32") != 0) {
		sbuf_addf(&out, "%sIDF_TARGET=", KC_CONFIG_PREFIX);
		emit_quoted(&out, target->cur_val);
		sbuf_addch(&out, '\n');
	}

	int with_labels = 0;
	const char *lbl = getenv("ESP_IDF_KCONFIG_MIN_LABELS");
	if (lbl && !strcmp(lbl, "1"))
		with_labels = 1;

	struct min_walk w = {.out = &out};
	min_walk_tree(ctx->root, &w, with_labels);
	free(w.pending);
	free(w.opened);

	if (write_file_atomic(path, out.buf, out.len) < 0)
		die_errno("cannot write '%s'", path);
	sbuf_release(&out);
}

/* ================================================================== */
/*  CMake writer (sdkconfig.cmake)                                    */
/* ================================================================== */

/* In cmake values are ALL quoted strings -- bool-y -> "y", bool-n ->
 * "" (empty), int/hex -> quoted numeric string.  Matches python
 * kconfgen's actual output. */
static void emit_cmake_symbol(struct sbuf *out, const struct ksym *s)
{
	const char *val = s->cur_val ? s->cur_val : "";
	const char *effective = val;

	if (s->type == KS_BOOL && strcmp(val, "y") != 0)
		effective = ""; /* bool-n renders as empty string */

	sbuf_addf(out, "set(%s%s ", KC_CONFIG_PREFIX, s->name);
	if (s->type == KS_HEX) {
		/*
		 * Hex values go through emit_hex_for_cmake here to match
		 * python's `%x` (lowercase, leading zeros stripped) -- the
		 * sdkconfig.h writer uses the case-preserving variant.
		 */
		sbuf_addch(out, '"');
		emit_hex_for_cmake(out, effective);
		sbuf_addch(out, '"');
	} else {
		emit_quoted(out, effective);
	}
	sbuf_addstr(out, ")\n");
}

struct cmake_walk {
	const struct kc_ctx *ctx;
	struct sbuf *list;
	int first;
};

/* Append @p name to the CONFIGS_LIST buffer with the proper separator. */
static void list_append(struct cmake_walk *w, const char *name)
{
	if (!w->first)
		sbuf_addch(w->list, ';');
	w->first = 0;
	sbuf_addf(w->list, "%s%s", KC_CONFIG_PREFIX, name);
}

static void cb_cmake(struct sbuf *out, const struct ksym *s, void *ud)
{
	struct cmake_walk *w = ud;
	if (!should_emit(s))
		return;
	emit_cmake_symbol(out, s);
	list_append(w, s->name);

	/*
	 * Any OLD names that rename to this sym also appear right after
	 * it in CONFIGS_LIST (matches python kconfgen).  We do not emit
	 * a set() line for them in the main block -- those land in the
	 * trailing "deprecated" block instead.
	 */
	if (!w->ctx->no_deprecated) {
		for (size_t i = 0; i < w->ctx->n_renames; i++) {
			const struct kc_rename *r = &w->ctx->renames[i];
			if (strcmp(r->new_name, s->name) != 0)
				continue;
			/* Only list the OLD name when the NEW sym is
			 * itself emitted; mirrors python's filter. */
			if (should_emit(s))
				list_append(w, r->old_name);
		}
	}
}

/*
 * Emit the deprecated-aliases block after CONFIGS_LIST:
 *
 *   # List of deprecated options for backward compatibility
 *   set(CONFIG_OLD_FOO "y")
 *   set(CONFIG_OLD_BAR "y")
 *
 * Values are derived from the NEW symbol's current value, flipped
 * when the rename has the `!` inversion flag.  Matches python
 * kconfgen's output byte-for-byte.
 */
static void emit_deprecated_cmake_for_sym(struct sbuf *out,
					  const struct ksym *s, void *ud)
{
	const struct dep_emit_ctx *dctx = ud;
	if (!should_emit(s))
		return;
	const char *val = s->cur_val ? s->cur_val : "";
	for (size_t i = 0; i < dctx->ctx->n_renames; i++) {
		const struct kc_rename *r = &dctx->ctx->renames[i];
		if (strcmp(r->new_name, s->name) != 0)
			continue;
		const char *effective = val;
		if (s->type == KS_BOOL) {
			int y = !strcmp(val, "y");
			if (r->invert)
				y = !y;
			effective = y ? "y" : "";
		}
		sbuf_addf(out, "set(%s%s ", KC_CONFIG_PREFIX, r->old_name);
		/* HEX emits through the cmake-specific formatter so the
		 * digits match python's `%x` rendering (lowercase, no
		 * zero-pad), otherwise the stored mixed-case literal leaks
		 * through emit_quoted. */
		if (s->type == KS_HEX) {
			sbuf_addch(out, '"');
			emit_hex_for_cmake(out, effective);
			sbuf_addch(out, '"');
		} else {
			emit_quoted(out, effective);
		}
		sbuf_addstr(out, ")\n");
	}
}

static void emit_deprecated_cmake(struct sbuf *out, const struct kc_ctx *ctx)
{
	if (ctx->no_deprecated || !ctx->n_renames)
		return;

	sbuf_addstr(
	    out, "\n# List of deprecated options for backward compatibility\n");

	struct dep_emit_ctx dctx = {.ctx = ctx};
	walk_syms(ctx->root, out, emit_deprecated_cmake_for_sym, &dctx);
}

/* ================================================================== */
/*  JSON helpers                                                      */
/* ================================================================== */

/*
 * Append a JSON-escaped string, with surrounding quotes.  Handles the
 * usual backslash escapes and emits \\uXXXX for control characters --
 * matches what python's json.dumps produces.
 */
static void json_emit_string(struct sbuf *out, const char *s)
{
	sbuf_addch(out, '"');
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':
			sbuf_addstr(out, "\\\"");
			break;
		case '\\':
			sbuf_addstr(out, "\\\\");
			break;
		case '\n':
			sbuf_addstr(out, "\\n");
			break;
		case '\t':
			sbuf_addstr(out, "\\t");
			break;
		case '\r':
			sbuf_addstr(out, "\\r");
			break;
		case '\b':
			sbuf_addstr(out, "\\b");
			break;
		case '\f':
			sbuf_addstr(out, "\\f");
			break;
		default:
			if (*p < 0x20)
				sbuf_addf(out, "\\u%04x", *p);
			else
				sbuf_addch(out, (char)*p);
			break;
		}
	}
	sbuf_addch(out, '"');
}

static void json_indent(struct sbuf *out, int level)
{
	for (int i = 0; i < level; i++)
		sbuf_addstr(out, "    ");
}

/* ================================================================== */
/*  JSON writer (flat name -> value)                                  */
/* ================================================================== */

struct json_kv {
	const char *name;
	enum ksym_type type;
	const char *val;
};

static int json_kv_cmp(const void *a, const void *b)
{
	const struct json_kv *x = a, *y = b;
	return strcmp(x->name, y->name);
}

static void emit_json_value(struct sbuf *out, const struct json_kv *kv)
{
	switch (kv->type) {
	case KS_BOOL:
		sbuf_addstr(out, !strcmp(kv->val, "y") ? "true" : "false");
		return;
	case KS_INT:
	case KS_HEX: {
		/* JSON has no hex literal; decode and emit as decimal. */
		char *e;
		long long v = strtoll(kv->val, &e, 0);
		if (e == kv->val || *e) {
			json_emit_string(out, kv->val);
			return;
		}
		sbuf_addf(out, "%lld", v);
		return;
	}
	case KS_FLOAT:
		sbuf_addf(out, "%s", *kv->val ? kv->val : "0.0");
		return;
	case KS_STRING:
	case KS_UNKNOWN:
		json_emit_string(out, kv->val);
		return;
	}
}

struct json_collect {
	struct json_kv *entries;
	size_t nr;
	size_t alloc;
};

static void cb_json_collect(struct sbuf *out, const struct ksym *s, void *ud)
{
	(void)out;
	struct json_collect *c = ud;
	if (s->emit_seen)
		return;
	if (!should_emit(s))
		return;
	((struct ksym *)s)->emit_seen = 1;
	ALLOC_GROW(c->entries, c->nr + 1, c->alloc);
	c->entries[c->nr].name = s->name;
	c->entries[c->nr].type = s->type;
	c->entries[c->nr].val = s->cur_val ? s->cur_val : "";
	c->nr++;
}

void kc_write_json(const struct kc_ctx *ctx, const char *path)
{
	struct json_collect c = {0};
	reset_emit_seen(ctx);
	walk_syms(ctx->root, NULL, cb_json_collect, &c);
	if (c.nr)
		qsort(c.entries, c.nr, sizeof(*c.entries), json_kv_cmp);

	struct sbuf out = SBUF_INIT;
	sbuf_addstr(&out, "{");
	for (size_t i = 0; i < c.nr; i++) {
		sbuf_addstr(&out, i ? ",\n    " : "\n    ");
		json_emit_string(&out, c.entries[i].name);
		sbuf_addstr(&out, ": ");
		emit_json_value(&out, &c.entries[i]);
	}
	sbuf_addstr(&out, c.nr ? "\n}" : "}");

	if (write_file_atomic(path, out.buf, out.len) < 0)
		die_errno("cannot write '%s'", path);
	free(c.entries);
	sbuf_release(&out);
}

/* ================================================================== */
/*  json_menus writer (menu-tree array)                               */
/* ================================================================== */

/* Return the "type" token for a JSON menu entry matching python's
 * kconfgen vocabulary. */
static const char *json_menu_type(const struct kmenu *m)
{
	if (m->kind == KM_MENU)
		return "menu";
	if (m->kind == KM_COMMENT)
		return "comment";
	if (m->kind == KM_CHOICE)
		return "choice";
	if (m->sym) {
		switch (m->sym->type) {
		case KS_BOOL:
			return "bool";
		case KS_INT:
			return "int";
		case KS_HEX:
			return "hex";
		case KS_STRING:
			return "string";
		case KS_FLOAT:
			return "float";
		case KS_UNKNOWN:
			break;
		}
	}
	return "unknown";
}

static const char *json_menu_title(const struct kmenu *m)
{
	if (m->prompt && *m->prompt)
		return m->prompt;
	if (m->sym) {
		for (const struct kprop *p = m->sym->props; p; p = p->next) {
			if (p->kind == KP_PROMPT && p->text)
				return p->text;
		}
	}
	return NULL;
}

static int json_menu_includes(const struct kmenu *m);

static void json_menu_write_node(struct sbuf *out, const struct kc_ctx *ctx,
				 const struct kmenu *m, int level);

/*
 * True when @p m (or any descendant) produces a JSON entry.  Menus
 * and comments with a prompt always go in the tree -- python
 * kconfig_menus.json includes even empty menus like
 * `menu "Configuration for components not included in the build"`
 * whose body only contains an @c osource that currently resolves to
 * nothing.  @c option env=... symbols are also kept (menuconfig
 * displays them) even though they're skipped by sdkconfig.h output.
 *
 * Walks children for every container-like node, including KM_SYM --
 * the parser's implicit-menuconfig finalize step hoists dependent
 * siblings into their parent sym's children list.
 */
static int json_menu_includes(const struct kmenu *m)
{
	if ((m->kind == KM_SYM || m->kind == KM_CHOICE) && m->sym &&
	    !is_pseudo_sym(m->sym))
		return 1;
	if ((m->kind == KM_MENU || m->kind == KM_COMMENT) && m->prompt &&
	    *m->prompt)
		return 1;
	for (const struct kmenu *c = m->children; c; c = c->next)
		if (json_menu_includes(c))
			return 1;
	return 0;
}

/* Emit the children array contents (between '[' and ']'), recursing
 * into nested menus and transparent if-blocks. */
static void json_menu_write_children(struct sbuf *out, const struct kc_ctx *ctx,
				     const struct kmenu *m, int level)
{
	int first = 1;
	for (const struct kmenu *c = m->children; c; c = c->next) {
		if (!json_menu_includes(c))
			continue;
		/*
		 * Dedup KM_SYM / KM_CHOICE entries -- a Kconfig symbol can
		 * back multiple menu nodes (multi-definition blocks); emit
		 * the first and skip the rest so the JSON tree matches
		 * python's one-node-per-symbol shape.
		 */
		if ((c->kind == KM_SYM || c->kind == KM_CHOICE) && c->sym &&
		    c->sym->emit_seen)
			continue;
		if (c->kind == KM_IF) {
			/* Transparent: its children become part of the
			 * parent's children array. */
			/* Recurse with `m = c` to walk c's children. */
			for (const struct kmenu *gc = c->children; gc;
			     gc = gc->next) {
				if (!json_menu_includes(gc))
					continue;
				if ((gc->kind == KM_SYM ||
				     gc->kind == KM_CHOICE) &&
				    gc->sym && gc->sym->emit_seen)
					continue;
				if (!first)
					sbuf_addstr(out, ",\n");
				first = 0;
				if ((gc->kind == KM_SYM ||
				     gc->kind == KM_CHOICE) &&
				    gc->sym)
					gc->sym->emit_seen = 1;
				json_menu_write_node(out, ctx, gc, level);
			}
			continue;
		}
		if (!first)
			sbuf_addstr(out, ",\n");
		first = 0;
		if ((c->kind == KM_SYM || c->kind == KM_CHOICE) && c->sym)
			c->sym->emit_seen = 1;
		json_menu_write_node(out, ctx, c, level);
	}
}

/*
 * Write the single-line string form of a dependency expression, as
 * python's kconfgen produces for the @c depends_on field.  Only
 * serializes symref / comparisons / boolean ops that commonly appear;
 * anything weird falls back to a compact dump.
 */
static void json_emit_dep(struct sbuf *out, const struct kexpr *e)
{
	if (!e) {
		sbuf_addstr(out, "null");
		return;
	}
	struct sbuf s = SBUF_INIT;
	switch (e->op) {
	case KE_SYMREF:
		if (e->sym)
			sbuf_addstr(&s, e->sym->name);
		break;
	case KE_LITERAL:
		if (e->str)
			sbuf_addstr(&s, e->str);
		break;
	case KE_NOT:
		sbuf_addch(&s, '!');
		if (e->l && e->l->op == KE_SYMREF && e->l->sym)
			sbuf_addstr(&s, e->l->sym->name);
		break;
	case KE_AND:
	case KE_OR:
	case KE_EQ:
	case KE_NE:
	case KE_LT:
	case KE_LE:
	case KE_GT:
	case KE_GE: {
		const char *op;
		switch (e->op) {
		case KE_AND:
			op = " && ";
			break;
		case KE_OR:
			op = " || ";
			break;
		case KE_EQ:
			op = " = ";
			break;
		case KE_NE:
			op = " != ";
			break;
		case KE_LT:
			op = " < ";
			break;
		case KE_LE:
			op = " <= ";
			break;
		case KE_GT:
			op = " > ";
			break;
		case KE_GE:
			op = " >= ";
			break;
		default:
			op = " ? ";
			break;
		}
		if (e->l && e->l->op == KE_SYMREF && e->l->sym)
			sbuf_addstr(&s, e->l->sym->name);
		else if (e->l && e->l->op == KE_LITERAL && e->l->str)
			sbuf_addstr(&s, e->l->str);
		sbuf_addstr(&s, op);
		if (e->r && e->r->op == KE_SYMREF && e->r->sym)
			sbuf_addstr(&s, e->r->sym->name);
		else if (e->r && e->r->op == KE_LITERAL && e->r->str)
			sbuf_addstr(&s, e->r->str);
		break;
	}
	}
	if (!s.len) {
		sbuf_addstr(out, "null");
	} else {
		json_emit_string(out, s.buf);
	}
	sbuf_release(&s);
}

/* Duplicate and rtrim a help string for JSON output (python strips
 * trailing whitespace on the serialized help body). */
static char *help_rtrimmed(const char *s)
{
	if (!s)
		return NULL;
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\t' || s[n - 1] == ' ' ||
		     s[n - 1] == '\r'))
		n--;
	return sbuf_strndup(s, n);
}

static void json_menu_write_node(struct sbuf *out, const struct kc_ctx *ctx,
				 const struct kmenu *m, int level)
{
	/*
	 * Python emits plain @c config entries with a "sym-like" shape
	 * (help / name / range fields present).  Menus, comments, and
	 * choices all use a "menu-like" shape: a synthesised id, a null
	 * name, and help / range omitted.
	 */
	int is_sym_entry = m->kind == KM_SYM && m->sym;

	json_indent(out, level);
	sbuf_addstr(out, "{\n");

	/* children: */
	json_indent(out, level + 1);
	sbuf_addstr(out, "\"children\": [");
	/*
	 * KM_SYM entries get a @c children array too, because the
	 * implicit-menuconfig hoisting in kc_parse.c moves dependent
	 * siblings into the parent sym's children list.  Leaf syms
	 * with no hoisted children simply render as `[]`.
	 */
	int has_children = m->children != NULL;
	struct sbuf children = SBUF_INIT;
	if (has_children)
		json_menu_write_children(&children, ctx, m, level + 2);
	if (children.len) {
		sbuf_addstr(out, "\n");
		sbuf_add(out, children.buf, children.len);
		sbuf_addstr(out, "\n");
		json_indent(out, level + 1);
	}
	sbuf_release(&children);
	sbuf_addstr(out, "],\n");

	/* depends_on: symbol's effective_dep for sym entries, menu's
	 * ctx_dep for menus.  Python emits the expression as a string. */
	json_indent(out, level + 1);
	sbuf_addstr(out, "\"depends_on\": ");
	{
		/*
		 * Walk the parent menu chain to collect every enclosing
		 * KM_IF condition, then AND in the symbol's own
		 * KP_DEPENDS props.  Python emits this combined
		 * expression as a single string like "A && B".  Choice
		 * members get a literal `<choice NAME>` sentinel appended
		 * last -- menuconfig uses it to look up the enclosing
		 * choice group without relying on the choice's own
		 * condition expression.
		 */
		struct sbuf acc = SBUF_INIT;
		int first = 1;
		/* Parent chain (nearest-out first, so we build
		 * innermost-first which reads more naturally). */
		for (const struct kmenu *p = m->parent; p; p = p->parent) {
			if (p->kind != KM_IF || !p->dep)
				continue;
			if (!first)
				sbuf_addstr(&acc, " && ");
			first = 0;
			struct sbuf tmp = SBUF_INIT;
			json_emit_dep(&tmp, p->dep);
			if (tmp.len >= 2 && tmp.buf[0] == '"')
				sbuf_add(&acc, tmp.buf + 1, tmp.len - 2);
			else
				sbuf_add(&acc, tmp.buf, tmp.len);
			sbuf_release(&tmp);
		}
		if (is_sym_entry) {
			for (const struct kprop *p = m->sym->props; p;
			     p = p->next) {
				if (p->kind != KP_DEPENDS || !p->expr)
					continue;
				if (!first)
					sbuf_addstr(&acc, " && ");
				first = 0;
				struct sbuf tmp = SBUF_INIT;
				json_emit_dep(&tmp, p->expr);
				if (tmp.len >= 2 && tmp.buf[0] == '"')
					sbuf_add(&acc, tmp.buf + 1,
						 tmp.len - 2);
				else
					sbuf_add(&acc, tmp.buf, tmp.len);
				sbuf_release(&tmp);
			}
		}
		if (is_sym_entry && m->sym->choice_parent) {
			if (!first)
				sbuf_addstr(&acc, " && ");
			/*
			 * The choice symbol is interned under
			 * `__choice:<NAME>` to avoid colliding with a
			 * same-named `config` (e.g. both a choice and a
			 * config symbol called BOOTLOADER_LOG_VERSION
			 * exist).  Strip the prefix here so the sentinel
			 * ends up as `<choice NAME>` exactly as python emits.
			 */
			const char *pn = m->sym->choice_parent->name;
			const char *colon = strchr(pn, ':');
			if (colon && !strncmp(pn, "__choice", 8))
				pn = colon + 1;
			sbuf_addf(&acc, "<choice %s>", pn);
		}
		if (acc.len)
			json_emit_string(out, acc.buf);
		else
			sbuf_addstr(out, "null");
		sbuf_release(&acc);
	}
	sbuf_addstr(out, ",\n");

	/* Sym entries get help / name / range; menus / comments omit them. */
	if (is_sym_entry) {
		json_indent(out, level + 1);
		sbuf_addstr(out, "\"help\": ");
		char *help = NULL;
		for (const struct kprop *p = m->sym->props; p; p = p->next)
			if (p->kind == KP_HELP && p->text) {
				help = help_rtrimmed(p->text);
				break;
			}
		if (help)
			json_emit_string(out, help);
		else
			sbuf_addstr(out, "null");
		free(help);
		sbuf_addstr(out, ",\n");

		json_indent(out, level + 1);
		sbuf_addstr(out, "\"id\": ");
		json_emit_string(out, m->sym->name);
		sbuf_addstr(out, ",\n");

		json_indent(out, level + 1);
		sbuf_addstr(out, "\"name\": ");
		json_emit_string(out, m->sym->name);
		sbuf_addstr(out, ",\n");

		json_indent(out, level + 1);
		sbuf_addstr(out, "\"range\": ");
		/*
		 * Pick the first range property whose @c if condition matches
		 * the current evaluation state (or has no condition).  The
		 * kconfig_menus.json consumer needs the effective bounds for
		 * the selected target, not the first one in declaration order,
		 * and python kconfiglib serialises the matched range here.
		 */
		const struct kprop *rng = NULL;
		for (const struct kprop *p = m->sym->props; p; p = p->next) {
			if (p->kind != KP_RANGE || !p->expr || !p->expr2)
				continue;
			if (p->cond && !kc_expr_bool(p->cond))
				continue;
			rng = p;
			break;
		}
		if (rng) {
			const char *los =
			    rng->expr->op == KE_LITERAL ? rng->expr->str
			    : rng->expr->op == KE_SYMREF && rng->expr->sym
				? rng->expr->sym->name
				: NULL;
			const char *his =
			    rng->expr2->op == KE_LITERAL ? rng->expr2->str
			    : rng->expr2->op == KE_SYMREF && rng->expr2->sym
				? rng->expr2->sym->name
				: NULL;
			/*
			 * Parse both bounds as integers first; if the symbol
			 * type is float (or either bound fails to parse as an
			 * int), re-parse as double so kconfig_menus.json holds
			 * the numeric literal python kconfiglib would emit.
			 */
			char *e;
			long long lv = los ? strtoll(los, &e, 0) : 0;
			int lo_num = los && e != los && !*e;
			long long hv = his ? strtoll(his, &e, 0) : 0;
			int hi_num = his && e != his && !*e;
			if (m->sym->type == KS_FLOAT || !lo_num || !hi_num) {
				double dlo = los ? kc_strtod_c(los, &e) : 0.0;
				int lo_f = los && e != los && !*e;
				double dhi = his ? kc_strtod_c(his, &e) : 0.0;
				int hi_f = his && e != his && !*e;
				if (lo_f && hi_f) {
					sbuf_addf(out, "[\n%*s%g,\n%*s%g\n%*s]",
						  (level + 2) * 4, "", dlo,
						  (level + 2) * 4, "", dhi,
						  (level + 1) * 4, "");
				} else {
					sbuf_addstr(out, "null");
				}
			} else {
				sbuf_addf(out, "[\n%*s%lld,\n%*s%lld\n%*s]",
					  (level + 2) * 4, "", lv,
					  (level + 2) * 4, "", hv,
					  (level + 1) * 4, "");
			}
		} else {
			sbuf_addstr(out, "null");
		}
		sbuf_addstr(out, ",\n");
	} else {
		/*
		 * Menu / comment / choice: python synthesises the id by
		 * walking the parent chain from root to this node, slug-
		 * ifying each prompt (`\W+` -> `-`, lowercased), joining
		 * with `-`, and appending the filename (slashes replaced
		 * by `-`) and the line number.  Matches esp_kconfiglib's
		 * @c MenuNode.id property.
		 *
		 * Choices also carry an explicit @c help text and their
		 * own @c name; menus / comments leave both null.
		 */
		if (m->kind == KM_CHOICE) {
			json_indent(out, level + 1);
			sbuf_addstr(out, "\"help\": ");
			char *help = NULL;
			if (m->sym) {
				for (const struct kprop *p = m->sym->props; p;
				     p = p->next)
					if (p->kind == KP_HELP && p->text) {
						help = help_rtrimmed(p->text);
						break;
					}
			}
			if (help)
				json_emit_string(out, help);
			else
				sbuf_addstr(out, "null");
			free(help);
			sbuf_addstr(out, ",\n");
		}
		json_indent(out, level + 1);
		sbuf_addstr(out, "\"id\": ");
		struct sbuf id = SBUF_INIT;
		/*
		 * Collect ancestor prompts (root-first) by walking up, so
		 * the slug chain reads as `<outer>-<inner>-<self>`.  Stop
		 * at the KM_ROOT node: python's MenuNode.id walk terminates
		 * at @c node.parent == None, so the mainmenu title (e.g.
		 * "Espressif IoT Development Framework Configuration") is
		 * not included in the slug.
		 */
		const struct kmenu *chain[64];
		int nchain = 0;
		for (const struct kmenu *p = m; p && nchain < 64;
		     p = p->parent) {
			if (p->kind == KM_ROOT)
				break;
			const char *t = json_menu_title(p);
			if (!t || !*t)
				continue;
			chain[nchain++] = p;
		}
		for (int i = nchain - 1; i >= 0; i--) {
			const char *t = json_menu_title(chain[i]);
			/*
			 * Python builds each slug as
			 * `re.sub(r"\W+", "-", prompt).lower()` and then
			 * joins with `-`.  No trailing-dash trim, so a
			 * prompt like "High resolution timer " (with a
			 * trailing space) becomes `high-resolution-timer-`
			 * and produces a visible `--` in the joined id.
			 */
			int in_dash = 1;
			if (id.len) {
				sbuf_addch(&id, '-');
				in_dash = 1;
			}
			for (const char *p = t; *p; p++) {
				unsigned char c = (unsigned char)*p;
				int word = (c >= 'a' && c <= 'z') ||
					   (c >= 'A' && c <= 'Z') ||
					   (c >= '0' && c <= '9') || c == '_';
				if (word) {
					if (c >= 'A' && c <= 'Z')
						c = (unsigned char)(c + 32);
					sbuf_addch(&id, (char)c);
					in_dash = 0;
				} else if (!in_dash) {
					sbuf_addch(&id, '-');
					in_dash = 1;
				}
			}
		}
		const char *file = m->src_file ? m->src_file : "";
		/*
		 * Relativize the source-file path against @c ctx->srctree
		 * first (python stores sourced-file paths relative to
		 * `$srctree` in @c MenuNode.filename), then strip a single
		 * leading '/' and replace remaining '/' with '-'.  python
		 * inserts exactly one '-' between the slug chain and the
		 * path when the path doesn't already start with a dash.
		 *
		 * The top-level --kconfig file itself is NOT relativized --
		 * python stores its filename verbatim from the Kconfig()
		 * constructor argument, skipping the srctree strip that
		 * @c _enter_file runs for @c source'd paths.  Compare
		 * against @c ctx->root_file to identify and leave as-is.
		 */
		if (ctx && ctx->srctree && ctx->srctree[0] &&
		    (!ctx->root_file || strcmp(file, ctx->root_file) != 0)) {
			size_t sn = strlen(ctx->srctree);
			if (!strncmp(file, ctx->srctree, sn))
				file += sn;
		}
		if (*file == '/')
			file++;
		if (id.len)
			sbuf_addch(&id, '-');
		for (const char *p = file; *p; p++)
			sbuf_addch(&id, *p == '/' ? '-' : *p);
		sbuf_addf(&id, "-%d", m->src_line);
		if (id.buf)
			id.buf[id.len] = '\0';
		json_emit_string(out, id.buf ? id.buf : "");
		sbuf_release(&id);
		sbuf_addstr(out, ",\n");
		if (m->kind == KM_CHOICE) {
			json_indent(out, level + 1);
			sbuf_addstr(out, "\"name\": ");
			if (m->sym && m->sym->name) {
				/*
				 * Emit the choice's user-visible name (strip
				 * the `__choice:` interning prefix we added
				 * in the parser to avoid collisions with
				 * same-named configs like
				 * BOOTLOADER_LOG_VERSION).
				 */
				const char *n = m->sym->name;
				const char *colon = strchr(n, ':');
				if (colon && !strncmp(n, "__choice", 8))
					n = colon + 1;
				json_emit_string(out, n);
			} else {
				sbuf_addstr(out, "null");
			}
			sbuf_addstr(out, ",\n");
		}
	}

	/* title. */
	json_indent(out, level + 1);
	sbuf_addstr(out, "\"title\": ");
	const char *title = json_menu_title(m);
	if (title)
		json_emit_string(out, title);
	else
		sbuf_addstr(out, "null");
	sbuf_addstr(out, ",\n");

	/* type. */
	json_indent(out, level + 1);
	sbuf_addstr(out, "\"type\": ");
	json_emit_string(out, json_menu_type(m));
	sbuf_addstr(out, "\n");

	json_indent(out, level);
	sbuf_addstr(out, "}");
}

void kc_write_json_menus(const struct kc_ctx *ctx, const char *path)
{
	struct sbuf out = SBUF_INIT;
	reset_emit_seen(ctx);
	sbuf_addstr(&out, "[");

	struct sbuf body = SBUF_INIT;
	json_menu_write_children(&body, ctx, ctx->root, 1);
	if (body.len) {
		sbuf_addstr(&out, "\n");
		sbuf_add(&out, body.buf, body.len);
		sbuf_addstr(&out, "\n");
	}
	sbuf_release(&body);
	sbuf_addstr(&out, "]");

	if (write_file_atomic(path, out.buf, out.len) < 0)
		die_errno("cannot write '%s'", path);
	sbuf_release(&out);
}

/* ================================================================== */
/*  Existing CMake writer                                             */
/* ================================================================== */

void kc_write_cmake(const struct kc_ctx *ctx, const char *path)
{
	struct sbuf out = SBUF_INIT;
	reset_emit_seen(ctx);

	/* Note the SINGLE space before "Configuration" here -- that's
	 * how python kconfgen renders the cmake banner, unlike the
	 * config / header banners which use a double space. */
	sbuf_addstr(&out, "#\n"
			  "# Automatically generated file. DO NOT EDIT.\n"
			  "# Espressif IoT Development Framework (ESP-IDF) "
			  "Configuration cmake include file\n"
			  "#\n");

	struct sbuf list = SBUF_INIT;
	struct cmake_walk w = {.ctx = ctx, .list = &list, .first = 1};
	walk_syms(ctx->root, &out, cb_cmake, &w);

	/* Trailing enumeration, unquoted, semicolon-separated.  When
	 * there are no deprecated aliases to follow, python kconfgen
	 * does NOT emit a newline after this final line; otherwise the
	 * deprecated block's own leading "\n" provides the separator. */
	sbuf_addf(&out, "set(CONFIGS_LIST %s)", list.buf);
	sbuf_release(&list);
	emit_deprecated_cmake(&out, ctx);

	if (write_file_atomic(path, out.buf, out.len) < 0)
		die_errno("cannot write '%s'", path);

	sbuf_release(&out);
}
