/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ice.c
 * @brief Top-level command entry point, global option table, and root manual.
 *
 * This file is part of libice.a so tests and any future external
 * libice consumer can reach cmd_ice() and friends without pulling
 * in the program's main() -- which lives in main.c.
 */
#include "ice.h"

/*
 * Try to expand argv[0] as an alias (config key alias.<name>).
 *
 * Value starting with '!' is a shell alias: passed straight to the
 * platform shell and the process exits with its status.
 *
 * Otherwise the value is split on whitespace; the tokens replace
 * argv[0], with the remaining args from argv[1..] appended.
 *
 * Returns 1 if expansion happened, 0 otherwise.
 */
static int try_expand_alias(int *argcp, const char ***argvp)
{
	static struct svec tokens = SVEC_INIT;
	struct sbuf key = SBUF_INIT;
	struct svec prev_args = SVEC_INIT;
	const char *value;
	char *copy;
	char *p;

	sbuf_addf(&key, "alias.%s", (*argvp)[0]);
	value = config_get(key.buf);
	sbuf_release(&key);

	if (!value)
		return 0;

	if (value[0] == '!') {
		int rc = run_shell(value + 1);

		exit(rc < 0 ? EXIT_FAILURE : rc);
	}

	for (int i = 1; i < *argcp; i++)
		svec_push(&prev_args, (*argvp)[i]);

	svec_clear(&tokens);

	copy = sbuf_strdup(value);
	p = copy;
	while (*p) {
		char *tok;

		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		tok = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (*p)
			*p++ = '\0';
		svec_push(&tokens, tok);
	}
	free(copy);

	for (size_t i = 0; i < prev_args.nr; i++)
		svec_push(&tokens, prev_args.v[i]);
	svec_clear(&prev_args);

	*argcp = (int)tokens.nr;
	*argvp = tokens.v;
	return 1;
}

/*
 * Return 1 if argv contains --help or -h before the first "--"
 * separator.  Used to skip setup_project() for help/usage requests
 * so @b{ice build --help} works outside a configured project.
 */
static int argv_wants_help(int argc, const char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--"))
			return 0;
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
			return 1;
	}
	return 0;
}

/* Dispatch: parse globals then fire the matched subcommand. */

/*
 * Written to by parse_options(); checked in main() (in main.c)
 * after parsing.  File-scope with external linkage so the option
 * table below has stable addresses to reference and main.c, cmake.c,
 * and other modules can read the post-parse values via extern
 * declarations in ice.h.
 */
int global_no_color;
int global_no_pager;
int global_version;
int global_verbose;
const char *global_profile;

static void complete_aliases(void)
{
	struct svec seen = SVEC_INIT;

	for (int i = 0; i < config.nr; i++) {
		const char *key = config.entries[i].key;
		const char *value = config.entries[i].value;
		const char *name;
		int dup = 0;

		if (strncmp(key, "alias.", 6) != 0 || !key[6])
			continue;
		name = key + 6;

		for (size_t j = 0; j < seen.nr; j++)
			if (!strcmp(seen.v[j], name)) {
				dup = 1;
				break;
			}
		if (!dup) {
			char desc[256];
			svec_push(&seen, name);
			if (value && *value) {
				snprintf(desc, sizeof(desc), "alias for '%s'",
					 value);
				complete_emit(name, desc);
			} else {
				complete_emit(name, NULL);
			}
		}
	}
	svec_clear(&seen);
}

/*
 * Walk a descriptor tree.
 *
 * For a namespace node (has @c subcommands), parse the node's options,
 * then recurse into the child whose @c .name matches argv[0].  If no
 * child matches and the namespace has a @c .fn, fire it.  Otherwise:
 * with no subcommand given, print the namespace manual (same shape as
 * bare @c ice); with an unknown subcommand, die.
 *
 * For a leaf node (no @c subcommands), do NOT call @c parse_options
 * here -- the leaf's own body calls it with the leaf's argv[0] still
 * serving as the prog-name slot, so the first positional survives.
 * Parsing at this level would shift argv[0] to the first positional
 * and the leaf's redundant @c parse_options call would then drop it.
 */
