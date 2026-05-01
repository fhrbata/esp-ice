/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/size/deps/deps.c
 * @brief `ice size deps` -- archive dependency graph from the Cross
 *        Reference Table.
 *
 * No idf.py equivalent.  Forwards to @c cmd_idf_size with
 * @c --archive-dependencies; useful for hunting why a component
 * pulled in a library you didn't expect.
 */
#include "cmd/size/size.h"

static const char *opt_format = "table";
static int opt_reverse;
static int opt_symbols;

static const struct option cmd_size_deps_opts[] = {
    OPT_STRING(0, "format", &opt_format, "fmt",
	       "output format: table, tree, csv, json2, raw, dot",
	       idf_size_complete_format),
    OPT_BOOL(0, "reverse", &opt_reverse,
	     "show reverse deps (def -> users) instead of forward"),
    OPT_BOOL(0, "symbols", &opt_symbols,
	     "include the symbol list per dependency edge"),
    OPT_END(),
};

/* clang-format off */
static const struct cmd_manual size_deps_manual = {
	.name = "ice size deps",
	.summary = "archive dependency graph",

	.description =
	H_PARA("Walk the linker's Cross Reference Table and emit one "
	       "entry per archive with the libraries it pulls in (or, "
	       "with @b{--reverse}, the libraries that pull it in).  "
	       "Useful for hunting why an unexpected component ended up "
	       "in the linked image.")
	H_PARA("With @b{--symbols}, each dependency edge is annotated "
	       "with the list of symbols that triggered it -- handy when "
	       "you want to know exactly which call into freertos pulled "
	       "in liblog, etc.")
	H_PARA("@b{--format dot} emits a Graphviz digraph; pipe to "
	       "@b{dot -Tsvg} (or similar) to render it.")
	H_PARA("Map file and chip target are read from the active "
	       "profile's build directory; the profile must already have "
	       "been built with @b{ice build}."),

	.examples =
	H_EXAMPLE("ice size deps")
	H_EXAMPLE("ice size deps --reverse --symbols")
	H_EXAMPLE("ice size deps --format dot | dot -Tsvg -o deps.svg"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice size",            "default summary view")
	H_ITEM("ice size components", "per-component breakdown"),
};
/* clang-format on */

static int cmd_size_deps(int argc, const char **argv);

const struct cmd_desc cmd_size_deps_desc = {
    .name = "deps",
    .fn = cmd_size_deps,
    .opts = cmd_size_deps_opts,
    .manual = &size_deps_manual,
    .needs = PROJECT_BUILT,
};

static int cmd_size_deps(int argc, const char **argv)
{
	const char *mapfile;
	const char *target;
	const char *forwarded[12];
	int n = 0;

	argc = parse_options(argc, argv, &cmd_size_deps_desc);
	if (argc > 0)
		die("too many arguments");

	mapfile = config_get("_project.mapfile");
	if (!mapfile)
		die("no map file for this profile; run @b{ice build} first");
	target = config_get("_project.target");

	/*
	 * deps takes more than one extra flag (--archive-dependencies
	 * plus optional --dep-reverse / --dep-symbols), so build the
	 * argv inline rather than going through size_invoke().
	 */
	forwarded[n++] = "ice size";
	if (target) {
		forwarded[n++] = "--target";
		forwarded[n++] = target;
	}
	forwarded[n++] = "--format";
	forwarded[n++] = opt_format;
	forwarded[n++] = "--archive-dependencies";
	if (opt_reverse)
		forwarded[n++] = "--dep-reverse";
	if (opt_symbols)
		forwarded[n++] = "--dep-symbols";
	forwarded[n++] = mapfile;
	forwarded[n] = NULL;

	return cmd_idf_size(n, forwarded);
}
