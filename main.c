/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file main.c
 * @brief Program entry point for the `ice` binary.
 *
 * This file is the only source NOT included in libice.a so that unit
 * tests (and any future external libice consumer) can supply their
 * own main().  The dispatch table, global option table and root
 * manual live in ice.c, which is part of libice.a; main() here drives
 * the startup sequence and dispatch.
 */
#include "ice.h"

/*
 * Defined at file scope (non-static) in ice.c because ice_global_opts
 * stores their addresses.  main() reads them after parse_options()
 * returns.
 */
extern int global_no_color;
extern int global_version;

/*
 * Tiny hand-rolled scan for -C and -B before the full parse.  The
 * values are needed early: -C to chdir before loading the local
 * config, -B to feed config_load_project() the right build directory.
 *
 * Unknown options are skipped; the full parse_options() pass handles
 * errors and sets CLI-scope entries.  Only -C is removed from argv
 * (to avoid a second chdir attempt against the already-changed cwd);
 * -B stays so the full parse can record it at CLI scope.
 */
static void pre_parse_location(int *argcp, const char **argv,
			       const char **out_chdir, const char **out_build)
{
	int argc = *argcp;
	int dst = 1;
	int i = 1;

	*out_chdir = NULL;
	*out_build = NULL;

	while (i < argc) {
		const char *a = argv[i];
		int drop = 0;
		int step = 1;

		if (!strcmp(a, "--") || a[0] != '-' || a[1] == '\0')
			break;

		if (!strcmp(a, "-C") && i + 1 < argc) {
			*out_chdir = argv[i + 1];
			drop = 1;
			step = 2;
		} else if (!strncmp(a, "-C", 2) && a[2]) {
			*out_chdir = a + 2;
			drop = 1;
		} else if ((!strcmp(a, "-B") || !strcmp(a, "--build-dir")) &&
			   i + 1 < argc) {
			*out_build = argv[i + 1];
		} else if (!strncmp(a, "-B", 2) && a[2]) {
			*out_build = a + 2;
		} else if (!strncmp(a, "--build-dir=", 12)) {
			*out_build = a + 12;
		}

		if (!drop) {
			for (int j = 0; j < step; j++) {
				if (dst != i + j)
					argv[dst] = argv[i + j];
				dst++;
			}
		}
		i += step;
	}

	while (i < argc) {
		if (dst != i)
			argv[dst] = argv[i];
		dst++;
		i++;
	}
	argv[dst] = NULL;
	*argcp = dst;
}

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
 * argv[0], with the remaining args from argv[1..] appended.  Global
 * options inside the alias value are NOT re-parsed; aliases are
 * intended to expand to subcommand invocations, not to inject new
 * global flags.
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

	/*
	 * argv may currently point into `tokens` from a previous expansion
	 * pass.  Snapshot the tail before we svec_clear() the storage.
	 */
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

static const struct cmd_struct *find_command(const char *name)
{
	for (const struct cmd_struct *c = ice_commands; c->name; c++)
		if (!strcmp(name, c->name))
			return c;
	return NULL;
}

int main(int argc, const char **argv)
{
	const char *chdir_to = NULL;
	const char *build_override = NULL;
	const struct cmd_struct *cmd;

	/* Enable color early so die() in parse_options is colored. */
	color_init(STDERR_FILENO);

	config_load_defaults(&config);
	config_load_file(&config, CONFIG_SCOPE_USER, user_config_path());

	pre_parse_location(&argc, argv, &chdir_to, &build_override);
	if (chdir_to && chdir(chdir_to))
		die_errno("cannot change to '%s'", chdir_to);

	config_load_file(&config, CONFIG_SCOPE_LOCAL, local_config_path());
	config_load_project(&config, build_override
					 ? build_override
					 : config_get("core.build-dir"));
	config_load_env(&config);

	argc = parse_options_manual(argc, argv, ice_global_opts,
				    ice_global_usage, &ice_root_manual);

	if (global_no_color)
		use_color = 0;

	if (global_version) {
		printf("%sice %s\n", use_vt ? "\xf0\x9f\xa7\x8a " : "",
		       VERSION);
		return EXIT_SUCCESS;
	}

	/* Bare 'ice': show the same manual as 'ice --help', exit 1. */
	if (argc < 1) {
		print_manual(argv[0], &ice_root_manual, ice_global_opts,
			     ice_global_usage);
		return EXIT_FAILURE;
	}

	/* Expand alias.<name> repeatedly (with a depth cap to break cycles). */
	for (int depth = 0; depth < 10; depth++) {
		if (!try_expand_alias(&argc, &argv))
			break;
		if (argc < 1) {
			print_manual(argv[0], &ice_root_manual, ice_global_opts,
				     ice_global_usage);
			return EXIT_FAILURE;
		}
	}

	cmd = find_command(argv[0]);
	if (!cmd) {
		fprintf(stderr,
			"ice: '%s' is not a command. "
			"See 'ice --help'.\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	return cmd->fn(argc, argv);
}
