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
