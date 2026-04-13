/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file options.h
 * @brief Declarative command-line option parsing.
 *
 * Define options as a table, call parse_options(), and the parser
 * handles short (-v) and long (--verbose, --build-dir=path) forms,
 * auto-generates help on -h/--help, and stops at "--".
 *
 * Usage:
 *   const char *dir = "build";
 *   int verbose = 0;
 *
 *   struct option opts[] = {
 *       OPT_STRING('B', "build-dir", &dir, "path", "build directory"),
 *       OPT_BOOL('v', "verbose", &verbose, "increase verbosity"),
 *       OPT_END(),
 *   };
 *
 *   const char *usage[] = {
 *       "ice build [-B <path>] [-v]",
 *       NULL,
 *   };
 *
 *   argc = parse_options(argc, argv, opts, usage);
 *   // argv[0..argc-1] are remaining non-option arguments
 */
#ifndef OPTIONS_H
#define OPTIONS_H

enum option_type {
	OPTION_BOOL,
	OPTION_STRING,
	OPTION_STRING_LIST,
	OPTION_INT,
	OPTION_END,
};

struct option {
	enum option_type type;
	int short_opt;		/**< Single char: 'v', 'C', 0 for none. */
	const char *long_opt;	/**< Long name: "verbose", NULL for none. */
	void *value;		/**< Pointer to int (BOOL/INT) or const char **. */
	const char *argh;	/**< Placeholder for help: "path", "n", etc. */
	const char *help;	/**< One-line description for -h output. */
};

#define OPT_BOOL(s, l, v, h)                                                  \
	{ OPTION_BOOL, (s), (l), (v), NULL, (h) }

#define OPT_STRING(s, l, v, a, h)                                             \
	{ OPTION_STRING, (s), (l), (v), (a), (h) }

#define OPT_STRING_LIST(s, l, v, a, h)                                        \
	{ OPTION_STRING_LIST, (s), (l), (v), (a), (h) }

#define OPT_INT(s, l, v, a, h)                                                \
	{ OPTION_INT, (s), (l), (v), (a), (h) }

#define OPT_END()                                                              \
	{ OPTION_END, 0, NULL, NULL, NULL, NULL }

/**
 * @brief Parse command-line options according to the option table.
 *
 * Processes argv in-place: recognized options are consumed and the
 * remaining positional arguments are packed to the front of argv.
 * Parsing stops at "--" (which is consumed) or at the first
 * non-option argument.
 *
 * If -h or --help is encountered, usage is printed and the process
 * exits with 0.
 *
 * @param argc   Argument count.
 * @param argv   Argument vector (modified in-place).
 * @param opts   NULL-terminated option table (OPT_END sentinel).
 * @param usage  NULL-terminated array of usage strings for -h output.
 * @return New argc (number of remaining arguments in argv).
 */
int parse_options(int argc, const char **argv, const struct option *opts,
		  const char **usage);

#endif /* OPTIONS_H */
