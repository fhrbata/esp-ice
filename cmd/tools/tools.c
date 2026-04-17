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
	static const struct cmd_manual manual = {.name = "ice tools list"};
	struct option opts[] = {OPT_END()};
	struct sbuf tools_dir = SBUF_INIT;

	parse_options(argc, argv, opts, &manual);

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
	static const struct cmd_manual manual = {.name = "ice tools info"};
	struct option opts[] = {OPT_END()};

	parse_options(argc, argv, opts, &manual);

	printf("Tools path: %s\n", ice_home());
	printf("Platform:   %s-%s\n", ICE_PLATFORM_OS, ICE_PLATFORM_ARCH);
	return cmd_tools_list(argc, argv);
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

static subcmd_fn tools_fn;

static const struct option cmd_tools_opts[] = {
    OPT_SUBCOMMAND("install", &tools_fn, cmd_install,
		   "download and install tools from a manifest"),
    OPT_SUBCOMMAND("list", &tools_fn, cmd_tools_list, "list installed tools"),
    OPT_SUBCOMMAND("info", &tools_fn, cmd_tools_info,
		   "show tool paths and installed tools"),
    OPT_END(),
};

/* clang-format off */
static const struct cmd_manual manual = {
	.name = "ice tools",
	.summary = "manage ESP-IDF toolchains",

	.description =
	H_PARA("Manage ESP-IDF toolchains (compilers, debuggers, etc.).")
	H_PARA("Run @b{ice tools <subcommand> --help} for details."),

	.examples =
	H_EXAMPLE("ice tools install ~/work/esp-idf/tools/tools.json")
	H_EXAMPLE("ice tools install --target esp32s3 tools/tools.json")
	H_EXAMPLE("ice tools list")
	H_EXAMPLE("ice tools info"),
};
/* clang-format on */

int cmd_tools(int argc, const char **argv)
{
	argc = parse_options(argc, argv, cmd_tools_opts, &manual);
	if (tools_fn)
		return tools_fn(argc, argv);

	die("expected a subcommand. See 'ice tools --help'.");
}
