/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/kconfgen/kconfgen.c
 * @brief `ice idf kconfgen` -- native Kconfig config generator.
 *
 * Replaces @c python -m kconfgen on the ESP-IDF build hot path.  Phase
 * 3 adds sdkconfig load / write on top of the parser (Phase 1) and
 * evaluator (Phase 2); header and cmake output formats and the
 * deprecated-rename handling arrive in subsequent phases.
 */
#include "ice.h"
#include "kc_ast.h"
#include "kc_eval.h"
#include "kc_io.h"

/* From kc_parse.c -- not exposed in kc_ast.h because the entry point
 * isn't (yet) used outside the command. */
void kc_parse_file(struct kc_ctx *ctx, const char *path,
		   const char *const *env);

/* clang-format off */
static const struct cmd_manual idf_kconfgen_manual = {
	.name = "ice idf kconfgen",
	.summary = "parse Kconfig and emit sdkconfig / header / cmake",

	.description =
	H_PARA("Native C replacement for @b{python -m kconfgen} used by "
	       "ESP-IDF's build system.  Given @b{--kconfig} pointing at "
	       "the root Kconfig file, walks the full source tree, "
	       "resolves symbol values against any @b{--config} / "
	       "@b{--defaults} inputs, and emits sdkconfig / sdkconfig.h / "
	       "sdkconfig.cmake via @b{--output fmt:path} (repeatable).")
	H_PARA("Load order: @b{--defaults} files are layered first (later "
	       "overrides earlier), then @b{--config} on top.  "
	       "@b{--dump-ast} and @b{--dump-symbols} emit human-readable "
	       "debug output without writing any files."),

	.examples =
	H_EXAMPLE("ice idf kconfgen --kconfig Kconfig --output config:sdkconfig")
	H_EXAMPLE("ice idf kconfgen -k Kconfig -c sdkconfig -o config:sdkconfig.out")
	H_EXAMPLE("ice idf kconfgen -k Kconfig --dump-symbols"),
};
/* clang-format on */

static const char *opt_kconfig;
static const char *opt_config;
static struct svec opt_defaults = SVEC_INIT;
static struct svec opt_output = SVEC_INIT;
static struct svec opt_renames = SVEC_INIT;
static struct svec opt_env = SVEC_INIT;
static const char *opt_env_file;
static const char *opt_list_separator_unused;
static int opt_dump_ast;
static int opt_dump_symbols;
static int opt_no_deprecated;

static const struct option cmd_idf_kconfgen_opts[] = {
    OPT_STRING('k', "kconfig", &opt_kconfig, "path",
	       "root Kconfig file (required)", NULL),
    OPT_STRING('c', "config", &opt_config, "path",
	       "existing sdkconfig to layer on top of defaults", NULL),
    OPT_STRING_LIST(0, "defaults", &opt_defaults, "path",
		    "defaults file (repeatable; later wins)", NULL),
    OPT_STRING_LIST('o', "output", &opt_output, "fmt:path",
		    "emit fmt (config|header|cmake) to path (repeatable)",
		    NULL),
    OPT_STRING_LIST(0, "sdkconfig-rename", &opt_renames, "path",
		    "deprecated->current symbol name mappings (repeatable)",
		    NULL),
    OPT_STRING_LIST('E', "env", &opt_env, "NAME=VAL",
		    "set an env variable for $(VAR) expansion (repeatable)",
		    NULL),
    OPT_STRING(0, "env-file", &opt_env_file, "path",
	       "NAME=VAL lines loaded before Kconfig parse", NULL),
    OPT_STRING(0, "list-separator", &opt_list_separator_unused, "sep",
	       "[compatibility] list-separator hint; ice hardcodes ';'", NULL),
    OPT_BOOL(0, "dont-write-deprecated", &opt_no_deprecated,
	     "skip the deprecated-aliases block in config / header outputs"),
    OPT_BOOL(0, "dump-ast", &opt_dump_ast, "parse and dump the AST to stdout"),
    OPT_BOOL(0, "dump-symbols", &opt_dump_symbols,
	     "evaluate and dump resolved symbol values to stdout"),
    OPT_END(),
};

int cmd_idf_kconfgen(int argc, const char **argv);

const struct cmd_desc cmd_idf_kconfgen_desc = {
    .name = "kconfgen",
    .fn = cmd_idf_kconfgen,
    .opts = cmd_idf_kconfgen_opts,
    .manual = &idf_kconfgen_manual,
};

/*
 * Split an @c fmt:path output spec.  Returns the format substring
 * (inside @p spec) and writes the '/'-separator byte position into
 * @p *sep_out so the caller can treat @p spec + sep + 1 as the path.
 * Dies on missing ':'.
 */
