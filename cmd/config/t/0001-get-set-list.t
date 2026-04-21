#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# `ice config`: single-value get/set, --list output, defaults visibility.

. t/tap.sh
tap_setup

# Isolate HOME so --user writes never reach the developer's real
# ~/.iceconfig, and clear env vars the binary maps to config keys
# so tests are deterministic regardless of the host environment.
mkdir -p home
export HOME="$PWD/home" USERPROFILE="$PWD/home"
unset ESPPORT ESPBAUD IDF_TARGET

# get on an unset key exits non-zero with no output on stdout.
"$BINARY" config some.unset.key >got 2>/dev/null
rc=$?
tap_check test "$rc" -ne 0
tap_check test ! -s got
tap_done "get on unset key exits non-zero with empty stdout"

# Built-in defaults are reachable through plain get.
"$BINARY" config core.build-dir >got
tap_check grep -qx 'build' got
tap_done "default core.build-dir resolves to 'build'"

"$BINARY" config core.generator >got
tap_check grep -qx 'Ninja' got
tap_done "default core.generator resolves to 'Ninja'"

# set then get round-trip; default scope is --local.
"$BINARY" config core.build-dir out
"$BINARY" config core.build-dir >got
tap_check grep -qx 'out' got
tap_check test -f .ice/config
tap_done "set then get round-trip via --local default"

# Direct re-assignment replaces; single-value semantics.
"$BINARY" config core.build-dir other
"$BINARY" config core.build-dir >got
tap_check grep -qx 'other' got
tap_done "set replaces existing value at scope"

# --list shows every entry tagged with its scope, including defaults
# and the local override stacked above them.
"$BINARY" config --list >list
tap_check grep -qE '^default[[:space:]]+core\.build-dir=build$'   list
tap_check grep -qE '^default[[:space:]]+core\.generator=Ninja$'   list
tap_check grep -qE '^local[[:space:]]+core\.build-dir=other$'     list
tap_done "--list emits scope-tagged entries for defaults and locals"

tap_result
