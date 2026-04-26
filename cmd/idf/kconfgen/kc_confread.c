/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_confread.c
 * @brief sdkconfig loader: Makefile-style `CONFIG_X=value` / `# is not set`.
 *
 * Reads one file per call, accumulates values onto the symbol table
 * (latest-wins layering when multiple files are loaded), honours the
 * `# default:` ESP-IDF pragma that distinguishes "user-set" from
 * "round-tripped default", and rewrites deprecated names through any
 * loaded rename table before assignment.
 *
 * Tolerant by design: missing files are treated as empty (python
 * kconfgen parity for first-run @c idf.py set-target), and unknown
 * CONFIG_* names are silently dropped so stale or forward-compatible
 * defaults don't block the build.  Load-time type validation rejects
 * malformed values with a deferred warning rather than failing hard.
 *
 * @c kc_load_env_file (JSON object or Makefile-style lines) lives here
 * with the other loaders; it is declared in @c kc_io.h for callers.
 */
#include "ice.h"
#include "kc_ast.h"
#include "kc_eval.h"
#include "kc_io.h"
#include "kc_private.h"

/* ================================================================== */
/*  Env file loader                                                   */
/* ================================================================== */

void kc_load_env_file(struct svec *env, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0)
		die_errno("cannot read env-file '%s'", path);

	const char *p = sb.buf;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;

	if (*p == '{') {
		struct json_value *root = json_parse(sb.buf, sb.len);
		if (!root || json_type(root) != JSON_OBJECT)
			die("env-file '%s' is not a JSON object", path);
		for (int i = 0; i < root->u.object.nr; i++) {
			const struct json_member *m =
			    &root->u.object.members[i];
			const char *val;
			struct sbuf tmp = SBUF_INIT;
			switch (json_type(m->value)) {
			case JSON_STRING:
				val = json_as_string(m->value);
				sbuf_addf(&tmp, "%s=%s", m->key,
					  val ? val : "");
				break;
			case JSON_BOOL:
				sbuf_addf(&tmp, "%s=%s", m->key,
					  json_as_bool(m->value) ? "y" : "n");
				break;
			case JSON_NUMBER:
				sbuf_addf(&tmp, "%s=%lld", m->key,
					  (long long)json_as_number(m->value));
				break;
			case JSON_NULL:
				sbuf_addf(&tmp, "%s=", m->key);
				break;
			default:
				sbuf_release(&tmp);
				continue; /* arrays / objects -- skip */
			}
			svec_push(env, tmp.buf);
			sbuf_release(&tmp);
		}
		json_free(root);
	} else {
		size_t pos = 0;
		char *line;
		while ((line = sbuf_getline(sb.buf, sb.len, &pos)) != NULL) {
			while (*line == ' ' || *line == '\t')
				line++;
			if (!*line || *line == '#')
				continue;
			svec_push(env, line);
		}
	}
	sbuf_release(&sb);
}

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

/*
 * Verify @p val is a syntactically valid payload for a symbol of
 * @p type.  Returns 1 on match; 0 and leaves @c errno untouched on
 * mismatch so the caller can drop the line with a notification.
 * A typeless (KS_UNKNOWN) symbol accepts anything -- the evaluator
 * will sort it out downstream.
 */
static int value_matches_type(const char *val, enum ksym_type type)
{
	char *end;
	int saved_errno;

	switch (type) {
	case KS_BOOL:
		return !strcmp(val, "y") || !strcmp(val, "n");
	case KS_INT:
		saved_errno = errno;
		errno = 0;
		(void)strtol(val, &end, 10);
		if (errno == ERANGE) {
			errno = saved_errno;
			return 0;
		}
		errno = saved_errno;
		return end != val && *end == '\0';
	case KS_HEX:
		if (val[0] != '0' || (val[1] != 'x' && val[1] != 'X'))
			return 0;
		saved_errno = errno;
		errno = 0;
		(void)strtoul(val, &end, 16);
		if (errno == ERANGE) {
			errno = saved_errno;
			return 0;
		}
		errno = saved_errno;
		return end != val && *end == '\0';
	case KS_FLOAT:
		saved_errno = errno;
		errno = 0;
		(void)kc_strtod_c(val, &end);
		if (errno == ERANGE) {
			errno = saved_errno;
			return 0;
		}
		errno = saved_errno;
		return end != val && *end == '\0';
	case KS_STRING:
	case KS_UNKNOWN:
		return 1;
	}
	return 1;
}

/*
 * Set @p name's value but keep @c user_set clear -- treat the line as
 * a built-in default rather than user input.  This is the sink for
 * sdkconfig lines that follow a `# default:` pragma (python kconfgen's
 * ESP extension): the value itself comes from a previous auto-generation
 * so re-seeding it into cur_val keeps round-trips stable, but marking
 * it user_set would make the symbol survive evaluation even when its
 * `depends on` chain later goes false.
 */
static void set_default_seeded(struct kc_ctx *ctx, const char *name,
			       const char *val)
{
	struct ksym *s = smap_get(&ctx->symtab, name);
	if (!s)
		return;
	free(s->cur_val);
	s->cur_val = sbuf_strdup(val);
	/* s->user_set stays 0; mark default_seeded so the evaluator can
	 * honour KCONFIG_DEFAULTS_POLICY=sdkconfig and emit can still
	 * write the `# default:` pragma regardless of policy. */
	s->default_seeded = 1;
}

