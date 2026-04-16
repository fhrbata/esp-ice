/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file config.c
 * @brief Git-style cascading configuration store implementation.
 */
#include "ice.h"

#include <limits.h>

struct config config = CONFIG_INIT;

void config_init(struct config *c)
{
	c->entries = NULL;
	c->nr = 0;
	c->alloc = 0;
}

void config_release(struct config *c)
{
	for (int i = 0; i < c->nr; i++) {
		free(c->entries[i].key);
		free(c->entries[i].value);
	}
	free(c->entries);
	config_init(c);
}

static void append_entry(struct config *c, const char *key, const char *value,
			 enum config_scope scope)
{
	ALLOC_GROW(c->entries, c->nr + 1, c->alloc);
	c->entries[c->nr].key = sbuf_strdup(key);
	c->entries[c->nr].value = sbuf_strdup(value);
	c->entries[c->nr].scope = scope;
	c->nr++;
}

void config_add(struct config *c, const char *key, const char *value,
		enum config_scope scope)
{
	append_entry(c, key, value, scope);
}

void config_set(struct config *c, const char *key, const char *value,
		enum config_scope scope)
{
	config_unset(c, key, scope);
	append_entry(c, key, value, scope);
}

int config_unset(struct config *c, const char *key, enum config_scope scope)
{
	int removed = 0;
	int dst = 0;

	for (int i = 0; i < c->nr; i++) {
		if (c->entries[i].scope == scope &&
		    !strcmp(c->entries[i].key, key)) {
			free(c->entries[i].key);
			free(c->entries[i].value);
			removed++;
			continue;
		}
		if (dst != i)
			c->entries[dst] = c->entries[i];
		dst++;
	}
	c->nr = dst;
	return removed;
}

const char *config_get(const char *key)
{
	const char *value = NULL;
	enum config_scope best = CONFIG_SCOPE_DEFAULT;

	for (int i = 0; i < config.nr; i++) {
		if (strcmp(config.entries[i].key, key) != 0)
			continue;
		if (!value || config.entries[i].scope >= best) {
			value = config.entries[i].value;
			best = config.entries[i].scope;
		}
	}
	return value;
}

const char *config_get_at(const char *key, enum config_scope scope)
{
	const char *value = NULL;

	for (int i = 0; i < config.nr; i++) {
		if (config.entries[i].scope == scope &&
		    !strcmp(config.entries[i].key, key))
			value = config.entries[i].value;
	}
	return value;
}

int config_get_all(const char *key, struct config_entry ***out)
{
	struct config_entry **list = NULL;
	int n = 0, alloc = 0;

	for (int i = 0; i < config.nr; i++) {
		if (strcmp(config.entries[i].key, key) != 0)
			continue;
		ALLOC_GROW(list, n + 1, alloc);
		list[n++] = &config.entries[i];
	}
	*out = list;
	return n;
}

int config_has(const char *key)
{
	for (int i = 0; i < config.nr; i++) {
		if (!strcmp(config.entries[i].key, key))
			return 1;
	}
	return 0;
}

enum config_scope config_source(const char *key)
{
	enum config_scope best = CONFIG_SCOPE_DEFAULT;
	int found = 0;

	for (int i = 0; i < config.nr; i++) {
		if (strcmp(config.entries[i].key, key) != 0)
			continue;
		if (!found || config.entries[i].scope > best)
			best = config.entries[i].scope;
		found = 1;
	}
	if (!found)
		die("config_source: '%s' is not set", key);
	return best;
}

static int streq_ci(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
	}
	return *a == *b;
}

int config_get_int(const char *key, int *out)
{
	const char *s = config_get(key);
	char *end;
	long v;

	if (!s)
		return -1;
	if (!*s)
		return -2;

	errno = 0;
	v = strtol(s, &end, 10);
	if (*end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX)
		return -2;

	*out = (int)v;
	return 0;
}

