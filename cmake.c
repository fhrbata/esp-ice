/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmake.c
 * @brief Profile loading + init-gate checks for cmake-using commands.
 *
 * @b{ice init} is the single owner of the cmake configure-time state
 * (generator, -D flags, build-dir layout, build.ninja fixups).  Every
 * other cmake-based command just reads the profile, verifies init has
 * been run, and shells out to @b{cmake --build}.  If the user mangles
 * the build directory by hand or wants raw cmake control, that's on
 * them -- re-run @b{ice init} to get back to a known state.
 */
#include "ice.h"

void complete_profile_names(void)
{
	struct svec seen = SVEC_INIT;
	int seen_default = 0;

	for (int i = 0; i < config.nr; i++) {
		const char *key = config.entries[i].key;
		const char *p, *dot;
		struct sbuf nm = SBUF_INIT;
		int duplicate = 0;

		if (strncmp(key, "project.", 8) != 0)
			continue;
		p = key + 8;
		dot = strchr(p, '.');
		if (!dot)
			continue;

		sbuf_add(&nm, p, dot - p);
		for (size_t j = 0; j < seen.nr; j++) {
			if (!strcmp(seen.v[j], nm.buf)) {
				duplicate = 1;
				break;
			}
		}
		if (!duplicate) {
			printf("%s\n", nm.buf);
			if (!strcmp(nm.buf, "default"))
				seen_default = 1;
			svec_push(&seen, nm.buf);
		}
		sbuf_release(&nm);
	}
	svec_clear(&seen);

	if (!seen_default)
		printf("default\n");
}

void load_profile(const char *name)
{
	const char *build_dir;
	const char *idf_path;

	config_load_profile(name);

	/*
	 * Distinguish "not an ice project" (no .iceconfig anywhere near)
	 * from "project exists but this profile isn't bound yet" so the
	 * hint is actionable in each case -- same shape as git refusing
	 * to run outside a work tree.
	 */
	if (access(local_config_path(), F_OK) != 0) {
		hint("run @b{ice init <chip> <idf>} to bind, "
		     "or cd to an existing ice project");
		die("not an ice project (no @b{.iceconfig} in cwd)");
	}

	build_dir = config_get("project.build-dir");
	if (!build_dir || !*build_dir) {
		if (!strcmp(name, "default")) {
			hint("run @b{ice init <chip> <idf>} first");
			die("project not initialised");
		}
		hint("run @b{ice init <chip> <idf> %s} first", name);
		die("profile '%s' not configured", name);
	}

	idf_path = config_get("project.idf-path");
	if (idf_path && *idf_path) {
		struct sbuf env = SBUF_INIT;

		setup_tool_env(idf_path);
		/*
		 * The profile owns the binding, so IDF_PATH in the
		 * inherited shell env (e.g. a previously sourced
		 * export.sh) must not win over what init recorded.
		 * putenv keeps the pointer, so detach and leak -- the
		 * process lifetime is the env entry's lifetime.
		 */
		sbuf_addf(&env, "IDF_PATH=%s", idf_path);
		putenv(sbuf_detach(&env));
	}

	/*
	 * Disable IDF's Python-based component manager globally.  Setting
	 * it only during `ice init` is not enough: ninja later spawns
	 * sub-cmakes (bootloader, ULP, ...) that re-read project.cmake
	 * and will bring the manager back in if the env var is missing.
	 * Putting it in load_profile() ensures every ice command that
	 * touches cmake -- build, flash, menuconfig, init -- exports it
	 * before any child inherits the environment.  A native
	 * replacement is out of scope for the PoC.
	 */
	putenv((char *)"IDF_COMPONENT_MANAGER=0");
}

void project_load(const char *name)
{
	load_profile(name);
	require_project_initialized();

	const char *build_dir = config_get("project.build-dir");
	if (!build_dir || !*build_dir)
		build_dir = "build";

	/*
	 * config_load_profile() (called by load_profile()) clears the entire
	 * PROJECT scope before re-populating it, so any project.chip /
	 * project.flash-file entries from a previous call are gone.  We add
	 * ours after it returns, so there is no need to unset them here.
	 *
	 * Best-effort: if the build directory has no flasher_args.json yet
	 * (project initialised but not built), silently skip and leave the
	 * keys absent.  The caller can check config_has("project.chip") etc.
	 */
	struct sbuf path = SBUF_INIT;
	struct sbuf buf = SBUF_INIT;
	sbuf_addf(&path, "%s/flasher_args.json", build_dir);

	if (sbuf_read_file(&buf, path.buf) >= 0) {
		struct json_value *root = json_parse(buf.buf, buf.len);
		if (root) {
			/* project.chip */
			const char *chip = json_as_string(json_get(
			    json_get(root, "extra_esptool_args"), "chip"));
			if (chip && *chip)
				config_set(&config, "project.chip", chip,
					   CONFIG_SCOPE_PROJECT);

			/* project.flash-file: "offset=full_path" per image */
			struct json_value *files =
			    json_get(root, "flash_files");
			if (files && files->type == JSON_OBJECT) {
				for (int i = 0; i < files->u.object.nr; i++) {
					const char *offset =
					    files->u.object.members[i].key;
					const char *rel = json_as_string(
					    files->u.object.members[i].value);
					if (!offset || !rel)
						continue;
					struct sbuf entry = SBUF_INIT;
					sbuf_addf(&entry, "%s=%s/%s", offset,
						  build_dir, rel);
					config_add(
					    &config, "project.flash-file",
					    entry.buf, CONFIG_SCOPE_PROJECT);
					sbuf_release(&entry);
				}
			}
			json_free(root);
		}
	}

	sbuf_release(&buf);
	sbuf_release(&path);
}

void require_project_initialized(void)
{
	const char *build_dir = config_get("project.build-dir");
	struct sbuf cache = SBUF_INIT;
	int ok;

	if (!build_dir || !*build_dir)
		goto notinit;

	sbuf_addf(&cache, "%s/CMakeCache.txt", build_dir);
	ok = (access(cache.buf, F_OK) == 0);
	sbuf_release(&cache);

	if (ok)
		return;

notinit:
	hint("run @b{ice init <chip> <idf>} first");
	die("project not initialised");
}
