/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/ldgen/ldgen.c
 * @brief Linker script generation from fragment files.
 */
#include "ice.h"

#include "gen.h"
#include "lf.h"
#include "sdkconfig.h"
#include "sinfo.h"

/* clang-format off */
static const struct cmd_manual idf_ldgen_manual = {
	.name = "ice idf ldgen",
	.summary = "generate linker script from fragment (.lf) files",

	.description =
	H_PARA("Parses ESP-IDF linker fragment (@b{.lf}) files and -- when "
	       "given an entity database via @b{--info} or @b{--libraries-"
	       "file}, and a sdkconfig via @b{--config} -- resolves every "
	       "concrete @c{(archive, object, section)} triple against the "
	       "compiled rules and emits explicit per-object placements "
	       "per target.")
	H_PARA("Without any of those inputs, only reports the number of "
	       "@b{sections}, @b{schemes}, and @b{mappings} in each file "
	       "(use @b{--dump} to print the parsed AST).")
	H_PARA("With @b{--input} @i{<template>}, fills "
	       "@c{mapping[<target>]} markers in the template and writes "
	       "the filled script to @b{--output} (or stdout).  Without "
	       "@b{--input}, emits bare per-target blocks.  "
	       "@b{--canonical} dumps the resolved placement map as "
	       "@c{archive|object|section|target} lines for diffing "
	       "against the Python implementation."),

	.examples =
	H_EXAMPLE("ice idf ldgen components/freertos/linker.lf")
	H_EXAMPLE("ice idf ldgen --dump app.lf")
	H_EXAMPLE("ice idf ldgen --info sections.info --config sdkconfig app.lf")
	H_EXAMPLE("ice idf ldgen --libraries-file libs --config sdkconfig \\"
		  "    --input sections.ld.in --output sections.ld app.lf"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Runs the full build pipeline including ESP-IDF's own "
	       "linker-fragment merger."),
};
/* clang-format on */

static int opt_dump;
static int opt_canonical;
static const char *opt_info;
static const char *opt_libraries_file;
static const char *opt_fragments_list;
static const char *opt_config;
static const char *opt_input;
static const char *opt_output;
/* Python-ldgen compatibility shims -- accepted and ignored so that a
 * patched build.ninja COMMAND line produced by replacing `python .../
 * ldgen.py` with `ice idf ldgen` parses without modification. */
static const char *opt_kconfig_unused;
static const char *opt_env_file_unused;
static const char *opt_objdump_unused;

static const struct option cmd_idf_ldgen_opts[] = {
    OPT_BOOL('d', "dump", &opt_dump, "dump parsed AST"),
    OPT_STRING('i', "info", &opt_info, "path",
	       "entity DB from objdump-style sections.info", NULL),
    OPT_STRING(0, "libraries-file", &opt_libraries_file, "path",
	       "file with one archive path per line (AR+ELF path)", NULL),
    OPT_STRING('f', "fragments-list", &opt_fragments_list, "list",
	       "semicolon-separated fragment file paths", NULL),
    OPT_STRING('c', "config", &opt_config, "path",
	       "sdkconfig file for conditional evaluation", NULL),
    OPT_STRING(0, "input", &opt_input, "path",
	       "linker script template (with [target] markers)", NULL),
    OPT_STRING('o', "output", &opt_output, "path",
	       "write emitted script to path (default stdout)", NULL),
    OPT_BOOL(0, "canonical", &opt_canonical,
	     "emit canonical (archive|object|section|target) dump"),
    /* Absorb Python ldgen flags we don't need. */
    OPT_STRING(0, "kconfig", &opt_kconfig_unused, "path",
	       "[compatibility] ignored -- flat sdkconfig is sufficient", NULL),
    OPT_STRING(0, "env-file", &opt_env_file_unused, "path",
	       "[compatibility] ignored -- env vars not consulted", NULL),
    OPT_STRING(0, "objdump", &opt_objdump_unused, "path",
	       "[compatibility] ignored -- AR+ELF parsed in-process", NULL),
    OPT_END(),
};

int cmd_idf_ldgen(int argc, const char **argv);

const struct cmd_desc cmd_idf_ldgen_desc = {
    .name = "ldgen",
    .fn = cmd_idf_ldgen,
    .opts = cmd_idf_ldgen_opts,
    .manual = &idf_ldgen_manual,
};

/* ----- fragments-list and libraries-file loaders ----- */

static void collect_semicolon_list(struct svec *out, const char *s)
{
	const char *p = s;
	while (*p) {
		const char *q = p;
		while (*q && *q != ';')
			q++;
		size_t n = q - p;
		if (n) {
			char *seg = sbuf_strndup(p, n);
			svec_push(out, seg);
			free(seg);
		}
		p = *q ? q + 1 : q;
	}
}

