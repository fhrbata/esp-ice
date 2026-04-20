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
 *     Hidden.  Transforms the partial command line into a call to
 *     cmd_ice() with --ice-complete appended.  The normal dispatch
 *     chain routes to the deepest handler, whose parse_options()
 *     sees --ice-complete and prints candidates to stdout before
 *     exiting.  The shell captures the output.
 *
 *     Each candidate is emitted as "<name>[\t<description>]".  The
 *     description is derived from cmd_manual.summary (subcommands)
 *     or option.help (flags); candidates with no description are
 *     bare names.  The per-shell glue below adapts the stream to
 *     each shell's native completion-description convention.
 */
#include "ice.h"

/* ---- public: `ice completion <shell>` ------------------------------ */

/*
 * Candidates produced by `ice __complete` may carry an optional
 * description after a TAB (see options.c::print_sorted).  Each
 * per-shell glue below strips, forwards, or re-formats that TAB to
 * match the host shell's native completion-description convention:
 *
 *   bash        strip (cut -f1);  name-only behaviour
 *   zsh         translate TAB->':' and feed to _describe
 *   fish        forward as-is;    fish parses TAB natively
 *   powershell  split on TAB;     pass description to CompletionResult
 */
/* clang-format off */
static const char bash_script[] =
	"# ice bash completion (install: eval \"$(ice completion bash)\")\n"
	"#\n"
	"# Backend returns 'name\\tdesc' lines.  bash has no native concept\n"
	"# of a completion description, so on a list display we rewrite each\n"
	"# entry as 'name  -- desc' (aligned), but on a single match we strip\n"
	"# the description so only the bare name is inserted on the command\n"
	"# line.  Same trick Cobra uses for gh/kubectl/helm.\n"
	"_ice_complete() {\n"
	"    local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
	"    local tab=$'\\t'\n"
	"    local raw line name longest=0 i\n"
	"\n"
	"    raw=$(ice __complete \"$COMP_CWORD\" \"${COMP_WORDS[@]}\" 2>/dev/null)\n"
	"    [[ -z $raw ]] && return\n"
	"\n"
	"    COMPREPLY=()\n"
	"    while IFS= read -r line; do\n"
	"        name=\"${line%%$tab*}\"\n"
	"        [[ $name == \"$cur\"* ]] || continue\n"
	"        COMPREPLY+=(\"$line\")\n"
	"        (( ${#name} > longest )) && longest=${#name}\n"
	"    done <<<\"$raw\"\n"
	"\n"
	"    if (( ${#COMPREPLY[@]} == 1 )); then\n"
	"        COMPREPLY[0]=$(printf '%q' \"${COMPREPLY[0]%%$tab*}\")\n"
	"        return\n"
	"    fi\n"
	"\n"
	"    for i in \"${!COMPREPLY[@]}\"; do\n"
	"        line=\"${COMPREPLY[i]}\"\n"
	"        [[ $line == *$tab* ]] || continue\n"
	"        printf -v name '%-*s' \"$longest\" \"${line%%$tab*}\"\n"
	"        COMPREPLY[i]=\"$name  -- ${line#*$tab}\"\n"
	"    done\n"
	"}\n"
	"complete -o default -o nosort -F _ice_complete ice\n";

