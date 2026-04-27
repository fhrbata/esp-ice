#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Drive `ice idf kconfgen` over the bundled cmd/idf/kconfgen/t/fixtures/ok
# samples and diff each generated sdkconfig against the matching golden
# .out file.  The fixtures were ported from esp-idf-kconfig, so a
# byte-identical match is the load-bearing guarantee for python-parity.

FIXTURES="$(pwd)/cmd/idf/kconfgen/t/fixtures/ok"
SOURCING="$FIXTURES/kconfigs_for_sourcing"

. t/tap.sh
tap_setup

# A handful of fixtures expand environment variables -- list them
# here.  Anything not listed runs with a clean env so a stray host
# variable can't leak into the macro / source resolution.
fixture_env () {
	case "$1" in
	EnvironmentVariable) echo "MAX_NUMBER_OF_MOTORS=4" ;;
	SeveralConfigs)      echo "TEST_ENV_SET=y" ;;
	Source)              echo "TEST_FILE_PREFIX=$SOURCING" ;;
	*)                   echo "" ;;
	esac
}

for in_file in "$FIXTURES"/*.in; do
	name="$(basename "$in_file" .in)"
	out_file="$FIXTURES/$name.out"
	actual="$name.sdkconfig"

	# shellcheck disable=SC2046  # word-splitting the env list is intended
	env -i $(fixture_env "$name") \
		"$BINARY" idf kconfgen --kconfig "$in_file" \
		--output "config:$actual" >/dev/null 2>/dev/null
	tap_check diff -u "$out_file" "$actual"
	tap_done "$name"
done

tap_result
