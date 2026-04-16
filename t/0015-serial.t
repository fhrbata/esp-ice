#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Compile and run the C unit tests for the serial port backend.
# Uses POSIX pseudo-terminals (posix_openpt) as a stand-in for a real
# serial device, so no hardware is required.  Windows builds have a
# stub backend that returns -ENOSYS -- the tests below are skipped
# there because openpty is a POSIX-ism.

if [ "$S" != "linux" ] && [ "$S" != "macos" ]; then
    echo "1..0 # SKIP serial tests require POSIX ptys (S=$S)"
    exit 0
fi

O="$T_OUT/$(basename "$0" .t)"
rm -rf "$O" && mkdir -p "$O"

$CC -std=c99 $SAN_FLAGS -I. -It -o "$O/test_serial" t/test_serial.c "$LIBICE" $LINK_LIBS || exit 1
cd "$O" && ./test_serial
