/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/hints/hints.c
 * @brief Match regex patterns from a hints.yml against a log file.
 *
 * Reproduces the semantics of @c generate_hints_buffer from ESP-IDF's
 * @c tools/idf_py_actions/tools.py: read a YAML list of @c {re, hint,
 * match_to_output, variables} rules, normalize the log (join non-blank
 * stripped lines with a single space), and print a @b{HINT:} line for
 * every rule whose regex matches.
 *
 * Uses PCRE2 directly because a handful of hints.yml rules rely on
 * Perl-regex features (negative lookahead, @c \w / @c \d) that POSIX
 * regex cannot express.
 */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "ice.h"

/* clang-format off */
static const struct cmd_manual idf_hints_manual = {
	.name = "ice idf hints",
	.summary = "print hints matching regex rules in a log",

	.description =
	H_PARA("Reads a hints YAML file (@b{<hints.yml>}) of "
	       "@c{{re, hint, ...}} rules and a log file (@b{<log>}), "
	       "then prints a @b{HINT:} line for each rule whose regex "
	       "matches the normalized log.  The log is normalized by "
	       "stripping each line's leading and trailing whitespace, "
	       "dropping empty lines, and joining what remains with "
	       "single spaces -- matching ESP-IDF's "
	       "@c{generate_hints_buffer} preprocessing.")
	H_PARA("Rules may declare @c{match_to_output: True} to pass the "
	       "matched capture groups (joined with @c{, }) into the "
	       "hint template via a @c{{}} placeholder.  Rules may also "
	       "declare a @c{variables:} list that expands the rule into "
	       "one compiled pattern per entry, substituting "
	       "@c{re_variables} into the pattern and @c{hint_variables} "
	       "into the message.")
	H_PARA("Regex syntax is PCRE2; the full ESP-IDF "
	       "@c{tools/idf_py_actions/hints.yml} is supported as-is, "
	       "including Perl-only constructs such as @c{\\w}, @c{\\d}, "
	       "and @c{(?!...)} negative lookaheads."),

	.examples =
	H_EXAMPLE("ice idf hints hints.yml build.log")
	H_EXAMPLE("ice idf hints $IDF_PATH/tools/idf_py_actions/hints.yml build.log"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Drives ESP-IDF's build pipeline; its output is the "
	       "typical input to this command."),
};
/* clang-format on */

static const struct option cmd_idf_hints_opts[] = {
    OPT_POSITIONAL("hints.yml", NULL),
    OPT_POSITIONAL("log", NULL),
    OPT_END(),
};

int cmd_idf_hints(int argc, const char **argv);

const struct cmd_desc cmd_idf_hints_desc = {
    .name = "hints",
    .fn = cmd_idf_hints,
    .opts = cmd_idf_hints_opts,
    .manual = &idf_hints_manual,
};

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
		/* Find end of this line. */
		size_t start = i;
		while (i < len && buf[i] != '\n' && buf[i] != '\r')
			i++;
		size_t end = i;

		/* Consume the newline delimiter (handles CRLF, LF, CR). */
		if (i < len && buf[i] == '\r')
			i++;
		if (i < len && buf[i] == '\n')
			i++;

		/* Trim leading whitespace. */
		while (start < end &&
		       (buf[start] == ' ' || buf[start] == '\t'))
			start++;
		/* Trim trailing whitespace. */
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
 * from args[N]. {{ and }} are literal braces.  Returns 0 on success,
 * -1 on malformed input (unmatched brace, out-of-range index).  No
 * format spec, conversion, or keyword-style placeholders -- those
 * aren't used by hints.yml.
 */
