/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for options.c -- the declarative CLI option parser.
 *
 * Error paths call die() and terminate the process, so they are
 * exercised by integration tests (see e.g. cmd/config/t/0005-validation.t).
 * What this file covers is the success-path matrix: short / long forms,
 * attached vs separate values, repeatable lists, "--" terminator,
 * positional packing, and the _CFG variants that seed defaults from
 * the config store and environment before CLI parsing.
 */
#include "ice.h"
#include "tap.h"

int main(void)
{
	/* OPT_BOOL via short form. */
	{
		int verbose = 0;
		struct option opts[] = {
		    OPT_BOOL('v', "verbose", &verbose, "be loud"),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "-v"};
		int argc = parse_options(2, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(verbose == 1);
		tap_done("OPT_BOOL via short form sets the flag");
	}

	/* OPT_BOOL via long form. */
	{
		int verbose = 0;
		struct option opts[] = {
		    OPT_BOOL('v', "verbose", &verbose, "be loud"),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "--verbose"};
		int argc = parse_options(2, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(verbose == 1);
		tap_done("OPT_BOOL via long form sets the flag");
	}

	/* OPT_STRING with the value in the next argv slot (-B out). */
	{
		const char *path = NULL;
		struct option opts[] = {
		    OPT_STRING('B', "build-dir", &path, "path", "where", NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "-B", "out"};
		int argc = parse_options(3, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(path != NULL && strcmp(path, "out") == 0);
		tap_done("OPT_STRING short form takes value from next argv");
	}

	/* OPT_STRING with the value attached to the short flag (-Bout). */
	{
		const char *path = NULL;
		struct option opts[] = {
		    OPT_STRING('B', "build-dir", &path, "path", "where", NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "-Bout"};
		int argc = parse_options(2, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(path != NULL && strcmp(path, "out") == 0);
		tap_done("OPT_STRING short form takes attached value (-Bval)");
	}

	/* OPT_STRING long form with attached value (--build-dir=out). */
	{
		const char *path = NULL;
		struct option opts[] = {
		    OPT_STRING('B', "build-dir", &path, "path", "where", NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "--build-dir=out"};
		int argc = parse_options(2, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(path != NULL && strcmp(path, "out") == 0);
		tap_done(
		    "OPT_STRING long form takes attached value (--foo=val)");
	}

	/* OPT_STRING long form with separate value (--build-dir out). */
	{
		const char *path = NULL;
		struct option opts[] = {
		    OPT_STRING('B', "build-dir", &path, "path", "where", NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "--build-dir", "out"};
		int argc = parse_options(3, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(path != NULL && strcmp(path, "out") == 0);
		tap_done("OPT_STRING long form takes value from next argv");
	}

	/* OPT_INT parses through atoi(). */
	{
		int n = 0;
		struct option opts[] = {
		    OPT_INT('n', "count", &n, "n", "how many", NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "-n", "42"};
		int argc = parse_options(3, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(n == 42);
		tap_done("OPT_INT parses the value");
	}

	/* OPT_STRING_LIST is repeatable across short, long-attached, and
	 * long-separate forms. */
	{
		struct svec list = SVEC_INIT;
		struct option opts[] = {
		    OPT_STRING_LIST('D', "define", &list, "key=val",
				    "repeatable", NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog",	  "-D",	 "A=1",
				      "--define", "B=2", "--define=C=3"};
		int argc = parse_options(6, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(list.nr == 3);
		tap_check(strcmp(list.v[0], "A=1") == 0);
		tap_check(strcmp(list.v[1], "B=2") == 0);
		tap_check(strcmp(list.v[2], "C=3") == 0);
		svec_clear(&list);
		tap_done("OPT_STRING_LIST appends across all three forms");
	}

	/* Positionals after options pack to the front of argv. */
	{
		int verbose = 0;
		struct option opts[] = {
		    OPT_BOOL('v', "verbose", &verbose, "be loud"),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "-v", "first", "second"};
		int argc = parse_options(4, argv, &cmd_desc);
		tap_check(argc == 2);
		tap_check(strcmp(argv[0], "first") == 0);
		tap_check(strcmp(argv[1], "second") == 0);
		tap_check(verbose == 1);
		tap_done("positionals after options pack to argv[0..]");
	}

	/* The parser stops at the first non-option argv slot.  Anything
	 * after that -- including option-shaped tokens -- stays positional. */
	{
		int verbose = 0;
		struct option opts[] = {
		    OPT_BOOL('v', "verbose", &verbose, "be loud"),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "first", "-v"};
		int argc = parse_options(3, argv, &cmd_desc);
		tap_check(argc == 2);
		tap_check(strcmp(argv[0], "first") == 0);
		tap_check(strcmp(argv[1], "-v") == 0);
		tap_check(verbose == 0);
		tap_done("parser stops at first positional");
	}

	/* "--" forces the rest of argv to be positional even if it looks
	 * like an option. */
	{
		int verbose = 0;
		struct option opts[] = {
		    OPT_BOOL('v', "verbose", &verbose, "be loud"),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "-v", "--", "-v"};
		int argc = parse_options(4, argv, &cmd_desc);
		tap_check(argc == 1);
		tap_check(strcmp(argv[0], "-v") == 0);
		tap_check(verbose == 1);
		tap_done(
		    "\"--\" terminator forces remaining argv to positional");
	}

	/*
	 * OPT_STRING_CFG seeds the default from config when no CLI flag is
	 * passed.  The C variable's initializer acts as the final fallback.
	 */
	{
		const char *path = "fallback";
		struct option opts[] = {
		    OPT_STRING_CFG('B', "build-dir", &path, "path",
				   "core.build-dir", NULL, "where", NULL, NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog"};
		int argc;

		config_release(&config);
		config_set(&config, "core.build-dir", "from-config",
			   CONFIG_SCOPE_USER);
		argc = parse_options(1, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(path != NULL && strcmp(path, "from-config") == 0);
		config_release(&config);
		tap_done("OPT_STRING_CFG seeds default from config");
	}

	/* The CLI flag wins over both the config default and any initializer.
	 */
	{
		const char *path = "fallback";
		struct option opts[] = {
		    OPT_STRING_CFG('B', "build-dir", &path, "path",
				   "core.build-dir", NULL, "where", NULL, NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "-B", "cli"};
		int argc;

		config_release(&config);
		config_set(&config, "core.build-dir", "from-config",
			   CONFIG_SCOPE_USER);
		argc = parse_options(3, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(path != NULL && strcmp(path, "cli") == 0);
		config_release(&config);
		tap_done("CLI flag overrides OPT_STRING_CFG config seed");
	}

	/* env_var overrides config_key but loses to the CLI flag. */
	{
		const char *path = NULL;
		struct option opts[] = {
		    OPT_STRING_CFG('B', "build-dir", &path, "path",
				   "core.build-dir", "ICE_TEST_BUILD_DIR",
				   "where", NULL, NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog"};
		int argc;

		config_release(&config);
		config_set(&config, "core.build-dir", "from-config",
			   CONFIG_SCOPE_USER);
		setenv("ICE_TEST_BUILD_DIR", "from-env", 1);
		argc = parse_options(1, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(path != NULL && strcmp(path, "from-env") == 0);
		setenv("ICE_TEST_BUILD_DIR", "", 1);
		config_release(&config);
		tap_done("OPT_STRING_CFG env_var overrides config_key");
	}

	/* OPT_BOOL_CFG parses the config value via config_parse_bool(). */
	{
		int verbose = 0;
		struct option opts[] = {
		    OPT_BOOL_CFG('v', "verbose", &verbose, "core.verbose", NULL,
				 "be loud", NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog"};
		int argc;

		config_release(&config);
		config_set(&config, "core.verbose", "yes", CONFIG_SCOPE_USER);
		argc = parse_options(1, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(verbose == 1);
		config_release(&config);
		tap_done("OPT_BOOL_CFG parses the config value");
	}

	/*
	 * OPT_STRING_LIST_CFG collects every config entry for the key --
	 * regardless of scope -- before CLI -D values append more.
	 */
	{
		struct svec list = SVEC_INIT;
		struct option opts[] = {
		    OPT_STRING_LIST_CFG('D', "define", &list, "key=val",
					"cmake.define", NULL, "repeatable",
					NULL, NULL),
		    OPT_END(),
		};
		struct cmd_desc cmd_desc = {.opts = opts};
		const char *argv[] = {"prog", "-D", "C=3"};
		int argc;

		config_release(&config);
		config_add(&config, "cmake.define", "A=1", CONFIG_SCOPE_USER);
		config_add(&config, "cmake.define", "B=2", CONFIG_SCOPE_LOCAL);
		argc = parse_options(3, argv, &cmd_desc);
		tap_check(argc == 0);
		tap_check(list.nr == 3);
		tap_check(strcmp(list.v[0], "A=1") == 0);
		tap_check(strcmp(list.v[1], "B=2") == 0);
		tap_check(strcmp(list.v[2], "C=3") == 0);
		svec_clear(&list);
		config_release(&config);
		tap_done(
		    "OPT_STRING_LIST_CFG seeds from config then appends CLI");
	}

	return tap_result();
}
