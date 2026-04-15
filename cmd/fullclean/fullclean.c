/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/fullclean/fullclean.c
 * @brief The "ice fullclean" subcommand -- wipe the build directory.
 */
#include <dirent.h>

#include "ice.h"

/* clang-format off */
static const struct cmd_manual manual = {
	.description =
	H_PARA("Deletes every file and subdirectory inside the configured "
	       "build directory, forcing the next ice command to run "
	       "cmake from scratch.  Stronger than @b{ice clean}, which "
	       "invokes cmake's soft @b{clean} target and keeps the cmake "
	       "configuration (@b{CMakeCache.txt}, generator files) "
	       "intact.")
	H_PARA("Refuses to run if the target directory does not look like "
	       "a cmake build directory (no @b{CMakeCache.txt}) or "
	       "contains source-tree markers (@b{CMakeLists.txt}, "
	       "@b{.git}, @b{.svn}), as a guard against running "
	       "@b{ice fullclean} against a misconfigured "
	       "@b{core.build-dir}."),

	.examples =
	H_EXAMPLE("ice fullclean"),

	.extras =
	H_SECTION("CONFIG")
	H_ITEM("core.build-dir",
	       "Build directory to clean (default @b{build}).")
	H_ITEM("core.verbose",
	       "If true, print every entry as it is removed.")

	H_SECTION("SEE ALSO")
	H_ITEM("ice clean",
	       "Soft clean via cmake's @b{clean} target; keeps cmake "
	       "configuration.")
	H_ITEM("ice reconfigure",
	       "Regenerate the build system without wiping artifacts.")
	H_ITEM("ice set-target",
	       "Switch the chip target; invokes fullclean internally."),
};
/* clang-format on */

int fullclean_run(void)
{
	const char *build_dir;
	DIR *dir;
	struct dirent *de;
	int has_cache = 0;
	int n_entries = 0;
	int verbose = 0;

	build_dir = config_get("core.build-dir");

	if (access(build_dir, F_OK) != 0) {
		printf("Build directory '%s' not found.  Nothing to clean.\n",
		       build_dir);
		return 0;
	}

	/*
	 * First pass: safety checks, cache detection, emptiness test.
	 * Kept separate from the deletion pass so we never start wiping
	 * a directory that would fail the checks partway through.
	 */
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

	if (n_entries == 0) {
		printf("Build directory '%s' is empty.  Nothing to clean.\n",
		       build_dir);
		return 0;
	}
	if (!has_cache)
		die("'%s' does not look like a cmake build directory "
		    "(no CMakeCache.txt); delete it manually",
		    build_dir);

	config_get_bool("core.verbose", &verbose);
	return rmtree(build_dir, verbose) < 0 ? -1 : 0;
}

int cmd_fullclean(int argc, const char **argv)
{
	const char *usage[] = {"ice fullclean", NULL};
	struct option opts[] = {OPT_END()};

	parse_options_manual(argc, argv, opts, usage, &manual);
	return fullclean_run();
}
