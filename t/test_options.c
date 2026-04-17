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
 * positional packing, and the OPT_CONFIG* routes that write into the
 * global config store.
 */
#include "ice.h"
#include "tap.h"

int main(void)
{
	static const char *const usage[] = {"prog", NULL};

	/* OPT_BOOL via short form. */
	{
		int verbose = 0;
		struct option opts[] = {
		    OPT_BOOL('v', "verbose", &verbose, "be loud"),
		    OPT_END(),
		};
		const char *argv[] = {"prog", "-v"};
		int argc = parse_options(2, argv, opts);
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
		const char *argv[] = {"prog", "--verbose"};
		int argc = parse_options(2, argv, opts);
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
		const char *argv[] = {"prog", "-B", "out"};
		int argc = parse_options(3, argv, opts);
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
		const char *argv[] = {"prog", "-Bout"};
		int argc = parse_options(2, argv, opts);
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
		const char *argv[] = {"prog", "--build-dir=out"};
		int argc = parse_options(2, argv, opts);
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
		const char *argv[] = {"prog", "--build-dir", "out"};
		int argc = parse_options(3, argv, opts);
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
		const char *argv[] = {"prog", "-n", "42"};
		int argc = parse_options(3, argv, opts);
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
		const char *argv[] = {"prog",	  "-D",	 "A=1",
				      "--define", "B=2", "--define=C=3"};
		int argc = parse_options(6, argv, opts);
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
		const char *argv[] = {"prog", "-v", "first", "second"};
		int argc = parse_options(4, argv, opts);
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
		const char *argv[] = {"prog", "first", "-v"};
		int argc = parse_options(3, argv, opts);
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
		const char *argv[] = {"prog", "-v", "--", "-v"};
		int argc = parse_options(4, argv, opts);
		tap_check(argc == 1);
		tap_check(strcmp(argv[0], "-v") == 0);
		tap_check(verbose == 1);
		tap_done(
		    "\"--\" terminator forces remaining argv to positional");
	}

	/* OPT_CONFIG routes the value into the global config at CLI scope. */
	{
		struct option opts[] = {
		    OPT_CONFIG('B', "build-dir", "core.build-dir", "path",
			       "where", NULL),
		    OPT_END(),
		};
		const char *argv[] = {"prog", "-B", "out"};
		int argc;

		config_release(&config);
		argc = parse_options(3, argv, opts);
		tap_check(argc == 0);
		tap_check(config_get("core.build-dir") != NULL);
		tap_check(strcmp(config_get("core.build-dir"), "out") == 0);
		tap_check(config_source("core.build-dir") == CONFIG_SCOPE_CLI);
		config_release(&config);
		tap_done("OPT_CONFIG writes the value to config at CLI scope");
	}

	/* OPT_CONFIG_BOOL is a flag (no value) that records "true". */
	{
		struct option opts[] = {
		    OPT_CONFIG_BOOL('v', "verbose", "core.verbose", "be loud"),
		    OPT_END(),
		};
		const char *argv[] = {"prog", "-v"};
		int argc;

		config_release(&config);
		argc = parse_options(2, argv, opts);
		tap_check(argc == 0);
		tap_check(config_get("core.verbose") != NULL);
		tap_check(strcmp(config_get("core.verbose"), "true") == 0);
		config_release(&config);
		tap_done("OPT_CONFIG_BOOL records \"true\" at CLI scope");
	}

	/* OPT_CONFIG_LIST appends a new entry per occurrence. */
	{
		struct option opts[] = {
		    OPT_CONFIG_LIST('D', "define", "cmake.define", "key=val",
				    "repeatable", NULL),
		    OPT_END(),
		};
		const char *argv[] = {"prog", "-D", "A=1", "-D", "B=2"};
		int argc;
		struct config_entry **entries = NULL;
		int n;

		config_release(&config);
		argc = parse_options(5, argv, opts);
		tap_check(argc == 0);
		n = config_get_all("cmake.define", &entries);
		tap_check(n == 2);
		tap_check(strcmp(entries[0]->value, "A=1") == 0);
		tap_check(strcmp(entries[1]->value, "B=2") == 0);
		free(entries);
		config_release(&config);
		tap_done(
		    "OPT_CONFIG_LIST appends one config entry per occurrence");
	}

	return tap_result();
}
