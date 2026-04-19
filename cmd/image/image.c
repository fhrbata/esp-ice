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

extern const struct cmd_desc cmd_image_create_desc;
extern const struct cmd_desc cmd_image_info_desc;
extern const struct cmd_desc cmd_image_merge_desc;

static const struct option cmd_image_opts[] = {OPT_END()};

/* clang-format off */
static const struct cmd_manual image_manual = {
	.name = "ice image",
	.summary = "host-only image manipulation",

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

static const struct cmd_desc *const image_subs[] = {
    &cmd_image_create_desc,
    &cmd_image_info_desc,
    &cmd_image_merge_desc,
    NULL,
};

const struct cmd_desc cmd_image_desc = {
    .name = "image",
    .opts = cmd_image_opts,
    .manual = &image_manual,
    .subcommands = image_subs,
};
