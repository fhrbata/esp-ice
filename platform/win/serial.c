/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/win/serial.c
 * @brief Win32 Comm API implementation of @ref serial.h.
 *
 * Uses CreateFileA for device open, SetCommState / SetCommTimeouts
 * for configuration, ReadFile / WriteFile for I/O, and
 * EscapeCommFunction for DTR / RTS control.  COM port paths must be
 * in the "\\\\.\\COMn" form for port numbers > 9; callers on the CLI
 * may type plain "COM3" which is accepted as-is by CreateFileA for
 * low-numbered ports.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <cfgmgr32.h>
#include <devpropdef.h>
#include <setupapi.h>

#include "serial.h"
#include "wconv.h"

struct serial {
	HANDLE handle;
	unsigned baud;
	char path[256];
};

int serial_open(struct serial **out, const char *path)
{
	struct serial *s;
	HANDLE h;
	DCB dcb;
	COMMTIMEOUTS cto;

	*out = NULL;

	h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();
		return (err == ERROR_FILE_NOT_FOUND) ? -ENOENT : -EIO;
	}

	memset(&dcb, 0, sizeof dcb);
	dcb.DCBlength = sizeof dcb;
	if (!GetCommState(h, &dcb)) {
		CloseHandle(h);
		return -EIO;
	}

	dcb.BaudRate = CBR_115200;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary = TRUE;
	dcb.fDtrControl = DTR_CONTROL_DISABLE;
	dcb.fRtsControl = RTS_CONTROL_DISABLE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDsrSensitivity = FALSE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	dcb.fErrorChar = FALSE;
	dcb.fNull = FALSE;
	dcb.fAbortOnError = FALSE;

	if (!SetCommState(h, &dcb)) {
		CloseHandle(h);
		return -EIO;
	}

	/* Non-blocking reads: ReadFile returns immediately with
	 * whatever is available.  serial_read implements timeouts on
	 * top via ReadTotalTimeoutConstant. */
	memset(&cto, 0, sizeof cto);
	cto.ReadIntervalTimeout = MAXDWORD;
	cto.ReadTotalTimeoutMultiplier = 0;
	cto.ReadTotalTimeoutConstant = 0;
	cto.WriteTotalTimeoutMultiplier = 0;
	cto.WriteTotalTimeoutConstant = 0;

	if (!SetCommTimeouts(h, &cto)) {
		CloseHandle(h);
		return -EIO;
	}

	s = calloc(1, sizeof(*s));
	if (!s) {
		CloseHandle(h);
		return -ENOMEM;
	}

	s->handle = h;
	s->baud = 115200;
	strncpy(s->path, path, sizeof s->path - 1);
	*out = s;
	return 0;
}

void serial_close(struct serial *s)
{
	if (!s)
		return;
	if (s->handle != INVALID_HANDLE_VALUE)
		CloseHandle(s->handle);
	free(s);
}

int serial_set_baud(struct serial *s, unsigned baud)
{
	DCB dcb;

	memset(&dcb, 0, sizeof dcb);
	dcb.DCBlength = sizeof dcb;
	if (!GetCommState(s->handle, &dcb))
		return -EIO;

	dcb.BaudRate = (DWORD)baud;
	if (!SetCommState(s->handle, &dcb))
		return -EIO;

	s->baud = baud;
	return 0;
}

ssize_t serial_read(struct serial *s, void *buf, size_t n, unsigned timeout_ms)
{
	COMMTIMEOUTS cto;
	DWORD got = 0;

	memset(&cto, 0, sizeof cto);
	if (timeout_ms == 0) {
		/* Non-blocking: return immediately. */
		cto.ReadIntervalTimeout = MAXDWORD;
	} else {
		cto.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
	}

	if (!SetCommTimeouts(s->handle, &cto))
		return -1;
	if (!ReadFile(s->handle, buf, (DWORD)n, &got, NULL))
		return -1;

	return (ssize_t)got;
}

ssize_t serial_write(struct serial *s, const void *buf, size_t n)
{
	const char *p = (const char *)buf;
	size_t total = 0;

	while (total < n) {
		DWORD wrote = 0;

		if (!WriteFile(s->handle, p + total, (DWORD)(n - total), &wrote,
			       NULL))
			return -1;
		total += (size_t)wrote;
	}
	return (ssize_t)total;
}

int serial_set_dtr(struct serial *s, int on)
{
	DWORD func = on ? SETDTR : CLRDTR;

	if (!EscapeCommFunction(s->handle, func))
		return -EIO;
	return 0;
}

int serial_set_rts(struct serial *s, int on)
{
	DWORD func = on ? SETRTS : CLRRTS;

	if (!EscapeCommFunction(s->handle, func))
		return -EIO;
	return 0;
}

