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

int config_parse_bool(const char *s, int *out)
{
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

int config_get_bool(const char *key, int *out)
{
	return config_parse_bool(config_get(key), out);
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

/*
 * Accepts both:
 *   [section]
 *   [section "subsection"]           (git-style; key becomes
 * section.subsection.key)
 *
 * Section names are restricted to valid_name_char; subsection names
 * are free-form between the quotes (supports \" and \\ escapes).
 */
static void parse_section(const char *path, int lineno, char *line,
			  struct sbuf *section)
{
	char *start = line + 1; /* past '[' */
	char *end, *after, *p, *name_end;

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

	/* Section name runs until whitespace or end. */
	name_end = start;
	while (name_end < end && *name_end != ' ' && *name_end != '\t')
		name_end++;

	for (p = start; p < name_end; p++) {
		if (!valid_name_char((unsigned char)*p)) {
			warn("%s:%d: invalid character in section name", path,
			     lineno);
			return;
		}
	}

	sbuf_reset(section);
	sbuf_add(section, start, name_end - start);

	/* Optional subsection in quotes. */
	p = name_end;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if (p == end)
		return;

	if (*p != '"') {
		warn("%s:%d: expected '\"' to start subsection", path, lineno);
		sbuf_reset(section);
		return;
	}
	p++;

	sbuf_addch(section, '.');
	while (p < end && *p != '"') {
		if (*p == '\\' && p + 1 < end) {
			p++; /* skip escape; take next char literally */
		}
		sbuf_addch(section, *p++);
	}
	if (p >= end || *p != '"') {
		warn("%s:%d: unterminated subsection", path, lineno);
		sbuf_reset(section);
		return;
	}
	p++;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if (p != end) {
		warn("%s:%d: garbage after subsection", path, lineno);
		sbuf_reset(section);
		return;
	}
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

void config_load_buf(struct config *c, enum config_scope scope,
		     const char *label, char *buf, size_t len)
{
	struct sbuf section = SBUF_INIT;
	size_t pos = 0;
	char *line;
	int lineno = 0;

	while ((line = sbuf_getline(buf, len, &pos)) != NULL) {
		lineno++;
		parse_line(c, scope, label, lineno, line, &section);
	}

	sbuf_release(&section);
}

int config_load_file(struct config *c, enum config_scope scope,
		     const char *path)
{
	struct sbuf sb = SBUF_INIT;

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

	config_load_buf(c, scope, path, sb.buf, sb.len);
	sbuf_release(&sb);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Non-file loaders                                                  */
/* ------------------------------------------------------------------ */

/*
 * Built-in default keys.  Listed here so the same table drives both
 * default seeding (config_load_defaults) and description lookup for
 * completion (config_builtin_key_help).  Keep descriptions short --
 * they appear next to the key in the completion pop-up.
 */
static const struct {
	const char *key;
	const char *value;
	const char *summary;
} builtin_defaults[] = {
    {"core.build-dir", "build", "default build directory"},
    {"core.generator", "Ninja", "default cmake generator"},
    {"core.verbose", "false", "default verbose mode"},
    {"completion.descriptions", "true",
     "show descriptions in shell completion"},
    {NULL, NULL, NULL},
};

void config_load_defaults(struct config *c)
{
	for (size_t i = 0; builtin_defaults[i].key; i++)
		config_set(c, builtin_defaults[i].key,
			   builtin_defaults[i].value, CONFIG_SCOPE_DEFAULT);
}

const char *config_builtin_key_help(const char *key)
{
	for (size_t i = 0; builtin_defaults[i].key; i++)
		if (!strcmp(builtin_defaults[i].key, key))
			return builtin_defaults[i].summary;
	return NULL;
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
	struct svec headers = SVEC_INIT;
	FILE *fp;
	size_t written;

	/*
	 * Group by the key's parent path -- everything up to the last
	 * dot.  "alias.b" has parent "alias"; "project.default.chip"
	 * has parent "project.default".  A parent with a dot is written
	 * as a git-style [section "subsection"] header; the subsection
	 * carries characters (. in particular) that a bare section
	 * header's key-validator would reject on reload.
	 */
	for (int i = 0; i < c->nr; i++) {
		const char *last_dot;
		char *hdr;
		int already;

		if (c->entries[i].scope != scope)
			continue;

		last_dot = strrchr(c->entries[i].key, '.');
		if (!last_dot)
			continue; /* section-less keys are not written */

		hdr = sbuf_strndup(c->entries[i].key,
				   (size_t)(last_dot - c->entries[i].key));

		already = 0;
		for (size_t j = 0; j < headers.nr; j++) {
			if (!strcmp(headers.v[j], hdr)) {
				already = 1;
				break;
			}
		}
		if (!already)
			svec_push(&headers, hdr);
		free(hdr);
	}

	for (size_t h = 0; h < headers.nr; h++) {
		const char *header = headers.v[h];
		size_t hdr_len = strlen(header);
		const char *dot = strchr(header, '.');

		if (h > 0)
			sbuf_addch(&out, '\n');

		if (dot) {
			sbuf_addch(&out, '[');
			sbuf_add(&out, header, dot - header);
			sbuf_addstr(&out, " \"");
			for (const char *p = dot + 1; *p; p++) {
				if (*p == '"' || *p == '\\')
					sbuf_addch(&out, '\\');
				sbuf_addch(&out, *p);
			}
			sbuf_addstr(&out, "\"]\n");
		} else {
			sbuf_addf(&out, "[%s]\n", header);
		}

		for (int i = 0; i < c->nr; i++) {
			const char *key = c->entries[i].key;

			if (c->entries[i].scope != scope)
				continue;
			if (strncmp(key, header, hdr_len) != 0 ||
			    key[hdr_len] != '.')
				continue;
			/* Only direct children of this header; deeper
			 * nesting belongs to a different header entry. */
			if (strchr(key + hdr_len + 1, '.'))
				continue;

			sbuf_addf(&out, "\t%s = ", key + hdr_len + 1);
			write_value(c->entries[i].value, &out);
			sbuf_addch(&out, '\n');
		}
	}

	svec_clear(&headers);

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

/** Remove every entry at @p scope from @p c.  Returns count removed. */
static int config_clear_scope(struct config *c, enum config_scope scope)
{
	int dst = 0;
	int removed = 0;

	for (int i = 0; i < c->nr; i++) {
		if (c->entries[i].scope == scope) {
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

void config_reload_local(void)
{
	config_clear_scope(&config, CONFIG_SCOPE_LOCAL);
	config_load_file(&config, CONFIG_SCOPE_LOCAL, local_config_path());
}

void config_load_profile(const char *name)
{
	struct sbuf prefix = SBUF_INIT;
	struct sbuf new_key = SBUF_INIT;
	struct sbuf path = SBUF_INIT;
	struct sbuf buf = SBUF_INIT;
	struct sbuf derived = SBUF_INIT;
	struct cmakecache cache = CMAKECACHE_INIT;
	struct json_value *desc = NULL;
	const char *build_dir;
	int n_pre;

	/* Re-runnable: clear any previous PROJECT-scope state first. */
	config_clear_scope(&config, CONFIG_SCOPE_PROJECT);

	/*
	 * Promote @b{[project "<name>"]} entries (loaded from .iceconfig
	 * at LOCAL scope) up to a uniform @b{project.X} namespace at
	 * PROJECT scope so commands can read @b{project.build-dir} etc.
	 * without knowing which profile they're in.
	 */
	sbuf_addf(&prefix, "project.%s.", name);
	n_pre = config.nr;
	for (int i = 0; i < n_pre; i++) {
		const char *key = config.entries[i].key;
		if (config.entries[i].scope != CONFIG_SCOPE_LOCAL)
			continue;
		if (strncmp(key, prefix.buf, prefix.len) != 0)
			continue;
		sbuf_reset(&new_key);
		sbuf_addf(&new_key, "project.%s", key + prefix.len);
		config_add(&config, new_key.buf, config.entries[i].value,
			   CONFIG_SCOPE_PROJECT);
	}

	/* Derive project.target / project.mapfile / project.elf from the
	 * profile's build directory, if it has been configured. */
	build_dir = config_get("project.build-dir");
	if (!build_dir)
		goto out;

	sbuf_addf(&path, "%s/CMakeCache.txt", build_dir);
	if (cmakecache_load(&cache, path.buf) == 0) {
		const char *target = cmakecache_get(&cache, "IDF_TARGET");
		if (target)
			config_set(&config, "project.target", target,
				   CONFIG_SCOPE_PROJECT);
	}

	sbuf_reset(&path);
	sbuf_addf(&path, "%s/project_description.json", build_dir);
	if (sbuf_read_file(&buf, path.buf) >= 0)
		desc = json_parse(buf.buf, buf.len);

	if (desc) {
		const char *project_name =
		    json_as_string(json_get(desc, "project_name"));
		if (project_name) {
			sbuf_addf(&derived, "%s/%s.map", build_dir,
				  project_name);
			config_set(&config, "project.mapfile", derived.buf,
				   CONFIG_SCOPE_PROJECT);

			sbuf_reset(&derived);
			sbuf_addf(&derived, "%s/%s.elf", build_dir,
				  project_name);
			config_set(&config, "project.elf", derived.buf,
				   CONFIG_SCOPE_PROJECT);
		}
	}

out:
	json_free(desc);
	cmakecache_release(&cache);
	sbuf_release(&prefix);
	sbuf_release(&new_key);
	sbuf_release(&path);
	sbuf_release(&buf);
	sbuf_release(&derived);
}
