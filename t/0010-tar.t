#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Compile and run the C unit tests for tar extraction.

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

# Build three fixture archives with known content under $O: a top-level
# file, a nested file, and a relative symlink.  The three archives
# exercise plain / gzip / xz paths in tar.c respectively.
(
	cd "$O" || exit 1
	mkdir -p src/sub
	printf 'hello\n' >src/hello.txt
	printf 'nested\n' >src/sub/nested.txt
	ln -sf ../hello.txt src/sub/link.txt

	tar -cf  test.tar    -C src .
	tar -czf test.tar.gz -C src .
	tar -cJf test.tar.xz -C src .
) || exit 1

# Build an archive containing an entry with a ../ component to exercise
# the tar-slip guard.  We stage a file called "escape" and then fake up
# the archive path; `tar --transform` (GNU tar) is the easiest portable
# way to do this but BSD tar doesn't support it, so craft the payload
# with a small script that runs tar and rewrites the header on the fly.
#
# Simpler: just build an archive where a top-level file is explicitly
# named with leading "../" via --transform.  Skip on BSD tar.
(
	cd "$O" || exit 1
	printf 'do-not-extract\n' >escape
	if tar --version 2>/dev/null | grep -q 'GNU tar'; then
		tar -cf slip.tar --transform 's,^escape,../escape,' escape
	else
		# BSD tar: build the archive from stdin with a crafted name.
		tar -cf slip.tar -s ',^escape,../escape,' escape
	fi
	rm -f escape
) || exit 1

$CC -std=c99 $SAN_FLAGS -I. -It \
	-o "$O/test_tar" t/test_tar.c "$LIBICE" $LINK_LIBS || exit 1
cd "$O" && ./test_tar