static int format_template(struct sbuf *out, const char *s,
			   const char **args, int nargs)
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
	char *groups_joined; /**< Captures joined with ", " (match_to_output). */
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
			/* rc is the number of captured strings, counting the
			 * whole match as 0; Python's match.groups() excludes
			 * 0, so we join slots 1..rc-1. */
			struct sbuf sb = SBUF_INIT;
			PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
			for (int g = 1; g < rc; g++) {
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
 * Colored to match ESP-IDF's own yellow_print("HINT: ...") convention.
 * Color tokens live in the format string, not in @p msg, so a stray
 * '}' in the hint text cannot unbalance the color block.  When stdout
 * is piped (grep, redirection) use_color_for() strips the tokens and
 * the tests see the plain "HINT: ..." line.
 */
static void emit_hint(const char *msg) { printf("@y{HINT: %s}\n", msg); }

/*
 * Execute a single rule against the normalized log buffer.  @p hints_path
 * is used only for warn() diagnostics.
 */
static void run_rule(const struct yaml_value *rule, const char *log,
		     const char *hints_path)
{
	const char *re = yaml_as_string(yaml_get(rule, "re"));
	const char *hint = yaml_as_string(yaml_get(rule, "hint"));
	int m2o = yaml_as_bool(yaml_get(rule, "match_to_output"));
	const struct yaml_value *vars = yaml_get(rule, "variables");

	if (!re || !hint) {
		warn("%s: rule missing 're' or 'hint' -- skipping", hints_path);
		return;
	}

	if (yaml_seq_size(vars) > 0) {
		for (int v = 0; v < yaml_seq_size(vars); v++) {
			const struct yaml_value *entry = yaml_seq_at(vars, v);
			const struct yaml_value *rv =
			    yaml_get(entry, "re_variables");
			const struct yaml_value *hv =
			    yaml_get(entry, "hint_variables");
			const char **rs, **hs;
			int nr = collect_str_seq(rv, &rs);
			int nh = collect_str_seq(hv, &hs);
			struct sbuf pat = SBUF_INIT, msg = SBUF_INIT;

			if (format_template(&pat, re, rs, nr) < 0) {
				warn("%s: cannot expand 're' template /%s/"
				     " -- skipping",
				     hints_path, re);
				goto next_var;
			}
			if (format_template(&msg, hint, hs, nh) < 0) {
				warn("%s: cannot expand 'hint' template /%s/"
				     " -- skipping",
				     hints_path, hint);
				goto next_var;
			}

			struct re_match m = {0};
			pcre_search(pat.buf, log, 0, &m, hints_path);
			if (m.matched)
				emit_hint(msg.buf);
			free(m.groups_joined);

		next_var:
			sbuf_release(&pat);
			sbuf_release(&msg);
			free(rs);
			free(hs);
		}
		return;
	}

	/* No variables list: compile 're' as-is. */
	struct re_match m = {0};
	pcre_search(re, log, m2o, &m, hints_path);
	if (m.matched) {
		const char *arg = m.groups_joined ? m.groups_joined : "";
		const char *args[1] = {arg};
		struct sbuf msg = SBUF_INIT;
		if (format_template(&msg, hint, args, 1) == 0)
			emit_hint(msg.buf);
		else
			warn("%s: cannot expand 'hint' template -- skipping",
			     hints_path);
		sbuf_release(&msg);
	}
	free(m.groups_joined);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

int cmd_idf_hints(int argc, const char **argv)
{
	struct sbuf yml = SBUF_INIT, log = SBUF_INIT, normalized = SBUF_INIT;
	struct yaml_value *root;
	const char *hints_path, *log_path;

	argc = parse_options(argc, argv, &cmd_idf_hints_desc);
	if (argc != 2)
		die("usage: ice idf hints <hints.yml> <log>");
	hints_path = argv[0];
	log_path = argv[1];

	if (sbuf_read_file(&yml, hints_path) < 0)
		die_errno("%s", hints_path);
	root = yaml_parse(yml.buf, yml.len);
	if (!root)
		die("%s: YAML parse error", hints_path);
	if (yaml_type(root) != YAML_SEQ)
		die("%s: top level must be a sequence", hints_path);

	if (sbuf_read_file(&log, log_path) < 0)
		die_errno("%s", log_path);
	normalize_log(log.buf, log.len, &normalized);

	for (int i = 0; i < yaml_seq_size(root); i++) {
		const struct yaml_value *rule = yaml_seq_at(root, i);
		if (yaml_type(rule) != YAML_MAP) {
			warn("%s: rule %d is not a mapping -- skipping",
			     hints_path, i);
			continue;
		}
		run_rule(rule, normalized.buf, hints_path);
	}

	yaml_free(root);
	sbuf_release(&yml);
	sbuf_release(&log);
	sbuf_release(&normalized);
	return 0;
}
