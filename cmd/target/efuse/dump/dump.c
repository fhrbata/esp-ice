/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/efuse/dump/dump.c
 * @brief `ice target efuse dump` -- print the raw eFuse blocks.
 */
#include "cmd/target/efuse/efuse.h"
#include "ice.h"
#include "serial.h"

/* clang-format off */
static const struct cmd_manual target_efuse_dump_manual = {
	.name = "ice target efuse dump",
	.summary = "print the raw eFuse blocks of a connected chip",

	.description =
	H_PARA("Read every eFuse block of the chip and print it as 32-bit "
	       "words in hex, word 0 first, one block per line -- the "
	       "layout @b{espefuse dump} uses.  Use @b{ice target efuse "
	       "summary} to see the same bits decoded into named fields.")
	H_PARA("The chip returns zeros for a read-protected word; such words "
	       "are printed as @b{--------}.  Nothing is written to the "
	       "chip.  It is reset into its application when the command "
	       "ends."),

	.examples =
	H_EXAMPLE("ice target efuse dump")
	H_EXAMPLE("ice target efuse dump --port /dev/ttyACM0"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice target efuse summary",
	       "The same eFuses decoded into named fields."),
};
/* clang-format on */

static const struct option cmd_target_efuse_dump_opts[] = {
    EFUSE_COMMON_OPTS,
    OPT_END(),
};

int cmd_target_efuse_dump(int argc, const char **argv);

const struct cmd_desc cmd_target_efuse_dump_desc = {
    .name = "dump",
    .fn = cmd_target_efuse_dump,
    .opts = cmd_target_efuse_dump_opts,
    .manual = &target_efuse_dump_manual,
};

static void print_blocks(const struct efuse_session *s)
{
	const esp_loader_efuse_image_t *img = &s->image;

	printf("@b{%s} revision v%u.%u\n\n",
	       ice_chip_name(ice_chip_from_esf(s->chip)), s->revision / 100,
	       s->revision % 100);

	for (uint8_t b = 0; b < img->block_count; b++) {
		const esp_loader_efuse_block_t *blk = &img->blocks[b];

		printf("BLK%-2u ", blk->block);
		for (uint8_t w = 0; w < blk->word_count; w++) {
			if (blk->read_protected_word_mask & (1u << w))
				printf(" --------");
			else
				printf(" %08x", (unsigned)blk->words[w]);
		}
		printf("\n");
	}
}

int cmd_target_efuse_dump(int argc, const char **argv)
{
	struct efuse_session s;
	int rc = 1;

	argc = parse_options(argc, argv, &cmd_target_efuse_dump_desc);
	if (argc > 0)
		die("too many arguments");

	if (efuse_session_open(&s) != 0)
		goto out;
	print_blocks(&s);
	rc = 0;
out:
	efuse_session_close(&s);
	return rc;
}
