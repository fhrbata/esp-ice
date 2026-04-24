/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/docs/docs.c
 * @brief `ice docs` -- built-in documentation guides (namespace dispatcher).
 *
 * Each leaf under this namespace is a help-only entry: its manual page
 * IS the guide content, and its @c fn just calls print_manual() on its
 * own descriptor.  Reuses the existing help renderer end-to-end -- no
 * new prose format or output path.
 */
#include "ice.h"

extern const struct cmd_desc cmd_docs_getting_started_desc;

static const struct option cmd_docs_opts[] = {OPT_END()};

/* clang-format off */
static const struct cmd_manual docs_manual = {
	.name = "ice docs",
	.summary = "built-in documentation guides",

	.description =
	H_PARA("Onboarding guides and concept pages, rendered inside the "
	       "binary so they work offline.  Each guide is shown by its "
	       "own subcommand.")
	H_PARA("Run @b{ice docs <guide>} to view a guide, or "
	       "@b{ice docs <guide> --help} for the same output with "
	       "the standard help frame."),

	.examples =
	H_EXAMPLE("ice docs getting-started"),
};
/* clang-format on */

static const struct cmd_desc *const docs_subs[] = {
    &cmd_docs_getting_started_desc,
    NULL,
};

const struct cmd_desc cmd_docs_desc = {
    .name = "docs",
    .opts = cmd_docs_opts,
    .manual = &docs_manual,
    .subcommands = docs_subs,
};