int ice_dispatch(int argc, const char **argv, const struct cmd_desc *desc)
{
	if (!desc->subcommands) {
		/*
		 * --help / -h short-circuits to the manual inside
		 * parse_options before the handler does any real work,
		 * so skip setup_project() for that path -- otherwise
		 * @b{ice build --help} would demand a configured project
		 * just to render its usage.
		 */
		if (!argv_wants_help(argc, argv))
			setup_project(desc->needs);
		return desc->fn(argc, argv);
	}

	argc = parse_options(argc, argv, desc);

	if (argc > 0) {
		for (const struct cmd_desc *const *p = desc->subcommands; *p;
		     p++) {
			if (!strcmp(argv[0], (*p)->name))
				return ice_dispatch(argc, argv, *p);
		}
	}

	if (desc->fn) {
		if (!argv_wants_help(argc, argv))
			setup_project(desc->needs);
		return desc->fn(argc, argv);
	}

	if (argc == 0) {
		print_manual(desc->manual->name, desc);
		return 1;
	}

	die("'%s' is not a valid %s subcommand. See '%s --help'.", argv[0],
	    desc->manual->name, desc->manual->name);
}

const struct option ice_global_opts[] = {
    OPT_BOOL(0, "no-color", &global_no_color, "disable colored output"),
    OPT_BOOL(0, "no-pager", &global_no_pager, "disable the pager"),
    OPT_BOOL_CFG('v', "verbose", &global_verbose, "core.verbose", NULL,
		 "show full command output", NULL),
    OPT_STRING_CFG(0, "profile", &global_profile, "name",
		   "project.default-profile", "ICE_PROFILE",
		   "select profile (default: \"default\")", NULL, NULL),
    OPT_BOOL(0, "version", &global_version, "show version"),
    OPT_END(),
};

static const struct cmd_desc *const ice_subs[] = {
    &cmd_build_desc,
    &cmd_clean_desc,
    &cmd_completion_desc,
    &cmd_config_desc,
    &cmd_flash_desc,
    &cmd_help_desc,
    &cmd_idf_desc,
    &cmd_monitor_desc,
    &cmd_image_desc,
    &cmd_init_desc,
    &cmd_log_desc,
    &cmd_menuconfig_desc,
    &cmd_repo_desc,
    &cmd_status_desc,
    &cmd_target_desc,
    &cmd_tools_desc,
    /* Hidden plumbing (names start with '_'); help/completion skip. */
    &cmd___complete_desc,
    &cmd___flash_desc,
    NULL,
};

const struct cmd_desc ice_root_desc = {
    .name = "ice",
    .opts = ice_global_opts,
    .manual = &ice_root_manual,
    .subcommands = ice_subs,
    .extra_complete = complete_aliases,
};

