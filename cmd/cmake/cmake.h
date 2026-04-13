/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/cmake/cmake.h
 * @brief Shared infrastructure for cmake target commands.
 *
 * Every "ice <cmd>" that maps to a cmake/ninja target uses the same
 * two-step pattern: ensure the build directory is configured, then
 * invoke the target.  This header exposes both primitives so that
 * thin wrapper commands (build, clean, flash, ...) stay trivial.
 */
#ifndef CMD_CMAKE_H
#define CMD_CMAKE_H

/**
 * Ensure the build directory has a valid cmake configuration.
 *
 * Creates the build and log directories if needed.  Runs cmake when
 * CMakeCache.txt is missing, when @p force is set, or when any @p defines
 * entry is new or differs from the cached value.
 *
 * Validates that the configured generator matches @p generator; dies
 * on mismatch.  On configure failure, removes CMakeCache.txt to prevent
 * a half-valid cache.
 *
 * @return 0 on success, non-zero on failure.
 */
int ensure_build_directory(const char *build_dir, const char *generator,
			   const struct svec *defines, int verbose, int force);

/**
 * Run a cmake build target with progress logging.
 *
 * Invokes "cmake --build <build_dir> --target <target>" and logs
 * output to <build_dir>/log/<target>.log.
 *
 * @return 0 on success, non-zero on failure.
 */
int run_cmake_target(const char *target, const char *build_dir, int verbose);

#endif /* CMD_CMAKE_H */
