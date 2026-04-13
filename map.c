/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file map.c
 * @brief GCC/LD linker map file parser implementation.
 *
 * Parses linker map files produced by GCC/LD (`-Wl,-Map,file.map`).
 * The format knowledge comes from ld's ldlang.c (print_output_section_
 * statement, print_input_section, print_padding_statement, etc.).
 *
 * The parser works in-place on the source buffer: newlines are replaced
 * with NUL bytes to form lines, whitespace between tokens is replaced
 * with NUL bytes to form C strings, and '('/')' in archive references
 * are replaced to split archive and object names.  All string fields
 * in the output structures point directly into the modified buffer.
 *
 * Output section names are NOT restricted to starting with '.'.  Linker
 * scripts can define arbitrary names (e.g. FLASH, IRAM, my_section).
 * The parser detects output sections structurally: any non-whitespace
 * token at column 0 that is not a recognized keyword (LOAD, START GROUP,
 * etc.) is treated as an output section name.
 *
 * Input sections are also detected structurally: a line with a name
 * followed by two hex values (address, size) and an archive/object
 * reference, rather than relying on section name prefixes.
 */
#include "ice.h"

/* Sentinel for addresses not yet parsed (split-line case). */
#define ADDR_NONE UINT64_MAX

/* ---- Helpers -------------------------------------------------------- */

/** Check whether a NUL-terminated string starts with "0x" or "0X". */
static int is_hex(const char *s)
{
	return s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
}

/**
 * Check whether a NUL-terminated string is one of the explicit data
 * keywords: BYTE, SHORT, LONG, QUAD, SQUAD.
 */
static int is_explicit_data(const char *s)
{
	return strcmp(s, "BYTE") == 0 || strcmp(s, "SHORT") == 0 ||
	       strcmp(s, "LONG") == 0 || strcmp(s, "QUAD") == 0 ||
	       strcmp(s, "SQUAD") == 0;
}

/**
 * Check whether a column-0 line is a recognized non-section keyword.
 * Derived from ld's print_statement() dispatcher.
 */
static int is_top_level_keyword(const char *s)
{
	return strncmp(s, "LOAD ", 5) == 0 ||
	       strncmp(s, "START GROUP", 11) == 0 ||
	       strncmp(s, "END GROUP", 9) == 0 ||
	       strncmp(s, "OUTPUT(", 7) == 0 || strncmp(s, "TARGET(", 7) == 0 ||
	       strncmp(s, "INSERT ", 7) == 0 ||
	       strncmp(s, "Address of section ", 19) == 0;
}

/**
 * Split "archive(object)" in-place by replacing '(' and ')' with NUL.
 * Sets archive to "(exe)" for directly linked objects (no parentheses).
 */
static void parse_archive_object(char *s, const char **archive,
				 const char **object)
{
	char *paren = strchr(s, '(');
	char *close;

	if (!paren) {
		*archive = "(exe)";
		*object = s;
		return;
	}

	*paren = '\0';
	*archive = s;
	*object = paren + 1;
	close = strchr(*object, ')');
	if (close)
		*close = '\0';
}

/* ---- Input section finalization ------------------------------------- */

/**
 * Finalize and append a parsed input section to an output section.
 *
 * Incomplete input sections (address still ADDR_NONE because the
 * expected continuation line never arrived) are silently discarded.
 *
 * For complete sections, handles edge cases from the linker map format:
 *   - Input sections outside the output section's address range get
 *     their size zeroed.
 *   - When two input sections share the same address (e.g. -Og builds),
 *     the earlier one gets its size zeroed.
 *   - Zero-size input sections with fill have their fill redistributed
 *     to the last preceding input section with non-zero size.
 */
static void flush_input(struct map_section *sec, struct map_input *inp,
			int *alloc)
{
	struct map_input *prev;
	int i;

	if (!inp->name || inp->address == ADDR_NONE)
		return;

	if (!inp->archive) {
		inp->archive = "";
		inp->object = "";
	}

	if (sec->nr_inputs == 0)
		goto append;

	/* Zero size if outside output section range. */
	if (inp->address < sec->address ||
	    inp->address >= sec->address + sec->size)
		inp->size = 0;

	/* Same address as previous -- previous gets zeroed. */
	prev = &sec->inputs[sec->nr_inputs - 1];
	if (prev->address == inp->address)
		prev->size = 0;

	/* Redistribute fill from zero-size section. */
	if (!inp->size && inp->fill) {
		for (i = sec->nr_inputs - 1; i >= 0; i--) {
			if (sec->inputs[i].size) {
				sec->inputs[i].fill += inp->fill;
				break;
			}
		}
		inp->fill = 0;
	}

append:
	ALLOC_GROW(sec->inputs, sec->nr_inputs + 1, *alloc);
	sec->inputs[sec->nr_inputs] = *inp;
	sec->nr_inputs++;
}

