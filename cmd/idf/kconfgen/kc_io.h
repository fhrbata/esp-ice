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
struct svec;

/**
 * @brief Load a sdkconfig.rename mapping file.
 *
 * Lines:
 *   CONFIG_OLD CONFIG_NEW          old name -> new name
 *   CONFIG_OLD !CONFIG_NEW         same, plus bool inversion on load
 *
 * Blank / @c # comment lines are ignored.  Multiple calls accumulate.
 * After loading, kc_load_config() automatically translates any CONFIG_OLD
 * line it sees into a CONFIG_NEW assignment (with value inversion where
 * applicable).
 */
void kc_load_rename(struct kc_ctx *ctx, const char *path);

/**
 * @brief Load @c NAME=VAL entries from an env-file into @p env.
 *
 * Two formats are accepted:
 *   - JSON object ( @c {"NAME": "value", ...} ) -- what ESP-IDF's
 *     cmake writes to @c build/config.env.  Values of type
 *     string / bool / number / null are converted to their
 *     @c NAME=VAL string form; arrays / objects are skipped.
 *   - Plain @c NAME=VAL lines, one per row.  Blank and @c #-comment
 *     lines are ignored.
 *
 * The first non-whitespace byte of the file selects the parser.  The
 * file is additive -- existing @p env entries from earlier calls or
 * command-line @c --env flags are preserved.  Dies on I/O failure or
 * malformed JSON.
 */
void kc_load_env_file(struct svec *env, const char *path);

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

/**
 * @brief Write the resolved symbol table as a C header (sdkconfig.h).
 *
 *   bool  = y    -> #define CONFIG_X 1
 *   bool  = n    -> (omitted; matches python kconfgen)
 *   int / hex    -> #define CONFIG_X value
 *   string       -> #define CONFIG_X "value"  (escaped)
 *   option env=  -> (skipped)
 */
void kc_write_header(const struct kc_ctx *ctx, const char *path);

/**
 * @brief Write the resolved symbol table as a cmake include file.
 *
 *   set(CONFIG_X "value") for every non-env symbol (including bool-y
 *     as "y" and bool-n as ""), followed by a
 *   set(CONFIGS_LIST CONFIG_A;CONFIG_B;...) line enumerating them.
 */
void kc_write_cmake(const struct kc_ctx *ctx, const char *path);

/**
 * @brief Write the resolved symbols as a flat JSON object.
 *
 *   { "FOO": true, "BAR": 7, "BAZ": 255, "NAME": "world", ... }
 *
 * Alphabetically sorted keys, 4-space indent, one key per line --
 * matches the output of @c python -m kconfgen @c --output @c json.
 */
void kc_write_json(const struct kc_ctx *ctx, const char *path);

/**
 * @brief Write the menu structure as a JSON array.
 *
 * Each entry carries @c id / @c name / @c type / @c title / @c range
 * / @c depends_on / @c help / @c children.  @c menu / @c choice nodes
 * nest their members inside @c children; plain @c config entries have
 * an empty @c children array.  Used by @c idf.py menuconfig.
 */
void kc_write_json_menus(const struct kc_ctx *ctx, const char *path);

/**
 * @brief Write a minimal ("savedefconfig") sdkconfig.
 *
 * Only symbols whose current value differs from what the Kconfig
 * @c default properties would compute are emitted.  Bool-n values are
 * written as @c CONFIG_X=n (rather than the @c # CONFIG_X @c is @c not @c set
 * form the full config uses), matching python kconfgen's @c normalize_unset
 * path.  When @c ESP_IDF_KCONFIG_MIN_LABELS=1 is set in the environment,
 * @c # Menu @c Name / @c # end @c of @c Menu @c Name markers bracket groups of
 * non-default symbols.  If the current @c IDF_TARGET differs from the
 * default "esp32" its assignment is prepended to the body so the file
 * round-trips through @c kc_load_config cleanly.
 */
void kc_write_min_config(const struct kc_ctx *ctx, const char *path);

#endif /* KC_IO_H */
