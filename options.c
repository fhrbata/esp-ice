/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file options.c
 * @brief Declarative command-line option parser implementation.
 */
#include <limits.h>

#include "ice.h"

static int is_bool_opt(enum option_type t) { return t == OPTION_BOOL; }

static int parse_int_token(const char *s, int *out)
{
	char *end;
	long v;

	if (!s || !*s)
		return -1;
	errno = 0;
	v = strtol(s, &end, 10);
	if (*end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX)
		return -2;
	*out = (int)v;
	return 0;
}

/*
 * Apply @p val to @p o as if it had been passed on the command line,
 * but without the strictness of CLI parsing: malformed bool/int values
 * from a seed source (config/env) are silently ignored so a stray value
 * doesn't cause every ice invocation to die.  STRING stores the pointer
 * as-is -- callers must ensure it outlives the parse (config entries
 * and getenv() values both do).
 */
static void seed_apply(const struct option *o, const char *val)
{
	if (!val || !*val)
		return;
	switch (o->type) {
	case OPTION_BOOL: {
		int b;
		if (config_parse_bool(val, &b) == 0)
			*(int *)o->value = b;
		break;
	}
	case OPTION_STRING:
		*(const char **)o->value = val;
		break;
	case OPTION_STRING_LIST:
		svec_push((struct svec *)o->value, val);
		break;
	case OPTION_INT: {
		int n;
		if (parse_int_token(val, &n) == 0)
			*(int *)o->value = n;
		break;
	}
	default:
		break;
	}
}

static void seed_from_config(const struct option *o)
{
	if (o->type == OPTION_STRING_LIST) {
		struct config_entry **entries;
		int n;

		n = config_get_all(o->config_key, &entries);
		for (int i = 0; i < n; i++)
			svec_push((struct svec *)o->value, entries[i]->value);
		free(entries);
		return;
	}
	seed_apply(o, config_get(o->config_key));
}

static void seed_defaults(const struct option *opts)
{
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		if (o->config_key)
			seed_from_config(o);
		if (o->env_var)
			seed_apply(o, getenv(o->env_var));
	}
}

static void print_usage(const char *argv0, const struct cmd_desc *desc)
{
	const struct option *opts = desc->opts;
	int has_flags = 0;
	int has_subcmds = 0;
	const char *positional = NULL;

	for (const struct option *o = opts; o->type != OPTION_END; o++)
		has_flags = 1;
	if (desc->subcommands && *desc->subcommands)
		has_subcmds = 1;

	/* OPT_END's argh names the positional argument. */
	{
		const struct option *end = opts;
		while (end->type != OPTION_END)
			end++;
		positional = end->argh;
	}

	fprintf(stderr, "@b{usage}: %s", argv0);
	if (has_flags)
		fprintf(stderr, " [<options>]");
	if (has_subcmds)
		fprintf(stderr, " <subcommand> [<args>]");
	else if (positional) {
		/*
		 * argh with '<' or '[' is treated as a pre-formatted
		 * synopsis fragment so multi-positional commands can
		 * render e.g. "<ref> [<name|path>]".  A bare word gets
		 * wrapped in <> as usual.
		 */
		if (strchr(positional, '<') || strchr(positional, '['))
			fprintf(stderr, " %s", positional);
		else
			fprintf(stderr, " <%s>", positional);
	}
	fprintf(stderr, "\n\n");

	/* Print subcommands first, then options. */
	has_subcmds = 0;
	if (desc->subcommands) {
		for (const struct cmd_desc *const *p = desc->subcommands; *p;
		     p++) {
			if ((*p)->name[0] == '_')
				continue;
			if (!has_subcmds) {
				fprintf(stderr, "Subcommands:\n");
				has_subcmds = 1;
			}
			fprintf(stderr, "    @b{%-20s} %s\n", (*p)->name,
				(*p)->manual && (*p)->manual->summary
				    ? (*p)->manual->summary
				    : "");
		}
	}

	int has_opts = 0;
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		char short_str[8] = "";
		char long_str[64] = "";

		if (!has_opts) {
			if (has_subcmds)
				fprintf(stderr, "\n");
			has_opts = 1;
		}

		if (o->short_opt)
			snprintf(short_str, sizeof(short_str), "-%c",
				 o->short_opt);

		if (o->long_opt) {
			if (is_bool_opt(o->type))
				snprintf(long_str, sizeof(long_str), "--%s",
					 o->long_opt);
			else
				snprintf(long_str, sizeof(long_str),
					 "--%s=<%s>", o->long_opt,
					 o->argh ? o->argh : "...");
		}

		if (o->short_opt && o->long_opt)
			fprintf(stderr, "    @b{%s}, @b{%-20s} %s\n", short_str,
				long_str, o->help ? o->help : "");
		else if (o->short_opt)
			fprintf(stderr, "    @b{%-24s} %s\n", short_str,
				o->help ? o->help : "");
		else
			fprintf(stderr, "        @b{%-20s} %s\n", long_str,
				o->help ? o->help : "");
	}
}

/**
 * Print all completion candidates for this option table and exit.
 * Called when --ice-complete is encountered as a standalone argument.
 */
static void print_sorted(struct svec *v)
{
	svec_sort(v);
	for (size_t i = 0; i < v->nr; i++)
		printf("%s\n", v->v[i]);
	svec_clear(v);
}

