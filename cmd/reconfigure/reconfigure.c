/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/reconfigure/reconfigure.c
 * @brief The "ice reconfigure" subcommand -- re-run cmake from scratch.
 */
#include "ice.h"

/* clang-format off */
static const struct cmd_manual manual = {
	.name = "ice reconfigure",
	.summary = "regenerate the build system",

	.description =
	H_PARA("Forces cmake to regenerate the build system, even when "
	       "@b{CMakeCache.txt} is already present and up to date.  "
	       "Re-reads the project's top-level @b{CMakeLists.txt}, "
	       "honours @b{core.generator} and every @b{cmake.define} "
	       "entry, and re-derives project-scope config values "
	       "(@b{target}, @b{mapfile}, @b{elf}) from the regenerated "
	       "cache and @b{project_description.json}.")
	H_PARA("Run this after switching target chips, changing the "
	       "cmake generator, or pulling in top-level @b{CMakeLists} "
	       "changes that ice's on-demand reconfigure heuristic does "
	       "not detect."),

	.examples =
	H_EXAMPLE("ice reconfigure")
	H_EXAMPLE("ice -G \"Unix Makefiles\" reconfigure")
	H_EXAMPLE("ice -D IDF_TARGET=esp32c6 reconfigure"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Build after reconfiguring.")
	H_ITEM("ice clean",
	       "Remove build artifacts; combine with reconfigure for a "
	       "clean rebuild."),
};
/* clang-format on */

int cmd_reconfigure(int argc, const char **argv)
{
	struct option opts[] = {OPT_END()};

	parse_options(argc, argv, opts, &manual);
	return ensure_build_directory(1);
}
