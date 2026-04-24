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

/* env-file loader lives in kc_io.c as kc_load_env_file -- shared with
 * `ice idf menuconfig`. */

int cmd_idf_kconfgen(int argc, const char **argv)
{
	rewrite_output_pairs(&argc, argv);
	argc = parse_options(argc, argv, &cmd_idf_kconfgen_desc);
	(void)argv;

	if (!opt_kconfig)
		die("missing required --kconfig; see 'ice idf kconfgen "
		    "--help'");

	if (opt_env_file)
		kc_load_env_file(&opt_env, opt_env_file);

	struct kc_ctx ctx;
	kc_ctx_init(&ctx);
	kc_parse_file(&ctx, opt_kconfig,
		      opt_env.nr ? (const char *const *)opt_env.v : NULL);

	/*
	 * Stamp @c IDF_VERSION into the output preambles if the env
	 * supplies one.  Python kconfgen interpolates the same value
	 * into sdkconfig / sdkconfig.h so diff-based build tooling can
	 * spot IDF release changes at a glance.
	 */
	for (size_t i = 0; i < opt_env.nr; i++) {
		const char prefix[] = "IDF_VERSION=";
		if (!strncmp(opt_env.v[i], prefix, sizeof(prefix) - 1)) {
			ctx.idf_version =
			    sbuf_strdup(opt_env.v[i] + sizeof(prefix) - 1);
			break;
		}
	}
	if (!ctx.idf_version) {
		const char *v = getenv("IDF_VERSION");
		if (v)
			ctx.idf_version = sbuf_strdup(v);
	}

	/*
	 * Seed the `# default:` pragma resolution policy from the env.
	 * Python's esp_kconfiglib accepts the strings "sdkconfig" (default)
	 * and "kconfig"; any other value also falls back to the default.
	 * Check the --env table first so a command-line override wins over
	 * the real environment, matching how IDF_VERSION is resolved above.
	 */
	{
		const char *policy = NULL;
		const char prefix[] = "KCONFIG_DEFAULTS_POLICY=";
		for (size_t i = 0; i < opt_env.nr; i++) {
			if (!strncmp(opt_env.v[i], prefix,
				     sizeof(prefix) - 1)) {
				policy = opt_env.v[i] + sizeof(prefix) - 1;
				break;
			}
		}
		if (!policy)
			policy = getenv("KCONFIG_DEFAULTS_POLICY");
		if (policy && !strcmp(policy, "kconfig"))
			ctx.defaults_policy = 1;
	}

	if (opt_dump_ast)
		kc_ast_dump(&ctx);

	/*
	 * Seed values for symbols declared with `option env="VAR"`.
	 * esp_kconfiglib looks up VAR in the caller's environment and
	 * treats the result as the symbol's value -- not as a default
	 * that a Kconfig `default` can override.  Mirror that here by
	 * installing the env value into @c cur_val via the user-set
	 * path (so subsequent eval preserves it) but flipping back to
	 * default_seeded afterwards, so the emit writes a `# default:`
	 * pragma (python marks env-sourced lines the same way).
	 */
	{
		const char *key;
		void *val;
		size_t it = 0;
		while (smap_iter(&ctx.symtab, &it, &key, &val)) {
			struct ksym *s = val;
			const char *env_name = NULL;
			for (const struct kprop *p = s->props; p; p = p->next)
				if (p->kind == KP_ENV) {
					env_name = p->text;
					break;
				}
			if (!env_name)
				continue;
			const char *env_val = NULL;
			size_t nlen = strlen(env_name);
			for (size_t j = 0; j < opt_env.nr; j++) {
				const char *entry = opt_env.v[j];
				const char *eq = strchr(entry, '=');
				if (!eq)
					continue;
				if ((size_t)(eq - entry) == nlen &&
				    !memcmp(entry, env_name, nlen)) {
					env_val = eq + 1;
					break;
				}
			}
			if (!env_val)
				env_val = getenv(env_name);
			if (!env_val) {
				/*
				 * Matches the UndefOptEnv warning
				 * esp_kconfiglib emits when an `option
				 * env="VAR"`'s referenced variable is absent
				 * from the environment; the test suite
				 * substring-greps this line.
				 */
				kc_ctx_notify(
				    &ctx,
				    "warning: %s has 'option env=\"%s\"', but "
				    "the environment variable %s is not set",
				    s->name, env_name, env_name);
				env_val = "";
			}
			free(s->cur_val);
			s->cur_val = sbuf_strdup(env_val);
			/*
			 * Seed as a sticky default: user_set stays clear so
			 * the emit prefixes the line with `# default:`
			 * (python marks env-backed lines with the same
			 * pragma), while default_seeded makes the fixpoint
			 * preserve the env value over any `default ...` the
			 * Kconfig may declare.
			 */
			s->user_set = 0;
			s->default_seeded = 1;
		}
	}

	/* Rename tables must be loaded before defaults / config so the
	 * loader can translate legacy CONFIG_* keys on the fly. */
	for (size_t i = 0; i < opt_renames.nr; i++)
		kc_load_rename(&ctx, opt_renames.v[i]);

	/*
	 * ESP-IDF passes per-component rename files via the
	 * @c COMPONENT_SDKCONFIG_RENAMES env var (semicolon-separated
	 * list) rather than as repeated @c --sdkconfig-rename flags.
	 * Python kconfgen reads that env var directly; we do the same
	 * here so the build-time rename coverage matches without
	 * requiring a cmake wrapper change.
	 */
	for (size_t i = 0; i < opt_env.nr; i++) {
		const char *entry = opt_env.v[i];
		const char prefix[] = "COMPONENT_SDKCONFIG_RENAMES=";
		if (strncmp(entry, prefix, sizeof(prefix) - 1) != 0)
			continue;
		const char *list = entry + sizeof(prefix) - 1;
		while (*list) {
			const char *sep = list;
			while (*sep && *sep != ';')
				sep++;
			if (sep > list) {
				char *path = sbuf_strndup(list, sep - list);
				kc_load_rename(&ctx, path);
				free(path);
			}
			list = *sep ? sep + 1 : sep;
		}
	}

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
			else if (!strcmp(fmt, "savedefconfig"))
				kc_write_min_config(&ctx, path);
			else
				die("--output: unsupported format '%s' "
				    "(expected config, header, cmake, json, "
				    "json_menus, or savedefconfig)",
				    fmt);
		}
	}

	/*
	 * Mirror esp_kconfiglib.report's end-of-run summary so tools /
	 * downstream tests looking for `Status: Finished ...` get the
	 * same signal from the C implementation.  The wording has to
	 * match upstream exactly -- the test suite grep-matches these
	 * lines via substring check against golden `.stderr` fixtures.
	 */
	if (ctx.n_notifications == 0)
		fprintf(stderr, "Status: Finished successfully\n");
	else
		fprintf(stderr, "Status: Finished with notifications\n");

	kc_ctx_release(&ctx);
	svec_clear(&opt_defaults);
	svec_clear(&opt_output);
	svec_clear(&opt_renames);
	svec_clear(&opt_env);
	return 0;
}
