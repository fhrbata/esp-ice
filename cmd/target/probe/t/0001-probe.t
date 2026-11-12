#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end tests for `ice target probe`.  The ports differ from host
# to host, so the checks hold whether a board is attached or not.

. t/tap.sh
tap_setup

"$BINARY" target probe >out 2>err
rc=$?
if [ $rc -eq 0 ]; then
	tap_check test -s out
	# Every line: path, then "VID:PID" in lowercase hex or "-".
	tap_check test -z "$(grep -Ev '^[^ ]+ +([0-9a-f]{4}:[0-9a-f]{4}|-)( |$)' out)"
else
	tap_check test $rc -eq 1
	tap_check test ! -s out
	tap_check grep -q 'no serial ports found' err
fi
tap_done "lists ports, or exits 1 with an empty stdout"

"$BINARY" target probe extra >out 2>err
tap_check test $? -ne 0
tap_check grep -q 'too many arguments' err
tap_done "rejects positional arguments"

"$BINARY" target --help >out 2>&1
tap_check grep -q 'probe' out
tap_done "listed under ice target"

tap_result
