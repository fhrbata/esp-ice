/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/partition/partition.c
 * @brief `ice target partition` -- runtime device partition tool.
 *
 * Replaces ESP-IDF's @b{parttool.py}.  Routes one of four verbs --
 * info / read / write / erase -- against a partition resolved from
 * either a CSV/binary file (offline) or the partition table read
 * straight off the chip's flash (online).
 */
#include "partition.h"
#include "esp_loader.h"
#include "ice.h"
#include "serial.h"

extern const struct cmd_desc cmd_target_partition_info_desc;
extern const struct cmd_desc cmd_target_partition_read_desc;
extern const struct cmd_desc cmd_target_partition_write_desc;
extern const struct cmd_desc cmd_target_partition_erase_desc;

/* clang-format off */
static const struct cmd_manual target_partition_manual = {
	.name = "ice target partition",
	.summary = "read / write / erase / inspect a partition on the device",

	.description =
	H_PARA("Drop-in replacement for ESP-IDF's @b{parttool.py}.  Each "
	       "verb resolves the target partition from a selector "
	       "(@b{--name}, the @b{--type}/@b{--subtype} pair, or "
	       "@b{--boot-default}) and either inspects it offline, reads "
	       "its bytes off the device, writes new bytes to it, or "
	       "erases it.")
	H_PARA("The partition table is read from @b{--partition-table-file} "
	       "when supplied, otherwise from the device at "
	       "@b{--partition-table-offset} (default 0x8000)."),

	.examples =
	H_EXAMPLE("ice target partition info --name nvs --partition-table-file pt.bin")
	H_EXAMPLE("ice target partition read --port /dev/ttyUSB0 --type data --subtype nvs --output nvs.bin")
	H_EXAMPLE("ice target partition write --port /dev/ttyUSB0 --name storage --input storage.bin")
	H_EXAMPLE("ice target partition erase --port /dev/ttyUSB0 --type data --subtype ota"),
};
/* clang-format on */

static const struct option cmd_target_partition_opts[] = {OPT_END()};

static const struct cmd_desc *const target_partition_subs[] = {
    &cmd_target_partition_info_desc,
    &cmd_target_partition_read_desc,
    &cmd_target_partition_write_desc,
    &cmd_target_partition_erase_desc,
    NULL,
};

const struct cmd_desc cmd_target_partition_desc = {
    .name = "partition",
    .opts = cmd_target_partition_opts,
    .manual = &target_partition_manual,
    .subcommands = target_partition_subs,
};

/* ------------------------------------------------------------------ */
/* Common option globals (defined once; declared in partition.h)       */
/* ------------------------------------------------------------------ */

const char *pt_opt_port;
int pt_opt_baud = 460800;
const char *pt_opt_pt_file;
const char *pt_opt_pt_offset = "0x8000";
const char *pt_opt_name;
const char *pt_opt_type;
const char *pt_opt_subtype;
int pt_opt_boot_default;
const char *pt_opt_primary_boot_offset;
const char *pt_opt_recovery_boot_offset;
struct svec pt_opt_extra_subtypes = SVEC_INIT;
int pt_opt_quiet;
struct svec pt_opt_esptool_args = SVEC_INIT;
struct svec pt_opt_esptool_write_args = SVEC_INIT;
struct svec pt_opt_esptool_read_args = SVEC_INIT;
struct svec pt_opt_esptool_erase_args = SVEC_INIT;

void pt_target_reset_opts(void)
{
	pt_opt_port = NULL;
	pt_opt_baud = 460800;
	pt_opt_pt_file = NULL;
	pt_opt_pt_offset = "0x8000";
	pt_opt_name = NULL;
	pt_opt_type = NULL;
	pt_opt_subtype = NULL;
	pt_opt_boot_default = 0;
	pt_opt_primary_boot_offset = NULL;
	pt_opt_recovery_boot_offset = NULL;
	pt_opt_quiet = 0;
	svec_clear(&pt_opt_extra_subtypes);
	svec_clear(&pt_opt_esptool_args);
	svec_clear(&pt_opt_esptool_write_args);
	svec_clear(&pt_opt_esptool_read_args);
	svec_clear(&pt_opt_esptool_erase_args);
	pt_clear_extras();
}

