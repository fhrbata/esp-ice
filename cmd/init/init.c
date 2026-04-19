/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/init/init.c
 * @brief `ice init` -- configure (or reconfigure) the project build.
 *
 * With no arguments, regenerates the build system from the existing
 * CMakeCache + config state (replaces the old `ice reconfigure`).
 *
 * @b{--chip <target>} switches the chip: wipes the build directory and
 * generates a fresh sdkconfig from defaults (replaces `ice set-target`).
 *
 * @b{--fresh} wipes the build directory before reconfiguring even when
 * the chip is unchanged (replaces `ice fullclean && ice reconfigure`).
 */
#include "ice.h"

/* clang-format off */
static const struct cmd_manual init_manual = {
	.name = "ice init",
	.summary = "configure (or reconfigure) the project build",

	.description =
	H_PARA("Bind an ESP-IDF source tree, install its tools, set the "
	       "chip target, and run cmake.  With no arguments, just "
	       "regenerates the build system from the existing "
	       "@b{.iceconfig} and @b{CMakeCache.txt} -- use this after "
	       "pulling top-level @b{CMakeLists} changes that ice does "
	       "not pick up automatically.")
	H_PARA("@b{--idf <path>} binds the project to an ESP-IDF source "
	       "tree.  A bare name (e.g. @b{v5.4}) resolves to "
	       "@b{~/.ice/checkouts/<name>/}; anything else is taken "
	       "verbatim.  The path is persisted to the project's "
	       "@b{.iceconfig} as @b{idf.path}, and the matching tool "
	       "set is installed under @b{~/.ice/tools/}.  TAB completion "
	       "lists existing checkouts; create more with "
	       "@b{ice repo checkout}.")
	H_PARA("@b{--chip <target>} switches the chip the project builds "
	       "for (@b{esp32}, @b{esp32s3}, @b{esp32c6}, ...).  The build "
	       "directory is wiped and any existing @b{sdkconfig} renamed "
	       "to @b{sdkconfig.old}; the fresh @b{sdkconfig} is generated "
	       "from @b{sdkconfig.defaults} (and any "
	       "@b{sdkconfig.defaults.<chip>} override).")
	H_PARA("@b{--fresh} wipes the build directory before reconfiguring "
	       "even when the chip is unchanged -- use when a cmake cache "
	       "has gotten into a bad state.")
	H_PARA("ESP-IDF resolution cascade: @b{--idf} > @b{$IDF_PATH} > "
	       "existing @b{idf.path} config entry.  An IDF must be "
	       "resolvable for @b{ice init} to run."),

	.examples =
	H_EXAMPLE("ice init --idf v5.4 --chip esp32s3")
	H_EXAMPLE("ice init --chip esp32c6")
	H_EXAMPLE("ice init")
	H_EXAMPLE("ice init --preview --chip linux")
	H_EXAMPLE("ice init --fresh"),

	.extras =
	H_SECTION("CONFIG")
	H_ITEM("idf.path",
	       "ESP-IDF source path; written by @b{--idf}.")
	H_ITEM("core.build-dir",
	       "Build directory to configure (default @b{build}).")
	H_ITEM("core.generator",
	       "cmake generator to use (default @b{Ninja}).")
	H_ITEM("cmake.define",
	       "Extra @b{-D<key>=<value>} entries forwarded to cmake.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice repo checkout",
	       "Create a named ESP-IDF checkout to bind with @b{--idf}.")
	H_ITEM("ice build",
	       "Build after init.")
	H_ITEM("ice clean",
	       "Remove built artifacts without touching cmake configuration."),
};
/* clang-format on */

static const char *opt_idf;
static const char *opt_chip;
static int opt_fresh;
static int opt_preview;

static int complete_checkouts_cb(const char *name, void *ud)
{
	(void)ud;
	printf("%s\n", name);
	return 0;
}

/** TAB completion for --idf: emit names of ~/.ice/checkouts/ entries. */
static void complete_idf(void)
{
	struct sbuf path = SBUF_INIT;

	sbuf_addf(&path, "%s/checkouts", ice_home());
	if (access(path.buf, F_OK) == 0)
		dir_foreach(path.buf, complete_checkouts_cb, NULL);
	sbuf_release(&path);
}

static const struct option cmd_init_opts[] = {
    OPT_STRING(0, "idf", &opt_idf, "path",
	       "bind project to an ESP-IDF source tree", complete_idf),
    OPT_STRING(0, "chip", &opt_chip, "target",
	       "chip target to build for (wipes build dir on change)", NULL),
    OPT_BOOL(0, "fresh", &opt_fresh, "wipe build dir before reconfiguring"),
    OPT_BOOL(0, "preview", &opt_preview,
	     "allow preview chip targets with --chip"),
    OPT_END(),
};

const struct cmd_desc cmd_init_desc = {
    .name = "init",
    .fn = cmd_init,
    .opts = cmd_init_opts,
    .manual = &init_manual,
};

