#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Compile and run unit tests for zip.c.  Fixture archives are built
# on the fly with the system `zip` utility so we don't check binary
# blobs into the repo; the Python fallback covers hosts without
# Info-ZIP.

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

make_zip_python () {
	python3 - "$@" <<'PY'
import sys, zipfile
out, method = sys.argv[1], sys.argv[2]
files = [tuple(a.split(':', 1)) for a in sys.argv[3:]]
mode = zipfile.ZIP_STORED if method == 'store' else zipfile.ZIP_DEFLATED
with zipfile.ZipFile(out, 'w', mode) as z:
    for arc, path in files:
        z.write(path, arc)
PY
}

(
	cd "$O" || exit 1

	mkdir -p src/sub
	printf 'hello\n' >src/hello.txt
	printf 'nested\n' >src/sub/nested.txt

	# Happy-path archive: DEFLATE-compressed, contains a top-level
	# file and a nested one.  Prefer the system `zip` if present;
	# fall back to Python's zipfile module otherwise.
	if command -v zip >/dev/null 2>&1; then
		(cd src && zip -q -r ../basic.zip hello.txt sub/nested.txt)
	else
		make_zip_python basic.zip deflate \
			"hello.txt:src/hello.txt" \
			"sub/nested.txt:src/sub/nested.txt"
	fi

	# Zip-slip fixtures: entries whose names contain "../" or a
	# backslash component.  `zip` itself won't write such entries,
	# so we always use Python here.
	printf 'do-not-extract\n' >escape
	python3 - <<'PY'
import zipfile
with zipfile.ZipFile('slip.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.write('escape', '../escape')
with zipfile.ZipFile('slip_bs.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('..\\escape_bs', 'do-not-extract\n')
PY
	rm -f escape

	# Garbage file that is not a ZIP.
	printf 'this is not a zip archive' >garbage.bin
) || exit 1

$CC -std=c99 $SAN_FLAGS -I. -It \
	-o "$O/test_zip" t/test_zip.c "$LIBICE" $LINK_LIBS || exit 1
cd "$O" && ./test_zip
