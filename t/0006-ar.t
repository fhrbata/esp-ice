#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Compile and run the C unit tests for ar.

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

# Build a tiny AR archive under $O with two members that have known
# sizes; test_ar.c cd's into $O and reads it by relative path.
(
	cd "$O" || exit 1
	printf 'contents of foo' >foo.txt
	printf 'file bar contents' >bar.txt
	ar rcs test.a foo.txt bar.txt
) || exit 1

$CC -std=c99 $SAN_FLAGS -I. -It -o "$O/test_ar" t/test_ar.c "$LIBICE" $LINK_LIBS || exit 1
cd "$O" && ./test_ar