static NORETURN void print_completions(const struct cmd_desc *desc)
{
	const struct option *opts = desc->opts;
	const struct option *end = opts;
	struct svec v = SVEC_INIT;
	char buf[64];

	while (end->type != OPTION_END)
		end++;

	/* Subcommands, sorted. */
	if (desc->subcommands) {
		for (const struct cmd_desc *const *p = desc->subcommands; *p;
		     p++) {
			if ((*p)->name[0] != '_')
				svec_push(&v, (*p)->name);
		}
	}
	print_sorted(&v);

	/*
	 * Positional completions: only fire when there are no subcommands
	 * at this level -- completion of a subcommand slot happens via the
	 * subcommand list above, not the leaf's positional callback.
	 */
	if (!desc->subcommands && end->complete)
		end->complete();

	/* Long flags, sorted. */
	svec_push(&v, "--help");
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		if (o->long_opt) {
			snprintf(buf, sizeof(buf), "--%s", o->long_opt);
			svec_push(&v, buf);
		}
	}
	print_sorted(&v);

	/* Short flags, sorted. */
	svec_push(&v, "-h");
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		if (o->short_opt) {
			snprintf(buf, sizeof(buf), "-%c", o->short_opt);
			svec_push(&v, buf);
		}
	}
	print_sorted(&v);

	/*
	 * OPT_END_COMPLETE at the root (or any non-leaf namespace) can
	 * still attach a callback that supplies extra candidates -- today
	 * @c ice_global_opts uses it for alias completion.  Fire it after
	 * the flag lists so it slots in alongside them.
	 */
	if (desc->subcommands && end->complete)
		end->complete();

	exit(0);
}

static const struct option *find_short(const struct option *opts, int c)
{
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		if (o->short_opt == c)
			return o;
	}
	return NULL;
}

static const struct option *find_long(const struct option *opts,
				      const char *name, const char **valp)
{
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		if (!o->long_opt)
			continue;

		size_t len = strlen(o->long_opt);

		if (!strncmp(name, o->long_opt, len)) {
			if (name[len] == '\0') {
				*valp = NULL;
				return o;
			}
			if (name[len] == '=') {
				*valp = name + len + 1;
				return o;
			}
		}
	}
	return NULL;
}

static int set_value(const struct option *o, const char *val)
{
	switch (o->type) {
	case OPTION_BOOL:
		*(int *)o->value = 1;
		return 0;
	case OPTION_STRING:
		*(const char **)o->value = val;
		return 0;
	case OPTION_STRING_LIST:
		svec_push((struct svec *)o->value, val);
		return 0;
	case OPTION_INT:
		*(int *)o->value = atoi(val);
		return 0;
	default:
		return -1;
	}
}

int parse_options(int argc, const char **argv, const struct cmd_desc *desc)
{
	const struct option *opts = desc->opts;
	const struct cmd_manual *manual = desc->manual;
	const char *name = (manual && manual->name) ? manual->name : argv[0];
	int out = 0;
	int i;

	seed_defaults(opts);

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		/* Stop at "--" */
		if (!strcmp(arg, "--")) {
			i++;
			break;
		}

		/* Not an option -- stop here and let the caller dispatch. */
		if (arg[0] != '-' || arg[1] == '\0') {
			if (desc->subcommands) {
				for (const struct cmd_desc *const *p =
					 desc->subcommands;
				     *p; p++) {
					if (!strcmp(arg, (*p)->name))
						goto done;
				}
			}
			/*
			 * Positional with no subcommand match.  If the
			 * completion trampoline appended --ice-complete later
			 * in argv, exit silently so the shell falls back to
			 * file completion (sensible for a <name|path> slot).
			 * Without this guard --ice-complete would be packed
			 * as another positional and commands that accept one
			 * (e.g. ice idf checkout) would treat it as data.
			 */
			for (int j = i + 1; j < argc; j++) {
				if (!strcmp(argv[j], "--ice-complete"))
					exit(0);
			}
			break;
		}

		/* -h: short usage always; --help: full manual if provided. */
		if (!strcmp(arg, "-h")) {
			print_usage(name, desc);
			exit(0);
		}
		if (!strcmp(arg, "--help")) {
			if (manual)
				print_manual(name, desc);
			else
				print_usage(name, desc);
			exit(0);
		}

		/* --ice-complete: dump all candidates and exit. */
		if (!strcmp(arg, "--ice-complete"))
			print_completions(desc);

		/* Long option */
		if (arg[1] == '-') {
			const char *val = NULL;
			const struct option *o = find_long(opts, arg + 2, &val);

			if (!o)
				die("unknown option: %s", arg);

			if (!is_bool_opt(o->type) && !val) {
				if (i + 1 >= argc)
					die("option '%s' requires a value",
					    arg);
				/* Complete value: next arg is --ice-complete.
				 */
				if (!strcmp(argv[i + 1], "--ice-complete")) {
					if (o->complete)
						o->complete();
					exit(0);
				}
				val = argv[++i];
			}

			set_value(o, val);
			continue;
		}

		/* Short option */
		const struct option *o = find_short(opts, arg[1]);

		if (!o)
			die("unknown option: %s", arg);

		if (is_bool_opt(o->type)) {
			set_value(o, NULL);
			continue;
		}

		/* Value: attached (-Cdir) or next arg (-C dir) */
		const char *val = arg[2] ? arg + 2 : NULL;

		if (!val) {
			if (i + 1 >= argc)
				die("option '%s' requires a value", arg);
			/* Complete value: next arg is --ice-complete. */
			if (!strcmp(argv[i + 1], "--ice-complete")) {
				if (o->complete)
					o->complete();
				exit(0);
			}
			val = argv[++i];
		}

		set_value(o, val);
	}

done:
	/* Pack remaining args to the front. */
	for (; i < argc; i++)
		argv[out++] = argv[i];
	argv[out] = NULL;

	return out;
}
