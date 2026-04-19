/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/init/init.c
 * @brief `ice init` -- bind a project to an ESP-IDF + chip target.
 *
 * Required positionals: @b{<chip>} @b{<idf>}.
 * Optional positional:  @b{[<name>]} -- project profile, defaults to
 * "default".  Each profile gets its own build directory and sdkconfig
 * so multi-target builds (e.g. esp32 + esp32s3 in the same project)
 * stay isolated.
 *
 * Every @b{ice init} call:
 *   1. Validates the chip and the IDF path (must contain tools/tools.json).
 *   2. Persists the profile under @b{[project "<name>"]} in @b{.iceconfig}.
 *   3. Installs (or skips if already installed) the IDF's tools.
 *   4. Renames the profile's @b{sdkconfig} to @b{sdkconfig.old}.
 *   5. Wipes the profile's build directory.
 *   6. Runs cmake from scratch.
 *
 * No partial / incremental modes; for a soft rebuild use @b{ice clean}
 * + @b{ice build}, which keeps the cmake configuration intact.
 */
#include <dirent.h>

#include "ice.h"

/* ------------------------------------------------------------------ */
/* Manual / options                                                    */
/* ------------------------------------------------------------------ */

/* clang-format off */
static const struct cmd_manual init_manual = {
	.name = "ice init",
	.summary = "bind project to an ESP-IDF + chip target",

	.description =
	H_PARA("Bind the current project to an ESP-IDF source tree and "
	       "chip target.  Always wipes the profile's build directory, "
	       "renames any existing @b{sdkconfig} to @b{sdkconfig.old}, "
	       "installs the IDF's tools, and runs cmake from scratch.")
	H_PARA("Both @b{<chip>} and @b{<idf>} are required.  An optional "
	       "third positional @b{[<name>]} selects the project profile "
	       "(default: @b{default}).  Use named profiles to keep build "
	       "artefacts for different chips or sdkconfig sets in "
	       "isolated build directories within the same project.")
	H_PARA("@b{<idf>} can be a bare name (e.g. @b{v5.4}, resolved to "
	       "@b{~/.ice/checkouts/v5.4/}) or any path to an existing "
	       "ESP-IDF tree.  Create named checkouts with @b{ice repo "
	       "checkout}.")
	H_PARA("Profile state is persisted to @b{.iceconfig} under "
	       "@b{[project \"<name>\"]} so subsequent ice commands can "
	       "look up what was configured."),

	.examples =
	H_EXAMPLE("ice init esp32 v5.4")
	H_EXAMPLE("ice init esp32s3 v5.5 production")
	H_EXAMPLE("ice init --preview linux v5.4")
	H_EXAMPLE("ice init -s sdkconfig.prod -S sdkconfig.defaults esp32 v5.4 prod"),

	.extras =
	H_SECTION("CONFIG")
	H_ITEM("project.<name>.chip",
	       "Chip target for profile @b{<name>}.")
	H_ITEM("project.<name>.idf-path",
	       "ESP-IDF source path for profile @b{<name>}.")
	H_ITEM("project.<name>.sdkconfig",
	       "Sdkconfig path "
	       "(default: @b{sdkconfig} for @b{default}, "
	       "@b{sdkconfig.<name>} otherwise).")
	H_ITEM("project.<name>.sdkconfig-defaults",
	       "Defaults files (multi-value).")
	H_ITEM("project.<name>.build-dir",
	       "Build directory "
	       "(default: @b{build} for @b{default}, "
	       "@b{build/<name>} otherwise).")
	H_ITEM("project.<name>.generator",
	       "cmake generator (default: @b{Ninja}).")
	H_ITEM("project.<name>.define",
	       "Extra @b{-D<key>=<value>} entries (multi-value).")

	H_SECTION("SEE ALSO")
	H_ITEM("ice repo checkout",
	       "Create a named ESP-IDF checkout to bind with @b{<idf>}.")
	H_ITEM("ice build",
	       "Build the project after init.")
	H_ITEM("ice clean",
	       "Soft rebuild: remove built artefacts but keep cmake "
	       "configuration intact."),
};
/* clang-format on */

static int opt_preview;
static const char *opt_sdkconfig;
static struct svec opt_sdkconfig_defaults;
static const char *opt_build_dir;
static const char *opt_generator;
static struct svec opt_defines;

/* ------------------------------------------------------------------ */
/* Per-slot completion                                                 */
/* ------------------------------------------------------------------ */

static int complete_dir_cb(const char *name, void *ud)
{
	(void)ud;
	printf("%s\n", name);
	return 0;
}

/** Slot 0 (chip): emit every supported and preview chip. */
static void complete_chip(void)
{
	for (const char *const *t = ice_supported_targets; *t; t++)
		printf("%s\n", *t);
	for (const char *const *t = ice_preview_targets; *t; t++)
		printf("%s\n", *t);
}

