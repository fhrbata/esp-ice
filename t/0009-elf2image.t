#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Compile and run the C unit tests for `ice elf2image`.

O="$T_OUT/$(basename "$0" .t)"
rm -rf "$O" && mkdir -p "$O"

$CC -std=c99 $SAN_FLAGS -I. -It \
    -o "$O/test_elf2image" t/test_elf2image.c "$LIBICE" || exit 1

cd "$O" && ./test_elf2image
