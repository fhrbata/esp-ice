/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/completion/completion.c
 * @brief Shell completion support.
 *
 * Two subcommands live here:
 *
 *   ice completion bash|zsh|fish|powershell
 *     Public.  Prints a tiny shell-specific init snippet on stdout,
 *     meant to be evaluated from the user's rc file:
 *
 *         eval "$(ice completion bash)"       # ~/.bashrc
 *         eval "$(ice completion zsh)"        # ~/.zshrc
 *         ice completion fish | source        # fish
 *         ice completion powershell | Out-String | Invoke-Expression
 *
 *     The snippet binds a dispatch function to the `ice` command that
 *     re-invokes the hidden __complete backend on every TAB.
 *
 *   ice __complete <cword> <word>...
 *     Hidden.  Inspects the partial argv (word[0] == "ice", cword is
 *     the cursor word index) and prints newline-separated candidates
 *     on stdout.  Filtering by the current prefix happens here so the
 *     shell glue can set COMPREPLY / compadd / complete directly.
 *
 * The split keeps shell glue tiny and shell-agnostic; all the real
 * knowledge (subcommands, option names, aliases, targets, config keys)
 * lives in C and stays in sync with the binary by construction.
 */
#include "ice.h"

/* ---- small helpers -------------------------------------------------- */

static int has_prefix(const char *s, const char *prefix)
{
	return !strncmp(s, prefix, strlen(prefix));
}

static void emit(const char *candidate, const char *prefix)
{
	if (!prefix || !*prefix || has_prefix(candidate, prefix))
		printf("%s\n", candidate);
}

static const struct cmd_struct *find_cmd(const char *name)
{
	for (const struct cmd_struct *c = ice_commands; c->name; c++)
		if (!strcmp(c->name, name))
			return c;
	return NULL;
}

/* ---- candidate lists ------------------------------------------------ */

/* Visible subcommand names plus any alias.<name> the user has defined. */
static void complete_subcommands(const char *prefix)
{
	struct svec seen = SVEC_INIT;

	for (const struct cmd_struct *c = ice_commands; c->name; c++) {
		if (c->hidden)
			continue;
		svec_push(&seen, c->name);
		emit(c->name, prefix);
	}

	for (int i = 0; i < config.nr; i++) {
		const char *key = config.entries[i].key;
		const char *name;
		int dup = 0;

		if (strncmp(key, "alias.", 6) != 0)
			continue;
		name = key + 6;
		if (!*name)
			continue;
		for (size_t j = 0; j < seen.nr; j++)
			if (!strcmp(seen.v[j], name)) {
				dup = 1;
				break;
			}
		if (!dup) {
			svec_push(&seen, name);
			emit(name, prefix);
		}
	}
	svec_clear(&seen);
}

/*
 * Long (--foo) and short (-x) flag names from @p opts, plus the
 * implicit -h / --help that parse_options_manual accepts for every
 * command.
 */
static void complete_flags(const struct option *opts, const char *prefix)
{
	emit("-h", prefix);
	emit("--help", prefix);

	if (!opts)
		return;
	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		char buf[64];

		if (o->long_opt) {
			snprintf(buf, sizeof(buf), "--%s", o->long_opt);
			emit(buf, prefix);
		}
		if (o->short_opt) {
			snprintf(buf, sizeof(buf), "-%c", o->short_opt);
			emit(buf, prefix);
		}
	}
}

/* `ice help <cmd>` */
static void complete_help_arg(const char *prefix)
{
	for (const struct cmd_struct *c = ice_commands; c->name; c++) {
		if (c->hidden)
			continue;
		emit(c->name, prefix);
	}
}

/* `ice completion <shell>` */
static void complete_completion_arg(const char *prefix)
{
	emit("bash", prefix);
	emit("zsh", prefix);
	emit("fish", prefix);
	emit("powershell", prefix);
}

/*
 * `ice set-target <target>`.  Reuses the authoritative lists from
 * set-target.c, so preview targets surface on TAB too -- the command
 * still refuses them at run-time without --preview.
 */
static void complete_target_arg(const char *prefix)
{
	for (const char *const *t = ice_supported_targets; *t; t++)
		emit(*t, prefix);
	for (const char *const *t = ice_preview_targets; *t; t++)
		emit(*t, prefix);
}

/*
 * `ice config <key>`.  Lists well-known keys plus whatever the active
 * config currently has set, so alias.<name> and user-defined keys also
 * surface on TAB.
 */
static const char *const completion_config_keys[] = {
    "core.build-dir", "core.generator", "core.verbose", "cmake.define",
    "serial.port",    "serial.baud",	NULL,
};

static void complete_config_arg(const char *prefix)
{
	struct svec seen = SVEC_INIT;

	for (const char *const *k = completion_config_keys; *k; k++) {
		svec_push(&seen, *k);
		emit(*k, prefix);
	}

	for (int i = 0; i < config.nr; i++) {
		const char *key = config.entries[i].key;
		int dup = 0;

		for (size_t j = 0; j < seen.nr; j++)
			if (!strcmp(seen.v[j], key)) {
				dup = 1;
				break;
			}
		if (!dup) {
			svec_push(&seen, key);
			emit(key, prefix);
		}
	}
	svec_clear(&seen);
}

