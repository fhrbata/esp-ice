/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/partition-table/subtypes-header/subtypes-header.c
 * @brief "ice idf partition-table subtypes-header" -- codegen.
 *
 * Replaces @b{gen_extra_subtypes_inc.py}.  Emits a header file
 * declaring user-defined partition subtypes as enum-style
 * @b{ESP_PARTITION_SUBTYPE_<TYPE>_<NAME>} constants.
 *
 * Output format matches the Python tool byte-for-byte (including
 * the leading-tab parttool.py usage block, the trailing space on
 * each quoted subtype, and the blank line after @b{#pragma once}).
 */
#include "ice.h"
#include "partition_table.h"

/* clang-format off */
static const struct cmd_manual idf_pt_subtypes_header_manual = {
	.name = "ice idf partition-table subtypes-header",
	.summary = "generate a header file with custom partition subtypes",

	.description =
	H_PARA("Drop-in replacement for ESP-IDF's "
	       "@b{gen_extra_subtypes_inc.py}.  Each remaining argument "
	       "is a CSV triple @b{TYPE,NAME,VALUE}.  TYPE and NAME are "
	       "upper-cased; VALUE is preserved verbatim.")
	H_PARA("With no triples the header is still emitted -- containing "
	       "just the auto-generated comment, blank line, and "
	       "@b{#pragma once}."),

	.examples =
	H_EXAMPLE("ice idf partition-table subtypes-header subtypes.h fs,foo,0x40 fs,bar,0x41")
	H_EXAMPLE("ice idf partition-table subtypes-header subtypes.h"),
};
/* clang-format on */

static const struct option cmd_idf_pt_subtypes_header_opts[] = {
    OPT_END(),
};

int cmd_idf_pt_subtypes_header(int argc, const char **argv);

const struct cmd_desc cmd_idf_pt_subtypes_header_desc = {
    .name = "subtypes-header",
    .fn = cmd_idf_pt_subtypes_header,
    .opts = cmd_idf_pt_subtypes_header_opts,
    .manual = &idf_pt_subtypes_header_manual,
};

/* Trim leading/trailing whitespace in place; matches Python's str.strip(). */
static void strip(char *s)
{
	char *p, *end;
	for (p = s; *p == ' ' || *p == '\t'; p++)
		;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
}

static void upper_in_place(char *s)
{
	for (; *s; s++)
		if (*s >= 'a' && *s <= 'z')
			*s = (char)(*s - 'a' + 'A');
}

int cmd_idf_pt_subtypes_header(int argc, const char **argv)
{
	const char *output_path;
	FILE *out;
	struct sbuf body = SBUF_INIT;
	int n_subtypes;

	argc = parse_options(argc, argv, &cmd_idf_pt_subtypes_header_desc);

	if (argc < 1)
		die("usage: ice idf partition-table subtypes-header <out> "
		    "[TYPE,NAME,VALUE]...");

	output_path = argv[0];
	n_subtypes = argc - 1;

	mkdirp_for_file(output_path);
	out = fopen(output_path, "w");
	if (!out) {
		err_errno("cannot write '%s'", output_path);
		return 1;
	}

	fputs("/* Automatically generated file. DO NOT EDIT. */\n\n", out);

	if (n_subtypes > 0) {
		fputs("/*\n\tIf you want to use parttool.py manually, please "
		      "use the following as an extra argument:\n\t",
		      out);
		fputs("--extra-partition-subtypes ", out);
		for (int i = 0; i < n_subtypes; i++)
			fprintf(out, "\"%s\" ", argv[1 + i]);
		fputs("\n*/\n\n", out);
	}

	fputs("#pragma once\n\n", out);

	for (int i = 0; i < n_subtypes; i++) {
		char *copy = sbuf_strdup(argv[1 + i]);
		char *p1, *p2;
		char ftype[64];
		char fname[64];
		char fval[64];

		p1 = strchr(copy, ',');
		if (!p1) {
			err("invalid subtype tuple '%s' (expected "
			    "TYPE,NAME,VALUE)",
			    argv[1 + i]);
			free(copy);
			fclose(out);
			return 1;
		}
		*p1++ = '\0';
		p2 = strchr(p1, ',');
		if (!p2) {
			err("invalid subtype tuple '%s' (expected "
			    "TYPE,NAME,VALUE)",
			    argv[1 + i]);
			free(copy);
			fclose(out);
			return 1;
		}
		*p2++ = '\0';

		strip(copy);
		strip(p1);
		strip(p2);
		snprintf(ftype, sizeof(ftype), "%s", copy);
		snprintf(fname, sizeof(fname), "%s", p1);
		snprintf(fval, sizeof(fval), "%s", p2);
		upper_in_place(ftype);
		upper_in_place(fname);

		fprintf(out, "ESP_PARTITION_SUBTYPE_%s_%s = %s,\n", ftype,
			fname, fval);

		free(copy);
	}

	fclose(out);
	sbuf_release(&body);
	return 0;
}
