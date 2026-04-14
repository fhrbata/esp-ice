/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file json.c
 * @brief Small in-house JSON DOM reader/writer implementation.
 */
#include "ice.h"

/* ------------------------------------------------------------------ */
/*  Construction                                                      */
/* ------------------------------------------------------------------ */

static struct json_value *make(enum json_type t)
{
	struct json_value *j = calloc(1, sizeof(*j));

	if (!j)
		die_errno("calloc");
	j->type = t;
	return j;
}

struct json_value *json_new_null(void) { return make(JSON_NULL); }
struct json_value *json_new_array(void) { return make(JSON_ARRAY); }
struct json_value *json_new_object(void) { return make(JSON_OBJECT); }

struct json_value *json_new_bool(int b)
{
	struct json_value *j = make(JSON_BOOL);

	j->u.boolean = b ? 1 : 0;
	return j;
}

struct json_value *json_new_number(double n)
{
	struct json_value *j = make(JSON_NUMBER);

	j->u.number = n;
	return j;
}

struct json_value *json_new_string(const char *s)
{
	struct json_value *j = make(JSON_STRING);

	j->u.string = sbuf_strdup(s ? s : "");
	return j;
}

void json_set(struct json_value *obj, const char *key, struct json_value *value)
{
	if (obj->type != JSON_OBJECT)
		die("json_set: not an object");

	for (int i = 0; i < obj->u.object.nr; i++) {
		if (!strcmp(obj->u.object.members[i].key, key)) {
			json_free(obj->u.object.members[i].value);
			obj->u.object.members[i].value = value;
			return;
		}
	}

	ALLOC_GROW(obj->u.object.members, obj->u.object.nr + 1,
		   obj->u.object.alloc);
	obj->u.object.members[obj->u.object.nr].key = sbuf_strdup(key);
	obj->u.object.members[obj->u.object.nr].value = value;
	obj->u.object.nr++;
}

void json_push(struct json_value *arr, struct json_value *value)
{
	if (arr->type != JSON_ARRAY)
		die("json_push: not an array");

	ALLOC_GROW(arr->u.array.items, arr->u.array.nr + 1, arr->u.array.alloc);
	arr->u.array.items[arr->u.array.nr++] = value;
}

/* ------------------------------------------------------------------ */
/*  Release                                                           */
/* ------------------------------------------------------------------ */

void json_free(struct json_value *j)
{
	if (!j)
		return;

	switch (j->type) {
	case JSON_STRING:
		free(j->u.string);
		break;
	case JSON_ARRAY:
		for (int i = 0; i < j->u.array.nr; i++)
			json_free(j->u.array.items[i]);
		free(j->u.array.items);
		break;
	case JSON_OBJECT:
		for (int i = 0; i < j->u.object.nr; i++) {
			free(j->u.object.members[i].key);
			json_free(j->u.object.members[i].value);
		}
		free(j->u.object.members);
		break;
	case JSON_NULL:
	case JSON_BOOL:
	case JSON_NUMBER:
		break;
	}
	free(j);
}

/* ------------------------------------------------------------------ */
/*  Accessors                                                         */
/* ------------------------------------------------------------------ */

enum json_type json_type(const struct json_value *j)
{
	return j ? j->type : JSON_NULL;
}

const char *json_as_string(const struct json_value *j)
{
	return (j && j->type == JSON_STRING) ? j->u.string : NULL;
}

double json_as_number(const struct json_value *j)
{
	return (j && j->type == JSON_NUMBER) ? j->u.number : 0.0;
}

int json_as_bool(const struct json_value *j)
{
	return (j && j->type == JSON_BOOL) ? j->u.boolean : 0;
}

struct json_value *json_get(const struct json_value *obj, const char *key)
{
	if (!obj || obj->type != JSON_OBJECT)
		return NULL;

	for (int i = 0; i < obj->u.object.nr; i++) {
		if (!strcmp(obj->u.object.members[i].key, key))
			return obj->u.object.members[i].value;
	}
	return NULL;
}

int json_array_size(const struct json_value *arr)
{
	return (arr && arr->type == JSON_ARRAY) ? arr->u.array.nr : 0;
}