static void split_output_spec(const char *spec, const char **fmt_out,
			      const char **path_out)
{
	const char *colon = strchr(spec, ':');
	if (!colon || colon == spec || !colon[1])
		die("--output expects fmt:path, got '%s'", spec);
	/* Return a heap copy of the fmt substring; the path points into
	 * @p spec verbatim. */
	static char fmt_buf[32];
	size_t n = (size_t)(colon - spec);
	if (n >= sizeof(fmt_buf))
		die("--output format name too long: '%s'", spec);
	memcpy(fmt_buf, spec, n);
	fmt_buf[n] = '\0';
	*fmt_out = fmt_buf;
	*path_out = colon + 1;
}

/*
 * Preprocess argv so the single-arg `--output fmt:path` form the option
 * parser expects can accept python kconfgen's two-arg `--output FMT
 * PATH` form that ESP-IDF's tools/cmake/kconfig.cmake hands us.  For
 * every @c --output / @c -o whose next argv slot has no ':', we
 * combine the pair into a single colon-joined string.  Operates
 * in-place on the (argc, argv) slice the caller passes to
 * parse_options; the stitched strings are stored in a small static
 * rewrite table so they outlive this function.
 */
static void rewrite_output_pairs(int *argc_inout, const char **argv)
{
	static char *stitched[32];
	static int n_stitched;

	int argc = *argc_inout;
	int dst = 0;
	for (int src = 0; src < argc; src++) {
		const char *a = argv[src];
		int is_output = !strcmp(a, "--output") || !strcmp(a, "-o");
		if (is_output && src + 2 < argc &&
		    !strchr(argv[src + 1], ':')) {
			/* Two-arg form: combine. */
			if (n_stitched >= 32)
				die("too many --output options");
			struct sbuf sb = SBUF_INIT;
			sbuf_addf(&sb, "%s:%s", argv[src + 1], argv[src + 2]);
			stitched[n_stitched] = sbuf_detach(&sb);
			argv[dst++] = a;
			argv[dst++] = stitched[n_stitched++];
			src += 2;
			continue;
		}
		argv[dst++] = a;
	}
	*argc_inout = dst;
}

/*
 * Load environment variables from @p path into @p env.
 *
 * Two file formats are accepted -- ESP-IDF's build passes a JSON
 * object (`{"NAME": "value", ...}`), while standalone usage from the
 * command line is typically plain NAME=VAL lines.  The first
 * non-whitespace byte disambiguates: '{' -> JSON parse, anything else
 * -> line-based parse.  Whatever @p env already holds from --env
 * flags is preserved; the file is additive.
 */
static void load_env_file(struct svec *env, const char *path)
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

int cmd_idf_kconfgen(int argc, const char **argv)
{
	rewrite_output_pairs(&argc, argv);
	argc = parse_options(argc, argv, &cmd_idf_kconfgen_desc);
	(void)argv;

	if (!opt_kconfig)
		die("missing required --kconfig; see 'ice idf kconfgen "
		    "--help'");

	if (opt_env_file)
		load_env_file(&opt_env, opt_env_file);

	struct kc_ctx ctx;
	kc_ctx_init(&ctx);
	kc_parse_file(&ctx, opt_kconfig,
		      opt_env.nr ? (const char *const *)opt_env.v : NULL);

	if (opt_dump_ast)
		kc_ast_dump(&ctx);

	/* Rename tables must be loaded before defaults / config so the
	 * loader can translate legacy CONFIG_* keys on the fly. */
	for (size_t i = 0; i < opt_renames.nr; i++)
		kc_load_rename(&ctx, opt_renames.v[i]);
	ctx.no_deprecated = opt_no_deprecated;

	/* Load user-provided values in the python-compatible layering
	 * order: --defaults in CLI order (later wins), then --config. */
	for (size_t i = 0; i < opt_defaults.nr; i++)
		kc_load_config(&ctx, opt_defaults.v[i]);
	if (opt_config)
		kc_load_config(&ctx, opt_config);

	if (opt_dump_symbols || opt_output.nr) {
		kc_eval(&ctx);
		if (opt_dump_symbols)
			kc_symbols_dump(&ctx);
		for (size_t i = 0; i < opt_output.nr; i++) {
			const char *fmt;
			const char *path;
			split_output_spec(opt_output.v[i], &fmt, &path);
			if (!strcmp(fmt, "config"))
				kc_write_config(&ctx, path);
			else if (!strcmp(fmt, "header"))
				kc_write_header(&ctx, path);
			else if (!strcmp(fmt, "cmake"))
				kc_write_cmake(&ctx, path);
			else if (!strcmp(fmt, "json"))
				kc_write_json(&ctx, path);
			else if (!strcmp(fmt, "json_menus"))
				kc_write_json_menus(&ctx, path);
			else
				die("--output: unsupported format '%s' "
				    "(expected config, header, cmake, json, "
				    "or json_menus)",
				    fmt);
		}
	}

	kc_ctx_release(&ctx);
	svec_clear(&opt_defaults);
	svec_clear(&opt_output);
	svec_clear(&opt_renames);
	svec_clear(&opt_env);
	return 0;
}
