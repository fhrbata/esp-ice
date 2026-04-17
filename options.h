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
 * auto-generates help on -h/--help, auto-generates completions on
 * --ice-complete, and stops at "--".
 *
 * Usage:
 *   const char *dir = "build";
 *   int verbose = 0;
 *
 *   struct option opts[] = {
 *       OPT_STRING('B', "build-dir", &dir, "path", "build directory", NULL),
 *       OPT_BOOL('v', "verbose", &verbose, "increase verbosity"),
 *       OPT_END(),
 *   };
 *
 *   argc = parse_options(argc, argv, opts);
 *   // argv[0..argc-1] are remaining non-option arguments
 */
#ifndef OPTIONS_H
#define OPTIONS_H

typedef int (*subcmd_fn)(int argc, const char **argv);

enum option_type {
	OPTION_BOOL,
	OPTION_STRING,
	OPTION_STRING_LIST,
	OPTION_INT,
	OPTION_CONFIG,	    /**< Routes value to config_set(key, ..., CLI). */
	OPTION_CONFIG_BOOL, /**< Flag; sets config "true" (or attached val). */
	OPTION_CONFIG_LIST, /**< Multi-value; routes to config_add. */
	OPTION_SUBCOMMAND,  /**< Positional subcommand; sets subcmd_fn *. */
	OPTION_END,
};

struct option {
	enum option_type type;
	int short_opt;		 /**< Single char: 'v', 'C', 0 for none. */
	const char *long_opt;	 /**< Long name: "verbose", NULL for none.
				  *   For SUBCOMMAND: the subcommand name. */
	void *value;		 /**<
				  * For BOOL/INT:          int *.
				  * For STRING:            const char **.
				  * For STRING_LIST:       struct svec *.
				  * For CONFIG*:           const char * config key.
				  * For SUBCOMMAND:        subcmd_fn * (set on match).
				  */
	const char *argh;	 /**< Placeholder for help: "path", "n", etc. */
	const char *help;	 /**< One-line description for -h output. */
	subcmd_fn subcommand_fn; /**< For SUBCOMMAND: the handler function. */
	void (*complete)(void);	 /**< Prints completion candidates to stdout.
				  *   On a value-taking option: called when
				  *   completing that option's value.
				  *   On OPT_END: called for positional arg
				  *   completions.
				  */
};

#define OPT_BOOL(s, l, v, h) {OPTION_BOOL, (s), (l), (v), NULL, (h), NULL, NULL}

#define OPT_STRING(s, l, v, a, h, c)                                           \
	{OPTION_STRING, (s), (l), (v), (a), (h), NULL, (c)}

#define OPT_STRING_LIST(s, l, v, a, h, c)                                      \
	{OPTION_STRING_LIST, (s), (l), (v), (a), (h), NULL, (c)}

#define OPT_INT(s, l, v, a, h, c)                                              \
	{OPTION_INT, (s), (l), (v), (a), (h), NULL, (c)}

#define OPT_CONFIG(s, l, key, a, h, c)                                         \
	{OPTION_CONFIG, (s), (l), (void *)(key), (a), (h), NULL, (c)}

#define OPT_CONFIG_BOOL(s, l, key, h)                                          \
	{OPTION_CONFIG_BOOL, (s), (l), (void *)(key), NULL, (h), NULL, NULL}

#define OPT_CONFIG_LIST(s, l, key, a, h, c)                                    \
	{OPTION_CONFIG_LIST, (s), (l), (void *)(key), (a), (h), NULL, (c)}

/**
 * @brief Declare a subcommand in the option table.
 *
 * @param name  Subcommand name (what the user types).
 * @param var   Pointer to a subcmd_fn variable (set on match).
 * @param fn    Handler function: int fn(int argc, const char **argv).
 * @param h     One-line description for help/completion.
 *
 * When the parser encounters @p name as a positional argument, it
 * stores @p fn into *@p var and stops parsing.  The remaining argv
 * (starting at the subcommand name) is left for the handler.
 */
#define OPT_SUBCOMMAND(name, var, fn, h)                                       \
	{OPTION_SUBCOMMAND, 0, (name), (var), NULL, (h), (fn), NULL}

#define OPT_END() {OPTION_END, 0, NULL, NULL, NULL, NULL, NULL, NULL}

/**
 * @brief OPT_END with positional argument metadata.
 * @param argh  Name shown in auto-generated usage (e.g. "target").
 * @param c     Completion callback for positional values, or NULL.
 */
#define OPT_END_COMPLETE(argh, c)                                              \
	{OPTION_END, 0, NULL, NULL, (argh), NULL, NULL, (c)}

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
 * On --ice-complete, all flags, subcommands and positional
 * completions are printed to stdout and the process exits 0.
 *
 * @param argc    Argument count.
 * @param argv    Argument vector (modified in-place).
 * @param opts    NULL-terminated option table (OPT_END sentinel).
 * @param manual  Optional full manual for --help; may be NULL.
 * @return New argc (number of remaining arguments in argv).
 */
int parse_options_manual(int argc, const char **argv, const struct option *opts,
			 const struct cmd_manual *manual);

/** @brief Shorthand: parse_options_manual() with no manual. */
int parse_options(int argc, const char **argv, const struct option *opts);

#endif /* OPTIONS_H */
