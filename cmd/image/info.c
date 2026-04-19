/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/image/info.c
 * @brief "ice image info" subcommand -- inspect an ESP flash image.
 *
 * Inverse of `ice image elf2image`: read a flash image produced by
 * any ESP-IDF build and pretty-print everything an operator might
 * want to confirm before shipping it -- target chip, flash
 * parameters, entry point, segment layout with memory-region
 * labels, checksum and SHA-256 validity, and the esp_app_desc_t
 * metadata placed by the linker at the head of DROM.  Shares all
 * per-chip knowledge with elf2image via binary.h.
 */
#include "binary.h"
#include "ice.h"
#include "vendor/sha256/sha256.h"

/* clang-format off */
static const struct cmd_manual image_info_manual = {
	.name = "ice image info",
	.summary = "display the structure of an ESP flash image",

	.description =
	H_PARA("Display the structure of an ESP flash image.  Equivalent "
	       "to @b{esptool image_info}: shows the common / extended "
	       "header, each loaded segment with its memory-region "
	       "classification (DROM / IROM / DRAM / IRAM / RTC), the "
	       "checksum and SHA-256 digest validity, and -- when the "
	       "first DROM segment carries one -- the @c esp_app_desc_t "
	       "metadata (project name, version, ESP-IDF version, "
	       "ELF SHA-256, revision constraints).")
	H_PARA("The target chip is auto-detected from the extended "
	       "header's @c chip_id field; pass @b{--chip} to override "
	       "or to process an image built for a chip whose id is not "
	       "yet recognised."),

	.examples =
	H_EXAMPLE("ice image info build/hello_world.bin")
	H_EXAMPLE("ice image info --chip esp32c3 firmware.bin"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice image elf2image",
	       "Build the images that this command inspects."),
};
/* clang-format on */

static const char *opt_chip;

int cmd_image_info(int argc, const char **argv);

static const struct option cmd_image_info_opts[] = {
    OPT_STRING(0, "chip", &opt_chip, "name",
	       "override the auto-detected target chip", NULL),
    OPT_END(),
};

const struct cmd_desc cmd_image_info_desc = {
    .name = "info",
    .fn = cmd_image_info,
    .opts = cmd_image_info_opts,
    .manual = &image_info_manual,
};

/* ------------------------------------------------------------------ */
/* Little-endian readers                                              */
/* ------------------------------------------------------------------ */

static uint16_t rd_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t rd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

/* ------------------------------------------------------------------ */
/* SHA-256 helper                                                     */
/* ------------------------------------------------------------------ */

static void sha256_buf(const uint8_t *data, size_t n, uint8_t *hash)
{
	SHA256_CTX ctx;

	sha256_init(&ctx);
	sha256_update(&ctx, data, n);
	sha256_final(&ctx, hash);
}

static void print_hex(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		printf("%02x", p[i]);
}

/* Trim trailing NULs and return a NUL-terminated copy of a fixed-size
 * string field.  Used for the 16/32-byte char[] members of
 * esp_app_desc_t. */
static void print_strz(const char *label, const uint8_t *p, size_t maxlen)
{
	size_t n = 0;

	while (n < maxlen && p[n] != '\0')
		n++;
	printf("  %-16s: %.*s\n", label, (int)n, (const char *)p);
}

/* ------------------------------------------------------------------ */
/* app_desc printer                                                   */
/* ------------------------------------------------------------------ */

/*
 * esp_app_desc_t layout (from esptool/cmds.py APP_DESC_STRUCT_FMT):
 *     0   magic_word    (4)   0xABCD5432
 *     4   secure_version(4)
 *     8   reserv1       (8)
 *    16   version       (32)  NUL-padded string
 *    48   project_name  (32)  NUL-padded string
 *    80   time          (16)  NUL-padded string
 *    96   date          (16)  NUL-padded string
 *   112   idf_ver       (32)  NUL-padded string
 *   144   app_elf_sha256(32)  hex bytes
 *   176   min_efuse_blk (2)
 *   178   max_efuse_blk (2)
 *   180   mmu_page_size (1)
 *   181   reserv3       (3)
 *   184   reserv2       (72)
 */