struct json_value *json_array_at(const struct json_value *arr, int idx)
{
	if (!arr || arr->type != JSON_ARRAY)
		return NULL;
	if (idx < 0 || idx >= arr->u.array.nr)
		return NULL;
	return arr->u.array.items[idx];
}

/* ------------------------------------------------------------------ */
/*  Parser                                                            */
/* ------------------------------------------------------------------ */

struct parser {
	const char *cur;
	const char *end;
};

static struct json_value *parse_value(struct parser *ps);

static void skip_ws(struct parser *ps)
{
	while (ps->cur < ps->end && (*ps->cur == ' ' || *ps->cur == '\t' ||
				     *ps->cur == '\n' || *ps->cur == '\r'))
		ps->cur++;
}

static struct json_value *parse_string(struct parser *ps)
{
	struct sbuf sb = SBUF_INIT;
	struct json_value *j;

	if (ps->cur >= ps->end || *ps->cur != '"')
		return NULL;
	ps->cur++;

	while (ps->cur < ps->end && *ps->cur != '"') {
		if (*ps->cur == '\\') {
			ps->cur++;
			if (ps->cur >= ps->end)
				goto fail;

			switch (*ps->cur++) {
			case '"':
				sbuf_addch(&sb, '"');
				break;
			case '\\':
				sbuf_addch(&sb, '\\');
				break;
			case '/':
				sbuf_addch(&sb, '/');
				break;
			case 'b':
				sbuf_addch(&sb, '\b');
				break;
			case 'f':
				sbuf_addch(&sb, '\f');
				break;
			case 'n':
				sbuf_addch(&sb, '\n');
				break;
			case 'r':
				sbuf_addch(&sb, '\r');
				break;
			case 't':
				sbuf_addch(&sb, '\t');
				break;
			default:
				/* \uXXXX and unknown escapes not supported. */
				goto fail;
			}
		} else if ((unsigned char)*ps->cur < 0x20) {
			goto fail;
		} else {
			sbuf_addch(&sb, *ps->cur++);
		}
	}

	if (ps->cur >= ps->end || *ps->cur != '"')
		goto fail;
	ps->cur++;

	j = make(JSON_STRING);
	j->u.string = sbuf_detach(&sb);
	return j;

fail:
	sbuf_release(&sb);
	return NULL;
}

static struct json_value *parse_number(struct parser *ps)
{
	char *endp;
	double d;

	d = strtod(ps->cur, &endp);
	if (endp == ps->cur || endp > ps->end)
		return NULL;
	ps->cur = endp;
	return json_new_number(d);
}

static struct json_value *parse_literal(struct parser *ps)
{
	size_t left = (size_t)(ps->end - ps->cur);

	if (left >= 4 && !memcmp(ps->cur, "true", 4)) {
		ps->cur += 4;
		return json_new_bool(1);
	}
	if (left >= 5 && !memcmp(ps->cur, "false", 5)) {
		ps->cur += 5;
		return json_new_bool(0);
	}
	if (left >= 4 && !memcmp(ps->cur, "null", 4)) {
		ps->cur += 4;
		return json_new_null();
	}
	return NULL;
}

static struct json_value *parse_array(struct parser *ps)
{
	struct json_value *arr;
	struct json_value *item;

	if (ps->cur >= ps->end || *ps->cur != '[')
		return NULL;
	ps->cur++;

	arr = json_new_array();

	skip_ws(ps);
	if (ps->cur < ps->end && *ps->cur == ']') {
		ps->cur++;
		return arr;
	}

	for (;;) {
		item = parse_value(ps);
		if (!item)
			goto fail;
		json_push(arr, item);

		skip_ws(ps);
		if (ps->cur >= ps->end)
			goto fail;
		if (*ps->cur == ',') {
			ps->cur++;
			continue;
		}
		if (*ps->cur == ']') {
			ps->cur++;
			return arr;
		}
		goto fail;
	}

fail:
	json_free(arr);
	return NULL;
}

static struct json_value *parse_object(struct parser *ps)
{
	struct json_value *obj;
	struct json_value *key;
	struct json_value *val;