/* ---- argv classification ------------------------------------------- */

/*
 * Does @p word take a separate value under @p opts (i.e. does the next
 * argv slot get consumed)?  Returns 0 for bools, for attached-value
 * forms (-Bdir, --build-dir=dir), and for unknown options.
 */
static int opt_takes_value(const struct option *opts, const char *word)
{
	if (!opts || word[0] != '-' || word[1] == '\0')
		return 0;

	if (word[1] == '-') {
		if (strchr(word, '='))
			return 0;
	} else if (word[2] != '\0') {
		return 0;
	}

	for (const struct option *o = opts; o->type != OPTION_END; o++) {
		int match = 0;

		if (word[1] == '-') {
			if (o->long_opt && !strcmp(word + 2, o->long_opt))
				match = 1;
		} else if (o->short_opt && word[1] == o->short_opt &&
			   !word[2]) {
			match = 1;
		}
		if (!match)
			continue;
		return o->type != OPTION_BOOL && o->type != OPTION_CONFIG_BOOL;
	}
	return 0;
}

/*
 * Walk words[1..cword-1] mirroring parse_options_manual's view of
 * argv: first honour global options, then on the first non-option
 * word switch to that subcommand's option table.  Populates:
 *
 *   *subpos_out  index of the subcommand word (-1 if not yet present)
 *   *argpos_out  0-indexed position of the cursor word within the
 *                subcommand's positional arguments (0 = first
 *                positional, 1 = second, ...).  Only meaningful when
 *                subpos >= 0.
 *   *opts_out    active option table at cword (global until the
 *                subcommand is crossed; the subcommand's table after).
 */
static void classify(const char **words, int cword, int *subpos_out,
		     int *argpos_out, const struct option **opts_out)
{
	const struct option *opts = ice_global_opts;
	int in_sub = 0;
	int argpos = 0;
	int i = 1;

	*subpos_out = -1;

	while (i < cword) {
		const char *w = words[i];

		if (!strcmp(w, "--")) {
			i++;
			continue;
		}
		if (w[0] == '-' && w[1]) {
			i += opt_takes_value(opts, w) ? 2 : 1;
			continue;
		}
		if (!in_sub) {
			const struct cmd_struct *c = find_cmd(w);

			if (c) {
				*subpos_out = i;
				opts = c->opts;
				in_sub = 1;
			}
		} else {
			argpos++;
		}
		i++;
	}

	*argpos_out = argpos;
	*opts_out = opts;
}

/* ---- public: `ice completion <shell>` ------------------------------ */

/* clang-format off */
static const char bash_script[] =
	"# ice bash completion (install: eval \"$(ice completion bash)\")\n"
	"_ice_complete() {\n"
	"    local IFS=$'\\n'\n"
	"    COMPREPLY=( $(ice __complete \"$COMP_CWORD\" \"${COMP_WORDS[@]}\" 2>/dev/null) )\n"
	"}\n"
	"complete -o default -F _ice_complete ice\n";

static const char zsh_script[] =
	"# ice zsh completion (install: eval \"$(ice completion zsh)\")\n"
	"_ice() {\n"
	"    local -a candidates\n"
	"    local IFS=$'\\n'\n"
	"    candidates=( ${(f)\"$(ice __complete $((CURRENT - 1)) \"${words[@]}\" 2>/dev/null)\"} )\n"
	"    compadd -a candidates && return\n"
	"    _files\n"
	"}\n"
	"compdef _ice ice\n";

static const char fish_script[] =
	"# ice fish completion (install: ice completion fish | source)\n"
	"function __ice_complete\n"
	"    set -l words (commandline -opc) (commandline -ct)\n"
	"    ice __complete (math (count $words) - 1) $words 2>/dev/null\n"
	"end\n"
	"complete -c ice -f -a '(__ice_complete)'\n";

static const char powershell_script[] =
	"# ice PowerShell completion\n"
	"# install: ice completion powershell | Out-String | Invoke-Expression\n"
	"Register-ArgumentCompleter -Native -CommandName 'ice' -ScriptBlock {\n"
	"    param($wordToComplete, $commandAst, $cursorPosition)\n"
	"\n"
	"    $words = @($commandAst.CommandElements | ForEach-Object { $_.ToString() })\n"
	"    if ($wordToComplete -eq '') { $words += '' }\n"
	"    $cword = $words.Count - 1\n"
	"\n"
	"    $results = @(& ice __complete $cword @words 2>$null | Where-Object { $_ })\n"
	"    if ($results.Count -gt 0) {\n"
	"        $results | ForEach-Object {\n"
	"            [System.Management.Automation.CompletionResult]::new(\n"
	"                $_, $_, 'ParameterValue', $_)\n"
	"        }\n"
	"    } else {\n"
	"        Get-ChildItem -Path \"$wordToComplete*\" -ErrorAction SilentlyContinue |\n"
	"            ForEach-Object {\n"
	"                [System.Management.Automation.CompletionResult]::new(\n"
	"                    $_.Name, $_.Name, 'ProviderItem', $_.FullName)\n"
	"            }\n"
	"    }\n"
	"}\n";

