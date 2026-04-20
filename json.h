/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file json.h
 * @brief Small in-house JSON DOM reader/writer.
 *
 * A single struct json_value serves both as the output of json_parse()
 * and the input to json_serialize().  Values are heap-allocated;
 * json_set() and json_push() transfer ownership into the container,
 * and json_free() recursively frees the whole tree.
 *
 * Usage -- reading:
 *   struct json_value *root = json_parse(buf, len);
 *   const char *name = json_as_string(json_get(root, "project_name"));
 *   json_free(root);
 *
 * Usage -- writing:
 *   struct json_value *o = json_new_object();
 *   json_set(o, "target", json_new_string("esp32s3"));
 *   json_set(o, "size",   json_new_number(4 * 1024 * 1024));
 *   struct sbuf out = SBUF_INIT;
 *   json_serialize(o, &out);
 *   ...
 *   json_free(o);
 *   sbuf_release(&out);
 *
 * Scope limits for v1: no \uXXXX escapes (parse fails on them), no
 * pretty-printing (compact output only), numbers are always doubles.
 * Extend as needed.
 */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>

struct sbuf; /* forward declaration — defined in sbuf.h */

enum json_type {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT,
};

struct json_value;

struct json_member {
	char *key;
	struct json_value *value;
};

struct json_value {
	enum json_type type;
	union {
		int boolean;
		double number;
		char *string;
		struct {
			struct json_value **items;
			int nr, alloc;
		} array;
		struct {
			struct json_member *members;
			int nr, alloc;
		} object;
	} u;
};

/**
 * @brief Parse a JSON document.
 *
 * @p buf must be NUL-terminated (number parsing uses strtod).  Returns
 * NULL on parse error or allocation failure.  The caller takes
 * ownership and must eventually call json_free().
 */
struct json_value *json_parse(const char *buf, size_t len);

/** Recursively free a value and everything it owns. */
void json_free(struct json_value *j);

/*
 * Type queries and accessors.
 *
 * Accessors tolerate NULL and wrong-type inputs: they return NULL,
 * 0, or 0.0 rather than dying.  Callers who need to distinguish
 * "missing" from "present but empty" can check json_type() first.
 */

/** Type of @p j, or JSON_NULL if @p j is NULL. */
enum json_type json_type(const struct json_value *j);

const char *json_as_string(const struct json_value *j);
double json_as_number(const struct json_value *j);
int json_as_bool(const struct json_value *j);

/**
 * @brief Look up @p key in a JSON object.
 *
 * @return The member value, or NULL if @p obj is not an object or the
 *         key is not present.  Caller does not own the result.
 */
struct json_value *json_get(const struct json_value *obj, const char *key);

/** Number of items in a JSON array (0 if not an array). */
int json_array_size(const struct json_value *arr);

/** Item at @p idx in a JSON array, or NULL if out of range or not an array. */
struct json_value *json_array_at(const struct json_value *arr, int idx);

/* Construction. */
struct json_value *json_new_null(void);
struct json_value *json_new_bool(int b);
struct json_value *json_new_number(double n);
struct json_value *json_new_string(const char *s);
struct json_value *json_new_array(void);
struct json_value *json_new_object(void);

/**
 * @brief Set @p obj[@p key] = @p value (ownership transferred).
 *
 * Overwrites any existing entry with the same key.  Dies if @p obj is
 * not an object.
 */
void json_set(struct json_value *obj, const char *key,
	      struct json_value *value);

/**
 * @brief Append @p value to @p arr (ownership transferred).
 *
 * Dies if @p arr is not an array.
 */
void json_push(struct json_value *arr, struct json_value *value);

/** Serialize @p j into @p out in compact form (no whitespace). */
void json_serialize(const struct json_value *j, struct sbuf *out);

#endif /* JSON_H */
