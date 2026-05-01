/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/win/fnmatch.c
 * @brief Windows backing for glob_match() -- portable C99 implementation.
 *
 * The Windows CRT doesn't ship @c <fnmatch.h>, so reimplement the
 * minimal subset @c glob_match exposes (@c '*', @c '?', @c "[seq]",
 * @c "[!seq]") as straightforward recursive C.  Behaviour matches
 * POSIX fnmatch(pattern, str, 0) closely enough for the call sites
 * we have today (-F PATTERN in @c "ice idf size").  No backslash
 * escaping and no @c FNM_* flag handling.
 */
#include <string.h>

#include "platform.h"

static int char_in_class(const char *p, const char *p_end, char c)
{
	int negate = 0;
	int matched = 0;

	if (p < p_end && *p == '!') {
		negate = 1;
		p++;
	}
	for (; p < p_end; p++) {
		if (*p == c)
			matched = 1;
	}
	return negate ? !matched : matched;
}

int glob_match(const char *pattern, const char *str)
{
	while (*pattern) {
		if (*pattern == '*') {
			pattern++;
			if (!*pattern)
				return 1;
			while (*str) {
				if (glob_match(pattern, str))
					return 1;
				str++;
			}
			return glob_match(pattern, str);
		}
		if (*pattern == '?') {
			if (!*str)
				return 0;
			pattern++;
			str++;
			continue;
		}
		if (*pattern == '[') {
			const char *end = strchr(pattern + 1, ']');
			if (!end)
				return 0;
			if (!*str)
				return 0;
			if (!char_in_class(pattern + 1, end, *str))
				return 0;
			pattern = end + 1;
			str++;
			continue;
		}
		if (*pattern != *str)
			return 0;
		pattern++;
		str++;
	}
	return *str == '\0';
}
