/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/posix/fnmatch.c
 * @brief POSIX backing for glob_match() -- thin libc wrapper.
 */
#include <fnmatch.h>

#include "platform.h"

int glob_match(const char *pattern, const char *str)
{
	return fnmatch(pattern, str, 0) == 0;
}
