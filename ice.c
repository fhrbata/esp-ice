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
 * Run a shell alias ("!<cmd>") via /bin/sh -c (POSIX) or cmd.exe /c
 * (Windows) and exit with its status.  Never returns.
 */
static NORETURN void run_shell_alias(const char *cmd)
{
	const char *sh_argv[4];
	struct process proc = PROCESS_INIT;
	int rc;

#ifdef _WIN32
	sh_argv[0] = "cmd.exe";
	sh_argv[1] = "/c";
#else
	sh_argv[0] = "/bin/sh";
	sh_argv[1] = "-c";
#endif
	sh_argv[2] = cmd;
	sh_argv[3] = NULL;

	proc.argv = sh_argv;
	rc = process_run(&proc);
	exit(rc < 0 ? EXIT_FAILURE : rc);
}

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

	if (value[0] == '!')
		run_shell_alias(value + 1);

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

/* Dispatch: parse globals then fire the matched subcommand. */

/*
 * Written to by parse_options(); checked in main() (in main.c)
 * after parsing.  File-scope with external linkage so the option
 * table below has stable addresses to reference and main.c, cmake.c,
 * and other modules can read the post-parse values via extern
 * declarations in ice.h.
 */
int global_no_color;
int global_version;
int global_verbose;
const char *global_build_dir;
const char *global_generator;
struct svec global_defines = SVEC_INIT;

static void complete_aliases(void)
{
	struct svec seen = SVEC_INIT;

	for (int i = 0; i < config.nr; i++) {
		const char *key = config.entries[i].key;
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
			svec_push(&seen, name);
			printf("%s\n", name);
		}
	}
	svec_clear(&seen);
}

static subcmd_fn ice_fn;

const struct option ice_global_opts[] = {
    OPT_STRING_CFG('B', "build-dir", &global_build_dir, "path",
		   "core.build-dir", NULL, "build directory", NULL, NULL),
    OPT_STRING_LIST_CFG('D', "define", &global_defines, "key=val",
			"cmake.define", NULL, "cmake cache entry (repeatable)",
			"Extra cmake cache entries forwarded as "
			"@b{-D<key>=<value>} to every cmake invocation.",
			NULL),
    OPT_STRING_CFG('G', "generator", &global_generator, "name",
		   "core.generator", NULL, "cmake generator", NULL, NULL),
    OPT_BOOL(0, "no-color", &global_no_color, "disable colored output"),
    OPT_BOOL_CFG('v', "verbose", &global_verbose, "core.verbose", NULL,
		 "show full command output", NULL),
    OPT_BOOL(0, "version", &global_version, "show version"),

    OPT_SUBCOMMAND("build", &ice_fn, cmd_build, "build the default target"),
    OPT_SUBCOMMAND("clean", &ice_fn, cmd_clean, "remove build artifacts"),
    OPT_SUBCOMMAND("cmake", &ice_fn, cmd_cmake,
		   "run an arbitrary cmake target"),
    OPT_SUBCOMMAND("completion", &ice_fn, cmd_completion,
		   "print shell completion script"),
    OPT_SUBCOMMAND("config", &ice_fn, cmd_config,
		   "inspect and modify configuration entries"),
    OPT_SUBCOMMAND("configdep", &ice_fn, cmd_configdep,
		   "sdkconfig-aware compiler wrapper"),
    OPT_SUBCOMMAND("flash", &ice_fn, cmd_flash, "flash firmware to the device"),
    OPT_SUBCOMMAND("fullclean", &ice_fn, cmd_fullclean,
		   "wipe the build directory"),
    OPT_SUBCOMMAND("help", &ice_fn, cmd_help, "show help for a subcommand"),
    OPT_SUBCOMMAND("idf", &ice_fn, cmd_idf, "manage the ESP-IDF source tree"),
    OPT_SUBCOMMAND("image", &ice_fn, cmd_image, "host-only image manipulation"),
    OPT_SUBCOMMAND("ldgen", &ice_fn, cmd_ldgen,
		   "analyse linker fragment (.lf) files"),
    OPT_SUBCOMMAND("menuconfig", &ice_fn, cmd_menuconfig,
		   "open the project configuration UI"),
    OPT_SUBCOMMAND("monitor", &ice_fn, cmd_monitor,
		   "display serial output from the device"),
    OPT_SUBCOMMAND("partition-table", &ice_fn, cmd_partition_table,
		   "generate partition table binary from CSV"),
    OPT_SUBCOMMAND("reconfigure", &ice_fn, cmd_reconfigure,
		   "regenerate the build system"),
    OPT_SUBCOMMAND("size", &ice_fn, cmd_size,
		   "analyse firmware memory usage by region"),
    OPT_SUBCOMMAND("target", &ice_fn, cmd_target, "manage the chip target"),
    OPT_SUBCOMMAND("tools", &ice_fn, cmd_tools, "manage ESP-IDF toolchains"),
    OPT_SUBCOMMAND("__complete", &ice_fn, cmd_complete,
		   "shell completion backend (internal)"),
    OPT_END_COMPLETE(NULL, complete_aliases),
};

