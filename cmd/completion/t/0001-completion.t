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

# ---- `ice completion <shell>` prints the expected init script ----

"$BINARY" completion bash >bash.out
tap_check grep -q 'complete -o default -o nosort -F _ice_complete ice' bash.out
tap_done "ice completion bash emits complete registration"

"$BINARY" completion zsh >zsh.out
tap_check grep -q 'compdef _ice ice' zsh.out
tap_done "ice completion zsh emits compdef registration"

"$BINARY" completion fish >fish.out
tap_check grep -q 'complete -c ice -f -k -a' fish.out
tap_done "ice completion fish emits complete registration"

"$BINARY" completion powershell >pwsh.out
tap_check grep -q "Register-ArgumentCompleter -Native -CommandName 'ice'" pwsh.out
tap_done "ice completion powershell emits Register-ArgumentCompleter registration"

tap_check ! "$BINARY" completion nushell 2>/dev/null
tap_done "unknown shell is rejected with non-zero exit"

# ---- `ice __complete`: subcommand-name candidates ----

"$BINARY" __complete 1 ice "" >cmds.out
tap_check grep -qx 'build'      cmds.out
tap_check grep -qx 'config'     cmds.out
tap_check grep -qx 'completion' cmds.out
tap_check grep -qx 'target'    cmds.out
tap_check grep -qx 'tools'     cmds.out
tap_check ! grep -qx '__complete' cmds.out
tap_done "subcommand list includes visible commands and hides __complete"

"$BINARY" __complete 1 ice "co" >pref.out
tap_check grep -qx 'completion' pref.out
tap_check grep -qx 'config'     pref.out
tap_done "subcommand list includes all visible commands (shell does prefix filtering)"

"$BINARY" __complete 2 ice idf "" >idfsubs.out
tap_check grep -qx 'monitor'         idfsubs.out
tap_check grep -qx 'size'            idfsubs.out
tap_check grep -qx 'configdep'       idfsubs.out
tap_check grep -qx 'ldgen'           idfsubs.out
tap_check grep -qx 'partition-table' idfsubs.out
tap_done "ice idf <TAB> lists bundled IDF tools"

# ---- `ice __complete`: per-subcommand flag candidates ----

"$BINARY" __complete 2 ice config "--" >cfgflags.out
tap_check grep -qx -- '--list'  cfgflags.out
tap_check grep -qx -- '--add'   cfgflags.out
tap_check grep -qx -- '--unset' cfgflags.out
tap_check grep -qx -- '--user'  cfgflags.out
tap_check grep -qx -- '--local' cfgflags.out
tap_done "config --<TAB> walks cmd_struct.opts"

"$BINARY" __complete 1 ice "-" >globflags.out
tap_check grep -qx -- '--verbose' globflags.out
tap_check grep -qx -- '-v'        globflags.out
tap_check grep -qx -- '--version' globflags.out
tap_check grep -qx -- '-h'        globflags.out
tap_check grep -qx -- '--help'    globflags.out
tap_done "global flag completion walks ice_global_opts (plus -h/--help)"

"$BINARY" __complete 2 ice config "-" >cfghelp.out
tap_check grep -qx -- '-h'     cfghelp.out
tap_check grep -qx -- '--help' cfghelp.out
tap_done "per-subcommand flag completion emits -h/--help"

# ---- `ice __complete`: positional candidates ----

"$BINARY" __complete 1 ice "t" >tpref.out
tap_check grep -qx 'target'    tpref.out
tap_check grep -qx 'tools'     tpref.out
tap_done "subcommand list includes target and tools (shell does prefix filtering)"

"$BINARY" __complete 2 ice help "" >helpcmds.out
tap_check grep -qx 'build'       helpcmds.out
tap_check grep -qx 'completion'  helpcmds.out
tap_check ! grep -qx '__complete' helpcmds.out
tap_done "help <TAB> lists visible subcommands"

"$BINARY" __complete 2 ice completion "" >shells.out
tap_check grep -qx 'bash'       shells.out
tap_check grep -qx 'zsh'        shells.out
tap_check grep -qx 'fish'       shells.out
tap_check grep -qx 'powershell' shells.out
tap_done "completion <TAB> lists supported shells"

# ---- `ice __complete`: positional with no completion callback ----

"$BINARY" __complete 4 ice repo checkout v5.4 "" >posfile.out
tap_check test ! -s posfile.out
tap_done "positional with NULL callback emits nothing (shell handles file completion)"

tap_result