/** Slot 1 (idf): emit names of @b{~/.ice/checkouts/} entries. */
static void complete_idf(void)
{
	struct sbuf path = SBUF_INIT;

	sbuf_addf(&path, "%s/checkouts", ice_home());
	if (access(path.buf, F_OK) == 0)
		dir_foreach(path.buf, complete_dir_cb, NULL);
	sbuf_release(&path);
}

static const struct option cmd_init_opts[] = {
    OPT_STRING('s', "sdkconfig", &opt_sdkconfig, "file",
	       "sdkconfig path (default: sdkconfig[.<name>])", NULL),
    OPT_STRING_LIST('S', "sdkconfig-defaults", &opt_sdkconfig_defaults, "file",
		    "sdkconfig defaults file (repeatable)", NULL),
    OPT_STRING('b', "build-dir", &opt_build_dir, "dir",
	       "build directory (default: build[/<name>])", NULL),
    OPT_STRING('g', "generator", &opt_generator, "name",
	       "cmake generator (default: Ninja)", NULL),
    OPT_STRING_LIST('d', "define", &opt_defines, "key=val",
		    "extra cmake -D<key>=<value> entry (repeatable)", NULL),
    OPT_BOOL(0, "preview", &opt_preview, "allow preview chip targets"),
    OPT_POSITIONAL("chip", complete_chip),
    OPT_POSITIONAL("idf", complete_idf),
    OPT_POSITIONAL("[<name>]", complete_profile_names),
    OPT_END(),
};

const struct cmd_desc cmd_init_desc = {
    .name = "init",
    .fn = cmd_init,
    .opts = cmd_init_opts,
    .manual = &init_manual,
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int in_list(const char *target, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(target, *list))
			return 1;
	return 0;
}

/**
 * Resolve the @b{<idf>} positional to a real filesystem path.
 *
 * A bare name (no separator, no leading @b{.} / @b{~} / @b{/}) maps to
 * @b{~/.ice/checkouts/<name>/}, mirroring @b{ice repo checkout}'s
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
 * Wipe the contents of @p build_dir.
 *
 * Safety checks (kept verbatim from the old `ice fullclean`): refuse
 * to clean a directory that does not have @b{CMakeCache.txt} or that
 * contains source-tree markers (@b{CMakeLists.txt}, @b{.git},
 * @b{.svn}) -- guards against a misconfigured build-dir.
 */
