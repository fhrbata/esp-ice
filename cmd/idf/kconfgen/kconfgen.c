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
static int opt_dump_ast;
static int opt_dump_symbols;

static const struct option cmd_idf_kconfgen_opts[] = {
    OPT_STRING('k', "kconfig", &opt_kconfig, "path",
	       "root Kconfig file (required)", NULL),
    OPT_STRING('c', "config", &opt_config, "path",
	       "existing sdkconfig to layer on top of defaults", NULL),
    OPT_STRING_LIST(0, "defaults", &opt_defaults, "path",
		    "defaults file (repeatable; later wins)", NULL),
    OPT_STRING_LIST('o', "output", &opt_output, "fmt:path",
		    "emit fmt (config) to path (repeatable)", NULL),
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

int cmd_idf_kconfgen(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_idf_kconfgen_desc);
	(void)argv;

	if (!opt_kconfig)
		die("missing required --kconfig; see 'ice idf kconfgen "
		    "--help'");

	struct kc_ctx ctx;
	kc_ctx_init(&ctx);
	kc_parse_file(&ctx, opt_kconfig, NULL);

	if (opt_dump_ast)
		kc_ast_dump(&ctx);

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
			if (!strcmp(fmt, "config")) {
				kc_write_config(&ctx, path);
			} else {
				die("--output: unsupported format '%s' (only "
				    "'config' is implemented in this phase)",
				    fmt);
			}
		}
	}

	kc_ctx_release(&ctx);
	svec_clear(&opt_defaults);
	svec_clear(&opt_output);
	return 0;
}
