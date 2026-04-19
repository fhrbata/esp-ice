/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/set-target/set-target.c
 * @brief The "ice set-target" subcommand -- switch the chip target.
 */
#include "ice.h"

/*
 * Mirrors esp-idf/tools/idf_py_actions/constants.py.  Preview targets
 * require --preview to match idf.py's behaviour.  Exposed via ice.h
 * so the completion backend can reuse the same lists.
 */
const char *const ice_supported_targets[] = {
    "esp32",   "esp32s2", "esp32c3", "esp32s3",	 "esp32c2", "esp32c6",
    "esp32h2", "esp32p4", "esp32c5", "esp32c61", NULL,
};
const char *const ice_preview_targets[] = {
    "linux", "esp32h21", "esp32h4", "esp32s31", NULL,
};

/* clang-format off */
static const struct cmd_manual manual = {
	.name = "ice set-target",
	.summary = "switch the chip target",

	.description =
	H_PARA("Switches the project to build for chip @b{<target>} "
	       "(e.g. @b{esp32}, @b{esp32s3}, @b{esp32c6}).  Wipes the "
	       "build directory, renames any existing @b{sdkconfig} to "
	       "@b{sdkconfig.old}, then reconfigures cmake with the new "
	       "@b{IDF_TARGET}.  The fresh @b{sdkconfig} is generated "
	       "from @b{sdkconfig.defaults} (and any "
	       "@b{sdkconfig.defaults.<target>} override).")
	H_PARA("Use this when switching chips.  For ad-hoc cmake cache "
	       "tweaks that should not discard the current @b{sdkconfig}, "
	       "run @b{ice reconfigure} with a @b{-D IDF_TARGET=...} "
	       "override instead."),

	.examples =
	H_EXAMPLE("ice set-target esp32")
	H_EXAMPLE("ice set-target esp32s3")
	H_EXAMPLE("ice --preview set-target linux"),

	.extras =
	H_SECTION("CONFIG")
	H_ITEM("core.build-dir",
	       "Build directory to wipe and reconfigure "
	       "(default @b{build}).")
	H_ITEM("cmake.define",
	       "@b{IDF_TARGET=<target>} is appended to the in-memory "
	       "define list and forwarded to cmake as "
	       "@b{-DIDF_TARGET=<target>} for this invocation only.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice fullclean",
	       "Wipe the build directory without changing the target.")
	H_ITEM("ice reconfigure",
	       "Regenerate the build system without wiping sdkconfig.")
	H_ITEM("ice build",
	       "Build the project after switching target."),
};
/* clang-format on */

/* File-scope so the table can be const and reachable via cmd_struct.opts. */
static int opt_preview;

static const struct option cmd_set_target_opts[] = {
    OPT_BOOL(0, "preview", &opt_preview, "allow preview targets"),
    OPT_END(),
};

static int in_list(const char *target, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(target, *list))
			return 1;
	return 0;
}

int cmd_set_target(int argc, const char **argv)
{
	/*
	 * Handshake with esp-idf/tools/cmake/project.cmake: when this env
	 * var is set, project.cmake renames an existing sdkconfig to
	 * sdkconfig.old before __target_init runs, so a stale IDF_TARGET
	 * from the old sdkconfig is not consulted for consistency checks.
	 *
	 * putenv() stores the pointer, not a copy, so the storage must
	 * outlive the call -- a function-static array provides that.
	 */
	static char envstr[] = "_IDF_PY_SET_TARGET_ACTION=1";
	const char *target;
	struct sbuf define = SBUF_INIT;
	struct cmd_desc cmd_desc = {.opts = cmd_set_target_opts,
				    .manual = &manual};
	int rc;

	argc = parse_options(argc, argv, &cmd_desc);

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

	printf("Set Target to: %s, new sdkconfig will be created.\n", target);
	return ensure_build_directory(1);
}