static const struct cmd_manual completion_manual = {
	.description =
	H_PARA("Emits a shell-specific completion script on standard "
	       "output, meant to be evaluated from the user's rc file.  "
	       "The script registers a dispatch function that calls a "
	       "hidden @b{ice __complete} backend on every @b{TAB}, so "
	       "candidates -- subcommands, long / short flags, aliases, "
	       "config keys, chip targets -- are always generated by the "
	       "current binary and cannot drift from available features.")
	H_PARA("The protocol is minimal: the shell glue forwards the word "
	       "array and cursor index to @b{ice __complete}, which prints "
	       "newline-separated candidates filtered by the current "
	       "prefix.  No on-disk completion file, no installation step."),

	.examples =
	H_EXAMPLE("eval \"$(ice completion bash)\"    # ~/.bashrc")
	H_EXAMPLE("eval \"$(ice completion zsh)\"     # ~/.zshrc")
	H_EXAMPLE("ice completion fish | source       # fish")
	H_EXAMPLE("ice completion powershell | Out-String | Invoke-Expression   # $PROFILE"),

	.extras =
	H_SECTION("SUPPORTED SHELLS")
	H_ITEM("bash",
	       "Binds via @b{complete -F}.  Falls through to filename "
	       "completion via @b{-o default} when the backend produces no "
	       "candidates.")
	H_ITEM("zsh",
	       "Binds via @b{compdef}.  Falls through to @b{_files} when "
	       "no candidates are produced.")
	H_ITEM("fish",
	       "Binds via @b{complete -c ice -f -a}.  Re-invokes the "
	       "backend each @b{TAB}.")
	H_ITEM("powershell",
	       "Binds via @b{Register-ArgumentCompleter -Native}.  Falls "
	       "back to @b{Get-ChildItem} for filename completion when the "
	       "backend produces no candidates.  Works in Windows PowerShell "
	       "5.1 and PowerShell 7+."),
};
/* clang-format on */

int cmd_completion(int argc, const char **argv)
{
	const char *usage[] = {"ice completion bash|zsh|fish|powershell", NULL};
	struct option opts[] = {OPT_END()};

	argc =
	    parse_options_manual(argc, argv, opts, usage, &completion_manual);
	if (argc != 1)
		die("usage: ice completion bash|zsh|fish|powershell");

	if (!strcmp(argv[0], "bash"))
		fputs(bash_script, stdout);
	else if (!strcmp(argv[0], "zsh"))
		fputs(zsh_script, stdout);
	else if (!strcmp(argv[0], "fish"))
		fputs(fish_script, stdout);
	else if (!strcmp(argv[0], "powershell"))
		fputs(powershell_script, stdout);
	else
		die("unknown shell '%s' (supported: bash, zsh, fish, "
		    "powershell)",
		    argv[0]);
	return EXIT_SUCCESS;
}

/* ---- hidden: `ice __complete <cword> <word>...` -------------------- */

int cmd_complete(int argc, const char **argv)
{
	int cword;
	int nwords;
	const char **words;
	const char *cur;
	int subpos, argpos;
	const struct option *opts_ctx;

	/* argv[0]="__complete", argv[1]=cword, argv[2..]=words. */
	if (argc < 3)
		return EXIT_SUCCESS;

	cword = atoi(argv[1]);
	nwords = argc - 2;
	words = argv + 2;

	if (cword < 0 || cword >= nwords)
		return EXIT_SUCCESS;
	cur = words[cword];

	classify(words, cword, &subpos, &argpos, &opts_ctx);

	/* Flag: complete from whichever option table is active at cword. */
	if (cur[0] == '-' && cword > 0) {
		complete_flags(opts_ctx, cur);
		return EXIT_SUCCESS;
	}

	/*
	 * Value of a preceding -X / --foo that takes a separate value:
	 * emit custom candidates when known, otherwise let the shell's
	 * file completion take over (bash '-o default', zsh '_files').
	 */
	if (cword > 0 && words[cword - 1][0] == '-' &&
	    opt_takes_value(opts_ctx, words[cword - 1])) {
		if (!strcmp(words[cword - 1], "--target"))
			complete_target_arg(cur);
		return EXIT_SUCCESS;
	}

	/* Before the subcommand: complete the subcommand name itself. */
	if (subpos < 0) {
		complete_subcommands(cur);
		return EXIT_SUCCESS;
	}

	/* After the subcommand: per-subcommand positional candidates. */
	{
		const char *sub = words[subpos];

		if (argpos == 0) {
			if (!strcmp(sub, "help"))
				complete_help_arg(cur);
			else if (!strcmp(sub, "completion"))
				complete_completion_arg(cur);
			else if (!strcmp(sub, "set-target"))
				complete_target_arg(cur);
			else if (!strcmp(sub, "config"))
				complete_config_arg(cur);
		}
	}

	return EXIT_SUCCESS;
}
