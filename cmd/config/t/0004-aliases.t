#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# `ice config`: alias.<name> expansion -- command alias, shell alias
# (!cmd), chained aliases, exit-status propagation.

. t/tap.sh
tap_setup

mkdir -p home
export HOME="$PWD/home" USERPROFILE="$PWD/home"
unset ESPPORT ESPBAUD IDF_TARGET

# Command alias: `ice h` -> `ice help` -> top-level manual.
"$BINARY" config alias.h "help"
"$BINARY" h >out
tap_check grep -q '^NAME$' out
tap_done "command alias dispatches to subcommand"

# Alias tokens are appended to the typed argv tail.
# `ice cfg core.build-dir` -> `ice config core.build-dir`.
"$BINARY" config alias.cfg "config"
"$BINARY" cfg core.build-dir >got
tap_check grep -qx 'build' got
tap_done "alias preserves trailing user arguments"

# Alias chaining: alias resolves to another alias.
"$BINARY" config alias.x "h"
"$BINARY" x >out
tap_check grep -q '^NAME$' out
tap_done "alias chains through another alias"

# Shell alias: value beginning with '!' runs through /bin/sh -c.
"$BINARY" config alias.echoshell '!echo hi from shell'
"$BINARY" echoshell >got
tap_check grep -qx 'hi from shell' got
tap_done "shell alias runs through the platform shell"

# Shell alias propagates the underlying command's exit status.
"$BINARY" config alias.exitone '!exit 1'
tap_check ! "$BINARY" exitone 2>/dev/null
tap_done "shell alias propagates non-zero exit status"

# Aliases also live under --user scope.
"$BINARY" config --user alias.uh "help"
rm -rf .ice
"$BINARY" uh >out
tap_check grep -q '^NAME$' out
tap_done "alias defined at --user scope is honoured"

tap_result
