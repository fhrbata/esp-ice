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

# Wipe the three fixture-controlled vars so a stray host setting can't
# leak into macro / source resolution.  We can't `env -i` the whole
# environment because the Windows DLL loader needs PATH to find the
# binary's dependencies (system DLLs, the bundled libcurl / pcre2,
# ...), so do a targeted unset instead.
unset MAX_NUMBER_OF_MOTORS TEST_ENV_SET TEST_FILE_PREFIX

for in_file in "$FIXTURES"/*.in; do
	name="$(basename "$in_file" .in)"
	out_file="$FIXTURES/$name.out"
	actual="$name.sdkconfig"

	# Per-fixture env-var overrides.  Inline `VAR=value cmd` exports
	# only for the single invocation, then disappears -- no need to
	# unset between iterations.
	case "$name" in
	EnvironmentVariable)
		MAX_NUMBER_OF_MOTORS=4 \
			"$BINARY" idf kconfgen --kconfig "$in_file" \
			--output "config:$actual" >/dev/null 2>/dev/null
		;;
	SeveralConfigs)
		TEST_ENV_SET=y \
			"$BINARY" idf kconfgen --kconfig "$in_file" \
			--output "config:$actual" >/dev/null 2>/dev/null
		;;
	Source)
		TEST_FILE_PREFIX="$SOURCING" \
			"$BINARY" idf kconfgen --kconfig "$in_file" \
			--output "config:$actual" >/dev/null 2>/dev/null
		;;
	*)
		"$BINARY" idf kconfgen --kconfig "$in_file" \
			--output "config:$actual" >/dev/null 2>/dev/null
		;;
	esac
	tap_check diff -u "$out_file" "$actual"
	tap_done "$name"
done

tap_result