static void process_config_line(struct kc_ctx *ctx, char *line,
				int *default_pending)
{
	/* Skip leading whitespace. */
	while (*line == ' ' || *line == '\t')
		line++;
	if (!*line)
		return;

	/* `# default:` pragma marks the next CONFIG_* line as
	 * default-seeded rather than user-set. */
	if (*line == '#') {
		const char *p = line + 1;
		while (*p == ' ' || *p == '\t')
			p++;
		if (!strncmp(p, "default:", 8)) {
			const char *q = p + 8;
			while (*q == ' ' || *q == '\t')
				q++;
			if (!*q) {
				*default_pending = 1;
				return;
			}
		}

		/* `# CONFIG_X is not set` -> X = n */
		if (strncmp(p, KC_CONFIG_PREFIX, KC_CONFIG_PREFIX_LEN) != 0) {
			*default_pending = 0;
			return;
		}
		const char *name = p + KC_CONFIG_PREFIX_LEN;
		const char *end = name;
		while (*end && *end != ' ' && *end != '\t')
			end++;
		const char *tail = end;
		while (*tail == ' ' || *tail == '\t')
			tail++;
		if (strcmp(tail, "is not set") != 0) {
			*default_pending = 0;
			return;
		}
		char *name_copy = sbuf_strndup(name, (size_t)(end - name));
		char *val = sbuf_strdup("n");
		kc_rename_translate(ctx, &name_copy, &val);
		if (*default_pending) {
			set_default_seeded(ctx, name_copy, val);
		} else {
			/*
			 * A pragma-less `# CONFIG_X is not set` line in the
			 * deprecated-rename block shouldn't downgrade an
			 * already-default-seeded primary entry back to user-
			 * set -- python kconfgen keeps the `# default:` marker
			 * even after the rename alias round-trips through the
			 * loader.  If the symbol hasn't been seen yet OR its
			 * cur_val genuinely disagrees, treat the line as a
			 * user override; otherwise leave the existing
			 * user_set / cur_val alone.
			 */
			struct ksym *s = smap_get(&ctx->symtab, name_copy);
			if (!s || !s->cur_val ||
			    (strcmp(s->cur_val, val) != 0 && !s->user_set))
				kc_sym_set_user(ctx, name_copy, val);
			/* else: keep current state (user_set preserved). */
		}
		*default_pending = 0;
		free(name_copy);
		free(val);
		return;
	}

	/* `CONFIG_NAME=value` */
	if (strncmp(line, KC_CONFIG_PREFIX, KC_CONFIG_PREFIX_LEN) != 0) {
		*default_pending = 0;
		return;
	}
	char *eq = strchr(line, '=');
	if (!eq) {
		*default_pending = 0;
		return;
	}

	size_t name_len = (size_t)(eq - (line + KC_CONFIG_PREFIX_LEN));
	char *name = sbuf_strndup(line + KC_CONFIG_PREFIX_LEN, name_len);
	const char *rhs = eq + 1;

	/* Empty RHS on a bool shortcut means "n" (python parity). */
	char *val;
	if (!*rhs)
		val = sbuf_strdup("n");
	else if (*rhs == '"')
		val = unquote_value(rhs);
	else
		val = sbuf_strdup(rhs);

	kc_rename_translate(ctx, &name, &val);

	/*
	 * Reject malformed CONFIG_* lines here rather than letting a
	 * typo round-trip through the writer.  Unknown symbols are
	 * still silently dropped downstream (python parity); we only
	 * validate once we know the target's declared type.
	 */
	{
		struct ksym *s = smap_get(&ctx->symtab, name);
		if (s && !value_matches_type(val, s->type)) {
			kc_ctx_notify(ctx,
				      "value '%s' is not a valid %s "
				      "for CONFIG_%s; ignoring",
				      val,
				      s->type == KS_INT	    ? "int"
				      : s->type == KS_HEX   ? "hex"
				      : s->type == KS_FLOAT ? "float"
				      : s->type == KS_BOOL  ? "bool"
							    : "value",
				      name);
			*default_pending = 0;
			free(name);
			free(val);
			return;
		}
	}

	if (*default_pending) {
		set_default_seeded(ctx, name, val);
	} else {
		/*
		 * A pragma-less `CONFIG_X=value` line in the deprecated-
		 * rename block shouldn't promote an already-default-seeded
		 * primary entry to user-set -- e.g. CONFIG_LOG_BOOTLOADER_
		 * LEVEL_INFO=y in the compat block feeds through the rename
		 * table to BOOTLOADER_LOG_LEVEL_INFO which is already =y
		 * via the matching primary entry.  Same idempotent check as
		 * the `# is not set` branch.
		 */
		struct ksym *s = smap_get(&ctx->symtab, name);
		if (!s || !s->cur_val ||
		    (strcmp(s->cur_val, val) != 0 && !s->user_set))
			kc_sym_set_user(ctx, name, val);
		/* else: keep current state (user_set preserved). */
	}
	*default_pending = 0;
	free(name);
	free(val);
}

void kc_load_config(struct kc_ctx *ctx, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0) {
		if (errno != ENOENT)
			die_errno("cannot read config '%s'", path);
		sbuf_release(&sb);
		return;
	}

	size_t pos = 0;
	char *line;
	int default_pending = 0;
	while ((line = sbuf_getline(sb.buf, sb.len, &pos)) != NULL)
		process_config_line(ctx, line, &default_pending);

	sbuf_release(&sb);
}
