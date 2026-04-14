/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/config/config.c
 * @brief The "ice config" subcommand -- inspect and modify config files.
 *
 * Modes:
 *   ice config --list               list every entry with its scope
 *   ice config <key>                print the effective value of <key>
 *   ice config <key> <value>        set <key> in the target scope
 *   ice config --add <key> <value>  append (multi-value semantics)
 *   ice config --unset <key>        remove every entry for <key>
 *
 * Target scope for writes: --local (./.iceconfig, default) or --user
 * (~/.iceconfig).  Config files are whole-rewritten on every write;
 * comments and blank lines in the existing file are lost.
 */
#include "../../ice.h"

static const char *usage[] = {
	"ice config [--list]",
	"ice config [--user | --local] <key>",
	"ice config [--user | --local] <key> <value>",
	"ice config [--user | --local] --add <key> <value>",
	"ice config [--user | --local] --unset <key>",
	NULL,
};

static enum config_scope target_scope(int user, int local)
{
	if (user && local)
		die("cannot combine --user and --local");
	return user ? CONFIG_SCOPE_USER : CONFIG_SCOPE_LOCAL;
}

static const char *scope_path(enum config_scope scope)
{
	switch (scope) {
	case CONFIG_SCOPE_USER:		return user_config_path();
	case CONFIG_SCOPE_LOCAL:	return local_config_path();
	default:			return NULL;
	}
}

static int do_list(void)
{
	for (int i = 0; i < config.nr; i++)
		printf("%-8s %s=%s\n",
		       scope_name(config.entries[i].scope),
		       config.entries[i].key,
		       config.entries[i].value);
	return EXIT_SUCCESS;
}

static int do_get(const char *key)
{
	const char *value = config_get(key);

	if (!value)
		return EXIT_FAILURE;

	printf("%s\n", value);
	return EXIT_SUCCESS;
}

static void load_target(struct config *c, enum config_scope scope,
			const char **path_out)
{
	const char *path = scope_path(scope);

	if (!path)
		die("cannot locate config file for scope '%s'",
		    scope_name(scope));

	config_load_file(c, scope, path);
	*path_out = path;
}

static int do_set(enum config_scope scope, const char *key, const char *value)
{
	struct config c = CONFIG_INIT;
	const char *path;

	load_target(&c, scope, &path);
	config_set(&c, key, value, scope);
	if (config_write_file(&c, scope, path))
		die_errno("cannot write '%s'", path);
	config_release(&c);
	return EXIT_SUCCESS;
}

static int do_add(enum config_scope scope, const char *key, const char *value)
{
	struct config c = CONFIG_INIT;
	const char *path;

	load_target(&c, scope, &path);
	config_add(&c, key, value, scope);
	if (config_write_file(&c, scope, path))
		die_errno("cannot write '%s'", path);
	config_release(&c);
	return EXIT_SUCCESS;
}

static int do_unset(enum config_scope scope, const char *key)
{
	struct config c = CONFIG_INIT;
	const char *path;
	int removed;

	load_target(&c, scope, &path);
	removed = config_unset(&c, key, scope);
	if (!removed) {
		config_release(&c);
		die("'%s' is not set at scope '%s'", key, scope_name(scope));
	}
	if (config_write_file(&c, scope, path))
		die_errno("cannot write '%s'", path);
	config_release(&c);
	return EXIT_SUCCESS;
}

int cmd_config(int argc, const char **argv)
{
	int list = 0, add = 0, unset = 0;
	int user = 0, local = 0;
	int modes;
	enum config_scope scope;

	struct option opts[] = {
		OPT_BOOL('l', "list",  &list,  "list entries with scope"),
		OPT_BOOL(0,   "add",   &add,
			 "append a value (multi-value keys)"),
		OPT_BOOL(0,   "unset", &unset, "remove all entries for a key"),
		OPT_BOOL(0,   "user",  &user,
			 "act on the user config (~/.iceconfig)"),
		OPT_BOOL(0,   "local", &local,
			 "act on the local config (./.iceconfig) [default]"),
		OPT_END(),
	};

	argc = parse_options(argc, argv, opts, usage);

	modes = list + add + unset;
	if (modes > 1)
		die("cannot combine --list, --add, --unset");

	if (list) {
		if (argc > 0)
			die("--list takes no positional arguments");
		return do_list();
	}

	scope = target_scope(user, local);

	if (unset) {
		if (argc != 1)
			die("--unset takes <key>");
		return do_unset(scope, argv[0]);
	}

	if (add) {
		if (argc != 2)
			die("--add takes <key> <value>");
		return do_add(scope, argv[0], argv[1]);
	}

	if (argc == 1)
		return do_get(argv[0]);

	if (argc == 2)
		return do_set(scope, argv[0], argv[1]);

	die("usage: see 'ice config --help'");
}
