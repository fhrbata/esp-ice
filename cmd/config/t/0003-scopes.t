#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# `ice config`: scope file paths and precedence among the scopes that
# actually live in the config store (defaults < user < local < project).
# Environment variables and CLI flags seed command options directly and
# do NOT appear in `ice config` output -- that contract is covered by
# the unit tests for options.c and each command's own integration tests.

. t/tap.sh
tap_setup

mkdir -p home
export HOME="$PWD/home" USERPROFILE="$PWD/home"
unset ESPPORT ESPBAUD IDF_TARGET

# --user writes to ~/.iceconfig; --local to ./.ice/config.
"$BINARY" config --user core.generator from_user
"$BINARY" config --local core.build-dir from_local
tap_check test -f "$HOME/.iceconfig"
tap_check test -f .ice/config
tap_done "--user and --local target their own files"

# local beats user.
"$BINARY" config --user core.build-dir user_value
"$BINARY" config --local core.build-dir local_value
"$BINARY" config core.build-dir >got
tap_check grep -qx 'local_value' got
tap_done "local scope wins over user"

# After --local --unset, the user-scope value shines through.
"$BINARY" config --local --unset core.build-dir
"$BINARY" config core.build-dir >got
tap_check grep -qx 'user_value' got
tap_done "after --local --unset, get falls through to user"

# --list tags each entry with the scope it came from; only the four
# persistent scopes are ever reported.
"$BINARY" config --list >list
tap_check grep -qE '^default[[:space:]]+core\.build-dir=build$'   list
tap_check grep -qE '^user[[:space:]]+core\.build-dir=user_value$' list
tap_check grep -qE '^user[[:space:]]+core\.generator=from_user$'  list
tap_check ! grep -qE '^(env|cli)[[:space:]]' list
tap_done "--list reports default/user/local/project scope tags only"

tap_result
