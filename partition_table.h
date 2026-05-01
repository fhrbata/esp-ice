/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file partition_table.h
 * @brief ESP partition table CSV parser and binary writer.
 *
 * Replaces gen_esp32part.py.  Chip-agnostic: the partition table
 * binary format is identical across all ESP32 variants.
 */
#ifndef PARTITION_TABLE_H
#define PARTITION_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Binary layout constants */
#define PT_ENTRY_SIZE 32
#define PT_MAX_ENTRIES 96
#define PT_TABLE_SIZE 0x1000 /* flash sector occupied by the table */
#define PT_DATA_SIZE 0x0C00  /* writable data area: entries + MD5 */

/* Partition types */
#define PT_TYPE_APP 0x00
#define PT_TYPE_DATA 0x01
#define PT_TYPE_BOOT 0x02
#define PT_TYPE_PTABLE 0x03

/* Named data subtypes (default for an empty data subtype field). */
#define PT_SUBTYPE_DATA_UNDEFINED 0x06

/* Secure boot modes */
#define PT_SECURE_NONE 0
#define PT_SECURE_V1 1
#define PT_SECURE_V2 2

struct pt_entry {
	char name[17]; /* max 16 chars, null-terminated */
	uint8_t type;
	uint8_t subtype;
	uint32_t offset; /* explicit offset, or 0 if auto */
	uint32_t size;	 /* size in bytes, or end-address (see flag below) */
	uint8_t encrypted;
	uint8_t readonly;
	int offset_set;	      /* 1 if offset was given in CSV */
	int size_is_end_addr; /* 1 if @size holds an end-address to resolve */
};

struct pt_options {
	uint32_t table_offset;	       /* --offset (default 0x8000) */
	uint32_t primary_boot_offset;  /* --primary-bootloader-offset */
	uint32_t recovery_boot_offset; /* --recovery-bootloader-offset */
	int has_primary_boot;	       /* 1 if primary_boot_offset is set */
	int has_recovery_boot;	       /* 1 if recovery_boot_offset is set */
	int md5sum;		       /* 1 = append MD5 entry (default) */
	int secure;		       /* PT_SECURE_* */
	uint32_t flash_size;	       /* 0 = no size check */
};

/**
 * @brief Parse a CSV partition table file.
 *
 * Fills @p entries (up to PT_MAX_ENTRIES) and sets *count.
 * Auto-fills missing offsets per IDF alignment rules.
 *
 * @return 0 on success, -1 on error (message printed to stderr).
 */
int pt_parse_csv(const char *path, struct pt_entry *entries, int *count,
		 const struct pt_options *opts);

/**
 * @brief Serialise entries to the 3 KB binary partition table image.
 *
 * @p out must be PT_DATA_SIZE (0xC00) bytes.  Remaining bytes are
 * filled with 0xFF (reserved for signing).
 *
 * @return 0 on success, -1 on error.
 */
int pt_to_binary(const struct pt_entry *entries, int count,
		 const struct pt_options *opts, uint8_t out[PT_DATA_SIZE]);

/**
 * @brief Parse a serialised partition table back into entries.
 *
 * Walks 32-byte slots; stops at the first all-0xFF slot.  Skips the
 * MD5 entry (magic 0xEB 0xEB), verifying its digest when @p
 * verify_md5 is non-zero.  Rejects unknown magic words.
 *
 * Each filled @c pt_entry has @c offset_set = 1 and @c
 * size_is_end_addr = 0 (binary entries are always fully resolved).
 *
 * @return 0 on success, -1 on error (message printed via err()).
 */
int pt_from_binary(const uint8_t *bin, size_t len, struct pt_entry *entries,
		   int *count, int verify_md5);

/**
 * @brief Auto-detect CSV vs binary by looking at the first two bytes.
 *
 * On binary magic (0xAA 0x50 or 0xEB 0xEB) parses with @c
 * pt_from_binary (MD5 verified iff @p opts->md5sum); otherwise
 * forwards to @c pt_parse_csv.
 *
 * @return 0 on success, -1 on error.
 */
