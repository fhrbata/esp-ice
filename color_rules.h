/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file color_rules.h
 * @brief Shared keyword-to-color table used to colorize captured tool
 *        output.
 *
 * Both the failure-dump path in process_run_progress() and @b{ice log}
 * pass log content through color_text() with this table so the two
 * views never drift apart.  Covers the output shapes produced by the
 * tools ice spawns (cmake, ninja, gcc, git, ld).
 */
#ifndef COLOR_RULES_H
#define COLOR_RULES_H

#include "term.h"

/** NULL-terminated default rule table for tool output. */
extern const struct color_rule ice_default_color_rules[];

#endif /* COLOR_RULES_H */
