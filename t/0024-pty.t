#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Compile and run the C unit tests for the pty primitive.

O="$T_OUT/$(basename "$0" .t)"
rm -rf "$O" && mkdir -p "$O"

if [ "$S" = "win" ]; then
	echo "1..0 # SKIP pty test is POSIX-only (Windows uses ConPTY, tested separately)"
	exit 0
fi

$CC -std=c99 $SAN_FLAGS -I. -It -o "$O/test_pty" t/test_pty.c "$LIBICE" $LINK_LIBS || exit 1
cd "$O" && ./test_pty
