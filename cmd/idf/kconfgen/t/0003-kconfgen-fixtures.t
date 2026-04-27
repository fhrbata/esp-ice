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

# Copy the fixtures into a per-test working dir before tap_setup cd's
# us into it, so every path we hand to kconfgen below is relative.
# A native Windows ice.exe doesn't understand MSYS-style absolute
# paths (/d/a/...) -- relative paths work everywhere.
SRC="$(pwd)/cmd/idf/kconfgen/t/fixtures/ok"

. t/tap.sh
tap_setup

cp -R "$SRC" .

# Wipe the three fixture-controlled vars so a stray host setting can't
# leak into macro / source resolution.  We can't `env -i` the whole
# environment because the Windows DLL loader needs PATH to find the
# binary's dependencies.
unset MAX_NUMBER_OF_MOTORS TEST_ENV_SET TEST_FILE_PREFIX

cd ok

for in_file in *.in; do
	name="${in_file%.in}"
	out_file="$name.out"
	actual="$name.actual"

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
		TEST_FILE_PREFIX=kconfigs_for_sourcing \
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
