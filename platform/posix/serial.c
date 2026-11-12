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
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
/* IOSSIOSPEED — Darwin's ioctl for arbitrary baud rates.  Needed for
 * 460800/921600/... which <termios.h> on macOS does not expose as
 * B-symbols (only up to B230400).  No framework link required; the
 * header just provides the ioctl number. */
#include <IOKit/serial/ioss.h>
/* Port enumeration reads USB metadata from the IOKit registry; this
 * part does need IOKit and CoreFoundation at link time. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#endif

#include "ice.h"
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
#ifdef B57600
    {57600, B57600},
#endif
#ifdef B115200
    {115200, B115200},
#endif
#ifdef B230400
    {230400, B230400},
#endif
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
	speed_t speed = baud_lookup(baud);

	if (speed != 0) {
		struct termios tio;
		if (tcgetattr(s->fd, &tio) < 0)
			return -errno;
		if (cfsetispeed(&tio, speed) < 0)
			return -errno;
		if (cfsetospeed(&tio, speed) < 0)
			return -errno;
		if (tcsetattr(s->fd, TCSANOW, &tio) < 0)
			return -errno;
	} else {
#ifdef __APPLE__
		/* Non-standard rate: macOS does not define B-symbols past
		 * B230400, so termios cannot carry the value.  IOSSIOSPEED
		 * accepts an arbitrary speed_t and is honoured directly by
		 * the IOKit serial driver, bypassing the B-symbol table. */
		speed_t want = (speed_t)baud;
		if (ioctl(s->fd, IOSSIOSPEED, &want) < 0)
			return -errno;
#else
		return -EINVAL;
#endif
	}

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

int serial_set_dtr_rts(struct serial *s, int dtr, int rts)
{
	int bits;

	if (ioctl(s->fd, TIOCMGET, &bits) < 0)
		return -errno;

	if (dtr)
		bits |= TIOCM_DTR;
	else
		bits &= ~TIOCM_DTR;
	if (rts)
		bits |= TIOCM_RTS;
	else
		bits &= ~TIOCM_RTS;

	if (ioctl(s->fd, TIOCMSET, &bits) < 0)
		return -errno;
	return 0;
}

int serial_flush_input(struct serial *s)
{
	if (tcflush(s->fd, TCIFLUSH) < 0)
		return -errno;
	return 0;
}

int serial_list_ports(char ***out)
{
	static const char *patterns[] = {
	    "/dev/ttyUSB*",
	    "/dev/ttyACM*",
	    "/dev/cu.*",
	    NULL,
	};

	glob_t g;
	int found = 0;

	*out = NULL;
	memset(&g, 0, sizeof g);

	for (int i = 0; patterns[i]; i++) {
		int flags = GLOB_NOSORT | (found ? GLOB_APPEND : 0);
		int r = glob(patterns[i], flags, NULL, &g);

		if (r == GLOB_NOMATCH)
			continue;
		if (r != 0) {
			if (found)
				globfree(&g);
			return -EIO;
		}
		found = 1;
	}

	if (!found || g.gl_pathc == 0) {
		if (found)
			globfree(&g);
		return 0;
	}

	char **ports = calloc(g.gl_pathc + 1, sizeof(char *));
	if (!ports) {
		globfree(&g);
		return -ENOMEM;
	}

	int count = 0;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		size_t len = strlen(g.gl_pathv[i]);

		ports[i] = malloc(len + 1);
		if (!ports[i]) {
			serial_free_port_list(ports);
			globfree(&g);
			return -ENOMEM;
		}
		memcpy(ports[i], g.gl_pathv[i], len + 1);
		count++;
	}
	ports[count] = NULL;

	globfree(&g);
	*out = ports;
	return count;
}

void serial_free_port_list(char **ports)
{
	if (!ports)
		return;
	for (char **p = ports; *p; p++)
		free(*p);
	free(ports);
}

/* ------------------------------------------------------------------ */
/*  USB metadata                                                       */
/* ------------------------------------------------------------------ */

#if defined(__linux__)

/* Read one sysfs attribute of @p dir into a heap string, trailing
 * newline dropped.  NULL when the attribute is missing or empty.
 * Not sbuf_read_file(): sysfs reports every attribute as 4096 bytes
 * long, and the short read that follows looks like an error. */
static char *sysfs_attr(const char *dir, const char *attr)
{
	struct sbuf path = SBUF_INIT;
	struct sbuf val = SBUF_INIT;
	char line[256];
	FILE *f;

	sbuf_addf(&path, "%s/%s", dir, attr);
	f = fopen(path.buf, "r");
	sbuf_release(&path);
	if (!f)
		return NULL;
	if (fgets(line, sizeof line, f))
		sbuf_addstr(&val, line);
	fclose(f);

	sbuf_rtrim(&val);
	if (!val.len) {
		sbuf_release(&val);
		return NULL;
	}
	return sbuf_detach(&val);
}

/* Cut the last component off @p path in place. */
static void strip_last(char *path)
{
	char *slash = strrchr(path, '/');
	if (slash)
		*slash = '\0';
}

/*
 * Walk sysfs from the tty to its USB interface and device.  A
 * usb-serial bridge (ttyUSB*) hangs one level below the interface,
 * a CDC-ACM port (ttyACM*) is the interface itself; the USB device
 * is the interface's parent in both cases.
 */
