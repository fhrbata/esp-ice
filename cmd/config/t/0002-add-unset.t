#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# `ice config`: --add (multi-value semantics) and --unset (remove all).

. t/tap.sh
tap_setup

mkdir -p home
export HOME="$PWD/home"
unset ESPPORT ESPBAUD IDF_TARGET

# --add appends multiple entries for the same key at the same scope.
"$BINARY" config --add cmake.define FOO=1
"$BINARY" config --add cmake.define BAR=2
"$BINARY" config --list >list
tap_check grep -qE '^local[[:space:]]+cmake\.define=FOO=1$' list
tap_check grep -qE '^local[[:space:]]+cmake\.define=BAR=2$' list
tap_done "--add appends, does not replace"

# config_get returns the last entry added at the winning scope.
"$BINARY" config cmake.define >got
tap_check grep -qx 'BAR=2' got
tap_done "get returns the last added value"

# --unset removes every entry for the key at the target scope.
"$BINARY" config --unset cmake.define
"$BINARY" config cmake.define >got 2>/dev/null
rc=$?
tap_check test "$rc" -ne 0
tap_check test ! -s got
tap_done "--unset removes every entry, get then exits non-zero"

# --unset on a key that has nothing at the target scope dies non-zero.
tap_check ! "$BINARY" config --unset cmake.define 2>/dev/null
tap_done "--unset on missing key exits non-zero"

# Direct assignment of a multi-value key replaces all entries at scope.
"$BINARY" config --add cmake.define A=1
"$BINARY" config --add cmake.define B=2
"$BINARY" config cmake.define ONLY=ME
"$BINARY" config --list >list
tap_check ! grep -qE '^local[[:space:]]+cmake\.define=A=1$' list
tap_check ! grep -qE '^local[[:space:]]+cmake\.define=B=2$' list
tap_check grep -qE '^local[[:space:]]+cmake\.define=ONLY=ME$' list
tap_done "direct set replaces every prior --add at scope"

tap_result
