/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/size/size.h
 * @brief Namespace-shared helpers for `ice size` subcommands.
 *
 * Each subcommand resolves the active profile's map file + chip
 * target and forwards to @c cmd_idf_size with view-specific flags
 * (--files / --archives / --archive-details / --archive-dependencies).
 * The shared helper here builds the forwarded argv so each subcommand
 * stays small.
 */
#ifndef CMD_SIZE_SIZE_H
#define CMD_SIZE_SIZE_H

#include "cmd/idf/size/size.h" /* shared completion helpers */
#include "ice.h"

/* Plumbing entry point declared in cmd/idf/size/size.c. */
int cmd_idf_size(int argc, const char **argv);

/* Subcommand descriptors. */
extern const struct cmd_desc cmd_size_files_desc;
extern const struct cmd_desc cmd_size_components_desc;
extern const struct cmd_desc cmd_size_symbols_desc;
extern const struct cmd_desc cmd_size_deps_desc;

/**
 * @brief Run @c cmd_idf_size against the active profile's map file.
 *
 * Resolves the map path and chip target from project config (set by
 * @c setup_project()), then composes argv as:
 *
 *   ice size [--target X] --format F [extra_flag [extra_arg]] MAP
 *
 * @p extra_flag and @p extra_arg may be NULL when not needed
 * (summary view); @p extra_arg may be NULL when only a flag is added
 * (e.g. @c "--files").
 *
 * Dies if no map file is available (the profile hasn't been built).
 */
int size_invoke(const char *fmt, const char *extra_flag, const char *extra_arg);

#endif /* CMD_SIZE_SIZE_H */
