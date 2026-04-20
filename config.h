/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file config.h
 * @brief Git-style cascading configuration store.
 *
 * Values come from multiple sources (defaults, config files, project
 * state, env, CLI) stored in a single flat list of entries with scope
 * tracking per entry.  The highest-precedence scope wins for
 * config_get(); multiple entries for the same key are allowed (e.g.
 * for cmake.define).
 *
 * Scope precedence, low -> high:
 *   DEFAULT -> USER -> LOCAL -> PROJECT
 *
 * Environment variables and CLI flags do NOT live in this store -- they
 * seed the per-option C variables directly via the option table (see
 * options.h).
 *
 * Usage:
 *   config_init(&config);
 *   config_set(&config, "core.build-dir", "build", CONFIG_SCOPE_DEFAULT);
 *   ...
 *   const char *dir = config_get("core.build-dir");
 *   ...
 *   config_release(&config);
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

/** Config source, ordered low -> high precedence. */
enum config_scope {
	CONFIG_SCOPE_DEFAULT, /**< Built-in defaults. */
	CONFIG_SCOPE_USER,  /**< ~/.iceconfig (or %USERPROFILE%\.iceconfig). */
	CONFIG_SCOPE_LOCAL, /**< .iceconfig in project root. */
	CONFIG_SCOPE_PROJECT, /**< Auto-derived from CMakeCache, sdkconfig, etc.
			       */
};

/** A single (key, value, scope) entry in the config store. */
struct config_entry {
	char *key;
	char *value;
	enum config_scope scope;
};

/**
 * The config store: a flat list of entries, searched linearly.
 *
 * Strings are duplicated on insert and freed by config_release().
 */
struct config {
	struct config_entry *entries;
	int nr, alloc;
};

/** Static initializer. */
#define CONFIG_INIT {.entries = NULL, .nr = 0, .alloc = 0}

/** Process-wide config store. */
extern struct config config;

/** Initialize a config store to empty (equivalent to CONFIG_INIT). */
void config_init(struct config *c);

/** Free all entries and reset to empty. */
void config_release(struct config *c);

/**
 * @brief Set @p key to @p value at @p scope (single-value semantics).
 *
 * Any existing entries for this (key, scope) pair are removed and
 * replaced with a single new entry.  If none exist, a new entry is
 * appended.  Strings are duplicated.
 */
void config_set(struct config *c, const char *key, const char *value,
		enum config_scope scope);

/**
 * @brief Append @p value for @p key at @p scope (multi-value semantics).
 *
 * Always adds a new entry, even if others exist for the same (key,
 * scope) pair.  Use for naturally multi-valued keys such as
 * cmake.define.  Strings are duplicated.
 */
void config_add(struct config *c, const char *key, const char *value,
		enum config_scope scope);

/**
 * @brief Remove every entry for @p key at @p scope.
 *
 * @return The number of entries removed (0 if none matched).
 */
int config_unset(struct config *c, const char *key, enum config_scope scope);

/**
 * @brief Return the value of @p key from the highest-precedence scope.
 *
 * If multiple entries exist in the winning scope (from config_add),
 * the last one added is returned.  Returns NULL if @p key is not set.
 */
const char *config_get(const char *key);

/**
 * @brief Return the value of @p key from a specific scope.
 *
 * If multiple entries exist for (key, scope), the last one added is
 * returned.  Returns NULL if @p key is not set at @p scope.
 */
const char *config_get_at(const char *key, enum config_scope scope);

/**
 * @brief Collect all entries matching @p key across all scopes.
 *
 * Allocates an array of pointers to internal entries, in insertion
 * order, and stores it in @p *out.  Returns the number of entries
 * (0 if @p key is not set; @p *out is set to NULL).  The caller must
 * free() the outer array but NOT the individual entries.
 */
int config_get_all(const char *key, struct config_entry ***out);

/**
 * @brief Parse @p key as a signed int into @p *out.
 *
 * @return 0 on success, -1 if @p key is not set, -2 on parse error.
 */
int config_get_int(const char *key, int *out);

/**
 * @brief Parse @p key as a boolean into @p *out.
 *
 * Accepts (case-insensitive): true/false, yes/no, on/off, 1/0.
 * An empty value is treated as false.
 *
 * @return 0 on success, -1 if @p key is not set, -2 on parse error.
 */
int config_get_bool(const char *key, int *out);

/**
 * @brief Parse @p s as a boolean into @p *out without consulting the
 *        config store.
 *
 * Same token set as config_get_bool().  Returns 0 on success, -1 if
 * @p s is NULL, -2 on parse error.
 */
int config_parse_bool(const char *s, int *out);

/** Return 1 if @p key is set in any scope, 0 otherwise. */
int config_has(const char *key);

/**
 * @brief Return the scope of the highest-precedence entry for @p key.
 *
 * Dies if @p key is not set; check with config_has() first.
 */
enum config_scope config_source(const char *key);

/** Human-readable name of @p scope ("cli", "env", ...). */
const char *scope_name(enum config_scope scope);