static void reject_esptool_flag(const struct svec *v, const char *flag)
{
	if (v->nr == 0)
		return;
	die("%s is not supported.  ice manages flashing through "
	    "esp-serial-flasher directly and does not delegate to esptool, "
	    "so esptool-only options have no effect.  Use --port / --baud "
	    "for the common cases.",
	    flag);
}

void pt_target_finalize_opts(void)
{
	reject_esptool_flag(&pt_opt_esptool_args, "--esptool-args");
	reject_esptool_flag(&pt_opt_esptool_write_args, "--esptool-write-args");
	reject_esptool_flag(&pt_opt_esptool_read_args, "--esptool-read-args");
	reject_esptool_flag(&pt_opt_esptool_erase_args, "--esptool-erase-args");

	for (size_t i = 0; i < pt_opt_extra_subtypes.nr; i++) {
		const char *line = pt_opt_extra_subtypes.v[i];
		char buf[128];
		char *p1, *p2;
		uint8_t type, val;
		uint32_t v;

		if (snprintf(buf, sizeof(buf), "%s", line) >= (int)sizeof(buf))
			die("--extra-partition-subtypes entry too long: %s",
			    line);

		p1 = strchr(buf, ',');
		if (!p1)
			die("invalid --extra-partition-subtypes triple '%s' "
			    "(expected TYPE,NAME,VALUE)",
			    line);
		*p1++ = '\0';
		p2 = strchr(p1, ',');
		if (!p2)
			die("invalid --extra-partition-subtypes triple '%s' "
			    "(expected TYPE,NAME,VALUE)",
			    line);
		*p2++ = '\0';

		if (pt_parse_type(buf, &type) != 0)
			die("--extra-partition-subtypes: unknown type '%s' "
			    "in '%s'",
			    buf, line);

		{
			char *end;
			v = (uint32_t)strtoul(p2, &end, 0);
			if (end == p2 || *end || v > 0xFF)
				die("--extra-partition-subtypes: bad value "
				    "'%s' in '%s'",
				    p2, line);
		}
		val = (uint8_t)v;

		if (pt_register_subtype(type, p1, val) != 0)
			die("--extra-partition-subtypes: registration of "
			    "'%s' failed",
			    line);
	}
}

/* ------------------------------------------------------------------ */
/* Selector resolution                                                 */
/* ------------------------------------------------------------------ */

