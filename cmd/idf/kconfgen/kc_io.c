/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_io.c
 * @brief sdkconfig load / write implementation.
 */
#include "kc_io.h"
#include "ice.h"
#include "kc_ast.h"
#include "kc_eval.h"

#define CONFIG_PREFIX "CONFIG_"
#define CONFIG_PREFIX_LEN (sizeof(CONFIG_PREFIX) - 1)

/* ================================================================== */
/*  Rename table                                                      */
/* ================================================================== */

/*
 * Look up @p name in the context's rename table and, if found,
 * translate @p *name_inout (which holds the name without CONFIG_
 * prefix) and flip @p *val_inout when the entry has invert=1.
 * Returns 1 on a hit (caller frees / replaces the strings), 0 if
 * @p name wasn't renamed.
 */
static int rename_translate(const struct kc_ctx *ctx, char **name_inout,
			    char **val_inout)
{
	for (size_t i = 0; i < ctx->n_renames; i++) {
		const struct kc_rename *r = &ctx->renames[i];
		if (strcmp(r->old_name, *name_inout) != 0)
			continue;
		free(*name_inout);
		*name_inout = sbuf_strdup(r->new_name);
		if (r->invert && val_inout && *val_inout) {
			const char *flipped = NULL;
			if (!strcmp(*val_inout, "y"))
				flipped = "n";
			else if (!strcmp(*val_inout, "n"))
				flipped = "y";
			if (flipped) {
				free(*val_inout);
				*val_inout = sbuf_strdup(flipped);
			}
		}
		return 1;
	}
	return 0;
}

void kc_load_rename(struct kc_ctx *ctx, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0)
		die_errno("cannot read rename '%s'", path);

	size_t pos = 0;
	char *line;
	while ((line = sbuf_getline(sb.buf, sb.len, &pos)) != NULL) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line || *line == '#')
			continue;

		/* Expect: CONFIG_OLD CONFIG_NEW   or   CONFIG_OLD !CONFIG_NEW
		 */
		char *old_start = line;
		char *space = old_start;
		while (*space && *space != ' ' && *space != '\t')
			space++;
		if (!*space)
			continue;
		*space = '\0';
		char *new_start = space + 1;
		while (*new_start == ' ' || *new_start == '\t')
			new_start++;
		if (!*new_start)
			continue;

		int invert = 0;
		if (*new_start == '!') {
			invert = 1;
			new_start++;
		}

		if (strncmp(old_start, CONFIG_PREFIX, CONFIG_PREFIX_LEN) ||
		    strncmp(new_start, CONFIG_PREFIX, CONFIG_PREFIX_LEN))
			continue; /* not a CONFIG_* rename; skip silently */

		ALLOC_GROW(ctx->renames, ctx->n_renames + 1,
			   ctx->alloc_renames);
		struct kc_rename *r = &ctx->renames[ctx->n_renames++];
		r->old_name = sbuf_strdup(old_start + CONFIG_PREFIX_LEN);
		r->new_name = sbuf_strdup(new_start + CONFIG_PREFIX_LEN);
		r->invert = invert;
	}
	sbuf_release(&sb);
}

/* ================================================================== */
/*  Loader                                                            */
/* ================================================================== */

/*
 * Decode backslash escapes and strip surrounding quotes from a
 * string-valued RHS, returning a newly-allocated owned string.
 * Handles \\, \", \n, \t, \r and leaves unknown escapes literal.
 */
static char *unquote_value(const char *s)
{
	size_t n = strlen(s);
	const char *p = s;
	const char *end = s + n;
	if (n >= 2 && *p == '"' && s[n - 1] == '"') {
		p++;
		end--;
	}
	struct sbuf sb = SBUF_INIT;
	while (p < end) {
		if (*p == '\\' && p + 1 < end) {
			switch (p[1]) {
			case 'n':
				sbuf_addch(&sb, '\n');
				break;
			case 't':
				sbuf_addch(&sb, '\t');
				break;
			case 'r':
				sbuf_addch(&sb, '\r');
				break;
			case '\\':
				sbuf_addch(&sb, '\\');
				break;
			case '"':
				sbuf_addch(&sb, '"');
				break;
			default:
				sbuf_addch(&sb, '\\');
				sbuf_addch(&sb, p[1]);
				break;
			}
			p += 2;
		} else {
			sbuf_addch(&sb, *p++);
		}
	}
	return sbuf_detach(&sb);
}

