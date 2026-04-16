/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/posix/serial.c
 * @brief POSIX termios implementation of @ref serial.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "serial.h"

struct serial {
	int fd;
	unsigned baud;
	char path[256];
};

/* ------------------------------------------------------------------ */
/*  Baud-rate table                                                    */
/* ------------------------------------------------------------------ */

struct baud_entry {
	unsigned baud;
	speed_t speed;
};

static const struct baud_entry baud_table[] = {
    {9600, B9600},
    {19200, B19200},
    {38400, B38400},
    {57600, B57600},
    {115200, B115200},
    {230400, B230400},
#ifdef B460800
    {460800, B460800},
#endif
#ifdef B500000
    {500000, B500000},
#endif
#ifdef B576000
    {576000, B576000},
#endif
#ifdef B921600
    {921600, B921600},
#endif
#ifdef B1000000
    {1000000, B1000000},
#endif
#ifdef B1500000
    {1500000, B1500000},
#endif
#ifdef B2000000
    {2000000, B2000000},
#endif
    {0, 0},
};

static speed_t baud_lookup(unsigned baud)
{
	for (const struct baud_entry *e = baud_table; e->baud; e++)
		if (e->baud == baud)
			return e->speed;
	return 0;
}

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

int serial_open(struct serial **out, const char *path)
{
	struct termios tio;
	struct serial *s;
	int fd;

	*out = NULL;

	fd = open(path, O_RDWR | O_NOCTTY);
	if (fd < 0)
		return -errno;

	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
		goto fail;

	if (tcgetattr(fd, &tio) < 0)
		goto fail;

	tio.c_iflag &= (tcflag_t) ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
				    IGNCR | ICRNL | IXON | IXOFF | IXANY);
	tio.c_oflag &= (tcflag_t)~OPOST;
	tio.c_lflag &=
	    (tcflag_t) ~(ECHO | ECHOE | ECHONL | ICANON | ISIG | IEXTEN);
	tio.c_cflag &= (tcflag_t) ~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
	tio.c_cflag &= (tcflag_t)~CRTSCTS;
#endif
	tio.c_cflag |= CS8 | CREAD | CLOCAL;

	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &tio) < 0)
		goto fail;

	s = calloc(1, sizeof(*s));
	if (!s)
		goto fail;

	s->fd = fd;
	strncpy(s->path, path, sizeof s->path - 1);
	*out = s;
	return 0;

fail:;
	int err = errno;

	close(fd);
	return -err;
}

void serial_close(struct serial *s)
{
	if (!s)
		return;
	if (s->fd >= 0)
		close(s->fd);
	free(s);
}

int serial_set_baud(struct serial *s, unsigned baud)
{
	struct termios tio;
	speed_t speed = baud_lookup(baud);

	if (speed == 0)
		return -EINVAL;
	if (tcgetattr(s->fd, &tio) < 0)
		return -errno;
	if (cfsetispeed(&tio, speed) < 0)
		return -errno;
	if (cfsetospeed(&tio, speed) < 0)
		return -errno;
	if (tcsetattr(s->fd, TCSANOW, &tio) < 0)
		return -errno;

	s->baud = baud;
	return 0;
}

ssize_t serial_read(struct serial *s, void *buf, size_t n, unsigned timeout_ms)
{
	fd_set rfds;
	struct timeval tv;
	int rc;

	FD_ZERO(&rfds);
	FD_SET(s->fd, &rfds);

	tv.tv_sec = (long)(timeout_ms / 1000u);
	tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

	do {
		rc = select(s->fd + 1, &rfds, NULL, NULL, &tv);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
		return -1;
	if (rc == 0)
		return 0;

	return read(s->fd, buf, n);
}

ssize_t serial_write(struct serial *s, const void *buf, size_t n)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t total = 0;

	while (total < n) {
		ssize_t w = write(s->fd, p + total, n - total);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		total += (size_t)w;
	}
	return (ssize_t)total;
}

static int set_modem_bit(int fd, int bit, int on)
{
	int bits = bit;

	if (ioctl(fd, on ? TIOCMBIS : TIOCMBIC, &bits) < 0)
		return -errno;
	return 0;
}

int serial_set_dtr(struct serial *s, int on)
{
	return set_modem_bit(s->fd, TIOCM_DTR, on);
}

int serial_set_rts(struct serial *s, int on)
{
	return set_modem_bit(s->fd, TIOCM_RTS, on);
}

int serial_flush_input(struct serial *s)
{
	if (tcflush(s->fd, TCIFLUSH) < 0)
		return -errno;
	return 0;
}
