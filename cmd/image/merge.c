/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/image/merge.c
 * @brief "ice image merge" -- concatenate flash images at offsets.
 *
 * Drop-in replacement for `esptool merge-bin` / the older
 * `esptool merge_bin`.  Takes an interleaved list of (offset, file)
 * pairs and produces a single binary with each file placed at its
 * offset, the gaps filled with 0xFF (the flash-chip erase state).
 * Typical use is packaging the bootloader, partition table, and
 * application binaries into one "factory" image that can be flashed
 * in a single write at offset 0.
 */
#include "binary.h"
#include "ice.h"

/* clang-format off */
static const struct cmd_manual image_merge_manual = {
	.name = "ice image merge",
	.summary = "combine multiple flash images at offsets into one",

	.description =
	H_PARA("Concatenate multiple flash images at given offsets into "
	       "a single output file, padding the gaps with 0xFF (the "
	       "flash-chip erase state).  Positional arguments alternate "
	       "between an offset (hex or decimal) and the path of the "
	       "binary to place at that offset.")
	H_PARA("@b{--pad-to-size} pads the output to a specific flash "
	       "size (e.g. @b{4MB}), useful when preparing factory "
	       "images that must exactly fill the device's flash; "
	       "without it the output is sized to exactly cover the "
	       "highest-placed input file."),

	.examples =
	H_EXAMPLE("ice image merge -o factory.bin "
		  "0x0 bootloader.bin 0x8000 partition-table.bin "
		  "0x10000 app.bin")
	H_EXAMPLE("ice image merge --pad-to-size 4MB -o factory.bin "
		  "0x0 bootloader.bin 0x8000 partition-table.bin "
		  "0x10000 app.bin"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice image info",
	       "Inspect individual images before merging.")
};
/* clang-format on */

static const char *opt_out;
static const char *opt_pad_to_size;
static const char *opt_target_offset;

static const struct option cmd_image_merge_opts[] = {
    OPT_STRING('o', NULL, &opt_out, "path", "output image path", NULL),
    OPT_STRING(0, "pad-to-size", &opt_pad_to_size, "size",
	       "pad output to this flash size (e.g. 4MB) with 0xFF", NULL),
    OPT_STRING(0, "target-offset", &opt_target_offset, "hex",
	       "subtract this base offset from every placement", NULL),
    OPT_END(),
};

int cmd_image_merge(int argc, const char **argv);

const struct cmd_desc cmd_image_merge_desc = {
    .name = "merge",
    .fn = cmd_image_merge,
    .opts = cmd_image_merge_opts,
    .manual = &image_merge_manual,
};

static uint32_t parse_hex(const char *s, const char *flag)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno != 0 || *end != '\0')
		die("invalid %s value '%s'", flag, s);
	return (uint32_t)v;
}

/*
 * Parse an IDF-style flash-size string ("1MB" .. "128MB") into bytes.
 * Falls back to accepting a raw numeric byte count for flexibility.
 */
static size_t flash_size_bytes(const char *s)
{
	char *end;
	unsigned long n;

	errno = 0;
	n = strtoul(s, &end, 10);
	if (errno != 0 || end == s)
		die("invalid --flash-size '%s'", s);
	if (*end == '\0')
		return (size_t)n; /* raw bytes */
	if (!strcmp(end, "MB"))
		return (size_t)n * (size_t)1024u * (size_t)1024u;
	if (!strcmp(end, "KB"))
		return (size_t)n * (size_t)1024u;
	die("invalid --flash-size '%s' (expected e.g. 4MB, 16KB, or raw bytes)",
	    s);
	return 0; /* unreachable */
}

int cmd_image_merge(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_image_merge_desc);

	if (!opt_out)
		die("-o <output.bin> is required");
	if (argc < 2)
		die("need at least one <offset> <file.bin> pair");
	if (argc % 2 != 0)
		die("positional arguments must come in <offset> <file> pairs");

	uint32_t base = opt_target_offset
			    ? parse_hex(opt_target_offset, "--target-offset")
			    : 0u;

	/* First pass: read every input, find the highest placed byte. */
	int n_pairs = argc / 2;
	struct entry {
		uint32_t off;
		struct sbuf data;
	};
	struct entry *ent = malloc((size_t)n_pairs * sizeof *ent);

	if (ent == NULL)
		die_errno("malloc");

	size_t end = 0;

	for (int i = 0; i < n_pairs; i++) {
		int idx = 2 * i;
		const char *off_s = argv[idx];
		const char *path = argv[idx + 1];
		uint32_t raw = parse_hex(off_s, "offset");

		if (raw < base)
			die("offset %s is below --target-offset 0x%x", off_s,
			    (unsigned)base);
		ent[i].off = raw - base;
		sbuf_init(&ent[i].data);
		if (sbuf_read_file(&ent[i].data, path) < 0)
			die_errno("cannot read '%s'", path);

		size_t this_end = (size_t)ent[i].off + ent[i].data.len;

		if (this_end > end)
			end = this_end;
	}

	/* Detect overlapping inputs -- a diagnosable mistake. */
	for (int i = 0; i < n_pairs; i++) {
		size_t a0 = ent[i].off;
		size_t a1 = a0 + ent[i].data.len;

		for (int j = i + 1; j < n_pairs; j++) {
			size_t b0 = ent[j].off;
			size_t b1 = b0 + ent[j].data.len;

			if (a0 < b1 && b0 < a1)
				die("inputs overlap: '%s' at 0x%x and '%s' at "
				    "0x%x",
				    argv[2 * i + 1], /* NOLINT */
				    (unsigned)ent[i].off,
				    argv[2 * j + 1], /* NOLINT */
				    (unsigned)ent[j].off);
		}
	}

	size_t total = end;

	if (opt_pad_to_size) {
		size_t fs = flash_size_bytes(opt_pad_to_size);

		if (end > fs)
			die("inputs extend past --pad-to-size %s "
			    "(need 0x%zx, have 0x%zx)",
			    opt_pad_to_size, end, fs);
		total = fs;
	}

	if (total == 0)
		die("nothing to merge (all inputs are empty)");

	/* Second pass: allocate 0xFF buffer, place each file, write. */
	uint8_t *out = malloc(total);

	if (out == NULL)
		die_errno("malloc(%zu)", total);
	memset(out, 0xFF, total);
	for (int i = 0; i < n_pairs; i++)
		memcpy(out + ent[i].off, ent[i].data.buf, ent[i].data.len);

	mkdirp_for_file(opt_out);

	FILE *fp = fopen(opt_out, "wb");

	if (!fp)
		die_errno("cannot write '%s'", opt_out);
	if (fwrite(out, 1, total, fp) != total) {
		fclose(fp);
		die_errno("write error on '%s'", opt_out);
	}
	fclose(fp);

	free(out);
	for (int i = 0; i < n_pairs; i++)
		sbuf_release(&ent[i].data);
	free(ent);
	return 0;
}
