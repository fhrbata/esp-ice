/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/partition-table/check-partition/check-partition.c
 * @brief "ice idf partition-table check-partition" -- size guard.
 *
 * Replaces @b{check_sizes.py partition}.  Looks up partitions of a
 * given type / subtype in a partition-table file, finds the
 * smallest one, and reports whether the supplied binary fits with
 * the same wording the IDF build pipeline prints.
 */
#include "ice.h"
#include "partition_table.h"

/* clang-format off */
static const struct cmd_manual idf_pt_check_part_manual = {
	.name = "ice idf partition-table check-partition",
	.summary = "verify a binary fits in the smallest matching partition",

	.description =
	H_PARA("Drop-in replacement for ESP-IDF's @b{check_sizes.py "
	       "partition}.  The partition-table file may be either CSV "
	       "or the binary form -- @b{ice} auto-detects.")
	H_PARA("Behaviour mirrors the Python tool's intent (the upstream "
	       "@b{--allow_failures} flag is parsed but never propagated, "
	       "so it has no effect there; @b{ice} honours @b{--allow-failures} as documented):")
	H_LINE("@b{*} on a fit, prints the binary size, smallest matching partition size, and free space (rounded percent)")
	H_LINE("@b{*} when the smallest matching partition has under 5%% free, also emits a Warning line")
	H_LINE("@b{*} when only some partitions overflow, prints a Warning and exits 0")
	H_LINE("@b{*} when all matching partitions overflow, prints an Error to stderr and exits 1 (or Warning + exit 0 with @b{--allow-failures})"),

	.examples =
	H_EXAMPLE("ice idf partition-table check-partition --type app pt.bin app.bin")
	H_EXAMPLE("ice idf partition-table check-partition --type app --subtype tee_0 pt.bin tee.bin"),
};
/* clang-format on */

static const char *opt_type;
static const char *opt_subtype;
static const char *opt_offset = "0x8000";
static int opt_allow_failures;
static const char *opt_target_unused;

static const struct option cmd_idf_pt_check_part_opts[] = {
    OPT_STRING(0, "type", &opt_type, "T",
	       "partition type to check (e.g. app, data, or numeric)", NULL),
    OPT_STRING(0, "subtype", &opt_subtype, "S",
	       "partition subtype to filter by (optional)", NULL),
    OPT_STRING(0, "offset", &opt_offset, "hex",
	       "partition table offset (default 0x8000)", NULL),
    OPT_BOOL(0, "allow-failures", &opt_allow_failures,
	     "downgrade overflow errors to warnings"),
    OPT_STRING(0, "target", &opt_target_unused, "name",
	       "ignored; accepted for check_sizes.py compatibility", NULL),
    OPT_END(),
};

int cmd_idf_pt_check_partition(int argc, const char **argv);

const struct cmd_desc cmd_idf_pt_check_partition_desc = {
    .name = "check-partition",
    .fn = cmd_idf_pt_check_partition,
    .opts = cmd_idf_pt_check_part_opts,
    .manual = &idf_pt_check_part_manual,
};

/* Round half-to-even (Python's banker rounding) for n/d. */
static unsigned percent_round(uint64_t numerator, uint64_t denominator)
{
	uint64_t q = numerator / denominator;
	uint64_t r = numerator % denominator;
	uint64_t twice_r = r * 2;

	if (twice_r < denominator)
		return (unsigned)q;
	if (twice_r > denominator)
		return (unsigned)(q + 1);
	return (unsigned)((q & 1) ? q + 1 : q);
}

static long file_size(FILE *fp)
{
	long pos;
	if (fseek(fp, 0, SEEK_END) != 0)
		return -1;
	pos = ftell(fp);
	if (fseek(fp, 0, SEEK_SET) != 0)
		return -1;
	return pos;
}

static const char *basename_of(const char *path)
{
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}

