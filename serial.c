/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file serial.c
 * @brief Application-layer helpers built on top of @ref serial.h.
 *
 * Platform-specific code (open, read, write, modem lines, port
 * enumeration) lives in platform/{posix,win}/serial.c.  This file
 * contains only the OS-agnostic logic that sits above those primitives
 * and may call into ice.h (e.g. complete_emit).
 */
#include "serial.h"
#include "ice.h"

void serial_complete_port(void)
{
	char **ports;
	int n = serial_list_ports(&ports);

	if (n <= 0)
		return;
	for (char **p = ports; *p; p++)
		complete_emit(*p, NULL);
	serial_free_port_list(ports);
}
