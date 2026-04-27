/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/component/component.c
 * @brief "ice idf component" -- component-manager build-system hooks.
 *
 * Replaces IDF's `python -m idf_component_manager.prepare_components`
 * invocations from its CMake integration
 * (`tools/cmake/build.cmake` and `tools/cmake/component.cmake`).
 * The ice_shim installed by @b{ice init} intercepts those Python
 * calls and re-executes into this namespace -- so users never invoke
 * these subcommands directly.
 *
 * Subcommands mirror the Python tool's internal verbs:
 *   @b{prepare} -- resolve / download managed components and emit
 *     the CMake include file the build reads back
 *     (corresponds to Python `prepare_dependencies`).
 *   @b{inject}  -- rewrite the component requires file to add the
 *     managed dependencies to REQUIRES / PRIV_REQUIRES
 *     (corresponds to Python `inject_requirements`).
 *
 * Argument style follows the Python convention with underscore flag
 * names (@c --project_dir, @c --lock_path, ...), so the argv CMake
 * hands to Python works unchanged after the ice_shim rewrite.
 */
#include "ice.h"

extern const struct cmd_desc cmd_idf_component_prepare_desc;
extern const struct cmd_desc cmd_idf_component_inject_desc;

static const struct option cmd_idf_component_opts[] = {OPT_END()};

/* clang-format off */
static const struct cmd_manual idf_component_manual = {
	.name = "ice idf component",
	.summary = "component-manager hooks invoked by the CMake build",

	.description =
	H_PARA("Build-system-only surface -- users do not invoke these "
	       "subcommands directly.  The ice_shim written by "
	       "@b{ice init} intercepts the Python invocations from IDF's "
	       "CMake integration and dispatches them here.")
	H_PARA("Argument style matches the Python tool "
	       "(@b{--project_dir}, @b{--lock_path}, ...) so CMake's "
	       "command line is accepted unchanged."),

	.examples =
	H_EXAMPLE("# invoked indirectly via ice_shim; the CMake call\n"
		  "# `python -m idf_component_manager.prepare_components\n"
		  "#   ... prepare_dependencies ...` becomes\n"
		  "# `ice idf component prepare ...`"),
};
/* clang-format on */

static const struct cmd_desc *const idf_component_subs[] = {
    &cmd_idf_component_prepare_desc,
    &cmd_idf_component_inject_desc,
    NULL,
};

const struct cmd_desc cmd_idf_component_desc = {
    .name = "component",
    .opts = cmd_idf_component_opts,
    .manual = &idf_component_manual,
    .subcommands = idf_component_subs,
};
