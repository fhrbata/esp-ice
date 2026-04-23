/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hints.c
 * @brief Match YAML-defined regex rules against a log file.
 *
 * Ports the semantics of ESP-IDF's @c generate_hints_buffer (in
 * @c tools/idf_py_actions/tools.py).  The heavy lifting (YAML DOM,
 * PCRE2) lives behind clean boundaries so this file only wires them
 * together.  CLI presentation and option parsing live in
 * @c cmd/idf/hints/hints.c; @c process_run_progress() reuses the same
 * entry point after a child process exits non-zero.
 */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "hints.h"
#include "ice.h"

/* ------------------------------------------------------------------ */
/*  Log normalization                                                 */
/* ------------------------------------------------------------------ */

/*
 * Mirror of Python's
 *   ' '.join(line.strip() for line in output.splitlines() if line.strip())
 * so regex patterns compiled for the idf.py pipeline match identically.
 */
static void normalize_log(const char *buf, size_t len, struct sbuf *out)
{
	size_t i = 0;

	while (i < len) {
		size_t start = i;
		while (i < len && buf[i] != '\n' && buf[i] != '\r')
			i++;
		size_t end = i;

		if (i < len && buf[i] == '\r')
			i++;
		if (i < len && buf[i] == '\n')
			i++;

		while (start < end && (buf[start] == ' ' || buf[start] == '\t'))
			start++;
		while (end > start &&
		       (buf[end - 1] == ' ' || buf[end - 1] == '\t'))
			end--;

		if (end == start)
			continue;
		if (out->len)
			sbuf_addch(out, ' ');
		sbuf_add(out, buf + start, end - start);
	}
}

/* ------------------------------------------------------------------ */
/*  Template formatting ({} and {N} substitution, {{}} escapes)       */
/* ------------------------------------------------------------------ */

/*
 * Append @p s into @p out, translating Python-style {} / {0} / {1} ...
 * placeholders.  Bare {} consumes the next positional index; {N} pulls
 * from args[N].  {{ and }} are literal braces.  Returns 0 on success,
 * -1 on malformed input (unmatched brace, out-of-range index).  No
 * format spec, conversion, or keyword-style placeholders -- those
 * aren't used by hints.yml.
 */
static int format_template(struct sbuf *out, const char *s, const char **args,
			   int nargs)
{
	int auto_idx = 0;

	for (const char *p = s; *p;) {
		if (p[0] == '{' && p[1] == '{') {
			sbuf_addch(out, '{');
			p += 2;
			continue;
		}
		if (p[0] == '}' && p[1] == '}') {
			sbuf_addch(out, '}');
			p += 2;
			continue;
		}
		if (*p == '{') {
			int idx;
			p++;
			if (*p == '}') {
				idx = auto_idx++;
				p++;
			} else {
				char *end;
				long n = strtol(p, &end, 10);
				if (end == p || *end != '}' || n < 0)
					return -1;
				idx = (int)n;
				p = end + 1;
			}
			if (idx < 0 || idx >= nargs)
				return -1;
			sbuf_addstr(out, args[idx] ? args[idx] : "");
			continue;
		}
		if (*p == '}')
			return -1;
		sbuf_addch(out, *p++);
	}
	return 0;
}

/*
 * Collect a YAML sequence of strings into a newly allocated const char*
 * array suitable for format_template().  Non-string items are rendered
 * as empty strings.  Returns number of items; stores the array in
 * @p *out_arr (caller frees).
 */
static int collect_str_seq(const struct yaml_value *seq, const char ***out_arr)
{
	int n = yaml_seq_size(seq);
	const char **arr;

	if (!n) {
		*out_arr = NULL;
		return 0;
	}
	arr = calloc((size_t)n, sizeof(*arr));
	if (!arr)
		die_errno("calloc");
	for (int i = 0; i < n; i++) {
		const char *s = yaml_as_string(yaml_seq_at(seq, i));
		arr[i] = s ? s : "";
	}
	*out_arr = arr;
	return n;
}

/* ------------------------------------------------------------------ */
/*  PCRE2 helpers                                                     */
/* ------------------------------------------------------------------ */

struct re_match {
	char
	    *groups_joined; /**< Captures joined with ", " (match_to_output). */
	int matched;
};

/*
 * Compile @p pattern, search @p subject once.  On match, if
 * @p want_groups is non-zero, join all non-zeroth captured groups with
 * ", " and hand the heap string back via @p res->groups_joined.
 */
static void pcre_search(const char *pattern, const char *subject,
			int want_groups, struct re_match *res,
			const char *source)
{
	res->matched = 0;
	res->groups_joined = NULL;

	int errcode;
	PCRE2_SIZE erroffset;
	pcre2_code *code = pcre2_compile(
	    (PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
	    PCRE2_DOTALL | PCRE2_MULTILINE, &errcode, &erroffset, NULL);
	if (!code) {
		PCRE2_UCHAR msg[256];
		pcre2_get_error_message(errcode, msg, sizeof(msg));
		warn("%s: regex /%s/ at offset %zu: %s", source, pattern,
		     (size_t)erroffset, (const char *)msg);
		return;
	}

	pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);
	if (!md)
		die_errno("pcre2_match_data_create");

	int rc = pcre2_match(code, (PCRE2_SPTR)subject, PCRE2_ZERO_TERMINATED,
			     0, 0, md, NULL);
	if (rc >= 0) {
		res->matched = 1;
		if (want_groups) {
			/* rc counts the whole match as slot 0; Python's
			 * match.groups() excludes 0, so join slots 1..rc-1. */
			struct sbuf sb = SBUF_INIT;
			PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
			for (size_t g = 1; g < (size_t)rc; g++) {
				PCRE2_SIZE s = ov[2 * g];
				PCRE2_SIZE e = ov[2 * g + 1];
				if (s == PCRE2_UNSET)
					continue;
				if (sb.len)
					sbuf_addstr(&sb, ", ");
				sbuf_add(&sb, subject + s, (size_t)(e - s));
			}
			res->groups_joined = sbuf_detach(&sb);
		}
	}

	pcre2_match_data_free(md);
	pcre2_code_free(code);
}

