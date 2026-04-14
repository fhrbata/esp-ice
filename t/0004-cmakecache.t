#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Compile and run the C unit tests for cmakecache.

O="$T_OUT/$(basename "$0" .t)"
rm -rf "$O" && mkdir -p "$O"

if [ "$S" = "win" ]; then
	case "$(uname -s 2>/dev/null)" in
	MINGW* | MSYS* | CYGWIN*) ;;
	*)
		echo "1..0 # SKIP S=win binary cannot run on $(uname -s)"
		exit 0
		;;
	esac
fi

$CC -std=c99 -I. -It -o "$O/test_cmakecache" t/test_cmakecache.c "$LIBICE" || exit 1
cd "$O" && ./test_cmakecache
