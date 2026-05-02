/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/partition/partition.h
 * @brief Shared helpers for `ice target partition <verb>`.
 *
 * The four leaf subcommands (info, read, write, erase) share:
 *   - the same partition selector (--name | --type+--subtype | --boot-default)
 *   - the same partition-table source resolver (file or device)
 *   - the same device-connect flow (port / baud / chip)
 *
 * These live here so each leaf is just selector wiring + a single
 * device call.
 */
#ifndef CMD_TARGET_PARTITION_H
#define CMD_TARGET_PARTITION_H

#include "esf_port.h"
#include "esp_loader.h"
#include "partition_table.h"
#include "serial.h"

/**
 * @brief Partition-selector arguments common to every verb.
 *
 * Exactly one of @c name, the (@c type / @c subtype) pair, or @c
 * boot_default must be set.  @c subtype is optional when @c type
 * is given (defaults to "match any subtype with this type").
 */
struct pt_target_selector {
	const char *name;    /**< --name, or NULL */
	const char *type;    /**< --type, or NULL */
	const char *subtype; /**< --subtype, or NULL */
	int boot_default;    /**< --boot-default flag */
};

/**
 * @brief Common-option globals.  Each leaf declares them via the
 * @ref pt_target_common_opts macro inside its option table.
 */
extern const char *pt_opt_port;
extern int pt_opt_baud;
extern const char *pt_opt_pt_file;
extern const char *pt_opt_pt_offset;
extern const char *pt_opt_name;
extern const char *pt_opt_type;
extern const char *pt_opt_subtype;
extern int pt_opt_boot_default;
extern const char *pt_opt_primary_boot_offset;
extern const char *pt_opt_recovery_boot_offset;
extern struct svec pt_opt_extra_subtypes;
/* Accepted for parttool.py compat: IDF cmake invokes parttool.py with
 * @c -q to suppress its progress chatter.  We just record it -- ice's
 * partition verbs don't currently print the "Reading partition table..."
 * messages parttool.py muted, so there's nothing to gate.  Without this
 * flag the @b{partition_table_get_partition_info} cmake helper sees
 * @c{fatal: unknown option: -q}, captures empty output, and downstream
 * @c{flasher_args.json} ends up with @c{"offset": ""} for the app. */
extern int pt_opt_quiet;
/* parttool.py forwards these to esptool; ice doesn't shell out to
 * esptool, so accepting them and silently dropping the configuration
 * would be a workaround.  Each handler calls pt_target_finalize_opts()
 * which die()s with a useful message when any of them is non-empty. */
extern struct svec pt_opt_esptool_args;
extern struct svec pt_opt_esptool_write_args;
extern struct svec pt_opt_esptool_read_args;
extern struct svec pt_opt_esptool_erase_args;

/**
 * @brief Reset all pt_opt_* globals to their defaults.
 *
 * Call at the top of each leaf's handler so a previously-parsed
 * value (e.g. from a unit test) doesn't leak across runs.
 */
void pt_target_reset_opts(void);

/**
 * @brief Validate the post-parse option state shared by every verb:
 *
 *   - reject any of the four @b{--esptool-*-args} flags with a useful
 *     diagnostic (ice does not delegate to esptool);
 *   - register every @b{--extra-partition-subtypes} TYPE,NAME,VALUE
 *     triple via pt_register_subtype() so subsequent CSV parsing
 *     recognises the custom name.
 *
 * Must be called after parse_options() and before pt_target_load() in
 * each leaf.  Dies on any failure.
 */
void pt_target_finalize_opts(void);

/**
 * @brief Resolve --type / --subtype / --name into a single entry.
 *
 * Walks @p entries (count @p n) for a match.  --boot-default uses
 * IDF's bootloader fallback: read otadata if there is one and a
 * matching ota_N exists; otherwise fall through to factory; otherwise
 * fall through to ota_0.  When @c boot_default is requested but no
 * device handle is provided (@p loader is NULL), only the static
 * fallback (factory -> ota_0) is consulted.
 *
 * @return Pointer into @p entries on success, NULL if nothing matched
 *         (a message is printed via err()).
 */
const struct pt_entry *pt_target_select(const struct pt_entry *entries, int n,
					const struct pt_target_selector *sel,
					esp_loader_t *loader);

/**
 * @brief Open and connect to the device, leaving the loader ready
 * for flash_read / flash_write / flash_erase_region.
 *
 * @p port may be NULL to auto-detect; @p baud is the negotiated
 * post-handshake speed.  On success the caller owns the @p loader,
 * the @p sport (which lives for the loader's lifetime), and any
 * autoport string written through @p autoport_out (free()-able).
 *
 * @return 0 on success, non-zero error printed via err().
 */
int pt_target_connect(esp_loader_t *loader, esf_port_t *sport, const char *port,
		      int baud, char **autoport_out);

/**
 * @brief Read the partition table from the connected device.
 *
 * Reads PT_DATA_SIZE bytes at @p table_offset and parses with
 * pt_from_binary().  MD5 verification is enabled by default.
 *
 * @return 0 on success, non-zero on read or parse error.
 */
int pt_target_read_table(esp_loader_t *loader, uint32_t table_offset,
			 struct pt_entry *entries, int *count);

/**
 * @brief Load the partition table from --partition-table-file or, if
 * absent, from the device.
 *
 * For the file path, falls through to pt_load() (auto-detects CSV vs
 * binary).  When loading from device, the caller must have already
 * connected @p loader.
 */
int pt_target_load(struct pt_entry *entries, int *count, esp_loader_t *loader,
		   uint32_t table_offset);

#endif /* CMD_TARGET_PARTITION_H */
