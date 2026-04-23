/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file yaml.h
 * @brief Small in-house YAML DOM reader.
 *
 * A single struct yaml_value serves as the output of yaml_parse().
 * Values are heap-allocated; yaml_free() recursively frees the whole
 * tree.
 *
 * Usage:
 *   struct yaml_value *root = yaml_parse(buf, len);
 *   const char *s = yaml_as_string(yaml_get(root, "key"));
 *   yaml_free(root);
 *
 * Scope limits (v1 -- tailored to hints.yml and similar config-like
 * documents, not a full YAML 1.2 implementation):
 *   - block mappings with indentation,
 *   - block sequences ('- item'),
 *   - flow sequences ('[a, b, c]'),
 *   - single- and double-quoted scalars with common escape sequences,
 *   - unquoted scalars (plain),
 *   - booleans: True/False/true/false,
 *   - comments: '#' to end of line.
 *
 * Not supported: anchors (&), aliases (*), tags, multi-line scalars
 * ('|'/'>'), flow mappings ('{...}'), merge keys.  All unsupported
 * constructs produce a parse error (yaml_parse returns NULL).
 */
#ifndef YAML_H
#define YAML_H

#include <stddef.h>

enum yaml_type {
	YAML_NULL,
	YAML_BOOL,
	YAML_STRING,
	YAML_SEQ,
	YAML_MAP,
};

struct yaml_value;

struct yaml_member {
	char *key;
	struct yaml_value *value;
};

struct yaml_value {
	enum yaml_type type;
	union {
		int boolean;
		char *string;
		struct {
			struct yaml_value **items;
			int nr, alloc;
		} seq;
		struct {
			struct yaml_member *members;
			int nr, alloc;
		} map;
	} u;
};

/**
 * @brief Parse a YAML document.
 *
 * @p buf does not need to be NUL-terminated; @p len bounds the input.
 * Returns NULL on parse error or allocation failure.  The caller takes
 * ownership and must eventually call yaml_free().
 */
struct yaml_value *yaml_parse(const char *buf, size_t len);

/** Recursively free a value and everything it owns. */
void yaml_free(struct yaml_value *y);

/*
 * Type queries and accessors.
 *
 * Accessors tolerate NULL and wrong-type inputs: they return NULL or 0
 * rather than dying.  Callers who need to distinguish "missing" from
 * "present but empty" can check yaml_type() first.
 */

/** Type of @p y, or YAML_NULL if @p y is NULL. */
enum yaml_type yaml_type(const struct yaml_value *y);

const char *yaml_as_string(const struct yaml_value *y);
int yaml_as_bool(const struct yaml_value *y);

/**
 * @brief Look up @p key in a YAML mapping.
 *
 * @return The member value, or NULL if @p obj is not a mapping or the
 *         key is not present.  Caller does not own the result.
 */
struct yaml_value *yaml_get(const struct yaml_value *obj, const char *key);

/** Number of items in a YAML sequence (0 if not a sequence). */
int yaml_seq_size(const struct yaml_value *seq);

/** Item at @p idx in a YAML sequence, or NULL if out of range or not a
 * sequence. */
struct yaml_value *yaml_seq_at(const struct yaml_value *seq, int idx);

#endif /* YAML_H */