static int wipe_build_dir(const char *build_dir)
{
	DIR *dir;
	struct dirent *de;
	int has_cache = 0;
	int n_entries = 0;

	if (access(build_dir, F_OK) != 0)
		return 0;

	dir = opendir(build_dir);
	if (!dir)
		die_errno("cannot open '%s'", build_dir);

	while ((de = readdir(dir)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		n_entries++;
		if (!strcmp(de->d_name, "CMakeCache.txt"))
			has_cache = 1;
		if (!strcmp(de->d_name, "CMakeLists.txt") ||
		    !strcmp(de->d_name, ".git") ||
		    !strcmp(de->d_name, ".svn")) {
			closedir(dir);
			die("refusing to clean '%s': contains '%s'", build_dir,
			    de->d_name);
		}
	}
	closedir(dir);

	if (n_entries == 0)
		return 0;
	if (!has_cache)
		die("'%s' does not look like a cmake build directory "
		    "(no CMakeCache.txt); delete it manually",
		    build_dir);

	return rmtree(build_dir, global_verbose) < 0 ? -1 : 0;
}

/** Rename @p path to @p path.old if @p path exists. */
static void backup_sdkconfig(const char *sdkconfig)
{
	struct sbuf old = SBUF_INIT;

	if (access(sdkconfig, F_OK) != 0)
		goto done;
	sbuf_addf(&old, "%s.old", sdkconfig);
	if (rename(sdkconfig, old.buf) < 0)
		warn_errno("rename '%s' -> '%s'", sdkconfig, old.buf);
done:
	sbuf_release(&old);
}

/** Persist the profile under @b{[project "<name>"]} in @b{.iceconfig}. */
static void persist_profile(const char *name, const char *chip,
			    const char *idf_path, const char *sdkconfig,
			    const struct svec *sdkconfig_defaults,
			    const char *build_dir, const char *generator,
			    const struct svec *defines)
{
	struct config c = CONFIG_INIT;
	const char *path = local_config_path();
	struct sbuf key = SBUF_INIT;

	if (!path)
		die("cannot locate project @b{.iceconfig}");

	config_load_file(&c, CONFIG_SCOPE_LOCAL, path);

	/* Scalar entries (config_set replaces existing). */
	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.chip", name);
	config_set(&c, key.buf, chip, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.idf-path", name);
	config_set(&c, key.buf, idf_path, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.sdkconfig", name);
	config_set(&c, key.buf, sdkconfig, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.build-dir", name);
	config_set(&c, key.buf, build_dir, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.generator", name);
	config_set(&c, key.buf, generator, CONFIG_SCOPE_LOCAL);

	/* Multi-values (clear + re-add so re-init replaces, not appends). */
	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.sdkconfig-defaults", name);
	config_unset(&c, key.buf, CONFIG_SCOPE_LOCAL);
	for (size_t i = 0; i < sdkconfig_defaults->nr; i++)
		config_add(&c, key.buf, sdkconfig_defaults->v[i],
			   CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.define", name);
	config_unset(&c, key.buf, CONFIG_SCOPE_LOCAL);
	for (size_t i = 0; i < defines->nr; i++)
		config_add(&c, key.buf, defines->v[i], CONFIG_SCOPE_LOCAL);

	if (config_write_file(&c, CONFIG_SCOPE_LOCAL, path))
		die_errno("cannot write '%s'", path);
	config_release(&c);
	sbuf_release(&key);
}

/* ------------------------------------------------------------------ */
/* Command entry                                                       */
/* ------------------------------------------------------------------ */

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
	const char *chip;
	const char *name;
	char *idf_path = NULL;
	struct sbuf manifest = SBUF_INIT;
	struct sbuf sdkconfig_buf = SBUF_INIT;
	struct sbuf build_dir_buf = SBUF_INIT;
	struct sbuf joined = SBUF_INIT;
	struct sbuf entry = SBUF_INIT;
	const char *sdkconfig;
	const char *build_dir;
	const char *generator;
	int rc;

	argc = parse_options(argc, argv, &cmd_init_desc);

	if (argc < 2)
		die("usage: ice init <chip> <idf> [<name>]");
	if (argc > 3)
		die("too many arguments");

	chip = argv[0];
	idf_path = resolve_idf_arg(argv[1]);
	name = (argc >= 3) ? argv[2] : "default";

	/* Validate chip up front so we error before any state mutation. */
	if (!in_list(chip, ice_supported_targets)) {
		if (in_list(chip, ice_preview_targets)) {
			if (!opt_preview)
				die("'%s' is a preview target; "
				    "pass --preview to use it",
				    chip);
		} else {
			die("'%s' is not a supported target", chip);
		}
	}

	/* Validate the IDF path. */
	sbuf_addf(&manifest, "%s/tools/tools.json", idf_path);
	if (access(manifest.buf, F_OK) != 0)
		die("'%s' does not look like an ESP-IDF tree (no "
		    "tools/tools.json)",
		    idf_path);

	/* Auto-default sdkconfig and build-dir from profile name. */
	sdkconfig = opt_sdkconfig;
	if (!sdkconfig) {
		if (!strcmp(name, "default")) {
			sdkconfig = "sdkconfig";
		} else {
			sbuf_addf(&sdkconfig_buf, "sdkconfig.%s", name);
			sdkconfig = sdkconfig_buf.buf;
		}
	}

	build_dir = opt_build_dir;
	if (!build_dir) {
		if (!strcmp(name, "default")) {
			build_dir = "build";
		} else {
			sbuf_addf(&build_dir_buf, "build/%s", name);
			build_dir = build_dir_buf.buf;
		}
	}

	generator = opt_generator ? opt_generator : "Ninja";

	/* Persist the profile to .iceconfig. */
	persist_profile(name, chip, idf_path, sdkconfig,
			&opt_sdkconfig_defaults, build_dir, generator,
			&opt_defines);

	/* Install (or skip if already installed) tools for this IDF.
	 * Filtering by chip keeps target-specific tool sets minimal. */
	rc = install_from_manifest(manifest.buf, chip, NULL, 0);
	sbuf_release(&manifest);
	if (rc) {
		free(idf_path);
		return rc;
	}

	/* Prime PATH with the just-installed tools so the cmake invoked
	 * by ensure_build_directory() can find compilers etc. */
	setup_tool_env(idf_path);

	/* Wipe the profile's build dir and back up its sdkconfig. */
	backup_sdkconfig(sdkconfig);
	rc = wipe_build_dir(build_dir);
	if (rc) {
		free(idf_path);
		return rc;
	}

	/* Plug build-dir / generator / defines into the globals
	 * ensure_build_directory() consumes. */
	global_build_dir = build_dir;
	global_generator = generator;

	sbuf_addf(&entry, "IDF_TARGET=%s", chip);
	svec_push(&global_defines, entry.buf);
	sbuf_reset(&entry);

	sbuf_addf(&entry, "SDKCONFIG=%s", sdkconfig);
	svec_push(&global_defines, entry.buf);
	sbuf_reset(&entry);

	if (opt_sdkconfig_defaults.nr) {
		sbuf_addstr(&joined, "SDKCONFIG_DEFAULTS=");
		for (size_t i = 0; i < opt_sdkconfig_defaults.nr; i++) {
			if (i > 0)
				sbuf_addch(&joined, ';');
			sbuf_addstr(&joined, opt_sdkconfig_defaults.v[i]);
		}
		svec_push(&global_defines, joined.buf);
	}
	sbuf_release(&joined);
	sbuf_release(&entry);

	for (size_t i = 0; i < opt_defines.nr; i++)
		svec_push(&global_defines, opt_defines.v[i]);

	putenv(envstr);

	rc = ensure_build_directory(1);
	free(idf_path);
	sbuf_release(&sdkconfig_buf);
	sbuf_release(&build_dir_buf);
	return rc;
}
