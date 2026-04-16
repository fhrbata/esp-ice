/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmake.h
 * @brief Profile loading + init-gate checks for cmake-using commands.
 *
 * @b{ice init} owns the cmake configure-time state (generator, -D
 * flags, build-dir layout, build.ninja fixups); the functions here
 * are what every other cmake-using command needs in common: load the
 * profile, verify init has been run, and (for completion) list the
 * profile names defined in @b{.iceconfig}.
 *
 * Typical call sequence in a cmake-using subcommand:
 *
 *   parse_options(...)              // figure out the profile name
 *   load_profile(name);             // PATH + project.* config
 *   require_project_initialized();  // dies if user hasn't run ice init
 *   // ... exec cmake --build <project.build-dir> --target <...>
 */
#ifndef CMAKE_H
#define CMAKE_H

/**
 * Load profile @p name into process state.
 *
 * Reads @b{[project "<name>"]} from the config store and:
 *   - promotes its keys to the uniform @b{project.X} namespace at
 *     PROJECT scope so commands can read @b{project.build-dir},
 *     @b{project.chip}, etc. without knowing which profile is active;
 *   - derives @b{project.target} / @b{project.mapfile} /
 *     @b{project.elf} from the build directory's @b{CMakeCache.txt}
 *     and @b{project_description.json};
 *   - prepends the profile's IDF tools (compilers, ninja, cmake,
 *     openocd, ...) to @b{PATH} and sets the IDF's export_vars so
 *     later @b{cmake --build} calls resolve the toolchain without
 *     @b{export.sh}.
 *
 * Dies if the profile is not bound (no @b{ice init} has been run for
 * it).  @p name = "default" loads the default profile.
 */
void load_profile(const char *name);

/**
 * Full project setup for porcelain commands.
 *
 * Equivalent to calling load_profile(@p name), require_project_initialized(),
 * then parsing the build directory's @b{flasher_args.json} and
 * populating derived-state config keys at @c CONFIG_SCOPE_PROJECT:
 *
 *   - @b{project.chip}       — chip string from
 *                              @c extra_esptool_args.chip (e.g. "esp32c6").
 *   - @b{project.flash-file} — one multi-valued entry per image, each
 *                              formatted as @c "offset=full_path"
 *                              (e.g. @c "0x10000=build/app.bin").
 *
 * @b{flasher_args.json} parsing is best-effort: missing or unparseable
 * files are silently skipped so the function still succeeds when the
 * project has been initialised but not yet built.
 *
 * Porcelain call sites collapse to a single line:
 *
 * @code
 *   project_load(argc >= 1 ? argv[0] : "default");
 *   const char *chip = config_get("project.chip");
 * @endcode
 */
void project_load(const char *name);

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
 * Verify that @b{ice init} has been run successfully for the active
 * profile.  Checks that @b{project.build-dir} is set and contains a
 * @b{CMakeCache.txt}; dies with a "run ice init" hint otherwise.
 *
 * Post-init commands (build, flash, clean, menuconfig) call this to
 * fail fast when the user has skipped init or wiped the build
 * directory by hand.
 */
void require_project_initialized(void);

#endif /* CMAKE_H */
