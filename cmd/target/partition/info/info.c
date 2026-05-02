/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/partition/info/info.c
 * @brief "ice target partition info" -- print partition fields.
 *
 * Replaces @b{parttool.py get_partition_info}.  Resolves a
 * partition selector against either @b{--partition-table-file}
 * (offline) or the device, then prints space-separated field
 * values for the @b{--info} list (default: @b{offset size}).
 */
#include "../partition.h"
#include "ice.h"

/* clang-format off */
static const struct cmd_manual target_partition_info_manual = {
	.name = "ice target partition info",
	.summary = "print fields of a single partition",

	.description =
	H_PARA("Drop-in replacement for @b{parttool.py "
	       "get_partition_info}.  Selector and PT source are the same "
	       "as for the other @b{ice target partition} verbs; @b{--info} "
	       "controls which fields are printed (space-separated, "
	       "matching Python's @b{print(' '.join(...))})."),

	.examples =
	H_EXAMPLE("ice target partition info --partition-table-file pt.bin --name nvs")
	H_EXAMPLE("ice target partition info --port /dev/ttyUSB0 --type data --subtype nvs --info offset size")
	H_EXAMPLE("ice target partition info --partition-table-file pt.bin --boot-default --info name offset"),
};
/* clang-format on */

static struct svec opt_info = SVEC_INIT;
static int opt_part_list;

static const struct option cmd_target_partition_info_opts[] = {
    OPT_STRING('p', "port", &pt_opt_port, "dev",
	       "serial port (omit to auto-detect)", serial_complete_port),
    OPT_INT('b', "baud", &pt_opt_baud, "rate",
	    "negotiated baud rate (default 460800)", NULL),
    OPT_STRING(0, "partition-table-file", &pt_opt_pt_file, "path",
	       "load PT from file instead of device", NULL),
    OPT_STRING(0, "partition-table-offset", &pt_opt_pt_offset, "hex",
	       "PT offset in flash (default 0x8000)", NULL),
    OPT_STRING('n', "name", &pt_opt_name, "name", "select by partition name",
	       NULL),
    OPT_STRING('t', "type", &pt_opt_type, "T",
	       "select by type (combine with --subtype)", NULL),
    OPT_STRING('s', "subtype", &pt_opt_subtype, "S", "subtype filter", NULL),
    OPT_BOOL('d', "boot-default", &pt_opt_boot_default,
	     "select the default-boot app partition"),
    OPT_STRING_LIST(0, "info", &opt_info, "field",
		    "fields to print: name|type|subtype|offset|size|encrypted|"
		    "readonly (default: offset size)",
		    NULL),
    OPT_BOOL(0, "part_list", &opt_part_list,
	     "with --type/--subtype: list every matching partition "
	     "(otherwise only the first)"),
    OPT_STRING(0, "primary-bootloader-offset", &pt_opt_primary_boot_offset,
	       "hex", "primary bootloader offset (CSV partition tables only)",
	       NULL),
    OPT_STRING(0, "recovery-bootloader-offset", &pt_opt_recovery_boot_offset,
	       "hex", "recovery bootloader offset (CSV partition tables only)",
	       NULL),
    OPT_STRING_LIST(0, "extra-partition-subtypes", &pt_opt_extra_subtypes,
		    "T,N,V", "register custom subtypes (e.g. data,foo,0x80)",
		    NULL),
    OPT_BOOL('q', "quiet", &pt_opt_quiet,
	     "suppress progress chatter (parttool.py compat; no-op for now)"),
    OPT_STRING_LIST(0, "esptool-args", &pt_opt_esptool_args, "arg",
		    "rejected; ice does not delegate to esptool", NULL),
    OPT_STRING_LIST(0, "esptool-write-args", &pt_opt_esptool_write_args, "arg",
		    "rejected; ice does not delegate to esptool", NULL),
    OPT_STRING_LIST(0, "esptool-read-args", &pt_opt_esptool_read_args, "arg",
		    "rejected; ice does not delegate to esptool", NULL),
    OPT_STRING_LIST(0, "esptool-erase-args", &pt_opt_esptool_erase_args, "arg",
		    "rejected; ice does not delegate to esptool", NULL),
    OPT_END(),
};

int cmd_target_partition_info(int argc, const char **argv);

const struct cmd_desc cmd_target_partition_info_desc = {
    .name = "info",
    .fn = cmd_target_partition_info,
    .opts = cmd_target_partition_info_opts,
    .manual = &target_partition_info_manual,
};