int config_get_bool(const char *key, int *out)
{
	const char *s = config_get(key);

	if (!s)
		return -1;

	if (streq_ci(s, "true") || streq_ci(s, "yes") || streq_ci(s, "on") ||
	    !strcmp(s, "1")) {
		*out = 1;
		return 0;
	}
	if (streq_ci(s, "false") || streq_ci(s, "no") || streq_ci(s, "off") ||
	    !strcmp(s, "0") || !*s) {
		*out = 0;
		return 0;
	}
	return -2;
}

const char *scope_name(enum config_scope scope)
{
	switch (scope) {
	case CONFIG_SCOPE_DEFAULT:
		return "default";
	case CONFIG_SCOPE_USER:
		return "user";
	case CONFIG_SCOPE_LOCAL:
		return "local";
	case CONFIG_SCOPE_PROJECT:
		return "project";
	case CONFIG_SCOPE_ENV:
		return "env";
	case CONFIG_SCOPE_CLI:
		return "cli";
	}
	return "unknown";
}

/* ------------------------------------------------------------------ */
/*  Path helpers                                                      */
/* ------------------------------------------------------------------ */

const char *user_config_path(void)
{
	static struct sbuf path = SBUF_INIT;
	const char *home;

	if (path.len)
		return path.buf;

	home = getenv(HOME_ENV);
	if (!home || !*home)
		return NULL;

	sbuf_addstr(&path, home);
	sbuf_addch(&path, '/');
	sbuf_addstr(&path, ".iceconfig");
	return path.buf;
}

const char *local_config_path(void) { return "./.iceconfig"; }

const char *ice_home(void)
{
	static struct sbuf path = SBUF_INIT;
	const char *env;
	const char *home;

	if (path.len)
		return path.buf;

	env = getenv("ICE_HOME");
	if (env && *env) {
		sbuf_addstr(&path, env);
		return path.buf;
	}

	home = getenv(HOME_ENV);
	if (!home || !*home)
		die("cannot determine home directory; set ICE_HOME");

	sbuf_addf(&path, "%s/.ice", home);
	return path.buf;
}

/* ------------------------------------------------------------------ */
/*  INI parser                                                        */
/* ------------------------------------------------------------------ */

static int valid_name_char(int ch)
{
	return isalnum((unsigned char)ch) || ch == '-' || ch == '_';
}

static void parse_section(const char *path, int lineno, char *line,
			  struct sbuf *section)
{
	char *start = line + 1; /* past '[' */
	char *end, *after, *p;

	end = strchr(start, ']');
	if (!end) {
		warn("%s:%d: missing ']' in section header", path, lineno);
		return;
	}

	after = end + 1;
	while (*after == ' ' || *after == '\t')
		after++;
	if (*after && *after != '#' && *after != ';') {
		warn("%s:%d: garbage after section header", path, lineno);
		return;
	}

	while (*start == ' ' || *start == '\t')
		start++;
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
		end--;

	if (start >= end) {
		warn("%s:%d: empty section name", path, lineno);
		return;
	}

	for (p = start; p < end; p++) {
		if (!valid_name_char((unsigned char)*p)) {
			warn("%s:%d: invalid character in section name", path,
			     lineno);
			return;
		}
	}

	sbuf_reset(section);
	sbuf_add(section, start, end - start);
}

static void parse_kv(struct config *c, enum config_scope scope,
		     const char *path, int lineno, char *line,
		     struct sbuf *section)
{
	struct sbuf fullkey = SBUF_INIT;
	struct sbuf val = SBUF_INIT;
	char *eq, *key, *key_end, *value, *p;

	if (section->len == 0) {
		warn("%s:%d: key outside of any section", path, lineno);
		return;
	}

	eq = strchr(line, '=');
	if (!eq) {
		warn("%s:%d: expected 'key = value'", path, lineno);
		return;
	}

	key = line;
	key_end = eq;
	while (key_end > key && (key_end[-1] == ' ' || key_end[-1] == '\t'))
		key_end--;

	if (key >= key_end) {
		warn("%s:%d: empty key", path, lineno);
		return;
	}

