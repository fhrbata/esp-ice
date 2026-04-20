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

static void render_argh(const char *argh)
{
	/*
	 * argh with '<' or '[' is treated as a pre-formatted synopsis
	 * fragment (e.g. "<ref> [<name|path>]" or "[<name>]").  A bare
	 * word like "chip" gets wrapped in <> as usual.
	 */
	if (strchr(argh, '<') || strchr(argh, '['))
		fprintf(stderr, " %s", argh);
	else
		fprintf(stderr, " <%s>", argh);
}

static void print_usage(const char *argv0, const struct cmd_desc *desc)
{
	const struct option *opts = desc->opts;
	int has_flags = 0;
	int has_subcmds = 0;
	int has_positionals = 0;

	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		if (o->type == OPTION_POSITIONAL)
			has_positionals = 1;
		else
			has_flags = 1;
	}
	if (desc->subcommands && *desc->subcommands)
		has_subcmds = 1;

	fprintf(stderr, "@b{usage}: %s", argv0);
	if (has_flags)
		fprintf(stderr, " [<options>]");
	if (has_subcmds)
		fprintf(stderr, " <subcommand> [<args>]");
	else if (has_positionals) {
		for (const struct option *o = opts; o->type != OPTION_END; o++)
			if (o->type == OPTION_POSITIONAL && o->argh)
				render_argh(o->argh);
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

		if (o->type == OPTION_POSITIONAL)
			continue;

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
 *
 * Each candidate is printed as a line of the form
 *
 *     <name>[\t<description>]
 *
 * The TAB separator is consumed by the per-shell glue in
 * cmd/completion/completion.c: zsh feeds it to _describe, fish and
 * PowerShell read it natively, bash strips it via cut -f1 and keeps
 * name-only behaviour.  Candidates without a description (dynamic
 * values emitted by OPT_POSITIONAL callbacks, aliases, config values,
 * ...) stay as a bare name -- the TAB is optional.
 */
static void print_sorted(struct svec *v)
{
	svec_sort(v);
	for (size_t i = 0; i < v->nr; i++)
		printf("%s\n", v->v[i]);
	svec_clear(v);
}

/*
 * Honors the completion.descriptions config flag.  Defaults to on.
 * Cached so completion callbacks that emit many candidates in a loop
 * don't re-scan the config store on every candidate.
 */
static int completion_with_desc(void)
{
	static int cache = -1;

	if (cache < 0) {
		int enabled = 1;
		config_get_bool("completion.descriptions", &enabled);
		cache = enabled ? 1 : 0;
	}
	return cache;
}

/** Push a completion candidate with an optional one-line description. */
static void push_cand(struct svec *v, const char *name, const char *desc)
{
	if (desc && *desc && completion_with_desc())
		svec_pushf(v, "%s\t%s", name, desc);
	else
		svec_push(v, name);
}

/**
 * Emit a candidate line to stdout.  Public helper for completion
 * callbacks that print directly (as opposed to pushing into the
 * sorted svec used by print_completions).  Drops the description
 * when @c completion.descriptions is set to false.
 */
void complete_emit(const char *name, const char *desc)
{
	if (desc && *desc && completion_with_desc())
		printf("%s\t%s\n", name, desc);
	else
		printf("%s\n", name);
}

/**
 * Fire the completion callback for positional slot @p slot.
 *
 * Iterates OPT_POSITIONAL entries in declaration order and invokes the
 * matching slot's callback.  Returns 1 if a callback fired, 0 if no
 * slot matched (caller falls back to silent exit / file completion).
 */
static int fire_positional_complete(const struct option *opts, int slot)
{
	int idx = 0;

	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		if (o->type != OPTION_POSITIONAL)
			continue;
		if (idx == slot) {
			if (o->complete)
				o->complete();
			return 1;
		}
		idx++;
	}

	return 0;
}

static NORETURN void print_completions(const struct cmd_desc *desc)
{
	const struct option *opts = desc->opts;
	struct svec v = SVEC_INIT;
	char buf[64];

	/* Subcommands, sorted. */
	if (desc->subcommands) {
		for (const struct cmd_desc *const *p = desc->subcommands; *p;
		     p++) {
			if ((*p)->name[0] == '_')
				continue;
			push_cand(&v, (*p)->name,
				  (*p)->manual ? (*p)->manual->summary : NULL);
		}
	}
	print_sorted(&v);

	/*
	 * Positional completions: only fire when there are no subcommands
	 * at this level -- completion of a subcommand slot happens via the
	 * subcommand list above, not the leaf's positional callback.
	 */
	if (!desc->subcommands)
		fire_positional_complete(opts, 0);

	/*
	 * Namespace-level extra candidates (e.g. user aliases at the
	 * root) appear between subcommands and flags: they read as
	 * user-defined commands to the user, so they belong next to the
	 * real commands rather than after the flag listing.
	 */
	if (desc->extra_complete)
		desc->extra_complete();

	/* Long flags, sorted. */
	push_cand(&v, "--help", "show full manual");
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		if (o->long_opt) {
			snprintf(buf, sizeof(buf), "--%s", o->long_opt);
			push_cand(&v, buf, o->help);
		}
	}
	print_sorted(&v);

	/* Short flags, sorted. */
	push_cand(&v, "-h", "show short usage");
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		if (o->short_opt) {
			snprintf(buf, sizeof(buf), "-%c", o->short_opt);
			push_cand(&v, buf, o->help);
		}
	}
	print_sorted(&v);

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
			 * in argv, dispatch to the OPT_POSITIONAL slot at the
			 * cursor index (j - i positionals were typed before
			 * the cursor).  No matching slot -> silent exit so the
			 * shell falls back to file completion.  Without this
			 * guard --ice-complete would be packed as another
			 * positional and commands that accept one (e.g. ice
			 * repo checkout) would treat it as data.
			 */
			for (int j = i + 1; j < argc; j++) {
				if (!strcmp(argv[j], "--ice-complete")) {
					fire_positional_complete(opts, j - i);
					exit(0);
				}
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
