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
	OPTION_CONFIG,	    /**< Routes value to config_set(key, ..., CLI). */
	OPTION_CONFIG_BOOL, /**< Flag; sets config "true" (or attached val). */
	OPTION_CONFIG_LIST, /**< Multi-value; routes to config_add. */
	OPTION_END,
};

struct option {
	enum option_type type;
	int short_opt;	      /**< Single char: 'v', 'C', 0 for none. */
	const char *long_opt; /**< Long name: "verbose", NULL for none. */
	void *value;	      /**<
			       * For BOOL/INT:          int *.
			       * For STRING:            const char **.
			       * For STRING_LIST:       struct svec *.
			       * For CONFIG*:           const char * config key.
			       */
	const char *argh;     /**< Placeholder for help: "path", "n", etc. */
	const char *help;     /**< One-line description for -h output. */
};

#define OPT_BOOL(s, l, v, h) {OPTION_BOOL, (s), (l), (v), NULL, (h)}

#define OPT_STRING(s, l, v, a, h) {OPTION_STRING, (s), (l), (v), (a), (h)}

#define OPT_STRING_LIST(s, l, v, a, h)                                         \
	{OPTION_STRING_LIST, (s), (l), (v), (a), (h)}

#define OPT_INT(s, l, v, a, h) {OPTION_INT, (s), (l), (v), (a), (h)}

#define OPT_CONFIG(s, l, key, a, h)                                            \
	{OPTION_CONFIG, (s), (l), (void *)(key), (a), (h)}

#define OPT_CONFIG_BOOL(s, l, key, h)                                          \
	{OPTION_CONFIG_BOOL, (s), (l), (void *)(key), NULL, (h)}

#define OPT_CONFIG_LIST(s, l, key, a, h)                                       \
	{OPTION_CONFIG_LIST, (s), (l), (void *)(key), (a), (h)}

#define OPT_END() {OPTION_END, 0, NULL, NULL, NULL, NULL}

struct cmd_manual;

/**
 * @brief Parse command-line options according to the option table.
 *
 * Processes argv in-place: recognized options are consumed and the
 * remaining positional arguments are packed to the front of argv.
 * Parsing stops at "--" (which is consumed) or at the first
 * non-option argument.
 *
 * On -h, a short usage is printed and the process exits 0.  On
 * --help, if @p manual is non-NULL, print_manual() renders the full
 * man-style page; otherwise --help falls back to the short usage.
 *
 * @param argc    Argument count.
 * @param argv    Argument vector (modified in-place).
 * @param opts    NULL-terminated option table (OPT_END sentinel).
 * @param usage   NULL-terminated array of short usage strings.
 * @param manual  Optional full manual for --help; may be NULL.
 * @return New argc (number of remaining arguments in argv).
 */
int parse_options_manual(int argc, const char **argv, const struct option *opts,
			 const char **usage, const struct cmd_manual *manual);

/** @brief Shorthand: parse_options_manual() with no manual. */
int parse_options(int argc, const char **argv, const struct option *opts,
		  const char **usage);

#endif /* OPTIONS_H */