static void fill_usb_info(struct serial_port_info *pi)
{
	const char *name = strrchr(pi->path, '/');
	struct sbuf link = SBUF_INIT;
	char *dev, *subsys, *intf = NULL, *usbdev = NULL;
	char *vid = NULL, *pid = NULL;
	const char *kind;

	name = name ? name + 1 : pi->path;
	sbuf_addf(&link, "/sys/class/tty/%s/device", name);
	dev = path_realpath(link.buf);
	sbuf_reset(&link);
	if (!dev)
		goto out;

	sbuf_addf(&link, "%s/subsystem", dev);
	subsys = path_realpath(link.buf);
	if (!subsys)
		goto out;
	kind = strrchr(subsys, '/');
	kind = kind ? kind + 1 : subsys;
	if (!strcmp(kind, "usb-serial")) {
		intf = sbuf_strdup(dev);
		strip_last(intf);
	} else if (!strcmp(kind, "usb")) {
		intf = sbuf_strdup(dev);
	}
	free(subsys);
	if (!intf)
		goto out;

	usbdev = sbuf_strdup(intf);
	strip_last(usbdev);

	vid = sysfs_attr(usbdev, "idVendor");
	pid = sysfs_attr(usbdev, "idProduct");
	if (!vid || !pid)
		goto out;
	pi->vid = (unsigned int)strtoul(vid, NULL, 16);
	pi->pid = (unsigned int)strtoul(pid, NULL, 16);
	pi->usb = 1;
	pi->product = sysfs_attr(usbdev, "product");
	pi->manufacturer = sysfs_attr(usbdev, "manufacturer");
	pi->serial = sysfs_attr(usbdev, "serial");
	pi->usb_interface = sysfs_attr(intf, "interface");
	pi->location = sbuf_strdup(strrchr(intf, '/') + 1);

out:
	free(vid);
	free(pid);
	free(usbdev);
	free(intf);
	free(dev);
	sbuf_release(&link);
}

void serial_fill_usb_info(struct serial_port_info *ports, int n)
{
	for (int i = 0; i < n; i++)
		fill_usb_info(&ports[i]);
}

#elif defined(__APPLE__)

/* Look @p key up on @p svc or the first parent that has it. */
static CFTypeRef ioreg_search(io_object_t svc, const char *key)
{
	CFStringRef k =
	    CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFTypeRef v = IORegistryEntrySearchCFProperty(
	    svc, kIOServicePlane, k, kCFAllocatorDefault,
	    kIORegistryIterateRecursively | kIORegistryIterateParents);
	CFRelease(k);
	return v;
}

static char *ioreg_string(io_object_t svc, const char *key)
{
	CFTypeRef v = ioreg_search(svc, key);
	char buf[512];
	char *out = NULL;

	if (!v)
		return NULL;
	if (CFGetTypeID(v) == CFStringGetTypeID() &&
	    CFStringGetCString((CFStringRef)v, buf, sizeof buf,
			       kCFStringEncodingUTF8) &&
	    buf[0])
		out = sbuf_strdup(buf);
	CFRelease(v);
	return out;
}

static int ioreg_number(io_object_t svc, const char *key, long long *out)
{
	CFTypeRef v = ioreg_search(svc, key);
	int ok = 0;

	if (!v)
		return 0;
	if (CFGetTypeID(v) == CFNumberGetTypeID())
		ok = CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType,
				      out);
	CFRelease(v);
	return ok;
}

/* "<bus>-<port>.<port>..." from an IOUSB locationID, the way
 * pyserial spells it: the bus in the top byte, then one hub port per
 * nibble until the first zero nibble. */
static char *location_string(unsigned long loc)
{
	struct sbuf sb = SBUF_INIT;
	int first = 1;

	sbuf_addf(&sb, "%lu-", (loc >> 24) & 0xff);
	while (loc & 0xf00000) {
		sbuf_addf(&sb, first ? "%lu" : ".%lu", (loc >> 20) & 0xf);
		first = 0;
		loc = (loc << 4) & 0xffffffffUL;
	}
	return sbuf_detach(&sb);
}

void serial_fill_usb_info(struct serial_port_info *ports, int n)
{
	io_iterator_t it;
	io_object_t svc;

	if (IOServiceGetMatchingServices(
		MACH_PORT_NULL, IOServiceMatching(kIOSerialBSDServiceValue),
		&it) != KERN_SUCCESS)
		return;

	while ((svc = IOIteratorNext(it))) {
		char *path = ioreg_string(svc, kIOCalloutDeviceKey);
		struct serial_port_info *pi = NULL;
		long long vid, pid, loc;

		for (int i = 0; path && i < n; i++)
			if (!strcmp(ports[i].path, path))
				pi = &ports[i];
		free(path);

		if (pi && ioreg_number(svc, "idVendor", &vid) &&
		    ioreg_number(svc, "idProduct", &pid)) {
			pi->usb = 1;
			pi->vid = (unsigned int)vid;
			pi->pid = (unsigned int)pid;
			pi->product = ioreg_string(svc, "USB Product Name");
			pi->manufacturer = ioreg_string(svc, "USB Vendor Name");
			pi->serial = ioreg_string(svc, "USB Serial Number");
			pi->usb_interface =
			    ioreg_string(svc, "USB Interface Name");
			if (ioreg_number(svc, "locationID", &loc))
				pi->location =
				    location_string((unsigned long)loc);
		}
		IOObjectRelease(svc);
	}
	IOObjectRelease(it);
}

#else

void serial_fill_usb_info(struct serial_port_info *ports, int n)
{
	(void)ports;
	(void)n;
}

#endif