	for (p = key; p < key_end; p++) {
		if (!valid_name_char((unsigned char)*p)) {
			warn("%s:%d: invalid character in key", path, lineno);
			return;
		}
	}

	value = eq + 1;
	while (*value == ' ' || *value == '\t')
		value++;

	if (*value == '"') {
		char *vend, *after;

		value++;
		vend = strchr(value, '"');
		if (!vend) {
			warn("%s:%d: unterminated quoted value", path, lineno);
			return;
		}
		sbuf_add(&val, value, vend - value);

		after = vend + 1;
		while (*after == ' ' || *after == '\t')
			after++;
		if (*after && *after != '#' && *after != ';') {
			warn("%s:%d: garbage after quoted value", path, lineno);
			sbuf_release(&val);
			return;
		}
	} else {
		char *cur = value;
		char *last = value; /* one past last non-ws, non-comment */

		while (*cur && *cur != '#' && *cur != ';') {
			if (*cur != ' ' && *cur != '\t')
				last = cur + 1;
			cur++;
		}
		sbuf_add(&val, value, last - value);
	}

	sbuf_addstr(&fullkey, section->buf);
	sbuf_addch(&fullkey, '.');
	sbuf_add(&fullkey, key, key_end - key);

	config_add(c, fullkey.buf, val.buf, scope);

	sbuf_release(&fullkey);
	sbuf_release(&val);
}

static void parse_line(struct config *c, enum config_scope scope,
		       const char *path, int lineno, char *line,
		       struct sbuf *section)
{
	while (*line == ' ' || *line == '\t')
		line++;

	if (*line == '\0' || *line == '#' || *line == ';')
		return;

	if (*line == '[')
		parse_section(path, lineno, line, section);
	else
		parse_kv(c, scope, path, lineno, line, section);
}