int cmd_idf_pt_check_partition(int argc, const char **argv)
{
	struct pt_options pt_opts;
	struct pt_entry entries[PT_MAX_ENTRIES];
	int count = 0;
	const char *pt_path, *bin_path;
	const char *bin_name;
	uint8_t want_type, want_subtype = 0;
	int have_subtype = 0;
	uint32_t pt_offset;
	char ptype_str[32];
	long bin_size;
	FILE *fp;
	int matched_idx[PT_MAX_ENTRIES];
	int matched_n = 0;
	uint32_t smallest = 0;
	int too_small_idx[PT_MAX_ENTRIES];
	int too_small_n = 0;
	char *end;

	argc = parse_options(argc, argv, &cmd_idf_pt_check_partition_desc);

	if (!opt_type)
		die("usage: ice idf partition-table check-partition --type T "
		    "[--subtype S] <partition_table> <binary>");

	if (argc < 2)
		die("usage: ice idf partition-table check-partition --type T "
		    "[--subtype S] <partition_table> <binary>");

	pt_path = argv[0];
	bin_path = argv[1];
	bin_name = basename_of(bin_path);

	if (pt_parse_type(opt_type, &want_type) != 0)
		die("invalid --type value: %s", opt_type);

	if (opt_subtype) {
		if (pt_parse_subtype(want_type, opt_subtype, &want_subtype) !=
		    0)
			die("invalid --subtype value: %s (for type %s)",
			    opt_subtype, opt_type);
		have_subtype = 1;
		snprintf(ptype_str, sizeof(ptype_str), "%s (%s)", opt_type,
			 opt_subtype);
	} else {
		snprintf(ptype_str, sizeof(ptype_str), "%s", opt_type);
	}

	pt_offset = (uint32_t)strtoul(opt_offset, &end, 0);
	if (*end)
		die("invalid --offset value: %s", opt_offset);

	memset(&pt_opts, 0, sizeof(pt_opts));
	pt_opts.table_offset = pt_offset;
	pt_opts.md5sum = 1;

	if (pt_load(pt_path, entries, &count, &pt_opts) != 0)
		return 1;

	for (int i = 0; i < count; i++) {
		if (entries[i].type != want_type)
			continue;
		if (have_subtype && entries[i].subtype != want_subtype)
			continue;
		matched_idx[matched_n++] = i;
	}

	if (matched_n == 0) {
		printf("WARNING: Partition table does not contain any "
		       "partitions matching %s\n",
		       ptype_str);
		return 0;
	}

	fp = fopen(bin_path, "rb");
	if (!fp) {
		err_errno("cannot open '%s'", bin_path);
		return 1;
	}
	bin_size = file_size(fp);
	fclose(fp);
	if (bin_size < 0) {
		err("cannot determine size of '%s'", bin_path);
		return 1;
	}

	smallest = entries[matched_idx[0]].size;
	for (int j = 1; j < matched_n; j++) {
		uint32_t s = entries[matched_idx[j]].size;
		if (s < smallest)
			smallest = s;
	}

	if (smallest >= (uint32_t)bin_size) {
		uint32_t free = smallest - (uint32_t)bin_size;
		unsigned pct = percent_round((uint64_t)free * 100, smallest);

		printf("%s binary size 0x%lx bytes. Smallest %s partition is "
		       "0x%x bytes. 0x%x bytes (%u%%) free.\n",
		       bin_name, bin_size, ptype_str, smallest, free, pct);

		/* Python's threshold is 5% (free / smallest < 0.05).  We
		 * compare in integer space: free*100 < smallest*5. */
		if ((uint64_t)free * 100 < (uint64_t)smallest * 5) {
			printf("Warning: The smallest %s partition is nearly "
			       "full (%u%% free space left)!\n",
			       ptype_str, pct);
		}
		return 0;
	}

	for (int j = 0; j < matched_n; j++) {
		if (entries[matched_idx[j]].size < (uint32_t)bin_size)
			too_small_idx[too_small_n++] = matched_idx[j];
	}

	{
		struct sbuf msg = SBUF_INIT;
		int all_overflow = (too_small_n == matched_n);
		int is_error = !opt_allow_failures && all_overflow;
		FILE *out = is_error ? stderr : stdout;
		const char *prefix = is_error ? "Error" : "Warning";

		if (matched_n == 1)
			sbuf_addf(&msg, "%s partition is", ptype_str);
		else if (all_overflow)
			sbuf_addf(&msg, "All %s partitions are", ptype_str);
		else
			sbuf_addf(&msg, "%d/%d %s partitions are", too_small_n,
				  matched_n, ptype_str);

		sbuf_addf(&msg,
			  " too small for binary %s size 0x%lx:", bin_name,
			  bin_size);

		for (int j = 0; j < too_small_n; j++) {
			const struct pt_entry *e = &entries[too_small_idx[j]];
			sbuf_addf(&msg,
				  "\n  - Part '%s' %u/%u @ 0x%x size 0x%x "
				  "(overflow 0x%x)",
				  e->name, e->type, e->subtype, e->offset,
				  e->size, (uint32_t)bin_size - e->size);
		}

		fprintf(out, "%s: %s\n", prefix, msg.buf);
		sbuf_release(&msg);

		return is_error ? 1 : 0;
	}
}