static void print_app_desc(const uint8_t *p, size_t n)
{
	if (n < 181)
		return;
	if (rd_le32(p) != BIN_APP_DESC_MAGIC)
		return;

	printf("\nApplication information:\n");
	print_strz("Project name", p + 48, 32);
	print_strz("App version", p + 16, 32);
	print_strz("Compile date", p + 96, 16);
	print_strz("Compile time", p + 80, 16);
	print_strz("ESP-IDF ver", p + 112, 32);

	printf("  %-16s: ", "Secure version");
	printf("%u\n", (unsigned)rd_le32(p + 4));

	printf("  %-16s: ", "ELF SHA-256");
	print_hex(p + 144, 32);
	printf("\n");

	uint16_t min_efuse = rd_le16(p + 176);
	uint16_t max_efuse = rd_le16(p + 178);

	if (min_efuse != 0 || max_efuse != 0xFFFFu)
		printf("  %-16s: min %u, max %u\n", "EFuse blk rev",
		       (unsigned)min_efuse, (unsigned)max_efuse);

	uint8_t mmu = p[180];

	if (mmu != 0)
		printf("  %-16s: %u KB\n", "MMU page size",
		       (unsigned)mmu / 1024u);
}

/* ------------------------------------------------------------------ */
/* Main body                                                          */
/* ------------------------------------------------------------------ */

static void print_chip_rev(uint16_t rev)
{
	/* esptool encodes revisions as major*100 + minor. */
	printf("v%u.%u", (unsigned)(rev / 100u), (unsigned)(rev % 100u));
}

