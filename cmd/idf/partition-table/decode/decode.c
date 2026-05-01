/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/partition-table/decode/decode.c
 * @brief "ice idf partition-table decode" -- binary -> CSV.
 *
 * Implements the inverse of the default CSV->bin path.  Replaces
 * @b{gen_esp32part.py}'s reverse mode (auto-detected by content in
 * the Python tool, exposed as an explicit subcommand here).
 */
#include "ice.h"
#include "partition_table.h"

/* clang-format off */
static const struct cmd_manual idf_pt_decode_manual = {
	.name = "ice idf partition-table decode",
	.summary = "decode a binary partition table back to CSV",

	.description =
	H_PARA("Reads a binary partition table image (the 0xC00-byte form "
	       "produced by @b{ice idf partition-table} or "
	       "@b{gen_esp32part.py}) and writes the equivalent CSV.  When "
	       "the output path is omitted or set to @b{-}, the CSV is "
	       "written to stdout.")
	H_PARA("Symbolic type/subtype names are emitted whenever the "
	       "numeric value is recognised; unknown values fall back to "
	       "decimal.  Sizes use @b{K} / @b{M} suffixes when exactly "
	       "divisible, hex otherwise; offsets are always hex."),

	.examples =
	H_EXAMPLE("ice idf partition-table decode build/partition_table/partition-table.bin")
	H_EXAMPLE("ice idf partition-table decode pt.bin pt.csv")
	H_EXAMPLE("ice idf partition-table decode --no-verify pt.bin"),
};
/* clang-format on */

static int no_verify;
static int quiet;

static const struct option cmd_idf_pt_decode_opts[] = {
    OPT_BOOL('q', "quiet", &quiet, "suppress non-error output"),
    OPT_BOOL(0, "no-verify", &no_verify, "skip the MD5-checksum verification"),
    OPT_END(),
};

int cmd_idf_pt_decode(int argc, const char **argv);

const struct cmd_desc cmd_idf_pt_decode_desc = {
    .name = "decode",
    .fn = cmd_idf_pt_decode,
    .opts = cmd_idf_pt_decode_opts,
    .manual = &idf_pt_decode_manual,
};

int cmd_idf_pt_decode(int argc, const char **argv)
{
	struct pt_options opts;
	struct pt_entry entries[PT_MAX_ENTRIES];
	int count = 0;
	const char *input_path;
	const char *output_path;
	FILE *out;
	int rc;

	argc = parse_options(argc, argv, &cmd_idf_pt_decode_desc);

	if (argc < 1)
		die("usage: ice idf partition-table decode <input.bin> "
		    "[<output.csv>]");

	input_path = argv[0];
	output_path = (argc >= 2) ? argv[1] : "-";

	memset(&opts, 0, sizeof(opts));
	opts.md5sum = !no_verify;

	if (pt_load(input_path, entries, &count, &opts) != 0)
		return 1;

	if (count == 0 && !quiet)
		warn("partition table is empty");

	if (!strcmp(output_path, "-")) {
		rc = pt_to_csv(entries, count, stdout);
	} else {
		mkdirp_for_file(output_path);
		out = fopen(output_path, "w");
		if (!out) {
			err_errno("cannot write '%s'", output_path);
			return 1;
		}
		rc = pt_to_csv(entries, count, out);
		fclose(out);
	}

	return rc != 0;
}