static int in_list(const char *target, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(target, *list))
			return 1;
	return 0;
}

/**
 * Resolve a user-supplied --idf argument to a real filesystem path.
 *
 * A bare name (no separator, no leading '.' / '~' / '/') maps to
 * @b{~/.ice/checkouts/<name>}, mirroring @b{ice repo checkout}'s
 * shorthand.  Anything else is taken verbatim.  Returns a malloc'd
 * string the caller owns.
 */
static char *resolve_idf_arg(const char *arg)
{
	struct sbuf p = SBUF_INIT;
	int bare;

	bare = *arg && arg[0] != '/' && arg[0] != '.' && arg[0] != '~' &&
	       !strchr(arg, '/');

	if (bare)
		sbuf_addf(&p, "%s/checkouts/%s", ice_home(), arg);
	else
		sbuf_addstr(&p, arg);
	return sbuf_detach(&p);
}

/**
 * Persist idf.path to the project-local @b{.iceconfig} so subsequent
 * ice invocations pick it up via the normal config cascade.
 */
static void persist_idf_path(const char *idf_path)
{
	struct config c = CONFIG_INIT;
	const char *path = local_config_path();

	if (!path)
		die("cannot locate project @b{.iceconfig}");

	config_load_file(&c, CONFIG_SCOPE_LOCAL, path);
	config_set(&c, "idf.path", idf_path, CONFIG_SCOPE_LOCAL);
	if (config_write_file(&c, CONFIG_SCOPE_LOCAL, path))
		die_errno("cannot write '%s'", path);
	config_release(&c);

	/* Reflect immediately in the process-wide config so setup_tool_env()
	 * and any subsequent config_get("idf.path") see the new value. */
	config_set(&config, "idf.path", idf_path, CONFIG_SCOPE_LOCAL);
}

int cmd_init(int argc, const char **argv)
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
	struct sbuf manifest = SBUF_INIT;
	struct sbuf define = SBUF_INIT;
	const char *idf;
	char *idf_resolved = NULL;
	const char *env_idf;
	int rc;

	argc = parse_options(argc, argv, &cmd_init_desc);

	if (argc > 0)
		die("too many arguments");

	/* Validate chip up front so we error before any state mutation. */
	if (opt_chip) {
		if (!in_list(opt_chip, ice_supported_targets)) {
			if (in_list(opt_chip, ice_preview_targets)) {
				if (!opt_preview)
					die("'%s' is a preview target; "
					    "pass --preview to use it",
					    opt_chip);
			} else {
				die("'%s' is not a supported target", opt_chip);
			}
		}
	}

	/* Resolve IDF path: --idf > $IDF_PATH > existing config. */
	if (opt_idf) {
		idf_resolved = resolve_idf_arg(opt_idf);
		idf = idf_resolved;
	} else if ((env_idf = getenv("IDF_PATH")) && *env_idf) {
		idf = env_idf;
	} else {
		idf = config_get("idf.path");
		if (!idf || !*idf)
			die("no ESP-IDF configured\n"
			    "hint: pass @b{--idf <path>} or "
			    "@b{ice config idf.path <path>}");
	}

	/* Validate the resolved path looks like an IDF tree. */
	sbuf_addf(&manifest, "%s/tools/tools.json", idf);
	if (access(manifest.buf, F_OK) != 0)
		die("'%s' does not look like an ESP-IDF tree (no "
		    "tools/tools.json)",
		    idf);

	/* Persist when --idf was given; ephemeral otherwise. */
	if (opt_idf)
		persist_idf_path(idf);

	/*
	 * Install (or skip if already installed) the recommended tools for
	 * this IDF.  Filtering by --chip keeps target-specific tool sets
	 * minimal when known; without --chip every always-install tool is
	 * fetched so any later target works.
	 */
	rc = install_from_manifest(manifest.buf, opt_chip, NULL, 0);
	sbuf_release(&manifest);
	if (rc) {
		free(idf_resolved);
		return rc;
	}

	/*
	 * Re-prime PATH with the just-installed tools so the cmake invoked
	 * by ensure_build_directory() can find compilers etc.  The first
	 * call from main() may have been a no-op (idf.path not yet set) or
	 * pointed at the old IDF.
	 */
	setup_tool_env();

	if (opt_chip) {
		sbuf_addf(&define, "IDF_TARGET=%s", opt_chip);
		svec_push(&global_defines, define.buf);
		sbuf_release(&define);

		putenv(envstr);

		rc = fullclean_run();
		if (rc) {
			free(idf_resolved);
			return rc;
		}

		printf("Set chip to: %s, new sdkconfig will be created.\n",
		       opt_chip);
	} else if (opt_fresh) {
		rc = fullclean_run();
		if (rc) {
			free(idf_resolved);
			return rc;
		}
	}

	rc = ensure_build_directory(1);
	free(idf_resolved);
	return rc;
}