int pt_load(const char *path, struct pt_entry *entries, int *count,
	    const struct pt_options *opts);

/**
 * @brief Emit a partition table as canonical CSV.
 *
 * Format mirrors @b{gen_esp32part.py}'s @c PartitionTable.to_csv()
 * byte-for-byte:
 *
 * @code
 *   # ESP-IDF Partition Table
 *   # Name, Type, SubType, Offset, Size, Flags
 *   nvs,data,nvs,0x9000,24K,
 *   factory,app,factory,0x10000,1M,
 * @endcode
 *
 * Sizes use @c K / @c M when exactly divisible, otherwise hex.
 * Offsets are always hex.  The Flags column has a trailing comma
 * even when empty.  A trailing newline follows the last row.
 *
 * @return 0 on success, -1 on I/O error.
 */
int pt_to_csv(const struct pt_entry *entries, int count, FILE *out);

/**
 * @brief Format a partition type symbolically into @p buf.
 *
 * Writes the registered name (e.g. "app", "data") when known; falls
 * back to a decimal numeric formatting (matching gen_esp32part.py's
 * @c lookup_keyword default).
 *
 * @p buflen should be at least 16.  Returns @p buf for chaining.
 */
const char *pt_format_type(uint8_t type, char *buf, size_t buflen);

/**
 * @brief Format a partition subtype symbolically into @p buf.
 *
 * Handles the two synthetic ranges:
 *   - app subtype 0x10..0x1F   -> "ota_0".."ota_15"
 *   - app subtype 0x30..0x31   -> "tee_0".."tee_1"
 *
 * Then consults the static name table; falls back to decimal when
 * unknown (matching gen_esp32part.py).
 *
 * @p buflen should be at least 16.  Returns @p buf.
 */
const char *pt_format_subtype(uint8_t type, uint8_t subtype, char *buf,
			      size_t buflen);

/**
 * @brief Format a size with M/K suffix when divisible, hex otherwise.
 *
 * Mirrors gen_esp32part.py's @c addr_format(a, include_sizes=True).
 *
 * @p buflen should be at least 16.  Returns @p buf.
 */
const char *pt_format_size(uint32_t size, char *buf, size_t buflen);

/**
 * @brief Parse a partition type name to its numeric value.
 *
 * Accepts the symbolic names (@b{app}, @b{data}, @b{bootloader},
 * @b{partition_table}) or any numeric form @c strtoul(_, _, 0)
 * understands (decimal, @b{0x}-hex).
 *
 * @return 0 on success, -1 if @p s does not parse.
 */
int pt_parse_type(const char *s, uint8_t *out);

/**
 * @brief Parse a partition subtype name (in the context of @p type) to
 * its numeric value.
 *
 * Honours the synthetic ranges (app @c ota_0..ota_15, app
 * @c tee_0..tee_1), the static name table, any names registered via
 * pt_register_subtype(), and finally a numeric fallback.
 *
 * @return 0 on success, -1 if @p s does not parse.
 */
int pt_parse_subtype(uint8_t type, const char *s, uint8_t *out);

/**
 * @brief Register a custom subtype name for runtime lookup.
 *
 * Mirrors @b{gen_esp32part.py}'s @c add_extra_subtypes mechanism:
 * the (@p type, @p name, @p val) triple becomes recognised by
 * pt_parse_subtype() and emitted by pt_format_subtype() on the
 * matching numeric value.
 *
 * Duplicates of an existing static or extra entry are tolerated
 * silently when they match in value; conflicts (same name,
 * different value) are reported and rejected.
 *
 * @return 0 on success, -1 on a name/value conflict.
 */
int pt_register_subtype(uint8_t type, const char *name, uint8_t val);

/**
 * @brief Drop every entry registered via pt_register_subtype.
 *
 * Useful in tests and when re-running the parser within a single
 * process for different partition tables with different extras.
 */
void pt_clear_extras(void);

#endif /* PARTITION_TABLE_H */
