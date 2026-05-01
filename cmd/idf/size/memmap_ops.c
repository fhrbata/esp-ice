/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file memmap_ops.c
 * @brief Operations on a loaded memmap: diff, unify, trim, sort, ...
 *
 * The diff merge follows upstream: `cur` is treated as the working
 * tree, every entry from `ref` that doesn't already exist there is
 * cloned in (with size left at the ref value temporarily), then a
 * single recursive pass computes *_diff values according to which
 * side -- cur, ref, or both -- contributed each node.
 *
 * Sorting is destructive (reorders the children pointer arrays);
 * unify rebuilds the children arrays under abbreviated keys; trim
 * drops nodes outright.  All allocations stay on the heap and follow
 * the same ownership rules as mm_load().
 */
#include "ice.h"

#include "memmap.h"
#include "size.h"

/* ------------------------------------------------------------------ */
/* Cloning helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Each clone helper returns a freshly-heap-allocated node owning its
 * strings.  Used by both mm_diff (when ref-only nodes are spliced
 * into cur) and mm_unify (when collapsing keys by abbrev_name).
 */

static struct mm_symbol *clone_symbol(const struct mm_symbol *s)
{
	struct mm_symbol *r = calloc(1, sizeof(*r));
	if (!r)
		die_errno("calloc");
	r->name = sbuf_strdup(s->name);
	r->abbrev_name = sbuf_strdup(s->abbrev_name);
	r->size = s->size;
	r->size_diff = s->size_diff;
	return r;
}

static struct mm_object *clone_object(const struct mm_object *o)
{
	struct mm_object *r = calloc(1, sizeof(*r));
	if (!r)
		die_errno("calloc");
	r->name = sbuf_strdup(o->name);
	r->abbrev_name = sbuf_strdup(o->abbrev_name);
	r->size = o->size;
	r->size_diff = o->size_diff;
	for (int i = 0; i < o->nr_syms; i++) {
		ALLOC_GROW(r->syms, r->nr_syms + 1, r->alloc_syms);
		r->syms[r->nr_syms++] = clone_symbol(o->syms[i]);
	}
	return r;
}

static struct mm_archive *clone_archive(const struct mm_archive *a)
{
	struct mm_archive *r = calloc(1, sizeof(*r));
	if (!r)
		die_errno("calloc");
	r->name = sbuf_strdup(a->name);
	r->abbrev_name = sbuf_strdup(a->abbrev_name);
	r->size = a->size;
	r->size_diff = a->size_diff;
	for (int i = 0; i < a->nr_objs; i++) {
		ALLOC_GROW(r->objs, r->nr_objs + 1, r->alloc_objs);
		r->objs[r->nr_objs++] = clone_object(a->objs[i]);
	}
	return r;
}

static struct mm_section *clone_section(const struct mm_section *s)
{
	struct mm_section *r = calloc(1, sizeof(*r));
	if (!r)
		die_errno("calloc");
	r->name = sbuf_strdup(s->name);
	r->abbrev_name = sbuf_strdup(s->abbrev_name);
	r->size = s->size;
	r->size_diff = s->size_diff;
	for (int i = 0; i < s->nr_archives; i++) {
		ALLOC_GROW(r->archives, r->nr_archives + 1, r->alloc_archives);
		r->archives[r->nr_archives++] = clone_archive(s->archives[i]);
	}
	return r;
}

/* ------------------------------------------------------------------ */
/* Diff                                                               */
/* ------------------------------------------------------------------ */

/*
 * Find a child by name -- the linear search is fine since none of
 * these levels ever exceeds a few hundred entries.
 */
#define DEFINE_FIND(NAME, T, FIELD)                                            \
	static T *find_##NAME(T *const *arr, int nr, const char *name)         \
	{                                                                      \
		for (int i = 0; i < nr; i++)                                   \
			if (!strcmp(arr[i]->FIELD, name))                      \
				return arr[i];                                 \
		return NULL;                                                   \
	}

