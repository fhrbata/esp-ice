/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/image/image.c
 * @brief The "ice image" namespace.
 */
#include "ice.h"

int cmd_image_create(int argc, const char **argv);
int cmd_image_info(int argc, const char **argv);
int cmd_image_merge(int argc, const char **argv);

static subcmd_fn image_fn;

static const struct option cmd_image_opts[] = {
    OPT_SUBCOMMAND("create", &image_fn, cmd_image_create,
		   "create a flash image from an ELF executable"),
    OPT_SUBCOMMAND("info", &image_fn, cmd_image_info,
		   "display the structure of an ESP flash image"),
    OPT_SUBCOMMAND("merge", &image_fn, cmd_image_merge,
		   "combine multiple flash images at offsets into one"),
    OPT_END(),
};

/* clang-format off */
static const struct cmd_manual manual = {
	.description =
	H_PARA("Host-only image-manipulation subcommands.  Does not talk "
	       "to a chip -- everything here operates on local files.")
	H_PARA("Run @b{ice image <subcommand> --help} for the manual of "
	       "a specific subcommand."),

	.examples =
	H_EXAMPLE("ice image create --chip esp32 --flash-mode dio "
		  "--flash-freq 40m --flash-size 2MB -o app.bin app.elf"),
};
/* clang-format on */

int cmd_image(int argc, const char **argv)
{
	argc = parse_options_manual(argc, argv, cmd_image_opts, &manual);
	if (image_fn)
		return image_fn(argc, argv);

	die("expected a subcommand. See 'ice image --help'.");
}
