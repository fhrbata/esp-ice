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
 * An option stores its final value in the C variable passed as @c v.
 * Before the CLI is parsed, the value is optionally seeded from a
 * config key (via the config cascade) and then from an environment
 * variable -- so precedence is:
 *
 *   C initializer  <  config_key  <  env_var  <  CLI flag
 *
 * The extended OPT_*_CFG variants carry that metadata; the plain
 * macros remain unchanged for options that don't participate in
 * config integration.
 *
 * Usage:
 *   const char *dir = "build";
 *   int verbose = 0;
 *
 *   struct option opts[] = {
 *       OPT_STRING_CFG('B', "build-dir", &dir, "path",
 *                      "core.build-dir", NULL,
 *                      "build directory", NULL, NULL),
 *       OPT_BOOL('v', "verbose", &verbose, "increase verbosity"),
 *       OPT_END(),
 *   };
 *
 *   argc = parse_options(argc, argv, opts);
 *   // argv[0..argc-1] are remaining non-option arguments
 */
#ifndef OPTIONS_H
#define OPTIONS_H

enum option_type {
	OPTION_BOOL,
	OPTION_STRING,
	OPTION_STRING_LIST,
	OPTION_INT,
	OPTION_POSITIONAL,
	OPTION_END,
};

struct option {
	enum option_type type;
	int short_opt;		 /**< Single char: 'v', 'C', 0 for none. */
	const char *long_opt;	 /**< Long name: "verbose", NULL for none. */
	void *value;		 /**<
				  * For BOOL/INT:    int *.
				  * For STRING:      const char **.
				  * For STRING_LIST: struct svec *.
				  */
	const char *argh;	 /**< Placeholder for help: "path", "n", etc. */
	const char *help;	 /**< One-line description for -h output. */
	void (*complete)(void);	 /**< Prints completion candidates to stdout.
				  *   On a value-taking option: called when
				  *   completing that option's value.
				  *   On OPT_END: called for positional arg
				  *   completions.
				  */
	const char *config_key;	 /**< Config key consulted for the default
				  *   value via config_get() (or
				  *   config_get_all() for STRING_LIST).
				  *   NULL to skip the config lookup. */
	const char *env_var;	 /**< Environment variable consulted for the
				  *   default value.  Overrides config_key
				  *   but is overridden by the CLI flag.
				  *   NULL to skip the env lookup. */
	const char *config_help; /**< Optional description rendered in the
				  *   auto-generated CONFIG / ENVIRONMENT
				  *   manual sections.  When NULL the CLI
				  *   @c help string is reused. */
};

/* ------------------------------------------------------------------ */
/* Plain variants -- no config/env integration.                        */
/* ------------------------------------------------------------------ */

#define OPT_BOOL(s, l, v, h)                                                   \
	{OPTION_BOOL, (s), (l), (v), NULL, (h), NULL, NULL, NULL, NULL}

#define OPT_STRING(s, l, v, a, h, c)                                           \
	{OPTION_STRING, (s), (l), (v), (a), (h), (c), NULL, NULL, NULL}

#define OPT_STRING_LIST(s, l, v, a, h, c)                                      \
	{OPTION_STRING_LIST, (s), (l), (v), (a), (h), (c), NULL, NULL, NULL}

#define OPT_INT(s, l, v, a, h, c)                                              \
	{OPTION_INT, (s), (l), (v), (a), (h), (c), NULL, NULL, NULL}

/* ------------------------------------------------------------------ */
/* _CFG variants -- seed default from config/env, richer manual.       */
/*                                                                     */
/*   cfg:  config key consulted via config_get()/config_get_all().     */
/*         NULL to skip.                                               */
/*   env:  environment variable name consulted via getenv().  NULL to  */
/*         skip.                                                       */
/*   ch:   description rendered in the CONFIG / ENVIRONMENT manual     */
/*         sections.  NULL falls back to the plain @p h string.        */
/* ------------------------------------------------------------------ */

#define OPT_BOOL_CFG(s, l, v, cfg, env, h, ch)                                 \
	{OPTION_BOOL, (s), (l), (v), NULL, (h), NULL, (cfg), (env), (ch)}

#define OPT_STRING_CFG(s, l, v, a, cfg, env, h, ch, c)                         \
	{OPTION_STRING, (s), (l), (v), (a), (h), (c), (cfg), (env), (ch)}

#define OPT_STRING_LIST_CFG(s, l, v, a, cfg, env, h, ch, c)                    \
	{OPTION_STRING_LIST, (s), (l), (v), (a), (h), (c), (cfg), (env), (ch)}

#define OPT_INT_CFG(s, l, v, a, cfg, env, h, ch, c)                            \
	{OPTION_INT, (s), (l), (v), (a), (h), (c), (cfg), (env), (ch)}

#define OPT_END()                                                              \
	{OPTION_END, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}

/**
 * @brief Declare a positional argument slot with per-slot completion.
 *
 * Place one OPT_POSITIONAL per slot before OPT_END.  The slot's @p argh
 * appears in the auto-generated synopsis (wrapped in @c <> if it does
 * not already contain @c < or @c [) and the slot's @p complete fires
 * when the user TABs at that positional's column.
 *
 * Example:
 *   OPT_POSITIONAL("chip",      complete_chip),
 *   OPT_POSITIONAL("idf",       complete_idf),
 *   OPT_POSITIONAL("[<name>]",  complete_profile),
 *   OPT_END(),
 *
 * For namespace-level extra completion candidates (alongside
 * subcommands and flags, like alias names at the root), use
 * @c cmd_desc.extra_complete instead -- it is not a positional.
 */
#define OPT_POSITIONAL(argh, c)                                                \
	{OPTION_POSITIONAL, 0, NULL, NULL, (argh), NULL, (c), NULL, NULL, NULL}

struct cmd_manual;
struct cmd_desc;

/**
 * @brief Parse command-line options according to a command descriptor.
 *
 * Before the CLI walk, every option with a @c config_key or @c env_var
 * is seeded: config_get() / config_get_all() populates the C variable
 * from the merged config store, then a set @c env_var overrides it.
 * The CLI flag then has the final say.
 *
 * Processes argv in-place: recognized options are consumed and the
 * remaining positional arguments are packed to the front of argv.
 * Parsing stops at "--" (which is consumed) or at the first
 * non-option argument.
 *
 * On -h, a short usage is printed and the process exits 0.  On
 * --help, if @c desc->manual is non-NULL, print_manual() renders the
 * full man-style page; otherwise --help falls back to the short usage.
 *
 * On --ice-complete, all flags, subcommands and positional
 * completions are printed to stdout and the process exits 0.
 *
 * @param argc    Argument count.
 * @param argv    Argument vector (modified in-place).
 * @param desc    Command descriptor carrying @c opts, @c manual and
 *                (optionally) @c subcommands.
 * @return New argc (number of remaining arguments in argv).
 */
int parse_options(int argc, const char **argv, const struct cmd_desc *desc);

#endif /* OPTIONS_H */
