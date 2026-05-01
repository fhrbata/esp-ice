/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file format_json.c
 * @brief raw / json2 emitters.
 *
 * raw    -- the entire memmap tree dumped as JSON, matching upstream's
 *           json.dumps(memmap, indent=4).  Used by tests for round-trip.
 * json2  -- a simplified `version: 1.2` summary view (memory_type +
 *           "parts") and the full archive/file/symbol summaries when
 *           one of those views is selected.
 */
#include "ice.h"

#include "format.h"
#include "json.h"
#include "memmap.h"
#include "size.h"
#include "views.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static struct json_value *jnum(int64_t n) { return json_new_number((double)n); }

static void put_int(struct json_value *obj, const char *key, int64_t n)
{
	json_set(obj, key, jnum(n));
}

static void put_str(struct json_value *obj, const char *key, const char *s)
{
	json_set(obj, key, json_new_string(s ? s : ""));
}

/* ------------------------------------------------------------------ */
/* JSON syntax highlighter                                             */
/* ------------------------------------------------------------------ */

/*
 * Walks the serialized JSON byte stream and rewrites it with @x{...}
 * color tokens.  The platform fprintf shim expands the tokens on a
 * tty and strips them when writing to a file or pipe, so piping into
 * `jq` still gets clean JSON.
 *
 * Recognized tokens:
 *   "..."  followed by ':'  -> key,             @c{}
 *   "..."  otherwise         -> string value,    @g{}
 *   -? digit ...             -> number,          @y{}
 *   true / false / null      -> literal,         @Y{}
 *   { } [ ] : , whitespace   -> structural,      passed through
 */
static void colorize_json(struct sbuf *out, const char *s, size_t len)
{
	const char *end = s + len;

	while (s < end) {
		char c = *s;

		if (c == '"') {
			const char *p = s + 1;
			const char *q;
			int is_key;

			while (p < end) {
				if (*p == '\\' && p + 1 < end) {
					p += 2;
					continue;
				}
				if (*p == '"')
					break;
				p++;
			}
			/* Look ahead past whitespace for ':' to detect keys. */
			q = (p < end) ? p + 1 : p;
			while (q < end && (*q == ' ' || *q == '\t' ||
					   *q == '\n' || *q == '\r'))
				q++;
			is_key = (q < end && *q == ':');

			sbuf_addstr(out, is_key ? "@c{" : "@g{");
			sbuf_add(out, s, (size_t)((p < end ? p + 1 : p) - s));
			sbuf_addch(out, '}');
			s = (p < end) ? p + 1 : end;
			continue;
		}

		if (c == '-' || (c >= '0' && c <= '9')) {
			const char *p = s + (c == '-' ? 1 : 0);
			while (p < end && ((*p >= '0' && *p <= '9') ||
					   *p == '.' || *p == 'e' ||
					   *p == 'E' || *p == '+' || *p == '-'))
				p++;
			sbuf_addstr(out, "@y{");
			sbuf_add(out, s, (size_t)(p - s));
			sbuf_addch(out, '}');
			s = p;
			continue;
		}

		if ((c == 't' && end - s >= 4 && !memcmp(s, "true", 4)) ||
		    (c == 'f' && end - s >= 5 && !memcmp(s, "false", 5)) ||
		    (c == 'n' && end - s >= 4 && !memcmp(s, "null", 4))) {
			size_t n = (c == 'f') ? 5 : 4;
			sbuf_addstr(out, "@Y{");
			sbuf_add(out, s, n);
			sbuf_addch(out, '}');
			s += n;
			continue;
		}

		sbuf_addch(out, c);
		s++;
	}
}

static void emit(const struct json_value *root, const struct mm_args *args)
{
	struct sbuf raw = SBUF_INIT;
	struct sbuf colored = SBUF_INIT;

	json_serialize_pretty(root, &raw, 4);
	colorize_json(&colored, raw.buf, raw.len);
	/* fputs goes through the platform shim, which strips color tokens
	 * when args->out isn't a tty -- so piped JSON stays parseable. */
	fputs(colored.buf, args->out);
	fputc('\n', args->out);
	sbuf_release(&colored);
	sbuf_release(&raw);
}

