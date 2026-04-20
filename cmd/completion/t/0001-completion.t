#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end tests for `ice completion <shell>` and the hidden
# `ice __complete` backend that drives TAB candidates.

. t/tap.sh
tap_setup

# Candidates are emitted as "<name>[\t<description>]" -- presence checks
# must allow the optional tab-prefixed description.
cand_present () {
	grep -qE "^$1"$'(\t|$)' "$2"
}

# ---- `ice completion <shell>` prints the expected init script ----

"$BINARY" completion bash >bash.out
tap_check grep -q 'complete -o default -o nosort -F _ice_complete ice' bash.out
tap_check grep -q 'COMPREPLY' bash.out
tap_done "ice completion bash emits complete registration"

"$BINARY" completion zsh >zsh.out
tap_check grep -q 'compdef _ice ice' zsh.out
tap_check grep -q -E 'compadd .*-V .*-l .*-d disp .*-a names' zsh.out
tap_done "ice completion zsh emits compdef + compadd wiring"

"$BINARY" completion fish >fish.out
tap_check grep -q 'complete -c ice -f -k -a' fish.out
tap_done "ice completion fish emits complete registration"

"$BINARY" completion powershell >pwsh.out
tap_check grep -q "Register-ArgumentCompleter -Native -CommandName 'ice'" pwsh.out
tap_check grep -q -- '-split "`t"' pwsh.out
tap_done "ice completion powershell emits Register-ArgumentCompleter registration"

tap_check ! "$BINARY" completion nushell 2>/dev/null
tap_done "unknown shell is rejected with non-zero exit"

# ---- `ice __complete`: subcommand-name candidates ----

"$BINARY" __complete 1 ice "" >cmds.out
tap_check cand_present 'build'      cmds.out
tap_check cand_present 'config'     cmds.out
tap_check cand_present 'completion' cmds.out
tap_check cand_present 'target'     cmds.out
tap_check cand_present 'tools'      cmds.out
tap_check ! cand_present '__complete' cmds.out
tap_done "subcommand list includes visible commands and hides __complete"

"$BINARY" __complete 1 ice "co" >pref.out
tap_check cand_present 'completion' pref.out
tap_check cand_present 'config'     pref.out
tap_done "subcommand list includes all visible commands (shell does prefix filtering)"

"$BINARY" __complete 2 ice idf "" >idfsubs.out
tap_check cand_present 'monitor'         idfsubs.out
tap_check cand_present 'size'            idfsubs.out
tap_check cand_present 'configdep'       idfsubs.out
tap_check cand_present 'ldgen'           idfsubs.out
tap_check cand_present 'partition-table' idfsubs.out
tap_done "ice idf <TAB> lists bundled IDF tools"

# ---- `ice __complete`: per-subcommand flag candidates ----

"$BINARY" __complete 2 ice config "--" >cfgflags.out
tap_check cand_present '--list'  cfgflags.out
tap_check cand_present '--add'   cfgflags.out
tap_check cand_present '--unset' cfgflags.out
tap_check cand_present '--user'  cfgflags.out
tap_check cand_present '--local' cfgflags.out
tap_done "config --<TAB> walks cmd_struct.opts"

"$BINARY" __complete 1 ice "-" >globflags.out
tap_check cand_present '--verbose' globflags.out
tap_check cand_present '-v'        globflags.out
tap_check cand_present '--version' globflags.out
tap_check cand_present '-h'        globflags.out
tap_check cand_present '--help'    globflags.out
tap_done "global flag completion walks ice_global_opts (plus -h/--help)"

"$BINARY" __complete 2 ice config "-" >cfghelp.out
tap_check cand_present '-h'     cfghelp.out
tap_check cand_present '--help' cfghelp.out
tap_done "per-subcommand flag completion emits -h/--help"

# ---- `ice __complete`: descriptions are attached where available ----

tap_check grep -qF -- $'build\tbuild the default target' cmds.out
tap_check grep -qF -- $'config\tinspect and modify configuration entries' cmds.out
tap_done "subcommand candidates carry the manual->summary description"

tap_check grep -qE -- $'^--list\t[^\t]' cfgflags.out
tap_check grep -qE -- $'^--add\t[^\t]'  cfgflags.out
tap_done "flag candidates carry the option.help description"

tap_check grep -qF -- $'--help\tshow full manual' cfghelp.out
tap_check grep -qF -- $'-h\tshow short usage'     cfghelp.out
tap_done "-h/--help emit synthesized descriptions"

# ---- `ice __complete`: positional candidates ----

"$BINARY" __complete 1 ice "t" >tpref.out
tap_check cand_present 'target'    tpref.out
tap_check cand_present 'tools'     tpref.out
tap_done "subcommand list includes target and tools (shell does prefix filtering)"

"$BINARY" __complete 2 ice help "" >helpcmds.out
tap_check cand_present 'build'       helpcmds.out
tap_check cand_present 'completion'  helpcmds.out
tap_check ! cand_present '__complete' helpcmds.out
tap_done "help <TAB> lists visible subcommands"

"$BINARY" __complete 2 ice completion "" >shells.out
tap_check cand_present 'bash'       shells.out
tap_check cand_present 'zsh'        shells.out
tap_check cand_present 'fish'       shells.out
tap_check cand_present 'powershell' shells.out
tap_done "completion <TAB> lists supported shells"

# ---- `ice __complete`: positional with no completion callback ----

"$BINARY" __complete 4 ice repo checkout v5.4 "" >posfile.out
tap_check test ! -s posfile.out
tap_done "positional with NULL callback emits nothing (shell handles file completion)"

tap_result