DEFINE_FIND(sym, struct mm_symbol, name)
DEFINE_FIND(obj, struct mm_object, name)
DEFINE_FIND(arch, struct mm_archive, name)
DEFINE_FIND(sec, struct mm_section, name)

/*
 * Recurse-set: subtree exists only on one side; mark every node with
 * (size_diff = sign * size) and (size = sign == -1 ? 0 : size).
 *
 * sign = +1: subtree is cur-only -- diff = size, size unchanged.
 * sign = -1: subtree was added from ref -- size becomes 0, diff = -size.
 */
static void sym_set_one_sided(struct mm_symbol *s, int sign)
{
	s->size_diff = (int64_t)sign * s->size;
	if (sign == -1)
		s->size = 0;
}

static void obj_set_one_sided(struct mm_object *o, int sign)
{
	o->size_diff = (int64_t)sign * o->size;
	if (sign == -1)
		o->size = 0;
	for (int i = 0; i < o->nr_syms; i++)
		sym_set_one_sided(o->syms[i], sign);
}

static void arch_set_one_sided(struct mm_archive *a, int sign)
{
	a->size_diff = (int64_t)sign * a->size;
	if (sign == -1)
		a->size = 0;
	for (int i = 0; i < a->nr_objs; i++)
		obj_set_one_sided(a->objs[i], sign);
}

static void sec_set_one_sided(struct mm_section *s, int sign)
{
	s->size_diff = (int64_t)sign * s->size;
	if (sign == -1)
		s->size = 0;
	for (int i = 0; i < s->nr_archives; i++)
		arch_set_one_sided(s->archives[i], sign);
}

static void diff_symbols(struct mm_object *cur_o, const struct mm_object *ref_o)
{
	for (int i = 0; i < cur_o->nr_syms; i++) {
		struct mm_symbol *cs = cur_o->syms[i];
		struct mm_symbol *rs =
		    find_sym(ref_o->syms, ref_o->nr_syms, cs->name);
		cs->size_diff = cs->size - (rs ? rs->size : 0);
	}
	for (int i = 0; i < ref_o->nr_syms; i++) {
		struct mm_symbol *rs = ref_o->syms[i];
		if (find_sym(cur_o->syms, cur_o->nr_syms, rs->name))
			continue;
		ALLOC_GROW(cur_o->syms, cur_o->nr_syms + 1, cur_o->alloc_syms);
		cur_o->syms[cur_o->nr_syms] = clone_symbol(rs);
		sym_set_one_sided(cur_o->syms[cur_o->nr_syms], -1);
		cur_o->nr_syms++;
	}
}

static void diff_objects(struct mm_archive *cur_a,
			 const struct mm_archive *ref_a)
{
	for (int i = 0; i < cur_a->nr_objs; i++) {
		struct mm_object *co = cur_a->objs[i];
		struct mm_object *ro =
		    find_obj(ref_a->objs, ref_a->nr_objs, co->name);
		co->size_diff = co->size - (ro ? ro->size : 0);
		if (ro)
			diff_symbols(co, ro);
		else {
			for (int j = 0; j < co->nr_syms; j++)
				sym_set_one_sided(co->syms[j], 1);
		}
	}
	for (int i = 0; i < ref_a->nr_objs; i++) {
		struct mm_object *ro = ref_a->objs[i];
		if (find_obj(cur_a->objs, cur_a->nr_objs, ro->name))
			continue;
		ALLOC_GROW(cur_a->objs, cur_a->nr_objs + 1, cur_a->alloc_objs);
		cur_a->objs[cur_a->nr_objs] = clone_object(ro);
		obj_set_one_sided(cur_a->objs[cur_a->nr_objs], -1);
		cur_a->nr_objs++;
	}
}