	if (ps->cur >= ps->end || *ps->cur != '{')
		return NULL;
	ps->cur++;

	obj = json_new_object();

	skip_ws(ps);
	if (ps->cur < ps->end && *ps->cur == '}') {
		ps->cur++;
		return obj;
	}

	for (;;) {
		skip_ws(ps);

		key = parse_string(ps);
		if (!key)
			goto fail;

		skip_ws(ps);
		if (ps->cur >= ps->end || *ps->cur != ':') {
			json_free(key);
			goto fail;
		}
		ps->cur++;

		val = parse_value(ps);
		if (!val) {
			json_free(key);
			goto fail;
		}

		json_set(obj, key->u.string, val);
		json_free(key);

		skip_ws(ps);
		if (ps->cur >= ps->end)
			goto fail;
		if (*ps->cur == ',') {
			ps->cur++;
			continue;
		}
		if (*ps->cur == '}') {
			ps->cur++;
			return obj;
		}
		goto fail;
	}

fail:
	json_free(obj);
	return NULL;
}

static struct json_value *parse_value(struct parser *ps)
{
	char c;

	skip_ws(ps);
	if (ps->cur >= ps->end)
		return NULL;

	c = *ps->cur;
	if (c == '{')
		return parse_object(ps);
	if (c == '[')
		return parse_array(ps);
	if (c == '"')
		return parse_string(ps);
	if (c == '-' || (c >= '0' && c <= '9'))
		return parse_number(ps);
	if (c == 't' || c == 'f' || c == 'n')
		return parse_literal(ps);
	return NULL;
}

struct json_value *json_parse(const char *buf, size_t len)
{
	struct parser ps = {.cur = buf, .end = buf + len};
	struct json_value *root;

	root = parse_value(&ps);
	if (!root)
		return NULL;

	skip_ws(&ps);
	if (ps.cur != ps.end) {
		json_free(root);
		return NULL;
	}
	return root;
}

/* ------------------------------------------------------------------ */
/*  Serializer                                                        */
/* ------------------------------------------------------------------ */

static void write_string(const char *s, struct sbuf *out)
{
	sbuf_addch(out, '"');
	for (; *s; s++) {
		switch (*s) {
		case '"':
			sbuf_addstr(out, "\\\"");
			break;
		case '\\':
			sbuf_addstr(out, "\\\\");
			break;
		case '\n':
			sbuf_addstr(out, "\\n");
			break;
		case '\r':
			sbuf_addstr(out, "\\r");
			break;
		case '\t':
			sbuf_addstr(out, "\\t");
			break;
		case '\b':
			sbuf_addstr(out, "\\b");
			break;
		case '\f':
			sbuf_addstr(out, "\\f");
			break;
		default:
			if ((unsigned char)*s < 0x20)
				sbuf_addf(out, "\\u%04x", (unsigned char)*s);
			else
				sbuf_addch(out, *s);
		}
	}
	sbuf_addch(out, '"');
}

void json_serialize(const struct json_value *j, struct sbuf *out)
{
	if (!j) {
		sbuf_addstr(out, "null");
		return;
	}

	switch (j->type) {
	case JSON_NULL:
		sbuf_addstr(out, "null");
		break;
	case JSON_BOOL:
		sbuf_addstr(out, j->u.boolean ? "true" : "false");
		break;
	case JSON_NUMBER:
		sbuf_addf(out, "%.17g", j->u.number);
		break;
	case JSON_STRING:
		write_string(j->u.string, out);
		break;
	case JSON_ARRAY:
		sbuf_addch(out, '[');
		for (int i = 0; i < j->u.array.nr; i++) {
			if (i)
				sbuf_addch(out, ',');
			json_serialize(j->u.array.items[i], out);
		}
		sbuf_addch(out, ']');
		break;
	case JSON_OBJECT:
		sbuf_addch(out, '{');
		for (int i = 0; i < j->u.object.nr; i++) {
			if (i)
				sbuf_addch(out, ',');
			write_string(j->u.object.members[i].key, out);
			sbuf_addch(out, ':');
			json_serialize(j->u.object.members[i].value, out);
		}
		sbuf_addch(out, '}');
		break;
	}
}