int serial_set_dtr_rts(struct serial *s, int dtr, int rts)
{
	/*
	 * Windows has no atomic "set both" API -- EscapeCommFunction
	 * only handles one line at a time.  The USB driver layer below
	 * it batches the control transfer pair so the two calls still
	 * arrive as one USB frame on most bridges.
	 */
	if (!EscapeCommFunction(s->handle, dtr ? SETDTR : CLRDTR))
		return -EIO;
	if (!EscapeCommFunction(s->handle, rts ? SETRTS : CLRRTS))
		return -EIO;
	return 0;
}

int serial_flush_input(struct serial *s)
{
	if (!PurgeComm(s->handle, PURGE_RXCLEAR))
		return -EIO;
	return 0;
}

int serial_list_ports(char ***out)
{
	HKEY key;
	DWORD index = 0;
	DWORD count = 0;
	char **ports;
	LONG r;

	*out = NULL;

	r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
			  0, KEY_READ, &key);
	if (r != ERROR_SUCCESS)
		return (r == ERROR_FILE_NOT_FOUND) ? 0 : -EIO;

	for (;;) {
		char name[256], value[256];
		DWORD name_sz = sizeof name, value_sz = sizeof value, type;

		r = RegEnumValueA(key, index++, name, &name_sz, NULL, &type,
				  (LPBYTE)value, &value_sz);
		if (r == ERROR_NO_MORE_ITEMS)
			break;
		if (r == ERROR_SUCCESS && type == REG_SZ)
			count++;
	}

	if (count == 0) {
		RegCloseKey(key);
		return 0;
	}

	ports = calloc(count + 1, sizeof(char *));
	if (!ports) {
		RegCloseKey(key);
		return -ENOMEM;
	}

	index = 0;
	DWORD filled = 0;
	for (;;) {
		char name[256], value[256];
		DWORD name_sz = sizeof name, value_sz = sizeof value, type;
		size_t len;

		r = RegEnumValueA(key, index++, name, &name_sz, NULL, &type,
				  (LPBYTE)value, &value_sz);
		if (r == ERROR_NO_MORE_ITEMS)
			break;
		if (r == ERROR_SUCCESS && type == REG_SZ) {
			len = strlen(value);
			ports[filled] = malloc(len + 1);
			if (!ports[filled]) {
				serial_free_port_list(ports);
				RegCloseKey(key);
				return -ENOMEM;
			}
			memcpy(ports[filled], value, len + 1);
			filled++;
		}
	}
	ports[filled] = NULL;

	RegCloseKey(key);
	*out = ports;
	return (int)filled;
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
/*  USB metadata (SetupAPI)                                            */
/* ------------------------------------------------------------------ */

/* Device setup classes a COM port registers under: Ports (UART
 * bridges, usbser CDC) and Modem (some CDC-ACM drivers). */
