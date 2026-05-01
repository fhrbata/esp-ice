/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/size/symbols/symbols.c
 * @brief `ice size symbols COMPONENT` -- per-symbol breakdown for one
 *        component.
 *
 * Forwards to @c cmd_idf_size with @c --archive-details.  Accepts the
 * bare component name (e.g. @c esp_system) and prepends @c "lib" /
 * @c ".a" before forwarding -- the plumbing matches against the .a
 * basename, but users think in IDF component names, so the porcelain
 * does the boilerplate.
 *
 * Tab-completion for the COMPONENT positional walks the active
 * profile's map file and emits each linked archive (see
 * @c complete_size_components()).
 */
#include "cmd/size/size.h"

static const char *opt_format = "table";

/*
 * Bare-name component completion: walk the active profile's map via
 * the shared helper, then strip @c "lib" / @c ".a" so the user sees
 * @c esp_system rather than @c libesp_system.a -- matching what the
 * positional itself accepts (this subcommand prepends the boilerplate
 * before forwarding to the plumbing).  Archives that don't match
 * @c lib<x>.a are skipped silently.
 */
static void emit_component_bare(const char *archive, void *ud)
{
	size_t blen = strlen(archive);
	char comp[256];

	(void)ud;
	if (blen < 5 || strncmp(archive, "lib", 3) != 0 ||
	    strcmp(archive + blen - 2, ".a") != 0)
		return;
	if (blen - 5 >= sizeof(comp))
		return;
	memcpy(comp, archive + 3, blen - 5);
	comp[blen - 5] = '\0';
	complete_emit(comp, NULL);
}

static void complete_components(void)
{
	idf_size_walk_archives(emit_component_bare, NULL);
}

static const struct option cmd_size_symbols_opts[] = {
    OPT_STRING(0, "format", &opt_format, "fmt",
	       "output format: table, tree, csv, json2, raw",
	       idf_size_complete_format),
    OPT_POSITIONAL("component", complete_components),
    OPT_END(),
};

/* clang-format off */
static const struct cmd_manual size_symbols_manual = {
	.name = "ice size symbols",
	.summary = "per-symbol breakdown for one component",

	.description =
	H_PARA("Pivot one component's contribution down to per-symbol "
	       "rows: each STT_FUNC / STT_OBJECT entry that ended up in "
	       "the linked image is shown with its size and the memory "
	       "region it landed in.  Useful for hunting unexpectedly "
	       "large functions or static buffers in a specific library.")
	H_PARA("@b{COMPONENT} is the IDF component name (e.g. "
	       "@b{esp_system}, @b{freertos}); the porcelain prepends "
	       "@b{lib} and @b{.a} before forwarding to "
	       "@b{ice idf size --archive-details}.  Tab-completion "
	       "lists every component currently in the link.")
	H_PARA("Map file and chip target are read from the active "
	       "profile's build directory; the profile must already have "
	       "been built with @b{ice build}."),

	.examples =
	H_EXAMPLE("ice size symbols esp_system")
	H_EXAMPLE("ice size symbols freertos --format csv | sort -t, -k2 -nr"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice size",            "default summary view")
	H_ITEM("ice size files",      "per-source-file breakdown")
	H_ITEM("ice size components", "per-component breakdown"),
};
/* clang-format on */

static int cmd_size_symbols(int argc, const char **argv);

const struct cmd_desc cmd_size_symbols_desc = {
    .name = "symbols",
    .fn = cmd_size_symbols,
    .opts = cmd_size_symbols_opts,
    .manual = &size_symbols_manual,
    .needs = PROJECT_BUILT,
};

static int cmd_size_symbols(int argc, const char **argv)
{
	struct sbuf archive = SBUF_INIT;
	int rc;

	argc = parse_options(argc, argv, &cmd_size_symbols_desc);
	if (argc < 1)
		die("missing component name; see 'ice size symbols --help'");
	if (argc > 1)
		die("too many arguments");

	sbuf_addf(&archive, "lib%s.a", argv[0]);
	rc = size_invoke(opt_format, "--archive-details", archive.buf);
	sbuf_release(&archive);
	return rc;
}
