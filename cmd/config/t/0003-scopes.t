#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# `ice config`: scope file paths and precedence (defaults < user <
# local < project < env < cli).  Project scope needs a real cmake
# build dir and is exercised separately.

. t/tap.sh
tap_setup

mkdir -p home
export HOME="$PWD/home" USERPROFILE="$PWD/home"
unset ESPPORT ESPBAUD IDF_TARGET

# --user writes to ~/.iceconfig; --local to ./.iceconfig.
"$BINARY" config --user core.generator from_user
"$BINARY" config --local core.build-dir from_local
tap_check test -f "$HOME/.iceconfig"
tap_check test -f .iceconfig
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

# env beats local.  ESPPORT maps to serial.port at env scope.
"$BINARY" config --local serial.port local_port
ESPPORT=env_value "$BINARY" config serial.port >got
tap_check grep -qx 'env_value' got
tap_done "env scope wins over local (ESPPORT -> serial.port)"

# cli beats local.  Global -B sets core.build-dir at CLI scope.
"$BINARY" -B from_cli config core.build-dir >got
tap_check grep -qx 'from_cli' got
tap_done "cli scope wins over local (-B sets core.build-dir)"

# --list tags each entry with the scope it came from.
ESPPORT=env_value "$BINARY" -B from_cli config --list >list
tap_check grep -qE '^default[[:space:]]+core\.build-dir=build$'   list
tap_check grep -qE '^user[[:space:]]+core\.build-dir=user_value$' list
tap_check grep -qE '^env[[:space:]]+serial\.port=env_value$'  list
tap_check grep -qE '^cli[[:space:]]+core\.build-dir=from_cli$'    list
tap_done "--list reports default/user/env/cli scope tags"

tap_result