static const char zsh_script[] =
	"# ice zsh completion (install: eval \"$(ice completion zsh)\")\n"
	"(( $+functions[compdef] )) || { autoload -Uz compinit && compinit -u; }\n"
	"_ice() {\n"
	"    # Parse the backend's 'name[\\tdescription]' lines into two\n"
	"    # parallel arrays: @names (the values inserted on selection)\n"
	"    # and @disp (the strings shown in the completion menu).  We\n"
	"    # use compadd directly rather than _describe because _describe\n"
	"    # reorders entries in a way that interleaves flags with\n"
	"    # subcommands -- compadd -V preserves insertion order verbatim.\n"
	"    local -a raw names disp\n"
	"    local IFS=$'\\n' c name desc\n"
	"    local -i maxn=0\n"
	"    raw=( ${(f)\"$(ice __complete $((CURRENT - 1)) \"${words[@]}\" 2>/dev/null)\"} )\n"
	"    if (( ${#raw} == 0 )); then\n"
	"        _files\n"
	"        return\n"
	"    fi\n"
	"    # First pass: longest name, so descriptions align in column.\n"
	"    for c in \"${raw[@]}\"; do\n"
	"        name=\"${c%%$'\\t'*}\"\n"
	"        (( ${#name} > maxn )) && maxn=${#name}\n"
	"    done\n"
	"    # Second pass: build names and display strings.\n"
	"    for c in \"${raw[@]}\"; do\n"
	"        if [[ \"$c\" == *$'\\t'* ]]; then\n"
	"            name=\"${c%%$'\\t'*}\"\n"
	"            desc=\"${c#*$'\\t'}\"\n"
	"            disp+=( \"${(r:$maxn:)name}  -- $desc\" )\n"
	"        else\n"
	"            name=\"$c\"\n"
	"            disp+=( \"$name\" )\n"
	"        fi\n"
	"        names+=( \"$name\" )\n"
	"    done\n"
	"    compadd -V unsorted -l -d disp -a names\n"
	"}\n"
	"compdef _ice ice\n";

static const char fish_script[] =
	"# ice fish completion (install: ice completion fish | source)\n"
	"# The backend emits 'name\\tdescription' lines; fish parses TAB\n"
	"# natively and renders descriptions in the pop-up.\n"
	"function __ice_complete\n"
	"    set -l words (commandline -opc) (commandline -ct)\n"
	"    ice __complete (math (count $words) - 1) $words 2>/dev/null\n"
	"end\n"
	"complete -c ice -f -k -a '(__ice_complete)'\n";

static const char powershell_script[] =
	"# ice PowerShell completion\n"
	"# install: ice completion powershell | Out-String | Invoke-Expression\n"
	"#\n"
	"# Tip: descriptions show inline in the completion list when PSReadLine\n"
	"# menu mode is enabled -- add this to your $PROFILE if TAB only cycles\n"
	"# names:\n"
	"#   Set-PSReadLineKeyHandler -Key Tab -Function MenuComplete\n"
	"Register-ArgumentCompleter -Native -CommandName 'ice' -ScriptBlock {\n"
	"    param($wordToComplete, $commandAst, $cursorPosition)\n"
	"\n"
	"    $words = @($commandAst.CommandElements | ForEach-Object { $_.ToString() })\n"
	"    if ($wordToComplete -eq '') { $words += '' }\n"
	"    $cword = $words.Count - 1\n"
	"\n"
	"    $lines = @(& ice __complete $cword @words 2>$null | Where-Object {\n"
	"        $_ -and ($_ -split \"`t\", 2)[0].StartsWith($wordToComplete)\n"
	"    })\n"
	"    if ($lines.Count -eq 0) {\n"
	"        Get-ChildItem -Path \"$wordToComplete*\" -ErrorAction SilentlyContinue |\n"
	"            ForEach-Object {\n"
	"                [System.Management.Automation.CompletionResult]::new(\n"
	"                    $_.Name, $_.Name, 'ProviderItem', $_.FullName)\n"
	"            }\n"
	"        return\n"
	"    }\n"
	"\n"
	"    # Find the longest name so descriptions align in a column.\n"
	"    $maxN = 0\n"
	"    foreach ($line in $lines) {\n"
	"        $n = ($line -split \"`t\", 2)[0].Length\n"
	"        if ($n -gt $maxN) { $maxN = $n }\n"
	"    }\n"
	"\n"
	"    foreach ($line in $lines) {\n"
	"        $parts = $line -split \"`t\", 2\n"
	"        $name  = $parts[0]\n"
	"        if ($parts.Count -gt 1 -and $parts[1]) {\n"
	"            $desc     = $parts[1]\n"
	"            # ListItemText carries the description so it's visible in\n"
	"            # every completion mode (TAB cycle, Ctrl+Space menu,\n"
	"            # MenuComplete).  ToolTip is shown in menu modes only.\n"
	"            $listItem = ('{0}  -- {1}' -f $name.PadRight($maxN), $desc)\n"
	"            $tip      = \"$name  --  $desc\"\n"
	"        } else {\n"
	"            $listItem = $name\n"
	"            $tip      = $name\n"
	"        }\n"
	"        [System.Management.Automation.CompletionResult]::new(\n"
	"            $name, $listItem, 'ParameterValue', $tip)\n"
	"    }\n"
	"}\n";