static void process_config_line(struct kc_ctx *ctx, char *line)
{
	/* Skip leading whitespace. */
	while (*line == ' ' || *line == '\t')
		line++;
	if (!*line)
		return;

	/* `# CONFIG_X is not set` -> X = n */
	if (*line == '#') {
		const char *p = line + 1;
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, CONFIG_PREFIX, CONFIG_PREFIX_LEN) != 0)
			return;
		const char *name = p + CONFIG_PREFIX_LEN;
		const char *end = name;
		while (*end && *end != ' ' && *end != '\t')
			end++;
		const char *tail = end;
		while (*tail == ' ' || *tail == '\t')
			tail++;
		if (strcmp(tail, "is not set") != 0)
			return;
		char *name_copy = sbuf_strndup(name, (size_t)(end - name));
		char *val = sbuf_strdup("n");
		rename_translate(ctx, &name_copy, &val);
		kc_sym_set_user(ctx, name_copy, val);
		free(name_copy);
		free(val);
		return;
	}

	/* `CONFIG_NAME=value` */
	if (strncmp(line, CONFIG_PREFIX, CONFIG_PREFIX_LEN) != 0)
		return;
	char *eq = strchr(line, '=');
	if (!eq)
		return;

	size_t name_len = (size_t)(eq - (line + CONFIG_PREFIX_LEN));
	char *name = sbuf_strndup(line + CONFIG_PREFIX_LEN, name_len);
	const char *rhs = eq + 1;

	/* Empty RHS on a bool shortcut means "n" (python parity). */
	char *val;
	if (!*rhs)
		val = sbuf_strdup("n");
	else if (*rhs == '"')
		val = unquote_value(rhs);
	else
		val = sbuf_strdup(rhs);

	rename_translate(ctx, &name, &val);
	kc_sym_set_user(ctx, name, val);
	free(name);
	free(val);
}

void kc_load_config(struct kc_ctx *ctx, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0)
		die_errno("cannot read config '%s'", path);

	size_t pos = 0;
	char *line;
	while ((line = sbuf_getline(sb.buf, sb.len, &pos)) != NULL)
		process_config_line(ctx, line);

	sbuf_release(&sb);
}

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

static int sym_has_env(const struct ksym *s)
{
	for (const struct kprop *p = s->props; p; p = p->next)
		if (p->kind == KP_ENV)
			return 1;
	return 0;
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
 * True when @p s has at least one KP_PROMPT property.  Python kconfgen
 * emits only user-facing symbols (those with a prompt) -- "hidden"
 * internal bool/int flags that track derived state but have no prompt
 * are not written to any of the generated artefacts.
 */
static int has_prompt(const struct ksym *s)
{
	for (const struct kprop *p = s->props; p; p = p->next)
		if (p->kind == KP_PROMPT)
			return 1;
	return 0;
}

/* True when a symbol's current value is "non-trivial" -- i.e. the
 * evaluator resolved it to something other than the type's zero
 * value.  Used as a secondary emit-filter for hidden symbols:
 * python kconfgen emits a no-prompt bool when its computed value is
 * "y" (but not "n"), and a no-prompt int/hex/string only when its
 * value is non-empty and non-zero.  Matching this keeps derived
 * flags like CONFIG_ESP_CONSOLE_UART in the output while filtering
 * out a forest of hidden zero-value defaults. */
static int has_nontrivial_value(const struct ksym *s)
{
	const char *v = s->cur_val ? s->cur_val : "";
	if (!*v)
		return 0;
	if (s->type == KS_BOOL)
		return !strcmp(v, "y");
	if (s->type == KS_INT || s->type == KS_HEX) {
		char *e;
		long long n = strtoll(v, &e, 0);
		return e != v && *e == '\0' ? n != 0 : 1;
	}
	return 1;
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
	if (sym_has_env(s))
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
	if (!has_p && has_nontrivial_value(s))
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
			sbuf_addf(out, "%s%s=y\n", CONFIG_PREFIX, s->name);
		} else {
			sbuf_addf(out, "# %s%s is not set\n", CONFIG_PREFIX,
				  s->name);
		}
		return;
	}

	if (s->type == KS_STRING) {
		sbuf_addf(out, "%s%s=", CONFIG_PREFIX, s->name);
		emit_quoted(out, val);
		sbuf_addch(out, '\n');
		return;
	}

	/* int / hex / float / unknown: emit raw.  For hex, default to
	 * "0x0" if the stored value happens to be missing a prefix. */
	sbuf_addf(out, "%s%s=%s\n", CONFIG_PREFIX, s->name, val);
}

