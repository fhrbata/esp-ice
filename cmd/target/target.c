/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file target.c
 * @brief `ice target` -- manage the chip target.
 *
 * Subcommands:
 *   set   - switch the project to a new chip target
 *   list  - list supported chip targets
 *   info  - show the current target
 */
#include "ice.h"

#include "cmake.h"

#include <string.h>

/* Forward decls for leaf handlers so the leaf descriptors can pin .fn. */
static int cmd_target_set(int argc, const char **argv);
static int cmd_target_list(int argc, const char **argv);
static int cmd_target_info(int argc, const char **argv);

/* ------------------------------------------------------------------ */
/* ice target set                                                      */
/* ------------------------------------------------------------------ */

static int opt_preview;

static void complete_targets(void)
{
	for (const char *const *t = ice_supported_targets; *t; t++)
		printf("%s\n", *t);
	for (const char *const *t = ice_preview_targets; *t; t++)
		printf("%s\n", *t);
}

static const struct option cmd_target_set_opts[] = {
    OPT_BOOL(0, "preview", &opt_preview, "allow preview targets"),
    OPT_END_COMPLETE("target", complete_targets),
};

/* clang-format off */
static const struct cmd_manual target_set_manual = {
	.name = "ice target set",
	.summary = "switch the project to a new chip target",

	.description =
	H_PARA("Switch the project to build for chip @b{<target>} "
	       "(e.g. @b{esp32}, @b{esp32s3}, @b{esp32c6}).  Wipes the "
	       "build directory, renames any existing @b{sdkconfig} to "
	       "@b{sdkconfig.old}, then reconfigures cmake with the new "
	       "@b{IDF_TARGET}.")
	H_PARA("Use this when switching chips.  For ad-hoc cmake cache "
	       "tweaks that should not discard the current @b{sdkconfig}, "
	       "run @b{ice reconfigure} with a @b{-D IDF_TARGET=...} "
	       "override instead."),

	.examples =
	H_EXAMPLE("ice target set esp32")
	H_EXAMPLE("ice target set esp32s3")
	H_EXAMPLE("ice target set --preview linux"),
};
/* clang-format on */

static const struct cmd_desc cmd_target_set_desc = {
    .name = "set",
    .fn = cmd_target_set,
    .opts = cmd_target_set_opts,
    .manual = &target_set_manual,
};

static int in_list(const char *target, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(target, *list))
			return 1;
	return 0;
}

static int cmd_target_set(int argc, const char **argv)
{
	static char envstr[] = "_IDF_PY_SET_TARGET_ACTION=1";
	const char *target;
	struct sbuf define = SBUF_INIT;
	int rc;

	argc = parse_options(argc, argv, &cmd_target_set_desc);

	if (argc < 1)
		die("missing <target> argument");
	if (argc > 1)
		die("too many arguments");
	target = argv[0];

	if (!in_list(target, ice_supported_targets)) {
		if (in_list(target, ice_preview_targets)) {
			if (!opt_preview)
				die("'%s' is a preview target; "
				    "pass --preview to use it",
				    target);
		} else {
			die("'%s' is not a supported target", target);
		}
	}

	rc = fullclean_run();
	if (rc)
		return rc;

	sbuf_addf(&define, "IDF_TARGET=%s", target);
	svec_push(&global_defines, define.buf);
	sbuf_release(&define);

	putenv(envstr);

	printf("Set target to: %s, new sdkconfig will be created.\n", target);
	return ensure_build_directory(1);
}

/* ------------------------------------------------------------------ */
/* ice target list                                                     */
/* ------------------------------------------------------------------ */

static const struct cmd_manual target_list_manual = {
    .name = "ice target list",
};

static const struct option cmd_target_list_opts[] = {OPT_END()};

static const struct cmd_desc cmd_target_list_desc = {
    .name = "list",
    .fn = cmd_target_list,
    .opts = cmd_target_list_opts,
    .manual = &target_list_manual,
};

static int cmd_target_list(int argc, const char **argv)
{
	parse_options(argc, argv, &cmd_target_list_desc);

	printf("Supported targets:\n");
	for (const char *const *t = ice_supported_targets; *t; t++)
		printf("  %s\n", *t);

	printf("\nPreview targets (use --preview with 'ice target set'):\n");
	for (const char *const *t = ice_preview_targets; *t; t++)
		printf("  %s\n", *t);

	return 0;
}

/* ------------------------------------------------------------------ */
/* ice target info                                                     */
/* ------------------------------------------------------------------ */

static const struct cmd_manual target_info_manual = {
    .name = "ice target info",
};

static const struct option cmd_target_info_opts[] = {OPT_END()};

static const struct cmd_desc cmd_target_info_desc = {
    .name = "info",
    .fn = cmd_target_info,
    .opts = cmd_target_info_opts,
    .manual = &target_info_manual,
};

static int cmd_target_info(int argc, const char **argv)
{
	const char *target;

	parse_options(argc, argv, &cmd_target_info_desc);

	target = config_get("target");
	if (!target || !*target) {
		fprintf(stderr, "No target set.\n"
				"hint: run @b{ice target set <chip>}\n");
		return 1;
	}

	printf("Target: %s\n", target);
	return 0;
}

/* ------------------------------------------------------------------ */
/* ice target -- namespace dispatcher                                  */
/* ------------------------------------------------------------------ */

static const struct option cmd_target_opts[] = {OPT_END()};

/* clang-format off */
static const struct cmd_manual target_manual = {
	.name = "ice target",
	.summary = "manage the chip target",

	.description =
	H_PARA("Manage the chip target for the current project.")
	H_PARA("Run @b{ice target <subcommand> --help} for details."),

	.examples =
	H_EXAMPLE("ice target set esp32s3")
	H_EXAMPLE("ice target list")
	H_EXAMPLE("ice target info"),
};
/* clang-format on */

static const struct cmd_desc *const target_subs[] = {
    &cmd_target_set_desc,
    &cmd_target_list_desc,
    &cmd_target_info_desc,
    NULL,
};

const struct cmd_desc cmd_target_desc = {
    .name = "target",
    .opts = cmd_target_opts,
    .manual = &target_manual,
    .subcommands = target_subs,
};
