/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/image/image.c
 * @brief The "ice image" namespace dispatcher.
 *
 * Groups the host-only image-manipulation subcommands (elf2image,
 * info, merge, ...) under a single top-level name.  A router, not a
 * worker: it parses the subcommand name from argv[1] and delegates.
 *
 * As native replacements for esptool's image-side verbs land, they
 * are added to @ref image_subs below.  Today only @c elf2image is
 * implemented; @c info and @c merge are listed in the manual for
 * discoverability but return an explicit "not yet implemented"
 * error.
 */
#include "ice.h"

int cmd_image_elf2image(int argc, const char **argv);

struct image_sub {
	const char *name;
	int (*fn)(int argc, const char **argv);
	const char *summary;
};

static const struct image_sub image_subs[] = {
    {"elf2image", cmd_image_elf2image,
     "convert an ELF executable to an ESP flash image"},
    {NULL, NULL, NULL},
};

static const char *image_usage[] = {
    "ice image <subcommand> [<args>]",
    NULL,
};

/* clang-format off */
static const struct cmd_manual manual = {
	.description =
	H_PARA("Host-only image-manipulation subcommands.  Does not talk "
	       "to a chip — everything here operates on local files.  "
	       "Long-term these commands replace esptool's image-side "
	       "verbs (@b{elf2image}, @b{image_info}, @b{merge_bin}); the "
	       "implementation is growing one verb at a time.")
	H_PARA("Run @b{ice image <subcommand> --help} for the manual of "
	       "a specific subcommand."),

	.examples =
	H_EXAMPLE("ice image elf2image --chip esp32 --flash-mode dio "
		  "--flash-freq 40m --flash-size 2MB -o app.bin app.elf"),

	.extras =
	H_SECTION("SUBCOMMANDS")
	H_ITEM("elf2image",
	       "Convert an ELF executable to an ESP flash image.  "
	       "Drop-in replacement for @b{esptool elf2image}.")
};
/* clang-format on */

static void print_subs(FILE *fp)
{
	fprintf(fp, "Subcommands:\n");
	for (const struct image_sub *s = image_subs; s->name; s++)
		fprintf(fp, "  %-12s  %s\n", s->name, s->summary);
}

int cmd_image(int argc, const char **argv)
{
	/*
	 * We deliberately do NOT run parse_options_manual here: the
	 * subcommand needs to consume its own arguments (including
	 * any --help) with its own option table.  Trap a bare
	 * "ice image --help" / "ice image help" up front and show
	 * the namespace manual; forward everything else.
	 */
	if (argc >= 2 && (!strcmp(argv[1], "--help") ||
			  !strcmp(argv[1], "-h") || !strcmp(argv[1], "help"))) {
		print_manual(argv[0], &manual, NULL, image_usage);
		return 0;
	}

	if (argc < 2) {
		fprintf(stderr, "usage: ice image <subcommand> [<args>]\n");
		print_subs(stderr);
		return 1;
	}

	for (const struct image_sub *s = image_subs; s->name; s++) {
		if (!strcmp(argv[1], s->name)) {
			/* Hand off argv starting at the subcommand name so
			 * the worker sees argv[0] == its own name (mirrors
			 * how main() dispatches top-level commands). */
			return s->fn(argc - 1, argv + 1);
		}
	}

	fprintf(
	    stderr,
	    "ice image: '%s' is not a subcommand. See 'ice image --help'.\n",
	    argv[1]);
	print_subs(stderr);
	return 1;
}
