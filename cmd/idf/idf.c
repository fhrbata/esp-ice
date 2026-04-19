/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/idf.c
 * @brief `ice idf` -- bundled ESP-IDF host tools.
 *
 * Namespace for the low-level utilities that ESP-IDF ships as
 * separate Python packages (or build-system helpers).  Each
 * subcommand mirrors the upstream package name where one exists --
 * e.g. @b{ice idf monitor} ↔ @b{esp-idf-monitor},
 * @b{ice idf size} ↔ @b{esp-idf-size}.
 *
 * These commands are explicit (no profile auto-discovery, no project
 * binding); they take their inputs directly on the command line.
 * The profile-aware porcelain wrappers (@b{ice monitor},
 * @b{ice size}, ...) live separately and call into here with
 * arguments derived from the active profile.
 */
#include "ice.h"

/* clang-format off */
static const struct cmd_manual idf_manual = {
	.name = "ice idf",
	.summary = "bundled ESP-IDF host tools",

	.description =
	H_PARA("Low-level utilities shipped with ESP-IDF, exposed under "
	       "a single namespace.  Subcommand names mirror upstream "
	       "Python package names where applicable (e.g. "
	       "@b{ice idf monitor} ↔ @b{esp-idf-monitor}).")
	H_PARA("These commands take their inputs explicitly on the "
	       "command line -- they do not auto-discover from a project "
	       "profile.  For project-aware shortcuts that derive their "
	       "arguments from the active profile, see the porcelain "
	       "wrappers (@b{ice monitor}, @b{ice size}, ...)."),

	.examples =
	H_EXAMPLE("ice idf monitor -p /dev/ttyUSB0")
	H_EXAMPLE("ice idf size build/app.map")
	H_EXAMPLE("ice idf partition-table partitions.csv build/pt.bin")
	H_EXAMPLE("ice idf ldgen --dump app.lf"),
};
/* clang-format on */

static const struct option cmd_idf_opts[] = {OPT_END()};

static const struct cmd_desc *const idf_subs[] = {
    &cmd_configdep_desc,       &cmd_ldgen_desc, &cmd_monitor_desc,
    &cmd_partition_table_desc, &cmd_size_desc,	NULL,
};

const struct cmd_desc cmd_idf_desc = {
    .name = "idf",
    .opts = cmd_idf_opts,
    .manual = &idf_manual,
    .subcommands = idf_subs,
};
