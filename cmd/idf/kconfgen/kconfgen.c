/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/kconfgen/kconfgen.c
 * @brief `ice idf kconfgen` -- native Kconfig config generator.
 *
 * Replaces @c python -m kconfgen on the ESP-IDF build hot path.
 * Phase 1 implements parse-only with an @c --dump-ast debug path so
 * we can validate the grammar against real component Kconfigs before
 * wiring up symbol evaluation and config I/O.
 *
 * See docs/design/ice-idf-kconfgen.md (WIP) for the full plan.
 */
#include "ice.h"
#include "kc_ast.h"
#include "kc_lex.h"

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
	H_PARA("Phase 1 ships only parser validation: use "
	       "@b{--dump-ast} on a single Kconfig file to print the "
	       "parsed menu tree.  Symbol evaluation, config I/O, and "
	       "output emission land in later phases."),

	.examples =
	H_EXAMPLE("ice idf kconfgen --kconfig Kconfig --dump-ast")
	H_EXAMPLE("ice idf kconfgen -k components/esp32/Kconfig -d"),
};
/* clang-format on */

static const char *opt_kconfig;
static int opt_dump_ast;

static const struct option cmd_idf_kconfgen_opts[] = {
    OPT_STRING('k', "kconfig", &opt_kconfig, "path",
	       "root Kconfig file (required)", NULL),
    OPT_BOOL('d', "dump-ast", &opt_dump_ast,
	     "parse and dump the AST to stdout"),
    OPT_END(),
};

int cmd_idf_kconfgen(int argc, const char **argv);

const struct cmd_desc cmd_idf_kconfgen_desc = {
    .name = "kconfgen",
    .fn = cmd_idf_kconfgen,
    .opts = cmd_idf_kconfgen_opts,
    .manual = &idf_kconfgen_manual,
};

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

	kc_ctx_release(&ctx);
	return 0;
}