static void cb_config(struct sbuf *out, const struct ksym *s, void *ud)
{
	(void)ud;
	if (!should_emit(s))
		return;
	emit_symbol(out, s);
}

/*
 * Emit the `# Deprecated options for backward compatibility` block
 * that python kconfgen appends after the main body.  Each rename
 * entry echoes the OLD symbol's "effective" value -- i.e. the
 * current value of the NEW symbol, flipped if the rename has `!`.
 * Only bool symbols produce `y` / `# ... is not set`; non-bool NEW
 * types fall through to an `=<value>` alias.
 */
static void emit_deprecated_config(struct sbuf *out, const struct kc_ctx *ctx)
{
	if (ctx->no_deprecated || !ctx->n_renames)
		return;

	sbuf_addstr(out, "\n"
			 "# Deprecated options for backward compatibility\n");

	for (size_t i = 0; i < ctx->n_renames; i++) {
		const struct kc_rename *r = &ctx->renames[i];
		struct ksym *new_sym = smap_get(&ctx->symtab, r->new_name);
		if (!new_sym)
			continue;
		/* Emit an OLD alias only when the NEW sym is itself
		 * emitted -- otherwise the OLD entry would reference
		 * a CONFIG python also skipped and break diff-based
		 * tooling.  Matches python kconfgen's filter. */
		if (!should_emit(new_sym))
			continue;
		const char *val = new_sym->cur_val ? new_sym->cur_val : "";

		if (new_sym->type == KS_BOOL) {
			int y = !strcmp(val, "y");
			if (r->invert)
				y = !y;
			if (y)
				sbuf_addf(out, "%s%s=y\n", CONFIG_PREFIX,
					  r->old_name);
			else
				sbuf_addf(out, "# %s%s is not set\n",
					  CONFIG_PREFIX, r->old_name);
			continue;
		}
		if (new_sym->type == KS_STRING) {
			sbuf_addf(out, "%s%s=", CONFIG_PREFIX, r->old_name);
			emit_quoted(out, val);
			sbuf_addch(out, '\n');
			continue;
		}
		sbuf_addf(out, "%s%s=%s\n", CONFIG_PREFIX, r->old_name, val);
	}
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

static void emit_deprecated_header(struct sbuf *out, const struct kc_ctx *ctx)
{
	if (ctx->no_deprecated || !ctx->n_renames)
		return;

	sbuf_addstr(out, "\n/* List of deprecated options */\n");
	for (size_t i = 0; i < ctx->n_renames; i++) {
		const struct kc_rename *r = &ctx->renames[i];
		if (r->invert)
			continue; /* can't express inversion as #define */
		struct ksym *new_sym = smap_get(&ctx->symtab, r->new_name);
		if (!new_sym || !should_emit(new_sym))
			continue;
		if (!header_defines_sym(new_sym))
			continue;
		sbuf_addf(out, "#define %s%s %s%s\n", CONFIG_PREFIX,
			  r->old_name, CONFIG_PREFIX, r->new_name);
	}
}

void kc_write_config(const struct kc_ctx *ctx, const char *path)
{
	struct sbuf out = SBUF_INIT;

	/* Matches python kconfgen's header verbatim so diff-based
	 * build-system tooling that compares old/new sdkconfig doesn't
	 * flag a spurious header change.  The double space before
	 * "Project Configuration" mirrors python's output when no
	 * IDF version string is substituted. */
	sbuf_addstr(&out, "#\n"
			  "# Automatically generated file. DO NOT EDIT.\n"
			  "# Espressif IoT Development Framework (ESP-IDF)  "
			  "Project Configuration\n"
			  "#\n");

	walk_syms(ctx->root, &out, cb_config, NULL);
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
			sbuf_addf(out, "#define %s%s 1\n", CONFIG_PREFIX,
				  s->name);
		return;
	}
	if (s->type == KS_STRING) {
		sbuf_addf(out, "#define %s%s ", CONFIG_PREFIX, s->name);
		emit_quoted(out, val);
		sbuf_addch(out, '\n');
		return;
	}
	/* int / hex / float / unknown: emit raw. */
	sbuf_addf(out, "#define %s%s %s\n", CONFIG_PREFIX, s->name, val);
}

