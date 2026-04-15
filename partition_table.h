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

#include <stdint.h>

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

#endif /* PARTITION_TABLE_H */