/* ------------------------------------------------------------------ */
/*  Rule handling                                                     */
/* ------------------------------------------------------------------ */

/*
 * Execute a single rule against the normalized log buffer.  Matched
 * expanded hints are appended to @p out via svec_push().  Returns the
 * number of hints pushed for this rule (0, or >= 1 with variables).
 * @p source is used only for warn() diagnostics.
 */
static int run_rule(const struct yaml_value *rule, const char *log,
		    const char *source, struct svec *out)
{
	const char *re = yaml_as_string(yaml_get(rule, "re"));
	const char *hint = yaml_as_string(yaml_get(rule, "hint"));
	int m2o = yaml_as_bool(yaml_get(rule, "match_to_output"));
	const struct yaml_value *vars = yaml_get(rule, "variables");
	int emitted = 0;

	if (!re || !hint) {
		warn("%s: rule missing 're' or 'hint' -- skipping", source);
		return 0;
	}

	if (yaml_seq_size(vars) > 0) {
		for (int v = 0; v < yaml_seq_size(vars); v++) {
			const struct yaml_value *entry = yaml_seq_at(vars, v);
			const struct yaml_value *rv =
			    yaml_get(entry, "re_variables");
			const struct yaml_value *hv =
			    yaml_get(entry, "hint_variables");
			const char **rs = NULL, **hs = NULL;
			int nr = collect_str_seq(rv, &rs);
			int nh = collect_str_seq(hv, &hs);
			struct sbuf pat = SBUF_INIT, msg = SBUF_INIT;

			if (format_template(&pat, re, rs, nr) < 0) {
				warn("%s: cannot expand 're' template /%s/"
				     " -- skipping",
				     source, re);
				goto next_var;
			}
			if (format_template(&msg, hint, hs, nh) < 0) {
				warn("%s: cannot expand 'hint' template /%s/"
				     " -- skipping",
				     source, hint);
				goto next_var;
			}

			struct re_match m = {0};
			pcre_search(pat.buf, log, 0, &m, source);
			if (m.matched) {
				svec_push(out, msg.buf);
				emitted++;
			}
			free(m.groups_joined);

		next_var:
			sbuf_release(&pat);
			sbuf_release(&msg);
			free(rs);
			free(hs);
		}
		return emitted;
	}

	/* No variables list: compile 're' as-is. */
	struct re_match m = {0};
	pcre_search(re, log, m2o, &m, source);
	if (m.matched) {
		const char *arg = m.groups_joined ? m.groups_joined : "";
		const char *args[1] = {arg};
		struct sbuf msg = SBUF_INIT;
		if (format_template(&msg, hint, args, 1) == 0) {
			svec_push(out, msg.buf);
			emitted++;
		} else {
			warn("%s: cannot expand 'hint' template /%s/"
			     " -- skipping",
			     source, hint);
		}
		sbuf_release(&msg);
	}
	free(m.groups_joined);
	return emitted;
}

/* ------------------------------------------------------------------ */
/*  Public entry point                                                */
/* ------------------------------------------------------------------ */

int hints_scan(const char *hints_yml_path, const char *log_path,
	       struct svec *out)
{
	struct sbuf yml = SBUF_INIT, log = SBUF_INIT, normalized = SBUF_INIT;
	struct yaml_value *root = NULL;
	int emitted = -1;

	if (sbuf_read_file(&yml, hints_yml_path) < 0) {
		warn_errno("%s", hints_yml_path);
		goto done;
	}
	root = yaml_parse(yml.buf, yml.len);
	if (!root) {
		warn("%s: YAML parse error", hints_yml_path);
		goto done;
	}
	if (yaml_type(root) != YAML_SEQ) {
		warn("%s: top level must be a sequence", hints_yml_path);
		goto done;
	}

	if (sbuf_read_file(&log, log_path) < 0) {
		warn_errno("%s", log_path);
		goto done;
	}
	normalize_log(log.buf, log.len, &normalized);

	emitted = 0;
	for (int i = 0; i < yaml_seq_size(root); i++) {
		const struct yaml_value *rule = yaml_seq_at(root, i);
		if (yaml_type(rule) != YAML_MAP) {
			warn("%s: rule %d is not a mapping -- skipping",
			     hints_yml_path, i);
			continue;
		}
		emitted += run_rule(rule, normalized.buf, hints_yml_path, out);
	}

done:
	yaml_free(root);
	sbuf_release(&yml);
	sbuf_release(&log);
	sbuf_release(&normalized);
	return emitted;
}
