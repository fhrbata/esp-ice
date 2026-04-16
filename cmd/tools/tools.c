/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tools.c
 * @brief `ice tools` -- manage ESP-IDF toolchains.
 *
 * Subcommands:
 *   install - download and install tools from a tools.json manifest
 *   list    - list installed tools
 *   info    - show tool paths and versions
 */
#include "ice.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Subcommands                                                         */
/* ------------------------------------------------------------------ */

struct list_version_ctx {
	const char *tool_name;
	const char *tool_dir;
};

static int list_version_cb(const char *version, void *ud)
{
	struct list_version_ctx *ctx = ud;
	struct sbuf path = SBUF_INIT;

	sbuf_addf(&path, "%s/%s", ctx->tool_dir, version);
	if (is_directory(path.buf))
		printf("  %-30s %s\n", ctx->tool_name, version);
	sbuf_release(&path);
	return 0;
}

static int list_tool_cb(const char *name, void *ud)
{
	const char *tools_dir = ud;
	struct sbuf path = SBUF_INIT;
	struct list_version_ctx ctx;

	sbuf_addf(&path, "%s/%s", tools_dir, name);
	if (!is_directory(path.buf)) {
		sbuf_release(&path);
		return 0;
	}

	ctx.tool_name = name;
	ctx.tool_dir = path.buf;
	dir_foreach(path.buf, list_version_cb, &ctx);
	sbuf_release(&path);
	return 0;
}

static int cmd_tools_list(int argc, const char **argv)
{
	struct sbuf tools_dir = SBUF_INIT;

	(void)argc;
	(void)argv;

	sbuf_addf(&tools_dir, "%s/tools", ice_home());

	if (access(tools_dir.buf, F_OK) != 0) {
		fprintf(stderr,
			"No tools installed.\n"
			"hint: run @b{ice tools install <tools.json>}\n");
		sbuf_release(&tools_dir);
		return 1;
	}

	dir_foreach(tools_dir.buf, list_tool_cb, (void *)tools_dir.buf);
	sbuf_release(&tools_dir);
	return 0;
}

static int cmd_tools_info(int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	printf("Tools path: %s\n", ice_home());
	printf("Platform:   %s-%s\n", ICE_PLATFORM_OS, ICE_PLATFORM_ARCH);
	return cmd_tools_list(argc, argv);
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

struct tools_sub {
	const char *name;
	int (*fn)(int argc, const char **argv);
	const char *summary;
};

static const struct tools_sub tools_subs[] = {
    {"install", cmd_install, "download and install tools from a manifest"},
    {"list", cmd_tools_list, "list installed tools"},
    {"info", cmd_tools_info, "show tool paths and installed tools"},
    {NULL, NULL, NULL},
};

static const char *tools_usage[] = {
    "ice tools <subcommand> [<args>]",
    NULL,
};

/* clang-format off */
static const struct cmd_manual manual = {
	.description =
	H_PARA("Manage ESP-IDF toolchains (compilers, debuggers, etc.).")
	H_PARA("Run @b{ice tools <subcommand> --help} for details."),

	.examples =
	H_EXAMPLE("ice tools install ~/work/esp-idf/tools/tools.json")
	H_EXAMPLE("ice tools install --target esp32s3 tools/tools.json")
	H_EXAMPLE("ice tools list")
	H_EXAMPLE("ice tools info"),

	.extras =
	H_SECTION("SUBCOMMANDS")
	H_ITEM("install [options] <tools.json>",
	       "Download and install tools from an ESP-IDF tools.json "
	       "manifest.  See @b{ice tools install --help} for options.")
	H_ITEM("list",
	       "List installed tools and their versions.")
	H_ITEM("info",
	       "Show tool paths, platform, and installed tools.")
};
/* clang-format on */

static void print_subs(FILE *fp)
{
	fprintf(fp, "Subcommands:\n");
	for (const struct tools_sub *s = tools_subs; s->name; s++)
		fprintf(fp, "  %-12s  %s\n", s->name, s->summary);
}

int cmd_tools(int argc, const char **argv)
{
	if (argc >= 2 && (!strcmp(argv[1], "--help") ||
			  !strcmp(argv[1], "-h") || !strcmp(argv[1], "help"))) {
		print_manual(argv[0], &manual, NULL, tools_usage);
		return 0;
	}

	if (argc < 2) {
		fprintf(stderr, "usage: ice tools <subcommand> [<args>]\n");
		print_subs(stderr);
		return 1;
	}

	for (const struct tools_sub *s = tools_subs; s->name; s++) {
		if (!strcmp(argv[1], s->name))
			return s->fn(argc - 1, argv + 1);
	}

	fprintf(stderr,
		"ice tools: '%s' is not a subcommand. "
		"See 'ice tools --help'.\n",
		argv[1]);
	print_subs(stderr);
	return 1;
}
