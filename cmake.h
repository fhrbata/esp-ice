/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmake.h
 * @brief Project-lifecycle API consumed by the dispatcher.
 *
 * Commands declare their precondition via @c cmd_desc.needs; the
 * dispatcher calls setup_project() before the leaf handler runs.
 * Handlers can assume @c _project.* is populated and all marker /
 * toolchain state is in place -- no per-command @c load_profile or
 * @c require_project_initialized call is needed.
 */
#ifndef CMAKE_H
#define CMAKE_H

/**
 * Print profile names from @b{[project "<name>"]} sections of the
 * loaded config to stdout, one per line.  Always emits "default"
 * even when no @b{[project "default"]} section exists yet.
 *
 * Suitable as the @c --profile completion callback and as the
 * positional-slot completer wherever a profile name is accepted.
 *
 * Declared here (not in ice.h) because it is a cmake-profile concern
 * consumed only from command option tables.  setup_project() itself
 * lives in ice.h so its @c enum project_need argument is visible
 * without extra includes.
 */
void complete_profile_names(void);

#endif /* CMAKE_H */