static const struct cmd_manual completion_manual = {
	.name = "ice completion",
	.summary = "print shell completion script",

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
	       "Binds via @b{complete -F}.  Formats each candidate as "
	       "@b{name  -- description} in the list display (aligned to "
	       "the longest name, truncated to @b{$COLUMNS}) and strips the "
	       "description back off when a single match is inserted.  "
	       "Falls through to filename completion via @b{-o default} "
	       "when the backend produces no candidates.")
	H_ITEM("zsh",
	       "Binds via @b{compdef}.  Formats backend descriptions into "
	       "aligned display strings and calls @b{compadd -V} directly, "
	       "which preserves the backend's emission order (subcommands, "
	       "then long flags, then short flags).  Falls through to "
	       "@b{_files} when no candidates are produced.")
	H_ITEM("fish",
	       "Binds via @b{complete -c ice -f -a}.  Re-invokes the "
	       "backend each @b{TAB}; fish natively renders the "
	       "@b{name\\tdescription} lines with descriptions in the pop-up.")
	H_ITEM("powershell",
	       "Binds via @b{Register-ArgumentCompleter -Native}.  Bakes "
	       "the description into the @b{ListItemText} of each "
	       "@b{CompletionResult} (and repeats it in the tooltip) so "
	       "descriptions are visible in every completion mode -- "
	       "including default TAB cycling.  For a full menu, enable "
	       "@b{Set-PSReadLineKeyHandler -Key Tab -Function MenuComplete} "
	       "in your @b{$PROFILE}.  Falls back to @b{Get-ChildItem} for "
	       "filename completion when the backend produces no candidates. "
	       "Works in Windows PowerShell 5.1 and PowerShell 7+."),
};
/* clang-format on */

static void complete_shells(void) { printf("bash\nzsh\nfish\npowershell\n"); }

static const struct option cmd_completion_opts[] = {
    OPT_POSITIONAL("shell", complete_shells),
    OPT_END(),
};

const struct cmd_desc cmd_completion_desc = {
    .name = "completion",
    .fn = cmd_completion,
    .opts = cmd_completion_opts,
    .manual = &completion_manual,
};

int cmd_completion(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_completion_desc);
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

static const struct cmd_manual complete_manual = {.name = "ice __complete"};
static const struct option cmd___complete_opts[] = {OPT_END()};

const struct cmd_desc cmd___complete_desc = {
    .name = "__complete",
    .fn = cmd_complete,
    .opts = cmd___complete_opts,
    .manual = &complete_manual,
};

/**
 * Transform the partial command line into a cmd_ice() call with
 * --ice-complete appended.
 *
 * argv layout:  ["__complete", "<cword>", "ice", "target", "set", "es"]
 *
 * We take words[0..cword-1] (the resolved context before the cursor
 * word), append "--ice-complete", and call cmd_ice().  The normal
 * dispatch chain routes through subcommands until the deepest
 * parse_options() sees --ice-complete, dumps candidates, and exits.
 * The shell does prefix filtering.
 */
int cmd_complete(int argc, const char **argv)
{
	struct svec av = SVEC_INIT;
	int cword;
	int nwords;
	const char **words;

	/* argv[0]="__complete", argv[1]=cword, argv[2..]=words. */
	if (argc < 3)
		return EXIT_SUCCESS;

	cword = atoi(argv[1]);
	nwords = argc - 2;
	words = argv + 2;

	if (cword < 0 || cword >= nwords)
		return EXIT_SUCCESS;

	/* Build argv: words[0..cword-1] + "--ice-complete". */
	for (int i = 0; i < cword; i++)
		svec_push(&av, words[i]);
	svec_push(&av, "--ice-complete");

	cmd_ice((int)av.nr, (const char **)av.v);

	/* cmd_ice -> ... -> parse_options -> exit(0); normally unreached. */
	svec_clear(&av);
	return EXIT_SUCCESS;
}
