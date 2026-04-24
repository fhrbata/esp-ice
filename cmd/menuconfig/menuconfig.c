/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/menuconfig/menuconfig.c
 * @brief `ice menuconfig` -- porcelain wrapper around `ice idf menuconfig`.
 *
 * Project-aware entry point.  Resolves the Kconfig root path, the
 * active sdkconfig, and any layered sdkconfig.defaults files from the
 * profile, then calls the plumbing command.  All the ESP-IDF-side
 * Kconfig env vars (IDF_TARGET, COMPONENT_KCONFIGS_*, ...) are
 * preloaded by @ref setup_project via @c populate_kconfig_env, so the
 * parser picks them up from the process env and the porcelain has
 * nothing extra to plumb.
 */
#include "ice.h"

/* Plumbing entry point declared in cmd/idf/menuconfig/menuconfig.c. */
int cmd_idf_menuconfig(int argc, const char **argv);

int cmd_menuconfig(int argc, const char **argv);

static const struct option cmd_menuconfig_opts[] = {OPT_END()};

/* clang-format off */
static const struct cmd_manual manual = {
	.name = "ice menuconfig",
	.summary = "interactively edit the project sdkconfig",

	.description =
	H_PARA("Opens ice's native Kconfig TUI against the active "
	       "profile's @b{sdkconfig}.  The Kconfig root is taken "
	       "from @b{_project.idf-path}, the sdkconfig from "
	       "@b{_project.sdkconfig}, layered defaults from "
	       "@b{_project.sdkconfig-defaults}.  ESP-IDF-side env "
	       "(IDF_TARGET, COMPONENT_KCONFIGS_*, ...) is loaded by "
	       "the project-setup step from @b{<build>/config.env}, "
	       "so the parser sees the same environment cmake's own "
	       "menuconfig target would set up.")
	H_PARA("See @b{ice idf menuconfig --help} for the in-UI key "
	       "bindings (Enter to open / toggle / edit, @b{/} for "
	       "search, @b{?} for per-symbol help, @b{F1} for the "
	       "full reference)."),

	.examples =
	H_EXAMPLE("ice menuconfig")
	H_EXAMPLE("ice --profile production menuconfig"),

	.extras =
	H_SECTION("CONFIG")
	H_ITEM("_project.idf-path",
	       "Root of the bound ESP-IDF tree.  @b{Kconfig} at the top "
	       "is the tree root.")
	H_ITEM("_project.sdkconfig",
	       "Sdkconfig file loaded on entry and written on save.")
	H_ITEM("_project.sdkconfig-defaults",
	       "Optional layered defaults (repeatable key).")
	H_ITEM("_project.env.*",
	       "Mirrored entries from @b{<build>/config.env} -- IDF_TARGET, "
	       "COMPONENT_KCONFIGS_*, etc.  Populated by setup_project.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice idf menuconfig",
	       "Plumbing: open the TUI against explicit --kconfig / "
	       "--config / --defaults paths.")
	H_ITEM("ice init",
	       "Re-bind the profile if cache-time decisions need to "
	       "propagate (chip change, new component, ...)."),
};
/* clang-format on */

const struct cmd_desc cmd_menuconfig_desc = {
    .name = "menuconfig",
    .fn = cmd_menuconfig,
    .opts = cmd_menuconfig_opts,
    .manual = &manual,
    .needs = PROJECT_CONFIGURED,
};

int cmd_menuconfig(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_menuconfig_desc);
	if (argc > 0)
		die("too many arguments");

	const char *idf_path = config_get("_project.idf-path");
	const char *sdkconfig = config_get("_project.sdkconfig");

	if (!idf_path)
		die("@b{_project.idf-path} is unset; run @b{ice init} first");
	if (!sdkconfig)
		die("@b{_project.sdkconfig} is unset; run @b{ice init} first");

	struct sbuf root_kconfig = SBUF_INIT;
	struct sbuf root_rename = SBUF_INIT;
	sbuf_addf(&root_kconfig, "%s/Kconfig", idf_path);
	sbuf_addf(&root_rename, "%s/sdkconfig.rename", idf_path);

	/* Collect all sdkconfig.defaults entries the profile carries.
	 * config_get_all returns the owned array; we read it through
	 * the caller-owned pointers, so just free(entries) at the end
	 * without touching the entries themselves. */
	struct config_entry **defaults_entries = NULL;
	int n_defaults =
	    config_get_all("_project.sdkconfig-defaults", &defaults_entries);

	/* Build argv for the plumbing.  Fixed overhead is 7 slots
	 * (prog + -k + kconfig + -c + sdk + -o + sdk), plus optional
	 * --sdkconfig-rename (2 slots) and 2 per sdkconfig.defaults. */
	struct svec args = SVEC_INIT;
	svec_push(&args, "ice idf menuconfig");
	svec_push(&args, "-k");
	svec_push(&args, root_kconfig.buf);
	svec_push(&args, "-c");
	svec_push(&args, sdkconfig);
	svec_push(&args, "-o");
	svec_push(&args, sdkconfig);
	if (access(root_rename.buf, F_OK) == 0) {
		svec_push(&args, "--sdkconfig-rename");
		svec_push(&args, root_rename.buf);
	}
	for (int i = 0; i < n_defaults; i++) {
		svec_push(&args, "--defaults");
		svec_push(&args, defaults_entries[i]->value);
	}

	int rc = cmd_idf_menuconfig((int)args.nr, (const char **)args.v);

	svec_clear(&args);
	free(defaults_entries);
	sbuf_release(&root_kconfig);
	sbuf_release(&root_rename);
	return rc;
}
