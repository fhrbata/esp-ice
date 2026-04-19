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

		if (strncmp(key, "project.", 8))
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

	build_dir = config_get("project.build-dir");
	if (!build_dir || !*build_dir) {
		if (!strcmp(name, "default"))
			die("project not initialised\n"
			    "hint: run @b{ice init <chip> <idf>} first");
		die("profile '%s' not configured\n"
		    "hint: run @b{ice init <chip> <idf> %s} first",
		    name, name);
	}

	idf_path = config_get("project.idf-path");
	if (idf_path && *idf_path)
		setup_tool_env(idf_path);
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
	die("project not initialised\n"
	    "hint: run @b{ice init <chip> <idf>} first");
}
