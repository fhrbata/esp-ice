/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file color_rules.c
 * @brief Default keyword-to-color table for captured tool output.
 */
#include "color_rules.h"

const struct color_rule ice_default_color_rules[] = {
    COLOR_RULE("fatal error:", "COLOR_BOLD_RED"),
    COLOR_RULE("FAILED:", "COLOR_BOLD_RED"),
    COLOR_RULE("CMake Error", "COLOR_RED"),
    COLOR_RULE("CMake Warning", "COLOR_YELLOW"),
    COLOR_RULE("undefined reference to", "COLOR_RED"),
    COLOR_RULE("multiple definition of", "COLOR_RED"),
    COLOR_RULE("warning:", "COLOR_BOLD_YELLOW"),
    COLOR_RULE("error:", "COLOR_BOLD_RED"),
    COLOR_RULE("note:", "COLOR_CYAN"),
    COLOR_RULE("In file included from", "COLOR_CYAN"),
    COLOR_RULE("In function", "COLOR_CYAN"),
    {NULL, 0, NULL},
};
