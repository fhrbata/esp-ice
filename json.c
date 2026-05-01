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

/* Decode 4 hex digits at @p p into @p out.  Returns 0 on success. */
static int parse_u4(const char *p, unsigned *out)
{
	unsigned cp = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		unsigned d;
		if (c >= '0' && c <= '9')
			d = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			d = (unsigned)(c - 'A' + 10);
		else
			return -1;
		cp = (cp << 4) | d;
	}
	*out = cp;
	return 0;
}

/* Append @p cp's UTF-8 encoding to @p sb. */
static void utf8_emit(struct sbuf *sb, unsigned cp)
{
	if (cp < 0x80) {
		sbuf_addch(sb, (char)cp);
	} else if (cp < 0x800) {
		sbuf_addch(sb, (char)(0xC0 | (cp >> 6)));
		sbuf_addch(sb, (char)(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		sbuf_addch(sb, (char)(0xE0 | (cp >> 12)));
		sbuf_addch(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
		sbuf_addch(sb, (char)(0x80 | (cp & 0x3F)));
	} else {
		sbuf_addch(sb, (char)(0xF0 | (cp >> 18)));
		sbuf_addch(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
		sbuf_addch(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
		sbuf_addch(sb, (char)(0x80 | (cp & 0x3F)));
	}
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
			case 'u': {
				/* \uXXXX -> UTF-8.  Surrogate pairs (high
				 * + low) are recombined to a single code
				 * point before encoding; isolated surrogates
				 * fail.  See ECMA-404. */
				unsigned cp;
				if (ps->cur + 4 > ps->end ||
				    parse_u4(ps->cur, &cp) < 0)
					goto fail;
				ps->cur += 4;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					unsigned low;
					if (ps->cur + 6 > ps->end ||
					    ps->cur[0] != '\\' ||
					    ps->cur[1] != 'u' ||
					    parse_u4(ps->cur + 2, &low) < 0 ||
					    low < 0xDC00 || low > 0xDFFF)
						goto fail;
					cp = 0x10000 + ((cp - 0xD800) << 10) +
					     (low - 0xDC00);
					ps->cur += 6;
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					goto fail;
				}
				utf8_emit(&sb, cp);
				break;
			}
			default:
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

/*
 * Serialize a number using the smallest representation that still
 * round-trips: integers up to 2^53 print without a fractional part
 * (matching Python's json.dumps), other doubles use %.17g for
 * round-trip safety.  Negative zero is normalized to "0" so that
 * JSON output for ints stored as doubles never shows "-0".
 */
static void write_number(double n, struct sbuf *out)
{
	if (n == 0.0) {
		sbuf_addstr(out, "0");
		return;
	}

	if (n >= -9007199254740992.0 && n <= 9007199254740992.0) {
		double t;
		long long ll = (long long)n;
		t = (double)ll;
		if (t == n) {
			sbuf_addf(out, "%lld", ll);
			return;
		}
	}
	sbuf_addf(out, "%.17g", n);
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
		write_number(j->u.number, out);
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

static void put_indent(struct sbuf *out, int level, int width)
{
	int total = level * width;
	for (int i = 0; i < total; i++)
		sbuf_addch(out, ' ');
}

static void serialize_pretty(const struct json_value *j, struct sbuf *out,
			     int indent, int level)
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
		write_number(j->u.number, out);
		break;
	case JSON_STRING:
		write_string(j->u.string, out);
		break;
	case JSON_ARRAY:
		if (j->u.array.nr == 0) {
			sbuf_addstr(out, "[]");
			break;
		}
		sbuf_addstr(out, "[\n");
		for (int i = 0; i < j->u.array.nr; i++) {
			put_indent(out, level + 1, indent);
			serialize_pretty(j->u.array.items[i], out, indent,
					 level + 1);
			if (i + 1 < j->u.array.nr)
				sbuf_addch(out, ',');
			sbuf_addch(out, '\n');
		}
		put_indent(out, level, indent);
		sbuf_addch(out, ']');
		break;
	case JSON_OBJECT:
		if (j->u.object.nr == 0) {
			sbuf_addstr(out, "{}");
			break;
		}
		sbuf_addstr(out, "{\n");
		for (int i = 0; i < j->u.object.nr; i++) {
			put_indent(out, level + 1, indent);
			write_string(j->u.object.members[i].key, out);
			sbuf_addstr(out, ": ");
			serialize_pretty(j->u.object.members[i].value, out,
					 indent, level + 1);
			if (i + 1 < j->u.object.nr)
				sbuf_addch(out, ',');
			sbuf_addch(out, '\n');
		}
		put_indent(out, level, indent);
		sbuf_addch(out, '}');
		break;
	}
}

void json_serialize_pretty(const struct json_value *j, struct sbuf *out,
			   int indent)
{
	serialize_pretty(j, out, indent, 0);
}
