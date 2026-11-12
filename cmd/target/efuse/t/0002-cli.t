#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Command-line wiring of `ice target efuse`; no chip needed.

. t/tap.sh
tap_setup

"$BINARY" target --help >out 2>&1
tap_check grep -q 'efuse' out
tap_done "listed under ice target"

"$BINARY" target efuse summary --help >out 2>&1
tap_check grep -q -- '--no-stub' out
tap_check grep -q -- '--port' out
tap_done "summary takes the common device options"

"$BINARY" target efuse dump --help >out 2>&1
tap_check grep -q -- '--port' out
tap_done "dump takes the common device options"

"$BINARY" target efuse dump extra >out 2>err
tap_check test $? -ne 0
tap_check grep -q 'too many arguments' err
tap_done "dump rejects positional arguments"

tap_result
