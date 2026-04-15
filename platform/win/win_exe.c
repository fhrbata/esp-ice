/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/win/win_exe.c
 * @brief Resolve the absolute path of the running executable (Windows).
 *
 * Uses GetModuleFileNameA() to obtain the path of the running binary.
 */

#include <windows.h>

#include "../../ice.h"

const char *process_exe(void)
{
	static char buf[4096];
	static const char *result;
	static int initialized;

	if (initialized)
		return result;
	initialized = 1;

	DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
	if (n > 0 && n < (DWORD)sizeof(buf))
		result = buf;
	return result;
}
