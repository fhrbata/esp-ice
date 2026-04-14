/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file options.c
 * @brief Declarative command-line option parser implementation.
 */
#include "ice.h"

static int is_bool_opt(enum option_type t)
{
	return t == OPTION_BOOL || t == OPTION_CONFIG_BOOL;
}

static void print_usage(const struct option *opts, const char **usage)
{
	fprintf(stderr, "@b{usage}: ");
	for (int i = 0; usage[i]; i++) {
		if (i > 0)
			fprintf(stderr, "   @b{or}: ");
		fprintf(stderr, "%s\n", usage[i]);
	}

	fprintf(stderr, "\n");

	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		char short_str[8] = "";
		char long_str[64] = "";

		if (o->short_opt)
			snprintf(short_str, sizeof(short_str), "-%c",
				 o->short_opt);

		if (o->long_opt) {
			if (is_bool_opt(o->type))
				snprintf(long_str, sizeof(long_str), "--%s",
					 o->long_opt);
			else
				snprintf(long_str, sizeof(long_str), "--%s=<%s>",
					 o->long_opt,
					 o->argh ? o->argh : "...");
		}

		if (o->short_opt && o->long_opt)
			fprintf(stderr, "    @b{%s}, @b{%-20s} %s\n",
				short_str, long_str,
				o->help ? o->help : "");
		else if (o->short_opt)
			fprintf(stderr, "    @b{%-24s} %s\n", short_str,
				o->help ? o->help : "");
		else
			fprintf(stderr, "        @b{%-20s} %s\n", long_str,
				o->help ? o->help : "");
	}
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
	case OPTION_CONFIG:
		config_set(&config, (const char *)o->value, val,
			   CONFIG_SCOPE_CLI);
		return 0;
	case OPTION_CONFIG_BOOL:
		config_set(&config, (const char *)o->value,
			   val ? val : "true", CONFIG_SCOPE_CLI);
		return 0;
	case OPTION_CONFIG_LIST:
		config_add(&config, (const char *)o->value, val,
			   CONFIG_SCOPE_CLI);
		return 0;
	default:
		return -1;
	}
}

int parse_options_manual(int argc, const char **argv,
			 const struct option *opts, const char **usage,
			 const struct cmd_manual *manual)
{
	int out = 0;
	int i;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		/* Stop at "--" */
		if (!strcmp(arg, "--")) {
			i++;
			break;
		}

		/* Not an option -- stop parsing */
		if (arg[0] != '-' || arg[1] == '\0')
			break;

		/* -h: short usage always; --help: full manual if provided. */
		if (!strcmp(arg, "-h")) {
			print_usage(opts, usage);
			exit(0);
		}
		if (!strcmp(arg, "--help")) {
			if (manual)
				print_manual(argv[0], manual, opts, usage);
			else
				print_usage(opts, usage);
			exit(0);
		}

		/* Long option */
		if (arg[1] == '-') {
			const char *val = NULL;
			const struct option *o =
				find_long(opts, arg + 2, &val);

			if (!o)
				die("unknown option: %s", arg);

			if (!is_bool_opt(o->type) && !val) {
				if (i + 1 >= argc)
					die("option '%s' requires a value",
					    arg);
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
			val = argv[++i];
		}

		set_value(o, val);
	}

	/* Pack remaining args to the front. */
	for (; i < argc; i++)
		argv[out++] = argv[i];
	argv[out] = NULL;

	return out;
}

int parse_options(int argc, const char **argv, const struct option *opts,
		  const char **usage)
{
	return parse_options_manual(argc, argv, opts, usage, NULL);
}