static void diff_archives(struct mm_section *cur_s,
			  const struct mm_section *ref_s)
{
	for (int i = 0; i < cur_s->nr_archives; i++) {
		struct mm_archive *ca = cur_s->archives[i];
		struct mm_archive *ra =
		    find_arch(ref_s->archives, ref_s->nr_archives, ca->name);
		ca->size_diff = ca->size - (ra ? ra->size : 0);
		if (ra)
			diff_objects(ca, ra);
		else {
			for (int j = 0; j < ca->nr_objs; j++)
				obj_set_one_sided(ca->objs[j], 1);
		}
	}
	for (int i = 0; i < ref_s->nr_archives; i++) {
		struct mm_archive *ra = ref_s->archives[i];
		if (find_arch(cur_s->archives, cur_s->nr_archives, ra->name))
			continue;
		ALLOC_GROW(cur_s->archives, cur_s->nr_archives + 1,
			   cur_s->alloc_archives);
		cur_s->archives[cur_s->nr_archives] = clone_archive(ra);
		arch_set_one_sided(cur_s->archives[cur_s->nr_archives], -1);
		cur_s->nr_archives++;
	}
}

static void diff_sections(struct mm_type *cur_t, const struct mm_type *ref_t)
{
	for (int i = 0; i < cur_t->nr_sections; i++) {
		struct mm_section *cs = cur_t->sections[i];
		struct mm_section *rs =
		    find_sec(ref_t->sections, ref_t->nr_sections, cs->name);
		cs->size_diff = cs->size - (rs ? rs->size : 0);
		if (rs)
			diff_archives(cs, rs);
		else {
			for (int j = 0; j < cs->nr_archives; j++)
				arch_set_one_sided(cs->archives[j], 1);
		}
	}
	for (int i = 0; i < ref_t->nr_sections; i++) {
		struct mm_section *rs = ref_t->sections[i];
		if (find_sec(cur_t->sections, cur_t->nr_sections, rs->name))
			continue;
		ALLOC_GROW(cur_t->sections, cur_t->nr_sections + 1,
			   cur_t->alloc_sections);
		cur_t->sections[cur_t->nr_sections] = clone_section(rs);
		sec_set_one_sided(cur_t->sections[cur_t->nr_sections], -1);
		cur_t->nr_sections++;
	}
}

void mm_diff(struct memmap *cur, const struct memmap *ref)
{
	free(cur->target_diff);
	cur->target_diff = sbuf_strdup(ref->target);
	free(cur->project_path_diff);
	cur->project_path_diff = sbuf_strdup(ref->project_path);
	cur->image_size_diff = cur->image_size - ref->image_size;

	for (int i = 0; i < cur->nr_types; i++) {
		struct mm_type *ct = cur->types[i];
		struct mm_type *rt = mm_find_type(ref, ct->name);
		if (rt) {
			ct->size_diff = ct->size - rt->size;
			ct->used_diff = ct->used - rt->used;
			diff_sections(ct, rt);
		} else {
			ct->size_diff = ct->size;
			ct->used_diff = ct->used;
			for (int j = 0; j < ct->nr_sections; j++)
				sec_set_one_sided(ct->sections[j], 1);
		}
	}
	for (int i = 0; i < ref->nr_types; i++) {
		struct mm_type *rt = ref->types[i];
		struct mm_type *ct = mm_find_type(cur, rt->name);
		if (ct)
			continue;
		/* ref-only type: clone in. */
		ct = calloc(1, sizeof(*ct));
		if (!ct)
			die_errno("calloc");
		ct->name = sbuf_strdup(rt->name);
		ct->size = 0;
		ct->size_diff = -rt->size;
		ct->used = 0;
		ct->used_diff = -rt->used;
		for (int j = 0; j < rt->nr_sections; j++) {
			ALLOC_GROW(ct->sections, ct->nr_sections + 1,
				   ct->alloc_sections);
			ct->sections[ct->nr_sections] =
			    clone_section(rt->sections[j]);
			sec_set_one_sided(ct->sections[ct->nr_sections], -1);
			ct->nr_sections++;
		}
		ALLOC_GROW(cur->types, cur->nr_types + 1, cur->alloc_types);
		cur->types[cur->nr_types++] = ct;
	}
}

