/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/status/status.c
 * @brief The `ice status` subcommand -- show the project's effective state.
 *
 * Reports what the currently active profile binds (chip, idf-path,
 * sdkconfig, build-dir, generator) and what the build directory
 * contributes (CMakeCache presence, derived target / mapfile / elf).
 * Meant to answer "what will happen if I run @b{ice build} right
 * now" without actually running it.
 *
 * Dies via load_profile() when the cwd has no @b{.iceconfig} (git-style
 * "not a repository") or when the selected profile isn't bound.
 */
#include "ice.h"

/* clang-format off */
static const struct cmd_manual status_manual = {
	.name = "ice status",
	.summary = "show effective project state for the active profile",

	.description =
	H_PARA("Report the effective state for the active profile: the "
	       "bindings set by @b{ice init} (chip, idf-path, sdkconfig, "
	       "build-dir, generator), and whether the build directory "
	       "has been configured yet.  Meant to answer \"what will "
	       "@b{ice build} do right now?\" without running it.")
	H_PARA("@b{[<name>]} selects the profile (default: "
	       "@b{default}), the same positional every other "
	       "profile-consuming command takes."),

	.examples =
	H_EXAMPLE("ice status")
	H_EXAMPLE("ice status production"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice init",
	       "Bind (or re-bind) the project for a profile."),
};
/* clang-format on */

static const struct option cmd_status_opts[] = {
    OPT_POSITIONAL("[<name>]", complete_profile_names),
    OPT_END(),
};

const struct cmd_desc cmd_status_desc = {
    .name = "status",
    .fn = cmd_status,
    .opts = cmd_status_opts,
    .manual = &status_manual,
};

static void print_kv(const char *key, const char *value)
{
	printf("  %-11s %s\n", key, value ? value : "@y{(unset)}");
}

int cmd_status(int argc, const char **argv)
{
	const char *name;
	const char *build_dir;
	struct sbuf cache_path = SBUF_INIT;
	int has_cache;

	argc = parse_options(argc, argv, &cmd_status_desc);
	if (argc > 1)
		die("too many arguments");
	name = argc >= 1 ? argv[0] : "default";

	load_profile(name);

	printf("On profile @b{%s}\n\n", name);

	print_kv("chip", config_get("project.chip"));
	print_kv("idf-path", config_get("project.idf-path"));
	print_kv("sdkconfig", config_get("project.sdkconfig"));
	print_kv("build-dir", config_get("project.build-dir"));
	print_kv("generator", config_get("project.generator"));

	build_dir = config_get("project.build-dir");
	sbuf_addf(&cache_path, "%s/CMakeCache.txt", build_dir);
	has_cache = (access(cache_path.buf, F_OK) == 0);
	sbuf_release(&cache_path);

	printf("\nBuild:\n");
	print_kv("configured", has_cache ? "@g{yes} (CMakeCache.txt present)"
					 : "@y{no} (run ice build)");
	if (has_cache) {
		print_kv("target", config_get("project.target"));
		print_kv("elf", config_get("project.elf"));
		print_kv("mapfile", config_get("project.mapfile"));
	}

	return 0;
}