static int print_field(const struct pt_entry *e, const char *field)
{
	if (!strcmp(field, "name"))
		printf("%s", e->name);
	else if (!strcmp(field, "type"))
		printf("%u", e->type);
	else if (!strcmp(field, "subtype"))
		printf("%u", e->subtype);
	else if (!strcmp(field, "offset"))
		printf("0x%x", e->offset);
	else if (!strcmp(field, "size"))
		printf("0x%x", e->size);
	else if (!strcmp(field, "encrypted"))
		printf("%s", e->encrypted ? "True" : "False");
	else if (!strcmp(field, "readonly"))
		printf("%s", e->readonly ? "True" : "False");
	else {
		err("unknown --info field '%s'", field);
		return -1;
	}
	return 0;
}

/* Match all partitions selected by (type, subtype) pair, fill @p out.
 * @return number of matches (0..n). */
static int match_all(const struct pt_entry *entries, int n, uint8_t want_type,
		     uint8_t want_subtype, int have_subtype,
		     const struct pt_entry **out, int max)
{
	int matched = 0;
	for (int i = 0; i < n && matched < max; i++) {
		if (entries[i].type != want_type)
			continue;
		if (have_subtype && entries[i].subtype != want_subtype)
			continue;
		out[matched++] = &entries[i];
	}
	return matched;
}

int cmd_target_partition_info(int argc, const char **argv)
{
	struct pt_entry entries[PT_MAX_ENTRIES];
	int count = 0;
	struct pt_target_selector sel = {0};
	const struct pt_entry *single;
	const struct pt_entry *matches[PT_MAX_ENTRIES];
	int n_matches;
	uint32_t table_offset;
	char *end;
	esp_loader_t loader;
	esf_port_t sport;
	char *autoport = NULL;
	int rc = 1;
	int connected = 0;
	int wrote_any = 0;

	pt_target_reset_opts();
	svec_clear(&opt_info);
	opt_part_list = 0;

	argc = parse_options(argc, argv, &cmd_target_partition_info_desc);

	(void)argc;
	(void)argv;

	pt_target_finalize_opts();

	sel.name = pt_opt_name;
	sel.type = pt_opt_type;
	sel.subtype = pt_opt_subtype;
	sel.boot_default = pt_opt_boot_default;

	table_offset = (uint32_t)strtoul(pt_opt_pt_offset, &end, 0);
	if (*end)
		die("invalid --partition-table-offset: %s", pt_opt_pt_offset);

	if (!pt_opt_pt_file) {
		if (pt_target_connect(&loader, &sport, pt_opt_port, pt_opt_baud,
				      &autoport) != 0)
			goto out;
		connected = 1;
	}

	if (pt_target_load(entries, &count, connected ? &loader : NULL,
			   table_offset) != 0)
		goto out;

	if (opt_part_list) {
		uint8_t want_type, want_subtype = 0;
		int have_subtype = 0;

		if (!pt_opt_type) {
			err("--part_list requires --type [--subtype]");
			goto out;
		}
		if (pt_parse_type(pt_opt_type, &want_type) != 0) {
			err("invalid --type value: %s", pt_opt_type);
			goto out;
		}
		if (pt_opt_subtype) {
			if (pt_parse_subtype(want_type, pt_opt_subtype,
					     &want_subtype) != 0) {
				err("invalid --subtype value '%s' for type %s",
				    pt_opt_subtype, pt_opt_type);
				goto out;
			}
			have_subtype = 1;
		}
		n_matches = match_all(entries, count, want_type, want_subtype,
				      have_subtype, matches, PT_MAX_ENTRIES);
		if (n_matches == 0) {
			err("no partition matches type=%s%s%s", pt_opt_type,
			    have_subtype ? " subtype=" : "",
			    have_subtype ? pt_opt_subtype : "");
			goto out;
		}
	} else {
		single = pt_target_select(entries, count, &sel,
					  connected ? &loader : NULL);
		if (!single)
			goto out;
		matches[0] = single;
		n_matches = 1;
	}

	for (int m = 0; m < n_matches; m++) {
		const struct pt_entry *e = matches[m];

		if (opt_info.nr == 0) {
			if (wrote_any)
				printf(" ");
			printf("0x%x 0x%x", e->offset, e->size);
			wrote_any = 1;
		} else {
			for (size_t i = 0; i < opt_info.nr; i++) {
				if (wrote_any)
					printf(" ");
				if (print_field(e, opt_info.v[i]) != 0)
					goto out;
				wrote_any = 1;
			}
		}
	}
	printf("\n");

	rc = 0;
out:
	free(autoport);
	return rc;
}
