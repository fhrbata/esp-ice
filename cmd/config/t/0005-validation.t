#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# `ice config`: rejection of invalid flag combinations and bad arity.

. t/tap.sh
tap_setup

mkdir -p home
export HOME="$PWD/home"
unset ESPPORT ESPBAUD IDF_TARGET

# --user and --local cannot be combined.
tap_check ! "$BINARY" config --user --local core.build-dir x 2>/dev/null
tap_done "--user + --local is rejected"

# Mode flags (--list, --add, --unset) are mutually exclusive.
tap_check ! "$BINARY" config --list --add foo bar 2>/dev/null
tap_done "--list + --add is rejected"

tap_check ! "$BINARY" config --list --unset foo  2>/dev/null
tap_done "--list + --unset is rejected"

tap_check ! "$BINARY" config --add  --unset foo  2>/dev/null
tap_done "--add + --unset is rejected"

# --list takes no positional arguments.
tap_check ! "$BINARY" config --list core.build-dir 2>/dev/null
tap_done "--list rejects positional arguments"

# --add requires <key> <value>.
tap_check ! "$BINARY" config --add foo 2>/dev/null
tap_done "--add requires both key and value"

# --unset requires <key>.
tap_check ! "$BINARY" config --unset 2>/dev/null
tap_done "--unset requires a key"

# Bare config rejects more than two positionals (key, value).
tap_check ! "$BINARY" config a b c 2>/dev/null
tap_done "set rejects more than two positionals"

tap_result