/* ------------------------------------------------------------------ */
/* Unify -- collapse keys to abbreviated names                         */
/* ------------------------------------------------------------------ */

static struct mm_symbol *find_sym_by_abbrev(struct mm_symbol *const *arr,
					    int nr, const char *abbrev)
{
	for (int i = 0; i < nr; i++)
		if (!strcmp(arr[i]->abbrev_name, abbrev))
			return arr[i];
	return NULL;
}

static struct mm_object *find_obj_by_abbrev(struct mm_object *const *arr,
					    int nr, const char *abbrev)
{
	for (int i = 0; i < nr; i++)
		if (!strcmp(arr[i]->abbrev_name, abbrev))
			return arr[i];
	return NULL;
}

static struct mm_archive *find_arch_by_abbrev(struct mm_archive *const *arr,
					      int nr, const char *abbrev)
{
	for (int i = 0; i < nr; i++)
		if (!strcmp(arr[i]->abbrev_name, abbrev))
			return arr[i];
	return NULL;
}

static struct mm_section *find_sec_by_abbrev(struct mm_section *const *arr,
					     int nr, const char *abbrev)
{
	for (int i = 0; i < nr; i++)
		if (!strcmp(arr[i]->abbrev_name, abbrev))
			return arr[i];
	return NULL;
}

static void unify_object(struct mm_object *dst, const struct mm_object *src)
{
	for (int i = 0; i < src->nr_syms; i++) {
		struct mm_symbol *s = src->syms[i];
		struct mm_symbol *d =
		    find_sym_by_abbrev(dst->syms, dst->nr_syms, s->abbrev_name);
		if (!d) {
			d = calloc(1, sizeof(*d));
			if (!d)
				die_errno("calloc");
			d->name = sbuf_strdup(s->abbrev_name);
			d->abbrev_name = sbuf_strdup(s->abbrev_name);
			ALLOC_GROW(dst->syms, dst->nr_syms + 1,
				   dst->alloc_syms);
			dst->syms[dst->nr_syms++] = d;
		}
		d->size += s->size;
		d->size_diff += s->size_diff;
	}
}

static void unify_archive(struct mm_archive *dst, const struct mm_archive *src)
{
	for (int i = 0; i < src->nr_objs; i++) {
		struct mm_object *s = src->objs[i];
		struct mm_object *d =
		    find_obj_by_abbrev(dst->objs, dst->nr_objs, s->abbrev_name);
		if (!d) {
			d = calloc(1, sizeof(*d));
			if (!d)
				die_errno("calloc");
			d->name = sbuf_strdup(s->abbrev_name);
			d->abbrev_name = sbuf_strdup(s->abbrev_name);
			ALLOC_GROW(dst->objs, dst->nr_objs + 1,
				   dst->alloc_objs);
			dst->objs[dst->nr_objs++] = d;
		}
		d->size += s->size;
		d->size_diff += s->size_diff;
		unify_object(d, s);
	}
}

static void unify_section(struct mm_section *dst, const struct mm_section *src)
{
	for (int i = 0; i < src->nr_archives; i++) {
		struct mm_archive *s = src->archives[i];
		struct mm_archive *d = find_arch_by_abbrev(
		    dst->archives, dst->nr_archives, s->abbrev_name);
		if (!d) {
			d = calloc(1, sizeof(*d));
			if (!d)
				die_errno("calloc");
			d->name = sbuf_strdup(s->abbrev_name);
			d->abbrev_name = sbuf_strdup(s->abbrev_name);
			ALLOC_GROW(dst->archives, dst->nr_archives + 1,
				   dst->alloc_archives);
			dst->archives[dst->nr_archives++] = d;
		}
		d->size += s->size;
		d->size_diff += s->size_diff;
		unify_archive(d, s);
	}
}

/* Free entire children chain of a section (used after we've replaced it). */
static void section_free_children(struct mm_section *s);
static void archive_free_children(struct mm_archive *a);
static void object_free_children(struct mm_object *o);

