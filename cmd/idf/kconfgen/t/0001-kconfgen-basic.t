#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end tests for `ice idf kconfgen`: exercise the full pipeline
# (lex -> parse -> eval -> emit) against small Kconfig fixtures and
# verify the sdkconfig / header outputs byte-for-byte.

. t/tap.sh
tap_setup

# ---- Minimal Kconfig: one bool with a prompt and default ------------

cat >minimal.kconfig <<'EOF'
mainmenu "Test"
config MY_FLAG
	bool "My flag"
	default y
EOF

"$BINARY" idf kconfgen --kconfig minimal.kconfig --output config:out.sdkconfig >stdout 2>stderr
tap_check test $? -eq 0
tap_check grep -q '^CONFIG_MY_FLAG=y$' out.sdkconfig
tap_check grep -q '^Status: Finished successfully$' stderr
tap_done "minimal config emits CONFIG_MY_FLAG=y"

# ---- C header output -----------------------------------------------

"$BINARY" idf kconfgen --kconfig minimal.kconfig --output header:out.h >/dev/null 2>&1
tap_check grep -q '^#define CONFIG_MY_FLAG 1$' out.h
tap_done "header output defines CONFIG_MY_FLAG 1"

# ---- Existing sdkconfig overrides Kconfig default ------------------

cat >existing.sdkconfig <<'EOF'
# CONFIG_MY_FLAG is not set
EOF

"$BINARY" idf kconfgen --kconfig minimal.kconfig --config existing.sdkconfig \
	--output config:out2.sdkconfig >/dev/null 2>&1
tap_check grep -q '^# CONFIG_MY_FLAG is not set$' out2.sdkconfig
tap_done "user-set n overrides default y"

# ---- Type validation: bad int value is rejected with a warning -----

cat >int.kconfig <<'EOF'
mainmenu "Test"
config MY_INT
	int "My int"
	default 42
EOF

cat >bad.sdkconfig <<'EOF'
CONFIG_MY_INT=notanumber
EOF

"$BINARY" idf kconfgen --kconfig int.kconfig --config bad.sdkconfig \
	--output config:out3.sdkconfig >stdout 2>stderr
# Warning goes to stderr via the deferred report.
tap_check grep -q "not a valid int for CONFIG_MY_INT" stderr
tap_check grep -q '^CONFIG_MY_INT=42$' out3.sdkconfig
tap_check grep -q '^Status: Finished with notifications$' stderr
tap_done "bad int value is rejected, default preserved, warning emitted"

# ---- Rename table: old name is translated to new -------------------

cat >renames.kconfig <<'EOF'
mainmenu "Test"
config NEW_NAME
	bool "New name"
	default n
EOF

cat >rename.map <<'EOF'
CONFIG_OLD_NAME CONFIG_NEW_NAME
EOF

cat >old.sdkconfig <<'EOF'
CONFIG_OLD_NAME=y
EOF

"$BINARY" idf kconfgen --kconfig renames.kconfig --config old.sdkconfig \
	--sdkconfig-rename rename.map \
	--output config:out4.sdkconfig >/dev/null 2>&1
tap_check grep -q '^CONFIG_NEW_NAME=y$' out4.sdkconfig
tap_done "rename map translates CONFIG_OLD_NAME -> CONFIG_NEW_NAME"

# ---- Help output: rsource expansion -------------------------------

cat >child.kconfig <<'EOF'
config CHILD_FLAG
	bool "Child flag"
	default y
EOF

cat >parent.kconfig <<'EOF'
mainmenu "Parent"
rsource "child.kconfig"
EOF

"$BINARY" idf kconfgen --kconfig parent.kconfig --output config:out5.sdkconfig >/dev/null 2>&1
tap_check grep -q '^CONFIG_CHILD_FLAG=y$' out5.sdkconfig
tap_done "rsource pulls in child Kconfig"

# ---- Promptless derived bool re-evaluates after choice change ------
# Regression: when sdkconfig carries a stale `CONFIG_DERIVED=y` from a
# previous solve and the choice winner has since changed, the loader
# must not seed it as user_set. Otherwise the evaluator's stick rule
# freezes the value and the `default y if ...` chain never re-fires.
# Mirrors the bootloader_console_init duplicate-definition bug seen
# in esp-idf when ESP_CONSOLE_USB_SERIAL_JTAG was selected but the
# stale derived ESP_CONSOLE_UART=y survived the round-trip.

cat >derived.kconfig <<'EOF'
mainmenu "Test"
choice
	prompt "Pick one"
	default A_DEFAULT
	config A_DEFAULT
		bool "A"
	config B_DEFAULT
		bool "B"
endchoice
config DERIVED
	bool
	default y if A_DEFAULT
EOF

cat >stale.sdkconfig <<'EOF'
# CONFIG_A_DEFAULT is not set
CONFIG_B_DEFAULT=y
CONFIG_DERIVED=y
EOF

"$BINARY" idf kconfgen --kconfig derived.kconfig --config stale.sdkconfig \
	--output config:out6.sdkconfig >/dev/null 2>&1
tap_check ! grep -q '^CONFIG_DERIVED=y$' out6.sdkconfig
tap_check grep -q '^CONFIG_B_DEFAULT=y$' out6.sdkconfig
tap_done "promptless derived bool re-evaluates after choice change"

tap_result