/* ------------------------------------------------------------------ */
/* raw -- full memmap tree                                             */
/* ------------------------------------------------------------------ */

static struct json_value *symbol_to_json(const struct mm_symbol *sym)
{
	struct json_value *o = json_new_object();
	put_str(o, "abbrev_name", sym->abbrev_name);
	put_int(o, "size", sym->size);
	put_int(o, "size_diff", sym->size_diff);
	return o;
}

static struct json_value *object_to_json(const struct mm_object *obj)
{
	struct json_value *o = json_new_object();
	struct json_value *symbols = json_new_object();

	put_str(o, "abbrev_name", obj->abbrev_name);
	put_int(o, "size", obj->size);
	put_int(o, "size_diff", obj->size_diff);

	for (int i = 0; i < obj->nr_syms; i++)
		json_set(symbols, obj->syms[i]->name,
			 symbol_to_json(obj->syms[i]));
	json_set(o, "symbols", symbols);
	return o;
}

static struct json_value *archive_to_json(const struct mm_archive *a)
{
	struct json_value *o = json_new_object();
	struct json_value *files = json_new_object();

	put_str(o, "abbrev_name", a->abbrev_name);
	put_int(o, "size", a->size);
	put_int(o, "size_diff", a->size_diff);

	for (int i = 0; i < a->nr_objs; i++)
		json_set(files, a->objs[i]->name, object_to_json(a->objs[i]));
	json_set(o, "object_files", files);
	return o;
}

static struct json_value *section_to_json(const struct mm_section *s)
{
	struct json_value *o = json_new_object();
	struct json_value *archives = json_new_object();

	put_str(o, "abbrev_name", s->abbrev_name);
	put_int(o, "size", s->size);
	put_int(o, "size_diff", s->size_diff);

	for (int i = 0; i < s->nr_archives; i++)
		json_set(archives, s->archives[i]->name,
			 archive_to_json(s->archives[i]));
	json_set(o, "archives", archives);
	return o;
}

static struct json_value *type_to_json(const struct mm_type *t)
{
	struct json_value *o = json_new_object();
	struct json_value *sections = json_new_object();

	put_int(o, "size", t->size);
	put_int(o, "size_diff", t->size_diff);
	put_int(o, "used", t->used);
	put_int(o, "used_diff", t->used_diff);

	for (int i = 0; i < t->nr_sections; i++)
		json_set(sections, t->sections[i]->name,
			 section_to_json(t->sections[i]));
	json_set(o, "sections", sections);
	return o;
}

void fmt_json_raw(const struct memmap *mm, const struct mm_args *args)
{
	struct json_value *root = json_new_object();
	struct json_value *types = json_new_object();

	put_str(root, "version", "1.0");
	put_str(root, "target", mm->target);
	put_str(root, "target_diff", mm->target_diff);
	put_int(root, "image_size", mm->image_size);
	put_int(root, "image_size_diff", mm->image_size_diff);
	put_str(root, "project_path", mm->project_path);
	put_str(root, "project_path_diff", mm->project_path_diff);

	for (int i = 0; i < mm->nr_types; i++)
		json_set(types, mm->types[i]->name, type_to_json(mm->types[i]));
	json_set(root, "memory_types", types);

	emit(root, args);
	json_free(root);
}

/* ------------------------------------------------------------------ */
/* json2 -- versioned compact summary                                  */
/* ------------------------------------------------------------------ */

void fmt_json2_summary(const struct memmap *mm, const struct mm_args *args)
{
	struct json_value *root = json_new_object();
	struct json_value *layout = json_new_array();

	put_str(root, "version", "1.2");
	put_int(root, "total_size", mm->image_size);

	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = mm->types[i];
		struct json_value *tj = json_new_object();
		struct json_value *parts = json_new_object();

		put_str(tj, "name", t->name);
		put_int(tj, "total", t->size);
		put_int(tj, "used", t->used);
		put_int(tj, "free", t->size ? t->size - t->used : 0);

		for (int j = 0; j < t->nr_sections; j++) {
			struct mm_section *s = t->sections[j];
			struct json_value *sj = json_new_object();
			const char *key =
			    args->abbrev ? s->abbrev_name : s->name;
			put_int(sj, "size", s->size);
			json_set(parts, key, sj);
		}
		json_set(tj, "parts", parts);
		json_push(layout, tj);
	}
	json_set(root, "layout", layout);

	emit(root, args);
	json_free(root);
}

