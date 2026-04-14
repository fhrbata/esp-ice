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

/* clang-format off */
static const struct cmd_manual manual = {
	.description =
	H_PARA("@b{ice config} reads and writes configuration entries "
	       "across a stack of cascading scopes.  The effective value "
	       "of a key is resolved in precedence order: @b{cli > env > "
	       "project > local > user > defaults}.")
	H_PARA("With no flags: one positional argument prints the "
	       "effective value of that key (exits non-zero if unset), "
	       "and two positional arguments set the key in the target "
	       "scope.  The target scope for writes is @b{--local} "
	       "(./.iceconfig) by default; pass @b{--user} to write "
	       "~/.iceconfig.")
	H_PARA("@b{--list} dumps every entry in the active configuration "
	       "together with the scope it came from.  @b{--add} appends "
	       "an entry for keys with multi-value semantics "
	       "(e.g. @b{cmake.define}).  @b{--unset} removes every "
	       "entry for a key at the target scope.  These three modes "
	       "are mutually exclusive."),

	.examples =
	H_EXAMPLE("ice config --list")
	H_EXAMPLE("ice config core.build-dir")
	H_EXAMPLE("ice config core.build-dir out")
	H_EXAMPLE("ice config --user alias.b \"build -v\"")
	H_EXAMPLE("ice config --add cmake.define MY_OPT=ON")
	H_EXAMPLE("ice config --unset alias.b"),

	.extras =
	H_SECTION("SCOPES")
	H_ITEM("cli",
	       "Flags and @b{-D} entries passed on the command line "
	       "(highest precedence).")
	H_ITEM("env",
	       "Variables named @b{ICE_<SECTION>_<KEY>}, e.g. "
	       "@b{ICE_CORE_BUILD_DIR} or @b{ICE_SERIAL_PORT}.  Legacy "
	       "@b{ESPPORT} and @b{ESPBAUD} are also mapped to "
	       "@b{serial.port} and @b{serial.baud} for idf.py "
	       "compatibility.")
	H_ITEM("project",
	       "Auto-derived from build artifacts in @b{<build-dir>} -- "
	       "@b{target} from CMakeCache.txt, @b{mapfile} / @b{elf} "
	       "from project_description.json.  Best-effort; silently "
	       "skipped when the build tree is not yet configured.")
	H_ITEM("local",
	       "@b{./.iceconfig} in the current working directory.")
	H_ITEM("user",
	       "@b{~/.iceconfig} in the user's home directory.")
	H_ITEM("defaults",
	       "Built-in fallbacks (@b{core.build-dir=build}, "
	       "@b{core.generator=Ninja}, @b{core.verbose=false}).")

	H_SECTION("FILES")
	H_ITEM("./.iceconfig",
	       "Local project configuration (--local scope).")
	H_ITEM("~/.iceconfig",
	       "User configuration (--user scope).")

	H_SECTION("FILE FORMAT")
	H_PARA("Config files are plain text in an INI-like format.  "
	       "Sections introduce the key namespace; keys inside a "
	       "@b{[section]} header are stored as @b{section.key}.")
	H_LINE("")
	H_LINE("    @b{[core]}")
	H_LINE("    build-dir = build")
	H_LINE("    generator = Ninja")
	H_LINE("    verbose = false")
	H_LINE("")
	H_LINE("    @b{[cmake]}")
	H_LINE("    define = MY_OPT=ON")
	H_LINE("    define = ANOTHER=1")
	H_LINE("")
	H_LINE("    @b{[alias]}")
	H_LINE("    b = build -v")
	H_LINE("    ll = !ls -la")
	H_LINE("")
	H_PARA("Rules:")
	H_LINE("  - Section names and keys allow @b{[A-Za-z0-9_-]}.")
	H_LINE("  - Values are trimmed of surrounding whitespace; wrap "
	       "in double")
	H_LINE("    quotes to preserve leading/trailing spaces or "
	       "embedded @b{#} / @b{;}.")
	H_LINE("  - Lines starting with @b{#} or @b{;} are comments.")
	H_LINE("  - Blank lines are ignored.")
	H_LINE("  - Multi-value keys (e.g. @b{cmake.define}): use "
	       "@b{--add} to append;")
	H_LINE("    direct assignment replaces every entry at that "
	       "scope.")
	H_LINE("  - Files are rewritten whole on every write; existing "
	       "comments")
	H_LINE("    and blank lines are @b{not} preserved.")
	H_LINE("")

	H_SECTION("ALIASES")
	H_PARA("Aliases live under the @b{alias.<name>} config key.  "
	       "When @b{<name>} is typed as the subcommand, the alias "
	       "value replaces it and parsing continues from the "
	       "expanded argv; subsequent arguments on the original "
	       "command line are preserved after the expansion.")
	H_PARA("A command alias (the common case) is a shell-like "
	       "string that is split on whitespace into replacement "
	       "tokens.  A @b{shell alias} is a value that begins with "
	       "@b{!} -- everything after the bang is passed to "
	       "@b{/bin/sh -c} (or @b{cmd.exe /c} on Windows) and @b{ice} "
	       "exits with that command's status.  Global options are "
	       "not re-parsed through aliases.")
	H_PARA("Alias expansion loops (with a cycle-breaking depth cap) "
	       "so one alias may resolve to another.")
	H_EXAMPLE("ice config --user alias.b \"build -v\"")
	H_EXAMPLE("ice b                    # runs: ice build -v")
	H_EXAMPLE("ice config alias.ll \"!ls -la\"")
	H_EXAMPLE("ice ll                   # runs: /bin/sh -c 'ls -la'")
	H_LINE(""),
};
/* clang-format on */

static enum config_scope target_scope(int user, int local)
{
	if (user && local)
		die("cannot combine --user and --local");
	return user ? CONFIG_SCOPE_USER : CONFIG_SCOPE_LOCAL;
}

static const char *scope_path(enum config_scope scope)
{
	switch (scope) {
	case CONFIG_SCOPE_USER:
		return user_config_path();
	case CONFIG_SCOPE_LOCAL:
		return local_config_path();
	default:
		return NULL;
	}
}

static int do_list(void)
{
	for (int i = 0; i < config.nr; i++)
		printf("%-8s %s=%s\n", scope_name(config.entries[i].scope),
		       config.entries[i].key, config.entries[i].value);
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
	    OPT_BOOL('l', "list", &list, "list entries with scope"),
	    OPT_BOOL(0, "add", &add, "append a value (multi-value keys)"),
	    OPT_BOOL(0, "unset", &unset, "remove all entries for a key"),
	    OPT_BOOL(0, "user", &user, "act on the user config (~/.iceconfig)"),
	    OPT_BOOL(0, "local", &local,
		     "act on the local config (./.iceconfig) [default]"),
	    OPT_END(),
	};

	argc = parse_options_manual(argc, argv, opts, usage, &manual);

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