/**
 * @brief Return the user-level config path (~/.iceconfig).
 *
 * On POSIX this is "$HOME/.iceconfig"; on Windows it is
 * "%USERPROFILE%/.iceconfig".  Backed by static storage -- the
 * returned pointer is valid for the lifetime of the process.
 *
 * Returns NULL if the home environment variable is unset or empty;
 * callers should pass the result straight to config_load_file(),
 * which treats NULL as a silent no-op.
 */
const char *user_config_path(void);

/** Return the project-local config path ("./.iceconfig"). */
const char *local_config_path(void);

/**
 * @brief Return the ice home directory (~/.ice).
 *
 * Resolution order:
 *   1. ICE_HOME environment variable
 *   2. "$HOME/.ice" (POSIX) / "%USERPROFILE%/.ice" (Windows)
 *
 * Backed by static storage -- the returned pointer is valid for
 * the lifetime of the process.  Dies if the home environment
 * variable cannot be determined.
 */
const char *ice_home(void);

/**
 * @brief Load an INI-style config file into @p c at @p scope.
 *
 * Grammar:
 *   - "[section]" starts a section; section names allow [A-Za-z0-9_-].
 *   - "key = value" adds an entry as "section.key" via config_add().
 *   - '#' or ';' starts a line comment.
 *   - Values may be double-quoted to preserve internal whitespace.
 *
 * Missing files (ENOENT) and a NULL @p path are silent no-ops: this
 * is normal for optional configs (no ~/.iceconfig, no project-local
 * file).  Other I/O errors are warned and return -1.  Malformed
 * lines are warned with "path:lineno: ..." and parsing continues.
 *
 * @return 0 on success or if the file does not exist, -1 on I/O error.
 */
int config_load_file(struct config *c, enum config_scope scope,
		     const char *path);

/**
 * @brief Parse config content from an in-memory buffer.
 *
 * Same semantics as config_load_file() but reads from @p buf (of
 * @p len bytes).  @p label is used only for diagnostic messages
 * (e.g. "HEAD:.gitmodules").  The buffer is modified in place by
 * sbuf_getline() while walking lines, so the caller must pass a
 * writable buffer it owns.
 */
void config_load_buf(struct config *c, enum config_scope scope,
		     const char *label, char *buf, size_t len);

/**
 * @brief Populate @p c with built-in default values at DEFAULT scope.
 *
 * Currently: core.build-dir=build, core.generator=Ninja,
 * core.verbose=false.
 */
void config_load_defaults(struct config *c);

/**
 * @brief One-line description for a built-in config key, or NULL.
 *
 * Covers the keys seeded by config_load_defaults().  Used by the
 * completion backend to annotate `ice config <TAB>` candidates for
 * keys that are not declared in any option table.
 */
const char *config_builtin_key_help(const char *key);

/**
 * @brief Write all entries at @p scope to @p path as an INI file.
 *
 * Entries are grouped by section (the part of the key before the
 * first '.') and emitted in that order.  Values are double-quoted
 * only when needed (empty, edged with whitespace, or containing a
 * comment character).  Keys without a section prefix are skipped.
 *
 * This is a whole-file rewrite -- comments and blank lines in the
 * existing file are NOT preserved.  Callers that need to preserve
 * user formatting should edit the file by hand.
 *
 * @return 0 on success, -1 on I/O error (errno is set).
 */
int config_write_file(const struct config *c, enum config_scope scope,
		      const char *path);

/**
 * @brief Derive project-scope values from build-directory artifacts.
 *
 * Reads "@p build_dir/CMakeCache.txt" for IDF_TARGET -> `target`, and
 * "@p build_dir/project_description.json" for `project_name` to
 * derive `mapfile = <build>/<name>.map` and `elf = <build>/<name>.elf`.
 *
 * All values are stored at CONFIG_SCOPE_PROJECT.  Missing files, a
 * NULL @p build_dir, or missing keys are silent -- this loader is
 * best-effort and should not complain outside a configured project.
 */
/**
 * @brief Re-load @b{./.iceconfig} into the process-wide config store.
 *
 * Wipes every existing @c CONFIG_SCOPE_LOCAL entry first so re-loads
 * don't accumulate duplicates.  Called by @b{ice init} after writing
 * the file so a subsequent config_load_profile() picks up the freshly
 * persisted @b{[project "<name>"]} entries without restarting ice.
 */
void config_reload_local(void);

/**
 * @brief Promote @b{[project "<name>"]} into a uniform @b{project.X}
 *        namespace at @c CONFIG_SCOPE_PROJECT and derive build-state
 *        from the profile's build directory.
 *
 * The promotion lets every project-aware command read @b{project.X}
 * via config_get() without knowing which profile is active.  After
 * the promotion the function reads the profile's build directory
 * (if configured) and adds:
 *
 *   - @b{project.target}   from @c CMakeCache.txt's @c IDF_TARGET
 *   - @b{project.mapfile}  derived from @c project_description.json
 *   - @b{project.elf}      derived from @c project_description.json
 *
 * Re-runnable: every call clears the previous PROJECT-scope state
 * first.
 */
void config_load_profile(const char *name);

#endif /* CONFIG_H */
