/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/idf.c
 * @brief `ice idf` -- bundled ESP-IDF host tools.
 *
 * Namespace for the host-only utilities that ESP-IDF ships as
 * separate Python packages (or build-system helpers).  Each
 * subcommand mirrors the upstream package name where one exists --
 * e.g. @b{ice idf size} ↔ @b{esp-idf-size}.
 *
 * These commands are explicit (no profile auto-discovery, no project
 * binding); they take their inputs directly on the command line.
 * The profile-aware porcelain wrappers (@b{ice size}, ...) live
 * separately and call into here with arguments derived from the
 * active profile.
 */
#include "ice.h"

extern const struct cmd_desc cmd_idf_configdep_desc;
extern const struct cmd_desc cmd_idf_crt_bundle_desc;
extern const struct cmd_desc cmd_idf_hints_desc;
extern const struct cmd_desc cmd_idf_kconfgen_desc;
extern const struct cmd_desc cmd_idf_ldgen_desc;
extern const struct cmd_desc cmd_idf_menuconfig_desc;
extern const struct cmd_desc cmd_idf_partition_table_desc;
extern const struct cmd_desc cmd_idf_size_desc;

/* clang-format off */
static const struct cmd_manual idf_manual = {
	.name = "ice idf",
	.summary = "bundled ESP-IDF host tools",

	.description =
	H_PARA("Host-only utilities shipped with ESP-IDF, exposed under "
	       "a single namespace.  Subcommand names mirror upstream "
	       "Python package names where applicable (e.g. "
	       "@b{ice idf size} ↔ @b{esp-idf-size}).")
	H_PARA("These commands take their inputs explicitly on the "
	       "command line -- they do not auto-discover from a project "
	       "profile.  For project-aware shortcuts that derive their "
	       "arguments from the active profile, see the porcelain "
	       "wrappers (@b{ice size}, ...).")
	H_PARA("Chip-interacting commands (serial monitor, flash, ...) "
	       "live under @b{ice target}, not here."),

	.examples =
	H_EXAMPLE("ice idf size build/app.map")
	H_EXAMPLE("ice idf partition-table partitions.csv build/pt.bin")
	H_EXAMPLE("ice idf ldgen --dump app.lf")
	H_EXAMPLE("ice idf hints hints.yml build.log"),
};
/* clang-format on */

static const struct option cmd_idf_opts[] = {OPT_END()};

static const struct cmd_desc *const idf_subs[] = {
    &cmd_idf_configdep_desc,
    &cmd_idf_crt_bundle_desc,
    &cmd_idf_hints_desc,
    &cmd_idf_kconfgen_desc,
    &cmd_idf_ldgen_desc,
    &cmd_idf_menuconfig_desc,
    &cmd_idf_partition_table_desc,
    &cmd_idf_size_desc,
    NULL,
};

const struct cmd_desc cmd_idf_desc = {
    .name = "idf",
    .opts = cmd_idf_opts,
    .manual = &idf_manual,
    .subcommands = idf_subs,
};