int cmd_ice(int argc, const char **argv)
{
	argc = parse_options(argc, argv, ice_global_opts, &ice_root_manual);

	if (global_no_color)
		use_color = 0;

	if (global_version) {
		printf("%sice %s\n", use_vt ? "\xf0\x9f\xa7\x8a " : "",
		       VERSION);
		return 0;
	}

	if (ice_fn)
		return ice_fn(argc, argv);

	if (argc < 1) {
		print_manual(ice_root_manual.name, &ice_root_manual,
			     ice_global_opts);
		return 1;
	}

	/*
	 * Unrecognised command -- try alias expansion.  On success the
	 * first token is replaced with the alias value, and we look up
	 * the resulting subcommand name directly.  Global flags inside
	 * alias values are intentionally NOT re-parsed.
	 */
	for (int depth = 0; depth < 10; depth++) {
		if (!try_expand_alias(&argc, &argv))
			break;
	}
	if (argc >= 1) {
		for (const struct option *o = ice_global_opts;
		     o->type != OPTION_END; o++) {
			if (o->type == OPTION_SUBCOMMAND &&
			    !strcmp(argv[0], o->long_opt))
				return o->subcommand_fn(argc, argv);
		}
	}

	/* Try external command: ice-<name> on PATH. */
	if (argc >= 1) {
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

	die("'%s' is not a command. See 'ice --help'.", argc ? argv[0] : "");
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

	.list_commands = 1,
	.list_aliases  = 1,

	.examples =
	H_EXAMPLE("ice idf clone && ice idf checkout v5.4")
	H_EXAMPLE("ice target set esp32s3")
	H_EXAMPLE("ice build")
	H_EXAMPLE("ice flash")
	H_EXAMPLE("ice help config"),

	.extras =
	H_SECTION("GETTING STARTED")
	H_PARA("Set up the ice-managed ESP-IDF reference:")
	H_LINE("@y{$} @b{ice idf clone}                         clone ESP-IDF into ~/.ice/esp-idf")
	H_RAW("")
	H_PARA("Create a working checkout for a release and point ice at it:")
	H_LINE("@y{$} @b{ice idf checkout v5.4}                 creates ~/.ice/checkouts/v5.4")
	H_LINE("@y{$} @b{ice config idf.path ~/.ice/checkouts/v5.4}")
	H_RAW("")
	H_PARA("Pick a target and install build tools:")
	H_LINE("@y{$} @b{ice target set esp32s3}                set the chip target")
	H_LINE("@y{$} @b{ice tools install}                     install build tools")
	H_RAW("")
	H_PARA("Build and flash:")
	H_LINE("@y{$} @b{ice build}")
	H_LINE("@y{$} @b{ice flash}")
	H_RAW("")
	H_PARA("No @b{export.sh} or environment setup needed -- @b{ice} "
	       "finds the tools automatically.")

	H_SECTION("MANAGING ESP-IDF VERSIONS")
	H_PARA("List available versions, create more checkouts, or refresh the reference:")
	H_LINE("@y{$} @b{ice idf list}                          show available branches and tags")
	H_LINE("@y{$} @b{ice idf checkout release/v5.2 v5.2}   add a second checkout")
	H_LINE("@y{$} @b{ice idf checkout --list}               list existing checkouts")
	H_LINE("@y{$} @b{ice idf pull}                          refresh the reference to latest master")
	H_LINE("@y{$} @b{ice idf info}                          show reference and checkout status")

	H_SECTION("SEE ALSO")
	H_ITEM("ice help <command>",
	       "Show the manual page for a specific command.")
	H_ITEM("ice config --help",
	       "How configuration entries and scopes work."),
};
/* clang-format on */