/* ------------------------------------------------------------------ */
/* json2 -- archive/file/symbol summaries                              */
/* ------------------------------------------------------------------ */

static struct json_value *sv_section_to_json(const struct sv_section *s)
{
	struct json_value *o = json_new_object();
	put_int(o, "size", s->size);
	put_int(o, "size_diff", s->size_diff);
	put_str(o, "abbrev_name", s->abbrev_name);
	return o;
}

static struct json_value *sv_type_to_json(const struct sv_type *t)
{
	struct json_value *o = json_new_object();
	struct json_value *sections = json_new_object();

	put_int(o, "size", t->size);
	put_int(o, "size_diff", t->size_diff);

	for (int i = 0; i < t->nr_sections; i++)
		json_set(sections, t->sections[i]->name,
			 sv_section_to_json(t->sections[i]));
	json_set(o, "sections", sections);
	return o;
}

static struct json_value *sv_entry_to_json(const struct sv_entry *e)
{
	struct json_value *o = json_new_object();
	struct json_value *types = json_new_object();

	put_str(o, "abbrev_name", e->abbrev_name);
	put_int(o, "size", e->size);
	put_int(o, "size_diff", e->size_diff);

	for (int i = 0; i < e->nr_types; i++)
		json_set(types, e->types[i]->name,
			 sv_type_to_json(e->types[i]));
	json_set(o, "memory_types", types);
	return o;
}

static void emit_sv(const struct sv_summary *s, const struct mm_args *args)
{
	struct json_value *root = json_new_object();
	for (int i = 0; i < s->nr_entries; i++)
		json_set(root, s->entries[i]->name,
			 sv_entry_to_json(s->entries[i]));
	emit(root, args);
	json_free(root);
}

void fmt_json2_archives(const struct memmap *mm, const struct mm_args *args)
{
	struct sv_summary s;
	sv_archives(mm, args, &s);
	sv_filter(&s, args);
	sv_sort(&s, args);
	sv_sort_columns(&s, args);
	emit_sv(&s, args);
	sv_release(&s);
}

void fmt_json2_files(const struct memmap *mm, const struct mm_args *args)
{
	struct sv_summary s;
	sv_files(mm, args, &s);
	sv_filter(&s, args);
	sv_sort(&s, args);
	sv_sort_columns(&s, args);
	emit_sv(&s, args);
	sv_release(&s);
}

void fmt_json2_symbols(const struct memmap *mm, const struct mm_args *args)
{
	struct sv_summary s;
	sv_symbols(mm, args, &s);
	sv_filter(&s, args);
	sv_sort(&s, args);
	sv_sort_columns(&s, args);
	emit_sv(&s, args);
	sv_release(&s);
}

void fmt_json_deps(const struct map_file *mf, const struct memmap *mm,
		   const struct elf_symbols *syms, const struct mm_args *args)
{
	struct dep_summary d;
	struct json_value *root = json_new_object();

	dep_build(mf, mm, syms, args, &d);
	dep_filter(&d, args);

	for (int i = 0; i < d.nr_entries; i++) {
		struct dep_entry *e = d.entries[i];
		struct json_value *eo = json_new_object();
		struct json_value *archives = json_new_object();

		put_str(eo, "abbrev_name", e->abbrev_name);
		put_int(eo, "size", e->size);

		for (int j = 0; j < e->nr_archives; j++) {
			struct dep_archive *da = e->archives[j];
			struct json_value *ao = json_new_object();
			struct json_value *syms_arr = json_new_array();

			put_str(ao, "abbrev_name", da->abbrev_name);
			put_int(ao, "size", da->size);
			for (int k = 0; k < da->nr_symbols; k++)
				json_push(syms_arr,
					  json_new_string(da->symbols[k]));
			json_set(ao, "symbols", syms_arr);
			json_set(archives, da->name, ao);
		}
		json_set(eo, "archives", archives);
		json_set(root, e->name, eo);
	}

	emit(root, args);
	json_free(root);
	dep_release(&d);
}