const struct pt_entry *pt_target_select(const struct pt_entry *entries, int n,
					const struct pt_target_selector *sel,
					esp_loader_t *loader)
{
	(void)loader; /* boot-default w/ otadata read not implemented yet */

	if (sel->name) {
		for (int i = 0; i < n; i++)
			if (!strcmp(entries[i].name, sel->name))
				return &entries[i];
		err("partition '%s' not found in partition table", sel->name);
		return NULL;
	}

	if (sel->type) {
		uint8_t want_type = 0, want_subtype = 0;
		int have_subtype = 0;

		if (pt_parse_type(sel->type, &want_type) != 0) {
			err("invalid --type value: %s", sel->type);
			return NULL;
		}
		if (sel->subtype) {
			if (pt_parse_subtype(want_type, sel->subtype,
					     &want_subtype) != 0) {
				err("invalid --subtype value '%s' for type %s",
				    sel->subtype, sel->type);
				return NULL;
			}
			have_subtype = 1;
		}
		for (int i = 0; i < n; i++) {
			if (entries[i].type != want_type)
				continue;
			if (have_subtype && entries[i].subtype != want_subtype)
				continue;
			return &entries[i];
		}
		err("no partition matches type=%s%s%s%s", sel->type,
		    have_subtype ? " subtype=" : "",
		    have_subtype ? sel->subtype : "", "");
		return NULL;
	}

	if (sel->boot_default) {
		/* parttool.py's resolution: search factory, then
		 * ota_0..ota_15, return the first one in the table.  No
		 * otadata reading -- the bootloader does that at runtime,
		 * but `--boot-default` here just names the *configured*
		 * default-boot slot, not the live one. */
		uint8_t want[1 + 16];
		want[0] = 0x00; /* factory */
		for (int k = 0; k < 16; k++)
			want[1 + k] = (uint8_t)(0x10 + k); /* ota_0..ota_15 */
		for (size_t k = 0; k < sizeof(want); k++) {
			for (int i = 0; i < n; i++) {
				if (entries[i].type == PT_TYPE_APP &&
				    entries[i].subtype == want[k])
					return &entries[i];
			}
		}
		err("no factory or ota_N partition for --boot-default");
		return NULL;
	}

	err("specify --name, --type [--subtype], or --boot-default");
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Device connection / table loading                                   */
/* ------------------------------------------------------------------ */

int pt_target_connect(esp_loader_t *loader, esf_port_t *sport, const char *port,
		      int baud, char **autoport_out)
{
	esp_loader_error_t err_;

	*autoport_out = NULL;

	if (!port) {
		char *autoport = esf_find_esp_port(ICE_CHIP_UNKNOWN);
		if (!autoport) {
			err("no ESP device found.  Use --port to specify "
			    "explicitly.");
			return -1;
		}
		*autoport_out = autoport;
		port = autoport;
	}

	memset(sport, 0, sizeof(*sport));
	sport->port.ops = &esf_port_ops;
	sport->device = port;
	sport->baudrate = 115200;

	printf("Connecting to @b{%s}...\n", port);
	fflush(stdout);

	err_ = esp_loader_init_uart(loader, &sport->port);
	if (err_ != ESP_LOADER_SUCCESS) {
		err("failed to open %s", port);
		return -1;
	}

	{
		esp_loader_connect_args_t cargs = ESP_LOADER_CONNECT_DEFAULT();
		err_ = esp_loader_connect(loader, &cargs);
	}
	if (err_ != ESP_LOADER_SUCCESS) {
		err("connect failed -- is the device in bootloader mode? "
		    "(Hold BOOT, tap RESET, then release BOOT)");
		return -1;
	}

	if (baud > 115200) {
		err_ =
		    esp_loader_change_transmission_rate(loader, (uint32_t)baud);
		if (err_ != ESP_LOADER_SUCCESS)
			warn("failed to negotiate %d baud; staying at 115200",
			     baud);
	}

	return 0;
}

int pt_target_read_table(esp_loader_t *loader, uint32_t table_offset,
			 struct pt_entry *entries, int *count)
{
	uint8_t buf[PT_DATA_SIZE];
	esp_loader_error_t err_;

	err_ = esp_loader_flash_read(loader, buf, table_offset, sizeof(buf));
	if (err_ != ESP_LOADER_SUCCESS) {
		err("failed to read partition table at 0x%x (esp-loader err "
		    "%d)",
		    table_offset, err_);
		return -1;
	}
	return pt_from_binary(buf, sizeof(buf), entries, count, 1);
}

int pt_target_load(struct pt_entry *entries, int *count, esp_loader_t *loader,
		   uint32_t table_offset)
{
	if (pt_opt_pt_file) {
		struct pt_options opts = {0};
		char *end;

		opts.md5sum = 1;
		opts.table_offset = table_offset;

		if (pt_opt_primary_boot_offset) {
			opts.primary_boot_offset = (uint32_t)strtoul(
			    pt_opt_primary_boot_offset, &end, 0);
			if (*end)
				die("invalid --primary-bootloader-offset: %s",
				    pt_opt_primary_boot_offset);
			opts.has_primary_boot = 1;
		}
		if (pt_opt_recovery_boot_offset) {
			opts.recovery_boot_offset = (uint32_t)strtoul(
			    pt_opt_recovery_boot_offset, &end, 0);
			if (*end)
				die("invalid --recovery-bootloader-offset: %s",
				    pt_opt_recovery_boot_offset);
			opts.has_recovery_boot = 1;
		}
		return pt_load(pt_opt_pt_file, entries, count, &opts);
	}

	if (!loader) {
		err("partition table source not specified: pass "
		    "--partition-table-file or a serial port");
		return -1;
	}
	return pt_target_read_table(loader, table_offset, entries, count);
}
