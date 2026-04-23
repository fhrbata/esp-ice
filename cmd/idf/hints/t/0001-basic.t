#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test for `ice idf hints`: covers plain rules, match_to_output,
# variables expansion, and PCRE2-specific features (negative lookahead).

. t/tap.sh
tap_setup

# ---- Build a test hints.yml covering every rule shape ----

cat >hints.yml <<'YAML'
-
    re: "error: implicit declaration of function 'esp_foo'"
    hint: "esp_foo was removed. Use esp_bar instead."

-
    re: "fatal error: (spiram.h|esp_spiram.h): No such file or directory"
    hint: "{} was removed. Include esp_psram.h instead."
    match_to_output: True

-
    re: "error: implicit declaration of function '{}'"
    hint: "Function '{}' has been removed. Please use {}."
    variables:
        -
            re_variables: ['old_alpha']
            hint_variables: ['old_alpha()', 'new_alpha()']
        -
            re_variables: ['old_beta']
            hint_variables: ['old_beta()', 'new_beta()']

# Negative lookahead (PCRE-only) -- exercises that we're not on POSIX regex.
-
    re: "Failed to resolve component '(?!esp_ipc|newlib)(\\w+)'"
    hint: "Unknown component '{}' -- check spelling."
    match_to_output: True
YAML

# ---- Case 1: plain rule ----

cat >log1 <<'LOG'
src/main.c:20: error: implicit declaration of function 'esp_foo'
LOG
"$BINARY" idf hints hints.yml log1 >out1 2>err1
tap_check grep -qx 'HINT: esp_foo was removed. Use esp_bar instead.' out1
tap_done "plain rule matches and prints hint"

# ---- Case 2: match_to_output injects captured group into hint ----

cat >log2 <<'LOG'
src/main.c:30: fatal error: spiram.h: No such file or directory
LOG
"$BINARY" idf hints hints.yml log2 >out2 2>err2
tap_check grep -qx 'HINT: spiram.h was removed. Include esp_psram.h instead.' out2
tap_done "match_to_output substitutes the captured group"

# ---- Case 3: variables list -- only the matching entry fires ----

cat >log3 <<'LOG'
src/foo.c:1: error: implicit declaration of function 'old_beta'
LOG
"$BINARY" idf hints hints.yml log3 >out3 2>err3
tap_check grep -qx "HINT: Function 'old_beta()' has been removed. Please use new_beta()." out3
tap_check ! grep -q 'old_alpha' out3
tap_done "variables list: only the matching variable entry emits a hint"

# ---- Case 4: PCRE2 negative lookahead ----

cat >log4 <<'LOG'
Failed to resolve component 'mycomp'
LOG
"$BINARY" idf hints hints.yml log4 >out4 2>err4
tap_check grep -qx "HINT: Unknown component 'mycomp' -- check spelling." out4
tap_done "PCRE2 negative lookahead matches as expected"

# ---- Case 5: excluded by negative lookahead ----

cat >log5 <<'LOG'
Failed to resolve component 'esp_ipc'
LOG
"$BINARY" idf hints hints.yml log5 >out5 2>err5
tap_check ! grep -q '^HINT:' out5
tap_done "negative lookahead excludes matching alternatives"

# ---- Case 6: no matches -> no hint output ----

cat >log6 <<'LOG'
perfectly normal log line
LOG
"$BINARY" idf hints hints.yml log6 >out6 2>err6
tap_check ! grep -q '^HINT:' out6
tap_done "log with no known patterns produces no hints"

# ---- Case 7: non-existent hints file is fatal ----

tap_check ! "$BINARY" idf hints /no/such/hints.yml log1 2>/dev/null
tap_done "missing hints file exits non-zero"

# ---- Case 8: non-existent log file is fatal ----

tap_check ! "$BINARY" idf hints hints.yml /no/such/log 2>/dev/null
tap_done "missing log file exits non-zero"

tap_result
