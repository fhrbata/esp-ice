/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/hints/hints.c
 * @brief `ice idf hints` -- CLI wrapper around hints_scan().
 *
 * All the scan logic (YAML load, log normalization, PCRE2 matching,
 * template expansion) lives in the root @c hints.c module so it can
 * be reused by @c process_run_progress() on build failures.  This
 * file is just the CLI surface: option parsing, manual, usage.
 */
#include "ice.h"

#include "hints.h"

/* clang-format off */
static const struct cmd_manual idf_hints_manual = {
	.name = "ice idf hints",
	.summary = "print hints matching regex rules in a log",

	.description =
	H_PARA("Reads a hints YAML file (@b{<hints.yml>}) of "
	       "@c{{re, hint, ...}} rules and a log file (@b{<log>}), "
	       "then prints a @b{HINT:} line for each rule whose regex "
	       "matches the normalized log.  The log is normalized by "
	       "stripping each line's leading and trailing whitespace, "
	       "dropping empty lines, and joining what remains with "
	       "single spaces -- matching ESP-IDF's "
	       "@c{generate_hints_buffer} preprocessing.")
	H_PARA("Rules may declare @c{match_to_output: True} to pass the "
	       "matched capture groups (joined with @c{, }) into the "
	       "hint template via a @c{{}} placeholder.  Rules may also "
	       "declare a @c{variables:} list that expands the rule into "
	       "one compiled pattern per entry, substituting "
	       "@c{re_variables} into the pattern and @c{hint_variables} "
	       "into the message.")
	H_PARA("Regex syntax is PCRE2; the full ESP-IDF "
	       "@c{tools/idf_py_actions/hints.yml} is supported as-is, "
	       "including Perl-only constructs such as @c{\\w}, @c{\\d}, "
	       "and @c{(?!...)} negative lookaheads.")
	H_PARA("@b{ice build} (and every other command that goes through "
	       "@c{process_run_progress}) invokes this logic automatically "
	       "on failure, so the typical user never runs this command "
	       "directly.  Use @b{--no-hints} at the top level to disable "
	       "the automatic scan."),

	.examples =
	H_EXAMPLE("ice idf hints hints.yml build.log")
	H_EXAMPLE("ice idf hints $IDF_PATH/tools/idf_py_actions/hints.yml build.log"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice build",
	       "Drives ESP-IDF's build pipeline; its output is the "
	       "typical input to this command."),
};
/* clang-format on */

static const struct option cmd_idf_hints_opts[] = {
    OPT_POSITIONAL("hints.yml", NULL),
    OPT_POSITIONAL("log", NULL),
    OPT_END(),
};

int cmd_idf_hints(int argc, const char **argv);

const struct cmd_desc cmd_idf_hints_desc = {
    .name = "hints",
    .fn = cmd_idf_hints,
    .opts = cmd_idf_hints_opts,
    .manual = &idf_hints_manual,
};

int cmd_idf_hints(int argc, const char **argv)
{
	struct svec hints = SVEC_INIT;
	int rc = 0;

	argc = parse_options(argc, argv, &cmd_idf_hints_desc);
	if (argc != 2)
		die("usage: ice idf hints <hints.yml> <log>");

	if (hints_scan(argv[0], argv[1], &hints) < 0) {
		rc = 1;
		goto done;
	}

	/*
	 * Colored to match ESP-IDF's yellow_print("HINT: ...") convention.
	 * Color tokens live in the format string, not in the hint message,
	 * so a stray '}' in the text cannot unbalance the color block.
	 * When stdout is piped the tokens are stripped and callers see the
	 * plain "HINT: ..." line.
	 */
	for (size_t i = 0; i < hints.nr; i++)
		printf("@y{HINT: %s}\n", hints.v[i]);

done:
	svec_clear(&hints);
	return rc;
}