int config_load_file(struct config *c, enum config_scope scope,
		     const char *path)
{
	struct sbuf sb = SBUF_INIT;
	struct sbuf section = SBUF_INIT;
	size_t pos = 0;
	char *line;
	int lineno = 0;

	if (!path)
		return 0;

	if (sbuf_read_file(&sb, path) < 0) {
		int saved = errno;

		sbuf_release(&sb);
		if (saved == ENOENT)
			return 0;
		errno = saved;
		warn_errno("cannot read '%s'", path);
		return -1;
	}

	while ((line = sbuf_getline(sb.buf, sb.len, &pos)) != NULL) {
		lineno++;
		parse_line(c, scope, path, lineno, line, &section);
	}

	sbuf_release(&sb);
	sbuf_release(&section);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Non-file loaders                                                  */
/* ------------------------------------------------------------------ */

void config_load_defaults(struct config *c)
{
	config_set(c, "core.build-dir", "build", CONFIG_SCOPE_DEFAULT);
	config_set(c, "core.generator", "Ninja", CONFIG_SCOPE_DEFAULT);
	config_set(c, "core.verbose", "false", CONFIG_SCOPE_DEFAULT);
}

static int value_needs_quoting(const char *v)
{
	size_t len;

	if (!*v)
		return 1;
	if (*v == ' ' || *v == '\t')
		return 1;

	len = strlen(v);
	if (v[len - 1] == ' ' || v[len - 1] == '\t')
		return 1;

	for (const char *p = v; *p; p++) {
		if (*p == '#' || *p == ';' || *p == '"' || *p == '\\' ||
		    *p == '\n' || *p == '\r')
			return 1;
	}
	return 0;
}

static void write_value(const char *v, struct sbuf *out)
{
	if (!value_needs_quoting(v)) {
		sbuf_addstr(out, v);
		return;
	}

	for (const char *p = v; *p; p++) {
		if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r')
			die("cannot serialise value containing '%c' "
			    "(escape support not implemented)",
			    *p);
	}

	sbuf_addch(out, '"');
	sbuf_addstr(out, v);
	sbuf_addch(out, '"');
}

int config_write_file(const struct config *c, enum config_scope scope,
		      const char *path)
{
	struct sbuf out = SBUF_INIT;
	struct svec sections = SVEC_INIT;
	FILE *fp;
	size_t written;

	/* Collect unique section names (order of first appearance). */
	for (int i = 0; i < c->nr; i++) {
		const char *dot;
		char *sec;
		int already;

		if (c->entries[i].scope != scope)
			continue;

		dot = strchr(c->entries[i].key, '.');
		if (!dot)
			continue; /* section-less keys are not written */

		sec = sbuf_strndup(c->entries[i].key,
				   (size_t)(dot - c->entries[i].key));

		already = 0;
		for (size_t j = 0; j < sections.nr; j++) {
			if (!strcmp(sections.v[j], sec)) {
				already = 1;
				break;
			}
		}
		if (!already)
			svec_push(&sections, sec);
		free(sec);
	}

	for (size_t s = 0; s < sections.nr; s++) {
		const char *section = sections.v[s];
		size_t sec_len = strlen(section);

		if (s > 0)
			sbuf_addch(&out, '\n');
		sbuf_addf(&out, "[%s]\n", section);

		for (int i = 0; i < c->nr; i++) {
			const char *key = c->entries[i].key;

			if (c->entries[i].scope != scope)
				continue;
			if (strncmp(key, section, sec_len) != 0 ||
			    key[sec_len] != '.')
				continue;

			sbuf_addf(&out, "\t%s = ", key + sec_len + 1);
			write_value(c->entries[i].value, &out);
			sbuf_addch(&out, '\n');
		}
	}

	svec_clear(&sections);

	fp = fopen(path, "w");
	if (!fp) {
		sbuf_release(&out);
		return -1;
	}
	written = fwrite(out.buf, 1, out.len, fp);
	fclose(fp);

	if (written != out.len) {
		sbuf_release(&out);
		errno = EIO;
		return -1;
	}
	sbuf_release(&out);
	return 0;
}

void config_load_env(struct config *c)
{
	static const struct {
		const char *env;
		const char *key;
	} map[] = {
	    {"ESPPORT", "serial.port"},
	    {"ESPBAUD", "serial.baud"},
	    {"IDF_TARGET", "target"},
	};

	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		const char *val = getenv(map[i].env);

		if (val && *val)
			config_set(c, map[i].key, val, CONFIG_SCOPE_ENV);
	}
}

void config_load_project(struct config *c, const char *build_dir)
{
	struct sbuf path = SBUF_INIT;
	struct sbuf buf = SBUF_INIT;
	struct sbuf derived = SBUF_INIT;
	struct cmakecache cache = CMAKECACHE_INIT;
	struct json_value *desc = NULL;

	if (!build_dir)
		return;

	sbuf_addf(&path, "%s/CMakeCache.txt", build_dir);
	if (cmakecache_load(&cache, path.buf) == 0) {
		const char *target = cmakecache_get(&cache, "IDF_TARGET");

		if (target)
			config_set(c, "target", target, CONFIG_SCOPE_PROJECT);
	}

	sbuf_reset(&path);
	sbuf_addf(&path, "%s/project_description.json", build_dir);
	if (sbuf_read_file(&buf, path.buf) >= 0)
		desc = json_parse(buf.buf, buf.len);

	if (desc) {
		const char *name =
		    json_as_string(json_get(desc, "project_name"));
		if (name) {
			sbuf_addf(&derived, "%s/%s.map", build_dir, name);
			config_set(c, "mapfile", derived.buf,
				   CONFIG_SCOPE_PROJECT);

			sbuf_reset(&derived);
			sbuf_addf(&derived, "%s/%s.elf", build_dir, name);
			config_set(c, "elf", derived.buf, CONFIG_SCOPE_PROJECT);
		}
	}

	json_free(desc);
	cmakecache_release(&cache);
	sbuf_release(&path);
	sbuf_release(&buf);
	sbuf_release(&derived);
}