static const GUID guid_class_ports = {
    0x4d36e978,
    0xe325,
    0x11ce,
    {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
static const GUID guid_class_modem = {
    0x4d36e96d,
    0xe325,
    0x11ce,
    {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

/* DEVPKEY_Device_BusReportedDeviceDesc: the product string the USB
 * device itself reports, as opposed to the driver's INF name. */
static const DEVPROPKEY key_bus_reported_desc = {
    {0x540b947e,
     0x8b40,
     0x45bc,
     {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}},
    4};

static char *port_name(HDEVINFO h, SP_DEVINFO_DATA *d)
{
	wchar_t buf[64] = {0};
	DWORD sz = sizeof buf - sizeof(wchar_t), type;
	HKEY key;
	LONG r;

	key = SetupDiOpenDevRegKey(h, d, DICS_FLAG_GLOBAL, 0, DIREG_DEV,
				   KEY_READ);
	if (key == INVALID_HANDLE_VALUE)
		return NULL;
	r = RegQueryValueExW(key, L"PortName", NULL, &type, (LPBYTE)buf, &sz);
	RegCloseKey(key);
	if (r != ERROR_SUCCESS || type != REG_SZ || !buf[0])
		return NULL;
	return wcs_to_mbs(buf);
}

/* First string of a registry property (REG_SZ or REG_MULTI_SZ). */
static char *registry_prop(HDEVINFO h, SP_DEVINFO_DATA *d, DWORD prop)
{
	wchar_t buf[512] = {0};
	DWORD type;

	if (!SetupDiGetDeviceRegistryPropertyW(h, d, prop, &type, (PBYTE)buf,
					       sizeof buf - sizeof(wchar_t),
					       NULL))
		return NULL;
	if ((type != REG_SZ && type != REG_MULTI_SZ) || !buf[0])
		return NULL;
	return wcs_to_mbs(buf);
}

static char *bus_reported_desc(HDEVINFO h, SP_DEVINFO_DATA *d)
{
	wchar_t buf[256] = {0};
	DEVPROPTYPE type;

	if (!SetupDiGetDevicePropertyW(h, d, &key_bus_reported_desc, &type,
				       (PBYTE)buf, sizeof buf - sizeof(wchar_t),
				       NULL, 0) ||
	    type != DEVPROP_TYPE_STRING || !buf[0])
		return NULL;
	return wcs_to_mbs(buf);
}

/* Parse "<tag>XXXX" (four hex digits) out of a device instance ID. */
static int parse_id_field(const char *id, const char *tag, unsigned int *out)
{
	const char *p = strstr(id, tag);
	char hex[5];

	if (!p)
		return 0;
	p += strlen(tag);
	for (int i = 0; i < 4; i++) {
		if (!isxdigit((unsigned char)p[i]))
			return 0;
		hex[i] = p[i];
	}
	hex[4] = '\0';
	*out = (unsigned int)strtoul(hex, NULL, 16);
	return 1;
}

/* The serial number is the last component of a USB instance ID, but
 * only when the device reported one: Windows makes up an ID with '&'
 * in it otherwise. */
static char *serial_from_instance_id(const char *id)
{
	const char *last = strrchr(id, '\\');

	if (strncmp(id, "USB\\", 4) != 0 || !last || !last[1] ||
	    strchr(last, '&'))
		return NULL;
	return _strdup(last + 1);
}

static char *instance_id(DEVINST inst)
{
	wchar_t buf[MAX_DEVICE_ID_LEN] = {0};

	if (CM_Get_Device_IDW(inst, buf, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
		return NULL;
	return wcs_to_mbs(buf);
}

/*
 * "<bus>-<port>.<port>[:x.<interface>]" from a location path such as
 * "PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(2)#USB(1)#USBMI(0)", the way
 * pyserial spells it.
 */
static char *location_from_path(const char *loc)
{
	char buf[128];
	size_t len = 0;
	int ports = 0;

	buf[0] = '\0';
	for (const char *p = loc; (p = strchr(p, '#')); p++) {
		unsigned long v;
		const char *open = strchr(p, '(');

		if (!open)
			break;
		v = strtoul(open + 1, NULL, 10);
		if (!strncmp(p, "#USBROOT(", 9))
			len += (size_t)snprintf(buf + len, sizeof buf - len,
						"%lu", v + 1);
		else if (!strncmp(p, "#USB(", 5))
			len +=
			    (size_t)snprintf(buf + len, sizeof buf - len,
					     "%s%lu", ports++ ? "." : "-", v);
		else if (!strncmp(p, "#USBMI(", 7))
			len += (size_t)snprintf(buf + len, sizeof buf - len,
						":x.%lu", v);
		if (len >= sizeof buf)
			return NULL;
	}
	return len ? _strdup(buf) : NULL;
}

static void fill_usb_info_class(const GUID *cls, struct serial_port_info *ports,
				int n)
{
	HDEVINFO h = SetupDiGetClassDevsW(cls, NULL, NULL, DIGCF_PRESENT);
	SP_DEVINFO_DATA d;

	if (h == INVALID_HANDLE_VALUE)
		return;

	for (DWORD i = 0;; i++) {
		struct serial_port_info *pi = NULL;
		char *name, *id, *loc;
		DEVINST parent;

		d.cbSize = sizeof d;
		if (!SetupDiEnumDeviceInfo(h, i, &d))
			break;

		name = port_name(h, &d);
		for (int k = 0; name && k < n; k++)
			if (!_stricmp(ports[k].path, name))
				pi = &ports[k];
		free(name);
		if (!pi || pi->usb)
			continue;

		id = instance_id(d.DevInst);
		if (!id)
			continue;
		_strupr(id);
		if (!parse_id_field(id, "VID_", &pi->vid) ||
		    !parse_id_field(id, "PID_", &pi->pid)) {
			free(id);
			continue;
		}
		pi->usb = 1;

		pi->product = bus_reported_desc(h, &d);
		if (!pi->product)
			pi->product = registry_prop(h, &d, SPDRP_DEVICEDESC);

		/* A composite device's interface (MI_xx) has its serial
		 * number on the parent USB device. */
		if (strstr(id, "&MI_") &&
		    CM_Get_Parent(&parent, d.DevInst, 0) == CR_SUCCESS) {
			char *pid_str = instance_id(parent);
			if (pid_str) {
				pi->serial = serial_from_instance_id(pid_str);
				free(pid_str);
			}
		} else {
			pi->serial = serial_from_instance_id(id);
		}
		free(id);

		loc = registry_prop(h, &d, SPDRP_LOCATION_PATHS);
		if (loc) {
			pi->location = location_from_path(loc);
			free(loc);
		}
	}
	SetupDiDestroyDeviceInfoList(h);
}

void serial_fill_usb_info(struct serial_port_info *ports, int n)
{
	fill_usb_info_class(&guid_class_ports, ports, n);
	fill_usb_info_class(&guid_class_modem, ports, n);
}