/* ---- Section-specific parsers --------------------------------------- */

/**
 * Parse "Memory Configuration" into out->regions.
 * Advances *pos past the "Linker script and memory map" line.
 */
static void parse_memory_regions(char *buf, size_t len, size_t *pos,
				 struct map_file *out)
{
	char *line;
	char *tok[5];
	int found = 0, header = 0;
	int alloc = 0;
	int n;

	while ((line = sbuf_getline(buf, len, pos)) != NULL) {
		if (strncmp(line, "Linker script and memory map", 28) == 0)
			break;

		if (!found) {
			if (strncmp(line, "Memory Configuration", 20) == 0)
				found = 1;
			continue;
		}

		if (!header) {
			if (strncmp(line, "Name", 4) == 0)
				header = 1;
			continue;
		}

		n = sbuf_split(line, tok, 5);
		if (n == 0)
			continue;
		if (n != 3 && n != 4)
			die("map: unexpected memory region format");

		ALLOC_GROW(out->regions, out->nr_regions + 1, alloc);
		out->regions[out->nr_regions].name = tok[0];
		out->regions[out->nr_regions].origin =
		    strtoull(tok[1], NULL, 0);
		out->regions[out->nr_regions].length =
		    strtoull(tok[2], NULL, 0);
		out->regions[out->nr_regions].attrs = (n == 4) ? tok[3] : "";
		out->nr_regions++;
	}

	if (!found || !header)
		die("map: cannot find \"Memory Configuration\" section");
}

/**
 * Parse "Linker script and memory map" into out->sections.
 * Expects *pos to be right after the "Linker script and memory map" line.
 */
static void parse_sections(char *buf, size_t len, size_t *pos,
			   struct map_file *out)
{
	char *line, *p;
	char *tok[4];
	struct map_section sec;
	struct map_input inp;
	int sec_alloc = 0;
	int inp_alloc = 0;
	int in_section = 0;
	int in_input = 0;
	int has_input = 0;
	int n;

	memset(&sec, 0, sizeof(sec));
	memset(&inp, 0, sizeof(inp));

