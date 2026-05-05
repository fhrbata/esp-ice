/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/coredump/synth.h
 * @brief Synthesise a GDB-loadable ELF32 core file from a BIN_V*
 * dump.
 *
 * For ELF_* dumps (@c CORE_DUMP_MAJOR(dump_ver) >= 1) the data
 * section already is an ELF and ice extracts it directly in
 * @c coredump.c.  For BIN_V* dumps the chip wrote raw TCBs +
 * memory segments instead, and the host has to translate them to
 * ELF before gdb can consume them.  This module mirrors upstream
 * @c esp_coredump.corefile.loader._extract_bin_corefile.
 */
#ifndef CMD_IDF_COREDUMP_SYNTH_H
#define CMD_IDF_COREDUMP_SYNTH_H

#include <stddef.h>

struct core_header;
struct sbuf;

/*
 * Synthesise an ELF32 core file from the BIN data section of
 * @p header.  @p data must point to the bytes between the wire
 * header and the trailing checksum (i.e. @p data_len must equal
 * @c{tot_len - header_size - checksum_size}).
 *
 * On success returns 0 with the synthesised ELF written to
 * @p out.  On failure returns -1 and sets @p *err to a static
 * error string.  Failure modes: unsupported chip, malformed task /
 * segment array, sane-pointer rejection of every captured region.
 *
 * The resulting buffer is suitable for @c{gdb --core=<file>}.
 */
int core_synth_elf(const struct core_header *header, const void *data,
		   size_t data_len, struct sbuf *out, const char **err);

#endif /* CMD_IDF_COREDUMP_SYNTH_H */
