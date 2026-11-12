/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/probe/probe.c
 * @brief `ice target probe` -- list the host's serial ports.
 *
 * Collecting and printing are separate steps: the port list is the
 * whole result, and the text renderer reads only that list, so a
 * machine-readable renderer can sit next to it without touching the
 * enumeration.
 */
#include "ice.h"
#include "serial.h"

#define ESPRESSIF_VID 0x303a

/* clang-format off */
static const struct cmd_manual target_probe_manual = {
	.name = "ice target probe",
	.summary = "list the serial ports attached to this host",

	.description =
	H_PARA("List the serial ports of this computer, one per line: the "
	       "path to pass to @b{--port}, the USB vendor and product ID "
	       "as @b{VID:PID}, and the product name and manufacturer the "
	       "device reports.  An Espressif vendor ID is shown in green.  "
	       "A port that is not on USB shows @b{-} instead of the ID.")
	H_PARA("The ports most likely to have a chip come first: USB-UART "
	       "bridges (CP210x, CH340, FTDI, PL2303), then Espressif "
	       "USB-Serial/JTAG ports, then the rest.  Commands that pick a "
	       "port on their own, such as @b{ice monitor}, take the first "
	       "line.")
	H_PARA("No port is opened, so it is safe to run while another "
	       "program is using the device.  When there are no serial "
	       "ports, nothing is printed and the exit code is 1."),

	.examples =
	H_EXAMPLE("ice target probe"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice target monitor",
	       "Open one of the listed ports.")
	H_ITEM("ice target flash",
	       "Flash a device on one of the listed ports."),
};
/* clang-format on */

static const struct option cmd_target_probe_opts[] = {OPT_END()};

int cmd_target_probe(int argc, const char **argv);

const struct cmd_desc cmd_target_probe_desc = {
    .name = "probe",
    .fn = cmd_target_probe,
    .opts = cmd_target_probe_opts,
    .manual = &target_probe_manual,
};

/* "VID:PID" in lowercase hex, or "-" for a port that is not on USB. */
static void format_id(const struct serial_port_info *pi, char *buf, size_t sz)
{
	if (pi->usb)
		snprintf(buf, sz, "%04x:%04x", pi->vid, pi->pid);
	else
		snprintf(buf, sz, "-");
}

/* The product name, then the manufacturer in parentheses unless it
 * repeats the product. */
static void describe(const struct serial_port_info *pi, struct sbuf *out)
{
	if (pi->product)
		sbuf_addstr(out, pi->product);
	if (pi->manufacturer &&
	    (!pi->product || strcmp(pi->manufacturer, pi->product) != 0))
		sbuf_addf(out, "%s(%s)", out->len ? " " : "", pi->manufacturer);
}

static void print_ports(const struct serial_port_info *ports, int n)
{
	int wpath = 0, wid = 0;
	char id[16];
	struct sbuf desc = SBUF_INIT;

	for (int i = 0; i < n; i++) {
		int len = (int)strlen(ports[i].path);

		format_id(&ports[i], id, sizeof id);
		if (len > wpath)
			wpath = len;
		if ((int)strlen(id) > wid)
			wid = (int)strlen(id);
	}

	for (int i = 0; i < n; i++) {
		const struct serial_port_info *pi = &ports[i];
		int esp = pi->usb && pi->vid == ESPRESSIF_VID;

		format_id(pi, id, sizeof id);
		sbuf_reset(&desc);
		describe(pi, &desc);

		/* Pad outside the color tokens so the columns line up. */
		printf("@c{%s}%*s  ", pi->path, wpath - (int)strlen(pi->path),
		       "");
		if (esp)
			printf("@g{%s}", id);
		else
			printf("%s", id);
		printf("%*s  %s\n", wid - (int)strlen(id), "", desc.buf);
	}
	sbuf_release(&desc);
}

int cmd_target_probe(int argc, const char **argv)
{
	struct serial_port_info *ports;
	int n;

	argc = parse_options(argc, argv, &cmd_target_probe_desc);
	if (argc > 0)
		die("too many arguments");

	n = serial_list_port_info(&ports);
	if (n < 0)
		die("cannot list serial ports: %s", strerror(-n));
	if (n == 0) {
		fprintf(stderr, "no serial ports found\n");
		return 1;
	}

	serial_sort_ports(ports, n);
	print_ports(ports, n);
	serial_free_port_info(ports, n);
	return 0;
}
