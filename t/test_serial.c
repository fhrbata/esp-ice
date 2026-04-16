/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for serial.h / platform/posix/serial.c.
 *
 * Uses POSIX pseudo-terminals (posix_openpt) as a stand-in for a real
 * serial device.  Writing to the master is observed by serial_read on
 * the slave, and vice versa.
 */

#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ice.h"
#include "serial.h"
#include "tap.h"

static void open_pty_pair(int *master_fd, char *slave, size_t slave_sz)
{
	int m = posix_openpt(O_RDWR | O_NOCTTY);

	if (m < 0)
		die_errno("posix_openpt");
	if (grantpt(m) < 0)
		die_errno("grantpt");
	if (unlockpt(m) < 0)
		die_errno("unlockpt");

	const char *name = ptsname(m);

	if (name == NULL)
		die("ptsname returned NULL");
	strncpy(slave, name, slave_sz - 1);
	slave[slave_sz - 1] = '\0';

	*master_fd = m;
}

int main(void)
{
	struct serial *s = NULL;
	int master;
	char slave_path[256];
	uint8_t buf[64];
	ssize_t n;

	/* Non-existent path -> clean -ENOENT. */
	tap_check(serial_open(&s, "/dev/definitely-not-a-serial-device") ==
		  -ENOENT);
	tap_check(s == NULL);
	tap_done("serial_open reports -ENOENT for a bogus path");

	open_pty_pair(&master, slave_path, sizeof slave_path);

	tap_check(serial_open(&s, slave_path) == 0);
	tap_check(s != NULL);
	tap_done("serial_open on a pty slave succeeds");

	tap_check(serial_set_baud(s, 115200) == 0);
	tap_done("serial_set_baud accepts 115200");

	/* master -> slave */
	const char greeting[] = "hello";

	tap_check(write(master, greeting, sizeof greeting - 1) ==
		  (ssize_t)(sizeof greeting - 1));
	n = serial_read(s, buf, sizeof buf, 200);
	tap_check(n == (ssize_t)(sizeof greeting - 1));
	tap_check(memcmp(buf, greeting, sizeof greeting - 1) == 0);
	tap_done("serial_read sees bytes written on the master side");

	/* slave -> master */
	const char reply[] = "world";

	tap_check(serial_write(s, reply, sizeof reply - 1) ==
		  (ssize_t)(sizeof reply - 1));
	n = read(master, buf, sizeof buf);
	tap_check(n == (ssize_t)(sizeof reply - 1));
	tap_check(memcmp(buf, reply, sizeof reply - 1) == 0);
	tap_done("serial_write delivers bytes to the master side");

	/* Timeout: nothing queued, expect 0 after ~50ms. */
	n = serial_read(s, buf, sizeof buf, 50);
	tap_check(n == 0);
	tap_done("serial_read returns 0 on timeout");

	/* Modem control: on a pty the ioctl often returns -ENOTTY.
	 * We only verify the API is callable. */
	{
		int r_dtr = serial_set_dtr(s, 1);
		int r_rts = serial_set_rts(s, 1);

		tap_check(r_dtr == 0 || r_dtr == -ENOTTY || r_dtr == -EINVAL);
		tap_check(r_rts == 0 || r_rts == -ENOTTY || r_rts == -EINVAL);
		tap_done("serial_set_dtr / serial_set_rts are callable");
	}

	/* Flush: queue bytes on master, then flush. */
	tap_check(write(master, "stale", 5) == 5);
	{
		struct timespec ts = {0, 5 * 1000 * 1000};

		nanosleep(&ts, NULL);
	}
	tap_check(serial_flush_input(s) == 0);
	n = serial_read(s, buf, sizeof buf, 50);
	tap_check(n == 0);
	tap_done("serial_flush_input drops pending input");

	/* Unsupported baud rate. */
	tap_check(serial_set_baud(s, 12345) == -EINVAL);
	tap_done("serial_set_baud rejects non-standard rates with -EINVAL");

	serial_close(s);
	tap_done("serial_close succeeds");

	/* Closing NULL is safe. */
	serial_close(NULL);
	tap_done("serial_close(NULL) is safe");

	close(master);
	return tap_result();
}