static void object_free_children(struct mm_object *o)
{
	for (int i = 0; i < o->nr_syms; i++) {
		free(o->syms[i]->name);
		free(o->syms[i]->abbrev_name);
		free(o->syms[i]);
	}
	free(o->syms);
	o->syms = NULL;
	o->nr_syms = o->alloc_syms = 0;
}

static void archive_free_children(struct mm_archive *a)
{
	for (int i = 0; i < a->nr_objs; i++) {
		object_free_children(a->objs[i]);
		free(a->objs[i]->name);
		free(a->objs[i]->abbrev_name);
		free(a->objs[i]);
	}
	free(a->objs);
	a->objs = NULL;
	a->nr_objs = a->alloc_objs = 0;
}

static void section_free_children(struct mm_section *s)
{
	for (int i = 0; i < s->nr_archives; i++) {
		archive_free_children(s->archives[i]);
		free(s->archives[i]->name);
		free(s->archives[i]->abbrev_name);
		free(s->archives[i]);
	}
	free(s->archives);
	s->archives = NULL;
	s->nr_archives = s->alloc_archives = 0;
}

static void type_free_sections(struct mm_type *t)
{
	for (int i = 0; i < t->nr_sections; i++) {
		struct mm_section *s = t->sections[i];
		section_free_children(s);
		free(s->name);
		free(s->abbrev_name);
		free(s);
	}
	free(t->sections);
	t->sections = NULL;
	t->nr_sections = t->alloc_sections = 0;
}

void mm_unify(struct memmap *mm)
{
	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = mm->types[i];
		struct mm_section **old = t->sections;
		int nr_old = t->nr_sections;

		t->sections = NULL;
		t->nr_sections = t->alloc_sections = 0;

		for (int j = 0; j < nr_old; j++) {
			struct mm_section *s = old[j];
			struct mm_section *d = find_sec_by_abbrev(
			    t->sections, t->nr_sections, s->abbrev_name);
			if (!d) {
				d = calloc(1, sizeof(*d));
				if (!d)
					die_errno("calloc");
				d->name = sbuf_strdup(s->abbrev_name);
				d->abbrev_name = sbuf_strdup(s->abbrev_name);
				ALLOC_GROW(t->sections, t->nr_sections + 1,
					   t->alloc_sections);
				t->sections[t->nr_sections++] = d;
			}
			d->size += s->size;
			d->size_diff += s->size_diff;
			unify_section(d, s);
		}

		/* Free the originals. */
		for (int j = 0; j < nr_old; j++) {
			section_free_children(old[j]);
			free(old[j]->name);
			free(old[j]->abbrev_name);
			free(old[j]);
		}
		free(old);
	}
}

/* ------------------------------------------------------------------ */
/* remove_unused / ignore_flash_size                                  */
/* ------------------------------------------------------------------ */

void mm_remove_unused(struct memmap *mm)
{
	int dst_t = 0;

	/* 1. Drop sections whose archives list is empty (alignment-only
	 *    sections like .iram0.text_end). */
	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = mm->types[i];
		int dst_s = 0;
		for (int j = 0; j < t->nr_sections; j++) {
			if (!t->sections[j]->nr_archives) {
				section_free_children(t->sections[j]);
				free(t->sections[j]->name);
				free(t->sections[j]->abbrev_name);
				free(t->sections[j]);
				continue;
			}
			t->sections[dst_s++] = t->sections[j];
		}
		t->nr_sections = dst_s;

		/* Re-derive used = sum of section sizes. */
		t->used = 0;
		for (int j = 0; j < t->nr_sections; j++)
			t->used += t->sections[j]->size;
	}

	/* 2. Drop unused memory types (used == 0). */
	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = mm->types[i];
		if (!t->used) {
			type_free_sections(t);
			free(t->name);
			free(t);
			continue;
		}
		mm->types[dst_t++] = t;
	}
	mm->nr_types = dst_t;
}