static void collect_file_list(struct svec *out, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0)
		die_errno("cannot read '%s'", path);
	size_t pos = 0;
	char *line;
	while ((line = sbuf_getline(sb.buf, sb.len, &pos)) != NULL) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line || *line == '#')
			continue;
		svec_push(out, line);
	}
	sbuf_release(&sb);
}

static void analyse_mode(int argc, const char **argv)
{
	for (int i = 0; i < argc; i++) {
		struct sbuf sb = SBUF_INIT;

		if (sbuf_read_file(&sb, argv[i]) < 0)
			die_errno("cannot read '%s'", argv[i]);

		struct lf_file *f = lf_parse(sb.buf, argv[i]);

		if (opt_dump)
			lf_file_dump(f);

		int ns = 0, nc = 0, nm = 0;
		for (int j = 0; j < f->n_frags; j++) {
			switch (f->frags[j].kind) {
			case LF_SECTIONS:
				ns++;
				break;
			case LF_SCHEME:
				nc++;
				break;
			case LF_MAPPING:
				nm++;
				break;
			case LF_FRAG_COND:
				break;
			}
		}
		printf("%s: %d sections, %d schemes, %d mappings\n", argv[i],
		       ns, nc, nm);

		lf_file_free(f);
		sbuf_release(&sb);
	}
}

static void resolve_mode(int argc, const char **argv)
{
	/* Build the full fragment list: positional args + --fragments-list. */
	struct svec frags = SVEC_INIT;
	for (int i = 0; i < argc; i++)
		svec_push(&frags, argv[i]);
	if (opt_fragments_list)
		collect_semicolon_list(&frags, opt_fragments_list);
	if (!frags.nr)
		die("no fragment files (positional or --fragments-list)");

	/* Parse every fragment file. */
	struct lf_file **files = calloc(frags.nr, sizeof(*files));
	struct sbuf *bufs = calloc(frags.nr, sizeof(*bufs));
	if (!files || !bufs)
		die_errno("calloc");
	for (size_t i = 0; i < frags.nr; i++) {
		bufs[i] = (struct sbuf)SBUF_INIT;
		if (sbuf_read_file(&bufs[i], frags.v[i]) < 0)
			die_errno("cannot read '%s'", frags.v[i]);
		files[i] = lf_parse(bufs[i].buf, frags.v[i]);
		if (opt_dump)
			lf_file_dump(files[i]);
	}

	/* Load sdkconfig (optional). */
	struct sdkconfig cfg = {0};
	const struct sdkconfig *cfgp = NULL;
	if (opt_config) {
		sdkconfig_load(&cfg, opt_config);
		cfgp = &cfg;
	}

	/* Build entity DB. */
	struct sinfo_db db = {0};
	if (opt_info)
		sinfo_load(&db, opt_info);
	if (opt_libraries_file) {
		struct svec libs = SVEC_INIT;
		collect_file_list(&libs, opt_libraries_file);
		for (size_t i = 0; i < libs.nr; i++)
			sinfo_load_archive(&db, libs.v[i]);
		svec_clear(&libs);
	}

	struct gen_ctx ctx = {0};
	gen_compile(&ctx, files, (int)frags.nr, cfgp);
	gen_resolve(&ctx, &db);

	FILE *out = stdout;
	if (opt_output) {
		out = fopen(opt_output, "w");
		if (!out)
			die_errno("cannot open '%s' for writing", opt_output);
	}

	if (opt_canonical)
		gen_emit_canonical(out, &ctx);
	else if (opt_input)
		gen_fill_template(out, &ctx, opt_input);
	else
		gen_emit(out, &ctx);

	if (opt_output)
		fclose(out);

	gen_free(&ctx);
	sinfo_free(&db);
	if (cfgp)
		sdkconfig_free(&cfg);
	for (size_t i = 0; i < frags.nr; i++) {
		lf_file_free(files[i]);
		sbuf_release(&bufs[i]);
	}
	free(files);
	free(bufs);
	svec_clear(&frags);
}

int cmd_idf_ldgen(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_idf_ldgen_desc);

	int has_resolve_input = opt_info || opt_libraries_file || opt_config ||
				opt_fragments_list || opt_input ||
				opt_canonical;

	if (!has_resolve_input) {
		if (argc < 1)
			die("no input files; see 'ice idf ldgen --help'");
		analyse_mode(argc, argv);
		return 0;
	}

	resolve_mode(argc, argv);
	return 0;
}
