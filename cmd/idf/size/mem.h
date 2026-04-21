/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file mem.h
 * @brief ESP chip memory layout definitions.
 *
 * Hardcoded memory range tables for each supported ESP chip,
 * derived from esp-idf-size chip_info YAML files.  Each chip
 * defines a set of memory ranges with a display name, address
 * range, and optional secondary (alias) address.
 */
#ifndef MEM_H
#define MEM_H

#include <stdint.h>

/**
 * A memory range within a chip's address space.
 *
 * When secondary_addr is non-zero, the same physical memory is
 * accessible via two address ranges (aliasing).  Typical for DIRAM
 * where one SRAM bank is reachable through both the instruction
 * and data bus addresses.
 */
struct chip_mem_range {
	const char *name;	 /**< Display name (e.g. "IRAM", "DIRAM"). */
	uint64_t primary_addr;	 /**< Primary start address. */
	uint64_t length;	 /**< Size in bytes. */
	uint64_t secondary_addr; /**< Alias start address (0 = no alias). */
};

/**
 * Chip memory layout descriptor.
 *
 * The ranges array is NULL-terminated (name == NULL marks the end).
 */
struct chip_info {
	const char *name;		     /**< Chip name (e.g. "esp32s3"). */
	const struct chip_mem_range *ranges; /**< NULL-terminated array. */
};

/**
 * @brief Look up a chip by name.
 *
 * @param name  Chip name (e.g. "esp32s3").
 * @return Pointer to static chip info, or NULL if not found.
 */
const struct chip_info *chip_find(const char *name);

#endif /* MEM_H */
