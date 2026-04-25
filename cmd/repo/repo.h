/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/repo/repo.h
 * @brief Namespace-shared helpers for `ice repo` subcommands.
 *
 * Git primitives, path helpers, reference locking, checkout
 * enumeration, and version gating used by more than one subcommand
 * under cmd/repo/.  Subcommand-private helpers live in their own
 * subcommand directory.
 */
#ifndef CMD_REPO_REPO_H
#define CMD_REPO_REPO_H

#include "git.h"
#include "ice.h"

/** Upstream ESP-IDF URL used by @c ice repo clone when no URL is given. */
#define IDF_CLONE_URL "https://github.com/espressif/esp-idf.git"

/*
 * Minimum supported IDF major.minor -- tags and release branches
 * older than this are filtered from `ice repo list`.
 */
#define IDF_MIN_MAJOR 5
#define IDF_MIN_MINOR 0

/** Return ~/.ice/esp-idf (the ice-managed reference clone). */
const char *repo_reference_path(void);

/** Return ~/.ice/checkouts (parent of named checkouts). */
const char *repo_checkouts_path(void);

/**
 * die() with a helpful hint if ~/.ice/esp-idf does not exist; used by
 * subcommands that operate on the reference.
 */
void repo_ensure_reference(void);

/**
 * Check whether a version string (e.g. "v5.4.1", "5.3") meets the
 * IDF_MIN_MAJOR.IDF_MIN_MINOR threshold.  Skips a leading 'v' or
 * "release/" prefix.
 */
int repo_version_supported(const char *ver);

/** Collect names of subdirectories of ~/.ice/checkouts/, sorted. */
void repo_collect_checkouts(struct svec *out);

#endif /* CMD_REPO_REPO_H */
