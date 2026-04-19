/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmake.h
 * @brief Shared cmake orchestration for "ice" commands.
 *
 * The cmake-based "ice" commands (build, flash, menuconfig, clean,
 * ...) all operate on a project profile.  Each calls
 * load_profile(<name>) early to populate global_build_dir /
 * global_generator / global_defines from @b{[project "<name>"]} in
 * @b{.iceconfig}, set up the IDF's tool PATH, and derive
 * project-state config (target, mapfile, elf) from the build dir.
 * Then they call ensure_build_directory() / run_cmake_target() which
 * consume those globals.  Logs land under @b{<build-dir>/log/}.
 */
#ifndef CMAKE_H
#define CMAKE_H

/**
 * Load profile @p name into process state.
 *
 * Reads @b{[project "<name>"]} from the config store and:
 *   - populates @c global_build_dir, @c global_generator,
 *     @c global_defines so the cmake primitives below pick them up;
 *   - prepends the profile's IDF tools to @b{PATH};
 *   - derives @b{target} / @b{mapfile} / @b{elf} from the build
 *     directory's @b{CMakeCache.txt} and @b{project_description.json}.
 *
 * Dies if the profile is not configured (i.e. no @b{ice init} has
 * been run for it).  @p name = "default" loads the default profile.
 */
void load_profile(const char *name);

/**
 * Print profile names from @b{[project "<name>"]} sections of the
 * loaded config to stdout, one per line.  Always emits "default"
 * even when no @b{[project "default"]} section exists yet.
 *
 * Suitable as an OPT_POSITIONAL completion callback for the
 * @b{[<name>]} slot on every profile-consuming command.
 */
void complete_profile_names(void);

/**
 * Configure the build directory.
 *
 * Runs cmake when CMakeCache.txt is missing, when a cmake.define
 * differs from the cached value, or when @p force is non-zero.
 *
 * @return 0 on success, non-zero on failure.
 */
int ensure_build_directory(int force);

/**
 * Ensure the build directory is configured, then invoke @p target.
 *
 * @p label is shown in the progress display as "Running <label>:",
 * and on completion as "<label>" (success) or "<label> failed"
 * (failure).
 *
 * @p interactive non-zero runs the target with stdio connected to
 * the terminal (no capture, no progress display) -- required for
 * ncurses TUI targets like menuconfig.
 *
 * @return 0 on success, non-zero on failure.
 */
int run_cmake_target(const char *target, const char *label, int interactive);

#endif /* CMAKE_H */