static int contains_flash(const char *s)
{
	for (; *s; s++) {
		char c = *s;
		if (c >= 'A' && c <= 'Z')
			c = (char)(c + 32);
		if (c == 'f' && s[1] && s[2] && s[3] && s[4]) {
			if ((s[1] == 'l' || s[1] == 'L') &&
			    (s[2] == 'a' || s[2] == 'A') &&
			    (s[3] == 's' || s[3] == 'S') &&
			    (s[4] == 'h' || s[4] == 'H'))
				return 1;
		}
	}
	return 0;
}

void mm_ignore_flash_size(struct memmap *mm)
{
	for (int i = 0; i < mm->nr_types; i++)
		if (contains_flash(mm->types[i]->name))
			mm->types[i]->size = 0;
}

/* ------------------------------------------------------------------ */
/* Trim                                                                */
/* ------------------------------------------------------------------ */

enum { TRIM_ARCHIVE_DETAILS, TRIM_ARCHIVES, TRIM_OBJECTS, TRIM_ALL };

static int changed(int diff_value, const struct mm_args *a)
{
	if (!a->diff || a->show_unchanged)
		return 1;
	return diff_value ? 1 : 0;
}

void mm_trim(struct memmap *mm, const struct mm_args *args)
{
	int depth;
	int dst_t = 0;

	if (args->archive_details)
		depth = TRIM_ARCHIVE_DETAILS;
	else if (args->archives)
		depth = TRIM_ARCHIVES;
	else if (args->files)
		depth = TRIM_OBJECTS;
	else
		depth = TRIM_ALL;

	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = mm->types[i];
		int dst_s = 0;

		if (!changed((int)t->used_diff, args)) {
			type_free_sections(t);
			free(t->name);
			free(t);
			continue;
		}

		for (int j = 0; j < t->nr_sections; j++) {
			struct mm_section *s = t->sections[j];
			int dst_a = 0;

			if (!changed((int)s->size_diff, args)) {
				section_free_children(s);
				free(s->name);
				free(s->abbrev_name);
				free(s);
				continue;
			}

			for (int k = 0; k < s->nr_archives; k++) {
				struct mm_archive *a = s->archives[k];
				int keep = changed((int)a->size_diff, args);

				if (depth == TRIM_ARCHIVE_DETAILS &&
				    strcmp(a->abbrev_name,
					   args->archive_details) != 0)
					keep = 0;

				if (!keep) {
					archive_free_children(a);
					free(a->name);
					free(a->abbrev_name);
					free(a);
					continue;
				}

				if (depth == TRIM_ARCHIVES) {
					archive_free_children(a);
				} else {
					int dst_o = 0;
					for (int m = 0; m < a->nr_objs; m++) {
						struct mm_object *o =
						    a->objs[m];
						int o_keep = changed(
						    (int)o->size_diff, args);
						if (!o_keep) {
							object_free_children(o);
							free(o->name);
							free(o->abbrev_name);
							free(o);
							continue;
						}
						if (depth == TRIM_OBJECTS) {
							object_free_children(o);
						} else {
							int dst_y = 0;
							for (int n = 0;
							     n < o->nr_syms;
							     n++) {
								struct mm_symbol
								    *sy =
									o->syms
									    [n];
								if (!changed(
									(int)sy
									    ->size_diff,
									args)) {
									free(
									    sy->name);
									free(
									    sy->abbrev_name);
									free(
									    sy);
									continue;
								}
								o->syms
								    [dst_y++] =
								    sy;
							}
							o->nr_syms = dst_y;
						}
						a->objs[dst_o++] = o;
					}
					a->nr_objs = dst_o;
				}
				s->archives[dst_a++] = a;
			}
			s->nr_archives = dst_a;

			t->sections[dst_s++] = s;
		}
		t->nr_sections = dst_s;
		mm->types[dst_t++] = t;
	}
	mm->nr_types = dst_t;
}

/* ------------------------------------------------------------------ */
/* Sort                                                                */
/* ------------------------------------------------------------------ */

