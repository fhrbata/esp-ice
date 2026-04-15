/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/posix/posix_exe.c
 * @brief Resolve the absolute path of the running executable (POSIX).
 *
 * Linux:  readlink("/proc/self/exe")
 * macOS:  _NSGetExecutablePath()
 */

#include "../../ice.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

const char *process_exe(void)
{
	static char buf[4096];
	static const char *result;
	static int initialized;

	if (initialized)
		return result;
	initialized = 1;

#ifdef __APPLE__
	uint32_t size = (uint32_t)sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) == 0)
		result = buf;
#else
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		result = buf;
	}
#endif

	return result;
}