static void cb_header(struct sbuf *out, const struct ksym *s, void *ud)
{
	(void)ud;
	if (!should_emit(s))
		return;
	emit_header_symbol(out, s);
}

void kc_write_header(const struct kc_ctx *ctx, const char *path)
{
	struct sbuf out = SBUF_INIT;

	sbuf_addstr(&out, "/*\n"
			  " * Automatically generated file. DO NOT EDIT.\n"
			  " * Espressif IoT Development Framework (ESP-IDF)  "
			  "Configuration Header\n"
			  " */\n"
			  "#pragma once\n");

	walk_syms(ctx->root, &out, cb_header, NULL);
	emit_deprecated_header(&out, ctx);

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

	sbuf_addf(out, "set(%s%s ", CONFIG_PREFIX, s->name);
	emit_quoted(out, effective);
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
	sbuf_addf(w->list, "%s%s", CONFIG_PREFIX, name);
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
static void emit_deprecated_cmake(struct sbuf *out, const struct kc_ctx *ctx)
{
	if (ctx->no_deprecated || !ctx->n_renames)
		return;

	sbuf_addstr(
	    out, "\n# List of deprecated options for backward compatibility\n");

	for (size_t i = 0; i < ctx->n_renames; i++) {
		const struct kc_rename *r = &ctx->renames[i];
		struct ksym *new_sym = smap_get(&ctx->symtab, r->new_name);
		if (!new_sym || !should_emit(new_sym))
			continue;
		const char *val = new_sym->cur_val ? new_sym->cur_val : "";
		const char *effective = val;
		if (new_sym->type == KS_BOOL) {
			int y = !strcmp(val, "y");
			if (r->invert)
				y = !y;
			effective = y ? "y" : "";
		}
		sbuf_addf(out, "set(%s%s ", CONFIG_PREFIX, r->old_name);
		emit_quoted(out, effective);
		sbuf_addstr(out, ")\n");
	}
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
	if (!should_emit(s))
		return;
	ALLOC_GROW(c->entries, c->nr + 1, c->alloc);
	c->entries[c->nr].name = s->name;
	c->entries[c->nr].type = s->type;
	c->entries[c->nr].val = s->cur_val ? s->cur_val : "";
	c->nr++;
}

void kc_write_json(const struct kc_ctx *ctx, const char *path)
{
	struct json_collect c = {0};
	walk_syms(ctx->root, NULL, cb_json_collect, &c);
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

static void json_menu_write_node(struct sbuf *out, const struct kmenu *m,
				 int level);

/*
 * True when @p m (or any descendant) produces a JSON entry.  Menus
 * with no emittable content are skipped entirely so the output stays
 * dense.
 */
static int json_menu_includes(const struct kmenu *m)
{
	if ((m->kind == KM_SYM || m->kind == KM_CHOICE) && m->sym &&
	    !is_pseudo_sym(m->sym) && !sym_has_env(m->sym))
		return 1;
	if (m->kind == KM_MENU || m->kind == KM_COMMENT ||
	    m->kind == KM_CHOICE || m->kind == KM_IF || m->kind == KM_ROOT) {
		for (const struct kmenu *c = m->children; c; c = c->next)
			if (json_menu_includes(c))
				return 1;
	}
	return 0;
}

/* Emit the children array contents (between '[' and ']'), recursing
 * into nested menus and transparent if-blocks. */
static void json_menu_write_children(struct sbuf *out, const struct kmenu *m,
				     int level)
{
	int first = 1;
	for (const struct kmenu *c = m->children; c; c = c->next) {
		if (!json_menu_includes(c))
			continue;
		if (c->kind == KM_IF) {
			/* Transparent: its children become part of the
			 * parent's children array. */
			/* Recurse with `m = c` to walk c's children. */
			for (const struct kmenu *gc = c->children; gc;
			     gc = gc->next) {
				if (!json_menu_includes(gc))
					continue;
				if (!first)
					sbuf_addstr(out, ",\n");
				first = 0;
				json_menu_write_node(out, gc, level);
			}
			continue;
		}
		if (!first)
			sbuf_addstr(out, ",\n");
		first = 0;
		json_menu_write_node(out, c, level);
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

static void json_menu_write_node(struct sbuf *out, const struct kmenu *m,
				 int level)
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
	int has_children = (m->kind == KM_MENU || m->kind == KM_COMMENT ||
			    m->kind == KM_CHOICE);
	struct sbuf children = SBUF_INIT;
	if (has_children)
		json_menu_write_children(&children, m, level + 2);
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
	if (is_sym_entry && m->sym->choice_parent) {
		/* Choice member: python uses the literal "<choice>"
		 * sentinel rather than propagating the choice's
		 * condition -- menuconfig treats it specially. */
		sbuf_addstr(out, "\"<choice>\"");
	} else {
		/*
		 * Walk the parent menu chain to collect every enclosing
		 * KM_IF condition, then AND in the symbol's own
		 * KP_DEPENDS props.  Python emits this combined
		 * expression as a single string like "A && B".
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
		const struct kprop *rng = NULL;
		for (const struct kprop *p = m->sym->props; p; p = p->next)
			if (p->kind == KP_RANGE && p->expr && p->expr2) {
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
			char *e;
			long long lv = los ? strtoll(los, &e, 0) : 0;
			int lo_num = los && e != los && !*e;
			long long hv = his ? strtoll(his, &e, 0) : 0;
			int hi_num = his && e != his && !*e;
			if (lo_num && hi_num)
				sbuf_addf(out, "[\n%*s%lld,\n%*s%lld\n%*s]",
					  (level + 2) * 4, "", lv,
					  (level + 2) * 4, "", hv,
					  (level + 1) * 4, "");
			else
				sbuf_addstr(out, "null");
		} else {
			sbuf_addstr(out, "null");
		}
		sbuf_addstr(out, ",\n");
	} else {
		/*
		 * Menu / comment / choice: python synthesises the id
		 * from the title and source location, with slashes in
		 * the path replaced by '-'.  Matches python's
		 * _node_id() in esp_kconfiglib: "<lower-title>-<path-
		 * with-slashes-as-dashes>-<line>".
		 *
		 * Choices also get explicit @c help / @c name / @c range
		 * fields (all null) -- menu / comment entries omit them.
		 */
		if (m->kind == KM_CHOICE) {
			json_indent(out, level + 1);
			sbuf_addstr(out, "\"help\": null,\n");
		}
		json_indent(out, level + 1);
		sbuf_addstr(out, "\"id\": ");
		struct sbuf id = SBUF_INIT;
		const char *title = json_menu_title(m);
		if (title) {
			for (const char *p = title; *p; p++) {
				if (*p == ' ')
					sbuf_addch(&id, '-');
				else if (*p >= 'A' && *p <= 'Z')
					sbuf_addch(&id, *p + 32);
				else
					sbuf_addch(&id, *p);
			}
		}
		const char *file = m->src_file ? m->src_file : "";
		if (id.len)
			sbuf_addch(&id, '-');
		/* Full path, with '/' -> '-' and a trimmed leading slash. */
		if (*file == '/')
			file++;
		for (const char *p = file; *p; p++)
			sbuf_addch(&id, *p == '/' ? '-' : *p);
		sbuf_addf(&id, "-%d", m->src_line);
		json_emit_string(out, id.buf);
		sbuf_release(&id);
		sbuf_addstr(out, ",\n");
		if (m->kind == KM_CHOICE) {
			json_indent(out, level + 1);
			sbuf_addstr(out, "\"name\": null,\n");
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
	sbuf_addstr(&out, "[");

	struct sbuf body = SBUF_INIT;
	json_menu_write_children(&body, ctx->root, 1);
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
	 * does NOT emit a newline after this final line -- emit NL only
	 * when a deprecated block follows. */
	int has_dep = !ctx->no_deprecated && ctx->n_renames > 0;
	sbuf_addf(&out, "set(CONFIGS_LIST %s)%s", list.buf, has_dep ? "" : "");
	sbuf_release(&list);
	emit_deprecated_cmake(&out, ctx);

	if (write_file_atomic(path, out.buf, out.len) < 0)
		die_errno("cannot write '%s'", path);

	sbuf_release(&out);
}
