/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_io.h
 * @brief sdkconfig load / write (Makefile-style CONFIG_X=value format).
 *
 * Used by `ice idf kconfgen`:
 *
 *   kc_load_config() parses a Makefile-style config file and stamps
 *   every matching symbol as user-set via kc_sym_set_user().  Calls
 *   from multiple files stack (later wins) -- that's how --defaults
 *   and --config layer.  The loader is tolerant: unknown CONFIG_*
 *   keys are silently ignored, matching python kconfgen's behaviour.
 *
 *   kc_write_config() walks the resolved symbol list in appearance
 *   order and writes a canonical sdkconfig: `CONFIG_X=value` for set
 *   values, `# CONFIG_X is not set` for bool-n, hex preserving 0x,
 *   strings quoted-and-escaped, float as %g.  Symbols marked with
 *   `option env="VAR"` are skipped (their values live in the
 *   environment, not the config file).
 *
 * Both functions operate on paths.  The writer uses write_file_atomic
 * so the build system observes the full file or nothing.
 */
#ifndef KC_IO_H
#define KC_IO_H

struct kc_ctx;

/**
 * @brief Load a Makefile-style sdkconfig / defaults file.
 *
 * Recognised line shapes:
 *   CONFIG_NAME=y             -> set symbol NAME to "y"
 *   CONFIG_NAME=n             -> set symbol NAME to "n"
 *   CONFIG_NAME=42            -> "42"
 *   CONFIG_NAME=0x1a          -> "0x1a"
 *   CONFIG_NAME="quoted"      -> "quoted" (backslash-escapes decoded)
 *   # CONFIG_NAME is not set  -> set symbol NAME to "n"
 *   <blank>                   -> skipped
 *   # ...                     -> comment, skipped
 *   CONFIG_NAME=              -> set symbol NAME to "n" (bool shortcut)
 *
 * Missing files die with errno.  Unknown CONFIG_ keys are silently
 * ignored (so stale or forward-compatible defaults don't block the
 * build).
 */
void kc_load_config(struct kc_ctx *ctx, const char *path);

/**
 * @brief Write the resolved symbol table as an sdkconfig file.
 *
 * Written atomically via write_file_atomic() so partial-write crashes
 * leave the previous content in place.
 */
void kc_write_config(const struct kc_ctx *ctx, const char *path);

#endif /* KC_IO_H */