int cmd_ice(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &ice_root_desc);

	if (global_no_color)
		use_color = 0;

	if (global_version) {
		printf("%sice %s\n", use_vt ? "\xf0\x9f\xa7\x8a " : "",
		       VERSION);
		return 0;
	}

	if (argc < 1) {
		print_manual(ice_root_desc.name, &ice_root_desc);
		return 1;
	}

	/*
	 * Match the typed subcommand against the descriptor tree first.
	 * If no match, try alias expansion and look up the first token
	 * again (global flags inside alias values are intentionally not
	 * re-parsed).  Falling off the end means ice-<name> on $PATH.
	 */
	for (int tries = 0; tries < 2; tries++) {
		for (const struct cmd_desc *const *p = ice_subs; *p; p++) {
			if (!strcmp(argv[0], (*p)->name))
				return ice_dispatch(argc, argv, *p);
		}
		if (tries == 0) {
			int expanded = 0;
			for (int depth = 0; depth < 10; depth++) {
				if (!try_expand_alias(&argc, &argv))
					break;
				expanded = 1;
			}
			if (!expanded || argc < 1)
				break;
		}
	}

	/* Try external command: ice-<name> on PATH. */
	{
		struct sbuf cmd = SBUF_INIT;

		sbuf_addf(&cmd, "ice-%s", argv[0]);
		if (find_in_path(cmd.buf)) {
			const char **ext_argv;
			struct process proc = PROCESS_INIT;
			int rc;

			ext_argv = calloc((size_t)argc + 1, sizeof(*ext_argv));
			ext_argv[0] = cmd.buf;
			for (int i = 1; i < argc; i++)
				ext_argv[i] = argv[i];
			ext_argv[argc] = NULL;

			proc.argv = ext_argv;
			rc = process_run(&proc);

			free(ext_argv);
			sbuf_release(&cmd);
			return rc < 0 ? 1 : rc;
		}
		sbuf_release(&cmd);
	}

	die("'%s' is not a command. See 'ice --help'.", argv[0]);
}

/* clang-format off */
const struct cmd_manual ice_root_manual = {
	.name = "ice",
	.summary = "frontend for ESP-IDF projects",

	.description =
	H_PARA("@b{ice} drives the build, flash, configuration and size "
	       "tooling for ESP-IDF projects.  It replaces @b{idf.py} with "
	       "a single self-contained binary -- no Python, no "
	       "@b{export.sh}, no virtual environments."),

	.list_aliases = 1,

	.examples =
	H_EXAMPLE("ice repo clone && ice repo checkout v5.4")
	H_EXAMPLE("ice init esp32s3 v5.4")
	H_EXAMPLE("ice build")
	H_EXAMPLE("ice flash")
	H_EXAMPLE("ice help config"),

	.getting_started =
	H_PARA("Enable tab completion for your shell (see "
	       "@b{ice completion --help} for bash, zsh, fish, powershell):")
	H_LINE("@y{$} @b{eval \"$(ice completion bash)\"}          add to ~/.bashrc")
	H_RAW("")
	H_PARA("Set up the ice-managed ESP-IDF reference:")
	H_LINE("@y{$} @b{ice repo clone}                         clone ESP-IDF into ~/.ice/esp-idf")
	H_RAW("")
	H_PARA("Create a working checkout for a release:")
	H_LINE("@y{$} @b{ice repo checkout v5.4}                 creates ~/.ice/checkouts/v5.4")
	H_RAW("")
	H_PARA("Bind the project to a chip + IDF (installs tools, runs cmake):")
	H_LINE("@y{$} @b{ice init esp32s3 v5.4}                  default profile")
	H_LINE("@y{$} @b{ice init esp32 v5.4 production}         named profile")
	H_RAW("")
	H_PARA("Build and flash:")
	H_LINE("@y{$} @b{ice build}")
	H_LINE("@y{$} @b{ice flash}")
	H_RAW("")
	H_PARA("No @b{export.sh} or environment setup needed -- @b{ice} "
	       "finds the tools automatically."),

	.extras =
	H_SECTION("MANAGING ESP-IDF VERSIONS")
	H_PARA("List available versions, create more checkouts, or refresh the reference:")
	H_LINE("@y{$} @b{ice repo list}                          show available branches and tags")
	H_LINE("@y{$} @b{ice repo checkout release/v5.2 v5.2}   add a second checkout")
	H_LINE("@y{$} @b{ice repo checkout --list}               list existing checkouts")
	H_LINE("@y{$} @b{ice repo pull}                          refresh the reference to latest master")
	H_LINE("@y{$} @b{ice repo info}                          show reference and checkout status")

	H_SECTION("SEE ALSO")
	H_ITEM("ice help <command>",
	       "Show the manual page for a specific command.")
	H_ITEM("ice config --help",
	       "How configuration entries and scopes work."),
};
/* clang-format on */
