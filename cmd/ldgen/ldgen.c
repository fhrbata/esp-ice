/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ldgen.c
 * @brief Linker script generation from fragment files.
 */
#include "ice.h"
#include "lf.h"

/* clang-format off */
static const struct cmd_manual ldgen_manual = {
	.name = "ice idf ldgen",
	.summary = "analyse linker fragment (.lf) files",

	.description =
	H_PARA("Parses ESP-IDF linker fragment (@b{.lf}) files and "
	       "reports the number of @b{sections}, @b{schemes}, and "
	       "@b{mappings} found in each.  Use @b{--dump} to print "
	       "the parsed AST for debugging.")
	H_PARA("This is currently an analysis helper -- useful for "
	       "validating @b{.lf} syntax before a full build -- and "
	       "does not yet emit a final linker script.  A fragment is "
	       "a declarative description of which object/archive "
	       "sections land in which memory region; ESP-IDF's linker "
	       "pipeline merges them into a single generated @b{.ld}."),

	.examples =
	H_EXAMPLE("ice idf ldgen components/freertos/linker.lf")
	H_EXAMPLE("ice idf ldgen --dump app.lf")
	H_EXAMPLE("ice idf ldgen app.lf bootloader.lf"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Runs the full build pipeline including ESP-IDF's own "
	       "linker-fragment merger."),
};
/* clang-format on */

/* File-scope so the table can be const and reachable via cmd_struct.opts. */
static int opt_dump;

static const struct option cmd_ldgen_opts[] = {
    OPT_BOOL('d', "dump", &opt_dump, "dump parsed AST"),
    OPT_END(),
};

const struct cmd_desc cmd_ldgen_desc = {
    .name = "ldgen",
    .fn = cmd_ldgen,
    .opts = cmd_ldgen_opts,
    .manual = &ldgen_manual,
};

int cmd_ldgen(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_ldgen_desc);
	if (argc < 1)
		die("no input files; see 'ice idf ldgen --help'");

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

	return 0;
}