static int g_sort_reverse;
static int g_sort_diff;

/*
 * Direction encoding: g_sort_reverse == 1 means "reverse=True" in
 * Python -- i.e. larger values come first (descending), which is the
 * default behaviour.  Passing --sort-reverse on the CLI sets it to 0
 * (ascending).
 */
static int direction_cmp(int64_t va, int64_t vb)
{
	if (va < vb)
		return g_sort_reverse ? 1 : -1;
	if (va > vb)
		return g_sort_reverse ? -1 : 1;
	return 0;
}

static int cmp_type(const void *a, const void *b)
{
	const struct mm_type *ta = *(const struct mm_type *const *)a;
	const struct mm_type *tb = *(const struct mm_type *const *)b;
	int64_t va = g_sort_diff ? ta->used_diff : ta->used;
	int64_t vb = g_sort_diff ? tb->used_diff : tb->used;
	return direction_cmp(va, vb);
}

static int cmp_section(const void *a, const void *b)
{
	const struct mm_section *sa = *(const struct mm_section *const *)a;
	const struct mm_section *sb = *(const struct mm_section *const *)b;
	int64_t va = g_sort_diff ? sa->size_diff : sa->size;
	int64_t vb = g_sort_diff ? sb->size_diff : sb->size;
	return direction_cmp(va, vb);
}

static int cmp_archive(const void *a, const void *b)
{
	const struct mm_archive *aa = *(const struct mm_archive *const *)a;
	const struct mm_archive *ab = *(const struct mm_archive *const *)b;
	int64_t va = g_sort_diff ? aa->size_diff : aa->size;
	int64_t vb = g_sort_diff ? ab->size_diff : ab->size;
	return direction_cmp(va, vb);
}

static int cmp_object(const void *a, const void *b)
{
	const struct mm_object *oa = *(const struct mm_object *const *)a;
	const struct mm_object *ob = *(const struct mm_object *const *)b;
	int64_t va = g_sort_diff ? oa->size_diff : oa->size;
	int64_t vb = g_sort_diff ? ob->size_diff : ob->size;
	return direction_cmp(va, vb);
}

static int cmp_symbol(const void *a, const void *b)
{
	const struct mm_symbol *sa = *(const struct mm_symbol *const *)a;
	const struct mm_symbol *sb = *(const struct mm_symbol *const *)b;
	int64_t va = g_sort_diff ? sa->size_diff : sa->size;
	int64_t vb = g_sort_diff ? sb->size_diff : sb->size;
	return direction_cmp(va, vb);
}

void mm_sort(struct memmap *mm, const struct mm_args *args)
{
	/*
	 * Stable sort throughout so that ties (e.g. unused section
	 * cells with size==0 in --diff mode) keep memmap insertion
	 * order across runs -- matching Python's `sorted(reverse=...)`.
	 */
	g_sort_reverse = args->sort_reverse;
	g_sort_diff = args->sort_diff;

	if (mm->nr_types > 1)
		mm_stable_sort((void **)mm->types, mm->nr_types, cmp_type);

	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = mm->types[i];
		if (t->nr_sections > 1)
			mm_stable_sort((void **)t->sections, t->nr_sections,
				       cmp_section);

		for (int j = 0; j < t->nr_sections; j++) {
			struct mm_section *s = t->sections[j];
			if (s->nr_archives > 1)
				mm_stable_sort((void **)s->archives,
					       s->nr_archives, cmp_archive);

			for (int k = 0; k < s->nr_archives; k++) {
				struct mm_archive *a = s->archives[k];
				if (a->nr_objs > 1)
					mm_stable_sort((void **)a->objs,
						       a->nr_objs, cmp_object);

				for (int l = 0; l < a->nr_objs; l++) {
					struct mm_object *o = a->objs[l];
					if (o->nr_syms > 1)
						mm_stable_sort((void **)o->syms,
							       o->nr_syms,
							       cmp_symbol);
				}
			}
		}
	}
}