int cmd_image_info(int argc, const char **argv)
{
	struct sbuf img = SBUF_INIT;

	argc = parse_options(argc, argv, &cmd_image_info_desc);

	if (argc < 1)
		die("missing <image.bin> argument");
	if (argc > 1)
		die("too many positional arguments");

	const char *path = argv[0];

	if (sbuf_read_file(&img, path) < 0)
		die_errno("cannot read '%s'", path);

	const uint8_t *buf = (const uint8_t *)img.buf;
	size_t len = img.len;
	size_t min_header = BIN_HDR_LEN + BIN_EXT_HDR_LEN;

	if (len < min_header)
		die("%s: too short to be a flash image (%zu < %zu bytes)", path,
		    len, min_header);

	if (buf[0] != BIN_IMAGE_MAGIC)
		die("%s: bad magic byte 0x%02x (expected 0x%02x)", path,
		    (unsigned)buf[0], (unsigned)BIN_IMAGE_MAGIC);

	uint8_t n_segs = buf[1];
	uint8_t flash_mode = buf[2];
	uint8_t fs_freq = buf[3];
	uint32_t entry = rd_le32(buf + 4);

	/* Extended header (all ESP32-family images). */
	uint16_t chip_id = rd_le16(buf + 8 + 4);
	uint8_t min_rev_legacy = buf[8 + 6];
	uint16_t min_rev_full = rd_le16(buf + 8 + 7);
	uint16_t max_rev_full = rd_le16(buf + 8 + 9);
	uint8_t append_digest = buf[8 + BIN_EXT_APPEND_DIGEST_OFF];

	/* Chip resolution: --chip override wins; otherwise derive
	 * from chip_id.  Unresolved chip is non-fatal -- we can still
	 * report most of the image; we just can't classify segments. */
	enum bin_chip chip = BIN_CHIP_MAX;

	if (opt_chip) {
		chip = bin_chip_by_name(opt_chip);
		if (chip == BIN_CHIP_MAX)
			die("unsupported --chip '%s'", opt_chip);
	} else {
		chip = bin_chip_by_id(chip_id);
	}

	printf("File: %s\n", path);
	printf("File size: %zu bytes\n", len);
	printf("\n");
	printf("Image header:\n");
	printf("  %-16s: 0x%02x\n", "Magic", (unsigned)buf[0]);
	printf("  %-16s: %u\n", "Segments", (unsigned)n_segs);
	printf("  %-16s: %s (0x%02x)\n", "Flash mode",
	       bin_flash_mode_str(flash_mode), (unsigned)flash_mode);
	printf("  %-16s: %s (0x%02x)\n", "Flash size",
	       bin_flash_size_str(fs_freq & 0xF0u),
	       (unsigned)(fs_freq & 0xF0u));
	printf("  %-16s: %s (0x%02x)\n", "Flash freq",
	       bin_flash_freq_str(chip, fs_freq & 0x0Fu),
	       (unsigned)(fs_freq & 0x0Fu));
	printf("  %-16s: 0x%08x\n", "Entry point", (unsigned)entry);
	printf("  %-16s: 0x%04x (%s)\n", "Chip ID", (unsigned)chip_id,
	       chip == BIN_CHIP_MAX ? "unknown" : bin_chip_name(chip));
	if (min_rev_legacy != 0)
		printf("  %-16s: %u\n", "Min rev (legacy)",
		       (unsigned)min_rev_legacy);
	printf("  %-16s: ", "Min chip rev");
	print_chip_rev(min_rev_full);
	printf("\n");
	printf("  %-16s: ", "Max chip rev");
	print_chip_rev(max_rev_full);
	printf("\n");
	printf("  %-16s: %s\n", "Append digest", append_digest ? "yes" : "no");

	/* ---------------- Segments ---------------- */
	printf("\nSegments:\n");
	printf("  %-3s %-10s %-10s %-10s %s\n", "#", "File-Off", "Load-Addr",
	       "Size", "Region");

	size_t pos = min_header;
	uint32_t cksum = BIN_CHECKSUM_MAGIC;
	size_t trailer_off = len;

	/* The digest, if present, occupies the last 32 bytes. */
	if (append_digest && len >= BIN_DIGEST_LEN)
		trailer_off -= BIN_DIGEST_LEN;

	for (uint8_t i = 0; i < n_segs; i++) {
		if (pos + BIN_SEG_HDR_LEN > trailer_off)
			die("%s: truncated segment header at offset %zu", path,
			    pos);
		uint32_t load = rd_le32(buf + pos);
		uint32_t size = rd_le32(buf + pos + 4);

		if (pos + BIN_SEG_HDR_LEN + size > trailer_off)
			die("%s: segment %u data extends past image end", path,
			    (unsigned)i);

		enum bin_seg_type t = (chip != BIN_CHIP_MAX)
					  ? bin_classify(chip, load)
					  : BIN_SEG_UNKNOWN;
		printf("  %-3u 0x%08zx 0x%08x 0x%08x %s\n", (unsigned)i,
		       pos + BIN_SEG_HDR_LEN, (unsigned)load, (unsigned)size,
		       bin_seg_type_name(t));

		for (uint32_t j = 0; j < size; j++)
			cksum ^= buf[pos + BIN_SEG_HDR_LEN + j];

		pos += BIN_SEG_HDR_LEN + size;
	}

	/* ---------------- Checksum ---------------- */
	/* Pad to the byte BEFORE the 16-byte boundary: that byte is
	 * the XOR trailer. */
	size_t aligned = (pos + 15u) & ~(size_t)15u;

	if (aligned == pos)
		aligned += 16u;
	size_t cksum_pos = aligned - 1u;

	printf("\nChecksum:\n");
	if (cksum_pos >= trailer_off) {
		printf("  position 0x%zx is past the image end (invalid)\n",
		       cksum_pos);
	} else {
		uint8_t stored = buf[cksum_pos];

		printf("  %-16s: 0x%02x\n", "Stored", (unsigned)stored);
		printf("  %-16s: 0x%02x\n", "Computed", (unsigned)cksum);
		printf("  %-16s: %s\n", "Result",
		       stored == cksum ? "valid" : "MISMATCH");
	}

	/* ---------------- SHA-256 digest ---------------- */
	if (append_digest) {
		if (len < BIN_DIGEST_LEN) {
			printf("\nSHA-256 digest: image too short\n");
		} else {
			uint8_t computed[BIN_DIGEST_LEN];

			sha256_buf(buf, len - BIN_DIGEST_LEN, computed);
			bool match =
			    !memcmp(computed, buf + len - BIN_DIGEST_LEN,
				    BIN_DIGEST_LEN);

			printf("\nSHA-256 digest:\n");
			printf("  %-16s: ", "Stored");
			print_hex(buf + len - BIN_DIGEST_LEN, BIN_DIGEST_LEN);
			printf("\n");
			printf("  %-16s: ", "Computed");
			print_hex(computed, BIN_DIGEST_LEN);
			printf("\n");
			printf("  %-16s: %s\n", "Result",
			       match ? "valid" : "MISMATCH");
		}
	}

	/* ---------------- app_desc ---------------- */
	/* Walk the segments again looking for one whose data starts
	 * with the esp_app_desc_t magic word.  Typically this is the
	 * first DROM segment, but we do not assume. */
	size_t scan = min_header;

	for (uint8_t i = 0; i < n_segs; i++) {
		if (scan + BIN_SEG_HDR_LEN > trailer_off)
			break;
		uint32_t size = rd_le32(buf + scan + 4);

		if (scan + BIN_SEG_HDR_LEN + size > trailer_off)
			break;
		if (size >= 4 && rd_le32(buf + scan + BIN_SEG_HDR_LEN) ==
				     BIN_APP_DESC_MAGIC) {
			print_app_desc(buf + scan + BIN_SEG_HDR_LEN, size);
			break;
		}
		scan += BIN_SEG_HDR_LEN + size;
	}

	sbuf_release(&img);
	return 0;
}
