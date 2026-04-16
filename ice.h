/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ice.h
 * @brief Project-wide header -- include first in every .c file.
 *
 * Pulls in standard headers, the platform abstraction layer, and
 * all common project modules so that every translation unit starts
 * from a common baseline.
 */
#ifndef ICE_H
#define ICE_H

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "ar.h"
#include "cmake.h"
#include "cmakecache.h"
#include "config.h"
#include "csv.h"
#include "elf.h"
#include "error.h"
#include "fs.h"
#include "help.h"
#include "http.h"
#include "json.h"
#include "map.h"
#include "options.h"
#include "pager.h"
#include "platform.h"
#include "sbuf.h"
#include "svec.h"
#include "term.h"

/* Subcommands -- cmake wrappers (see cmake.h) */
int cmd_build(int argc, const char **argv);
int cmd_clean(int argc, const char **argv);
int cmd_cmake(int argc, const char **argv);
int cmd_flash(int argc, const char **argv);
int cmd_fullclean(int argc, const char **argv);
int cmd_menuconfig(int argc, const char **argv);
int cmd_reconfigure(int argc, const char **argv);
int cmd_set_target(int argc, const char **argv);

/**
 * @brief Wipe the contents of the configured build directory.
 *
 * Implements the body of "ice fullclean" and is also called by
 * cmd_set_target().  Returns 0 on success or when there was nothing
 * to clean, non-zero if any entry could not be removed.
 */
int fullclean_run(void);

/* Subcommands -- standalone */
int cmd_complete(int argc, const char **argv);
int cmd_completion(int argc, const char **argv);
int cmd_config(int argc, const char **argv);
int cmd_configdep(int argc, const char **argv);
int cmd_help(int argc, const char **argv);
int cmd_idf(int argc, const char **argv);
int cmd_image(int argc, const char **argv);
int cmd_install(int argc, const char **argv);
int cmd_ldgen(int argc, const char **argv);
int cmd_monitor(int argc, const char **argv);
int cmd_partition_table(int argc, const char **argv);
int cmd_size(int argc, const char **argv);

/*
 * Per-subcommand option tables, lifted to file scope so the completion
 * backend can walk them to list --flag candidates.  Commands with no
 * options don't expose a table; cmd_struct.opts is NULL for those.
 */
extern const struct option cmd_install_opts[];
extern const struct option cmd_config_opts[];
extern const struct option cmd_monitor_opts[];
extern const struct option cmd_image_create_opts[];
extern const struct option cmd_image_info_opts[];
extern const struct option cmd_image_merge_opts[];
extern const struct option cmd_ldgen_opts[];
extern const struct option cmd_partition_table_opts[];
extern const struct option cmd_set_target_opts[];
extern const struct option cmd_size_opts[];

/* NULL-terminated chip target lists defined in cmd/set-target/set-target.c. */
extern const char *const ice_supported_targets[];
extern const char *const ice_preview_targets[];

/**
 * @brief Dispatch entry for a subcommand.
 *
 * Holds the name (what the user types), the handler, a one-line summary
 * used by `ice help`, `ice --help`, and the NAME line of the per-command
 * manual page.  @c opts points at the command's option table (for the
 * completion backend; NULL for commands with no options).  @c hidden
 * commands are dispatchable but kept out of help and completion listings.
 */
struct cmd_struct {
	const char *name;
	int (*fn)(int argc, const char **argv);
	const char *summary;
	const struct option *opts;
	int hidden;
};

/** NULL-terminated (last entry has name == NULL). */
extern const struct cmd_struct ice_commands[];

/** Usage lines, option table, and manual for the top-level `ice`. */
extern const char *ice_global_usage[];
extern const struct option ice_global_opts[];
extern const struct cmd_manual ice_root_manual;

/**
 * @brief Look up the one-line summary for a subcommand.
 * @return Summary string, or NULL if @p name is not a known command.
 */
const char *ice_cmd_summary(const char *name);

#endif /* ICE_H */
