#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Compile and run the C unit tests for the image create engine.

O="$T_OUT/$(basename "$0" .t)"
rm -rf "$O" && mkdir -p "$O"

$CC -std=c99 $SAN_FLAGS -I. -It -o "$O/test_image_create" t/test_image_create.c "$LIBICE" || exit 1
cd "$O" && ./test_image_create
