#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Compile and run the C unit tests for elf.

O="$T_OUT/$(basename "$0" .t)"
rm -rf "$O" && mkdir -p "$O"

# $CC -c produces an object in the host's native format: ELF on Linux,
# Mach-O on macOS, COFF on MinGW/Windows.  Only the first gives the
# elf parser something real to chew on, so run only on S=linux.
if [ "$S" != "linux" ]; then
	echo "1..0 # SKIP elf test requires an ELF-producing toolchain (S=$S)"
	exit 0
fi

# Compile a small translation unit to produce a realistic fixture
# that has .text, .data, and .bss sections.  The same source is also
# linked as a host executable to give the segment reader a real ELF
# with PT_LOAD program headers to chew on.
cat >"$O/stub.c" <<'EOF'
int initialised = 42;
char big_array[1000];
int fn(int x) { return x + 1; }
int main(void) { return fn(0) + initialised + (int)big_array[0]; }
EOF
$CC -c "$O/stub.c" -o "$O/stub.o" || exit 1
$CC       "$O/stub.c" -o "$O/stub.elf" || exit 1

$CC -std=c99 $SAN_FLAGS -I. -It -o "$O/test_elf" t/test_elf.c "$LIBICE" $LINK_LIBS || exit 1
cd "$O" && ./test_elf