	while ((line = sbuf_getline(buf, len, pos)) != NULL) {
		if (strncmp(line, "Cross Reference Table", 21) == 0)
			break;

		if (in_section) {
			/* Trim leading whitespace. */
			p = line;
			while (*p == ' ' || *p == '\t')
				p++;

			if (!*p) {
				/* Empty line -- end of output section. */
				if (has_input)
					flush_input(&sec, &inp, &inp_alloc);

				ALLOC_GROW(out->sections, out->nr_sections + 1,
					   sec_alloc);
				out->sections[out->nr_sections] = sec;
				out->nr_sections++;

				in_section = 0;
				in_input = 0;
				has_input = 0;
				memset(&sec, 0, sizeof(sec));
				inp_alloc = 0;
				continue;
			}

			/*
			 * Output section split: name on previous line, this
			 * line should be addr+size.  If the first token is
			 * hex, parse as continuation; otherwise fall through
			 * to content processing without modifying the line.
			 */
			if (sec.address == ADDR_NONE && is_hex(p)) {
				sec.address = 0;
				sec.size = 0;
				n = sbuf_split(p, tok, 3);
				if (n >= 2) {
					sec.address = strtoull(tok[0], NULL, 0);
					sec.size = strtoull(tok[1], NULL, 0);
				}
				continue;
			}
			if (sec.address == ADDR_NONE) {
				sec.address = 0;
				sec.size = 0;
			}

			/*
			 * *fill* padding.  Always consume so it doesn't
			 * match the input section pattern.  Attribute to
			 * the current input section if we have one.
			 */
			if (strncmp(p, "*fill*", 6) == 0) {
				if (in_input) {
					n = sbuf_split(p, tok, 4);
					if (n >= 3) {
						uint64_t fa =
						    strtoull(tok[1], NULL, 0);
						uint64_t fs =
						    strtoull(tok[2], NULL, 0);
						if (inp.address == fa)
							inp.size = 0;
						inp.fill += fs;
					}
				}
				continue;
			}

			/* Skip wildcard patterns. */
			if (*p == '*')
				continue;

			/*
			 * Lines starting with hex (indented to column 16):
			 * explicit data, input continuation, or symbol
			 * listings.  Route by checking the line before
			 * splitting, to avoid destroying tokens we might
			 * need to re-parse.
			 */
			if (is_hex(p)) {
				if (in_input && inp.address == ADDR_NONE) {
					n = sbuf_split(p, tok, 3);
					if (n >= 3 && is_hex(tok[1])) {
						inp.address =
						    strtoull(tok[0], NULL, 0);
						inp.size =
						    strtoull(tok[1], NULL, 0);
						parse_archive_object(
						    tok[2], &inp.archive,
						    &inp.object);
					} else {
						memset(&inp, 0, sizeof(inp));
						has_input = 0;
						in_input = 0;
					}
				} else if (in_input) {
					n = sbuf_split(p, tok, 4);
					if (n >= 4 && is_explicit_data(tok[2]))
						inp.fill +=
						    strtoull(tok[1], NULL, 0);
				}
				continue;
			}

			/*
			 * Non-hex line: input section or split name.
			 *
			 * Input section on one line:
			 *   name  0xaddr  0xsize  archive(object)
			 *
			 * Split name (>= 15 chars, address on next line):
			 *   name
			 */
			n = sbuf_split(p, tok, 4);

			if (n >= 4 && is_hex(tok[1]) && is_hex(tok[2])) {
				if (has_input)
					flush_input(&sec, &inp, &inp_alloc);

				memset(&inp, 0, sizeof(inp));
				in_input = 1;
				has_input = 1;
				inp.name = tok[0];
				inp.address = strtoull(tok[1], NULL, 0);
				inp.size = strtoull(tok[2], NULL, 0);
				parse_archive_object(tok[3], &inp.archive,
						     &inp.object);
				continue;
			}

			if (n == 1 && !strchr(tok[0], ')')) {
				if (has_input)
					flush_input(&sec, &inp, &inp_alloc);

				memset(&inp, 0, sizeof(inp));
				in_input = 1;
				has_input = 1;
				inp.name = tok[0];
				inp.address = ADDR_NONE;
				continue;
			}

			continue;
		}

		/*
		 * Detect IDF_TARGET_<CHIP> symbol for chip target.
		 * Appears as a top-level symbol assignment before
		 * any output sections, e.g.:
		 *   0x00000000  IDF_TARGET_ESP32S3 = 0x0
		 */
		if (!out->target) {
			const char *t = strstr(line, "IDF_TARGET_");
			if (t) {
				t += 11; /* skip "IDF_TARGET_" */
				out->target = t;
				/* Lowercase in-place and NUL-terminate
				 * at the first non-alnum character. */
				for (p = (char *)t; (*p >= 'A' && *p <= 'Z') ||
				     (*p >= '0' && *p <= '9'); p++)
					*p |= 0x20;
				*p = '\0';
			}
		}

		/*
		 * Top level: detect new output section.  Any non-whitespace
		 * at column 0 that isn't a recognized keyword is a section.
		 */
		if (line[0] && line[0] != ' ' && line[0] != '\t' &&
		    !is_top_level_keyword(line)) {
			in_section = 1;
			in_input = 0;
			has_input = 0;
			inp_alloc = 0;
			memset(&sec, 0, sizeof(sec));
			memset(&inp, 0, sizeof(inp));

			n = sbuf_split(line, tok, 4);
			sec.name = tok[0];
			if (n >= 3 && is_hex(tok[1]) && is_hex(tok[2])) {
				sec.address = strtoull(tok[1], NULL, 0);
				sec.size = strtoull(tok[2], NULL, 0);
			} else {
				sec.address = ADDR_NONE;
			}
		}
	}

	/* Flush any trailing output section. */
	if (in_section) {
		if (sec.address == ADDR_NONE) {
			sec.address = 0;
			sec.size = 0;
		}
		if (has_input)
			flush_input(&sec, &inp, &inp_alloc);

		ALLOC_GROW(out->sections, out->nr_sections + 1, sec_alloc);
		out->sections[out->nr_sections] = sec;
		out->nr_sections++;
	}
}

/* ---- Public API ----------------------------------------------------- */

void map_read(char *buf, size_t len, struct map_file *out)
{
	size_t pos = 0;

	out->target = NULL;
	out->regions = NULL;
	out->nr_regions = 0;
	out->sections = NULL;
	out->nr_sections = 0;

	parse_memory_regions(buf, len, &pos, out);
	parse_sections(buf, len, &pos, out);
}

void map_release(struct map_file *out)
{
	int i;

	free(out->regions);
	for (i = 0; i < out->nr_sections; i++)
		free(out->sections[i].inputs);
	free(out->sections);

	out->regions = NULL;
	out->nr_regions = 0;
	out->sections = NULL;
	out->nr_sections = 0;
}
