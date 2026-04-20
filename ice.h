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
int cmd_flash(int argc, const char **argv);
int cmd_init(int argc, const char **argv);
int cmd_menuconfig(int argc, const char **argv);

/**
 * @brief Prepend installed tool directories to PATH and set export_vars.
 *
 * Reads @p idf_path / tools/tools.json, finds installed tools under
 * ice_home()/tools/, and modifies the environment so child processes
 * can find compilers, debuggers, and other tools without requiring
 * the user to source export.sh.
 *
 * No-op if @p idf_path is NULL/empty or its tools.json cannot be read.
 */
void setup_tool_env(const char *idf_path);

/* Subcommands -- standalone */
int cmd_complete(int argc, const char **argv);
int cmd_completion(int argc, const char **argv);
int cmd_config(int argc, const char **argv);
int cmd_configdep(int argc, const char **argv);
int cmd_help(int argc, const char **argv);
int cmd_ice(int argc, const char **argv);
int cmd_install(int argc, const char **argv);

/**
 * @brief Install ESP-IDF tools described by a tools.json manifest.
 *
 * Reads @p manifest_path, picks the recommended version of every tool
 * marked @c install: "always" (or just @p tool_filter when non-NULL),
 * downloads and verifies each archive, and extracts under
 * ice_home()/tools/<name>/<version>/.  Tools already installed are
 * skipped unless @p force is non-zero.  When @p target_filter is
 * non-NULL only tools whose @c supported_targets list contains the
 * target (or "all") are installed.
 *
 * @return 0 on success, non-zero if any tool failed to install.
 */
int install_from_manifest(const char *manifest_path, const char *target_filter,
			  const char *tool_filter, int force);
int cmd_ldgen(int argc, const char **argv);
int cmd_monitor(int argc, const char **argv);
int cmd_partition_table(int argc, const char **argv);
int cmd_size(int argc, const char **argv);
int cmd_status(int argc, const char **argv);

/**
 * @brief Descriptor for a command node (leaf or namespace).
 *
 * Every `ice <path>` command -- leaf or pure namespace -- is described
 * by one of these.  ice_dispatch() walks a tree of them to parse
 * options, recurse into subcommands, and fire the matching handler.
 *
 * @p opts is always non-NULL (use `{ OPT_END() }` for an empty table);
 * @p manual is always non-NULL.  @p fn is NULL for pure namespace nodes
 * that exist only to route into their @p subcommands array.  @p
 * subcommands is a NULL-terminated array of descriptor pointers, or
 * NULL for leaves.
 */
struct cmd_desc {
	const char *name;		 /**< "build", "clone", ... */
	int (*fn)(int, const char **);	 /**< Leaf handler; NULL for
					  *   pure namespaces. */
	const struct option *opts;	 /**< Option table (never NULL). */
	const struct cmd_manual *manual; /**< Manual (never NULL). */
	const struct cmd_desc *const *subcommands; /**< NULL-terminated
						    *   array; NULL for
						    *   leaves. */
	void (*extra_complete)(void); /**< Extra completion candidates
				       *   to emit alongside subcommand
				       *   and flag listings (e.g. user
				       *   aliases at the root).  NULL
				       *   if none. */
};

/**
 * @brief Recursively dispatch argv against a descriptor tree.
 *
 * Parses options for @p desc, then: if argv[0] names a subcommand,
 * recurses into it; otherwise fires @p desc->fn.  For a pure
 * namespace with no argv left, dies with a "expected a subcommand"
 * message.
 */
int ice_dispatch(int argc, const char **argv, const struct cmd_desc *desc);

/** Top-level descriptor for `ice` itself.  Points at ice_subs[]. */
extern const struct cmd_desc ice_root_desc;

/** Top-level command descriptors, indexed by ice_subs[] in ice.c. */
extern const struct cmd_desc cmd_build_desc;
extern const struct cmd_desc cmd_clean_desc;
extern const struct cmd_desc cmd_completion_desc;
extern const struct cmd_desc cmd_config_desc;
extern const struct cmd_desc cmd_configdep_desc;
extern const struct cmd_desc cmd_flash_desc;
extern const struct cmd_desc cmd_help_desc;
extern const struct cmd_desc cmd_idf_desc;
extern const struct cmd_desc cmd_image_desc;
extern const struct cmd_desc cmd_init_desc;
extern const struct cmd_desc cmd_ldgen_desc;
extern const struct cmd_desc cmd_menuconfig_desc;
extern const struct cmd_desc cmd_monitor_desc;
extern const struct cmd_desc cmd_partition_table_desc;
extern const struct cmd_desc cmd_repo_desc;
extern const struct cmd_desc cmd_size_desc;
extern const struct cmd_desc cmd_status_desc;
extern const struct cmd_desc cmd_target_desc;
extern const struct cmd_desc cmd_tools_desc;
extern const struct cmd_desc cmd___complete_desc;

/** Top-level option table and manual for `ice`. */
extern const struct option ice_global_opts[];
extern const struct cmd_manual ice_root_manual;

/** Global option values populated by parse_options() on ice_global_opts. */
extern int global_no_color;
extern int global_version;
extern int global_verbose;

/* NULL-terminated chip target lists defined in cmd/set-target/set-target.c. */
extern const char *const ice_supported_targets[];
extern const char *const ice_preview_targets[];

/** One-line summary for chip @p name, or NULL if the chip is unknown. */
const char *ice_chip_summary(const char *name);

/**
 * @brief Emit a completion candidate to stdout.
 *
 * Prints "<name>\t<desc>\n" when a description is provided and the
 * @c completion.descriptions config flag is enabled (its default),
 * or plain "<name>\n" otherwise.  Used by completion callbacks that
 * generate candidates dynamically (aliases, config keys, chip
 * targets, idf checkouts) so the description-disable toggle applies
 * uniformly across every emission site.
 */
void complete_emit(const char *name, const char *desc);

#endif /* ICE_H */
