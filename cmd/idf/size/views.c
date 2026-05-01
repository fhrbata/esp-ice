/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file views.c
 * @brief Per-archive / per-file / per-symbol summary builders + deps.
 *
 * Each summary entry carries a full memory-types-x-sections grid the
 * same way upstream esp-idf-size does, so the table emitter can lay
 * out columns identically without any second derivation pass.
 *
 * Archive dependency extraction works off map_file::xrefs (the parsed
 * Cross Reference Table) intersected with elf_symbols (to drop
 * symbols the linker discarded) and with the active archive set in
 * @p mm (so undefs and stripped libs don't appear).
 */
#include "ice.h"

#include "memmap.h"
#include "size.h"
#include "views.h"

/* ------------------------------------------------------------------ */
/* Skeleton for sv_entry.types[] / sv_type.sections[]                  */
/* ------------------------------------------------------------------ */

/*
 * Make a fresh copy of the memmap shape -- one sv_type per memory
 * type, one sv_section per section -- with all sizes zeroed.  Each
 * sv_entry takes its own clone so we can fill cells independently.
 */
static void clone_skeleton(const struct memmap *mm, struct sv_entry *e)
{
	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = mm->types[i];
		struct sv_type *st = calloc(1, sizeof(*st));
		if (!st)
			die_errno("calloc");
		st->name = sbuf_strdup(t->name);

		for (int j = 0; j < t->nr_sections; j++) {
			struct mm_section *s = t->sections[j];
			struct sv_section *ss = calloc(1, sizeof(*ss));
			if (!ss)
				die_errno("calloc");
			ss->name = sbuf_strdup(s->name);
			ss->abbrev_name = sbuf_strdup(s->abbrev_name);
			ALLOC_GROW(st->sections, st->nr_sections + 1,
				   st->alloc_sections);
			st->sections[st->nr_sections++] = ss;
		}

		ALLOC_GROW(e->types, e->nr_types + 1, e->alloc_types);
		e->types[e->nr_types++] = st;
	}
}

static struct sv_type *sv_entry_find_type(struct sv_entry *e, const char *name)
{
	for (int i = 0; i < e->nr_types; i++)
		if (!strcmp(e->types[i]->name, name))
			return e->types[i];
	return NULL;
}

static struct sv_section *sv_type_find_section(struct sv_type *st,
					       const char *name)
{
	for (int i = 0; i < st->nr_sections; i++)
		if (!strcmp(st->sections[i]->name, name))
			return st->sections[i];
	return NULL;
}

static struct sv_entry *sv_summary_find_entry(struct sv_summary *s,
					      const char *name)
{
	for (int i = 0; i < s->nr_entries; i++)
		if (!strcmp(s->entries[i]->name, name))
			return s->entries[i];
	return NULL;
}

static struct sv_entry *sv_summary_get_entry(struct sv_summary *s,
					     const char *name,
					     const char *abbrev_name,
					     const struct memmap *mm)
{
	struct sv_entry *e = sv_summary_find_entry(s, name);
	if (e)
		return e;
	e = calloc(1, sizeof(*e));
	if (!e)
		die_errno("calloc");
	e->name = sbuf_strdup(name);
	e->abbrev_name = sbuf_strdup(abbrev_name);
	clone_skeleton(mm, e);
	ALLOC_GROW(s->entries, s->nr_entries + 1, s->alloc_entries);
	s->entries[s->nr_entries++] = e;
	return e;
}

/* ------------------------------------------------------------------ */
/* Accumulators                                                       */
/* ------------------------------------------------------------------ */

static void add_to_cell(struct sv_entry *e, const char *type_name,
			const char *section_name, int64_t size,
			int64_t size_diff)
{
	struct sv_type *st = sv_entry_find_type(e, type_name);
	struct sv_section *ss;
	if (!st)
		return; /* skeleton missing this type -- shouldn't happen */
	ss = sv_type_find_section(st, section_name);
	if (!ss)
		return;
	ss->size += size;
	ss->size_diff += size_diff;
	st->size += size;
	st->size_diff += size_diff;
	e->size += size;
	e->size_diff += size_diff;
}

/* ------------------------------------------------------------------ */
/* sv_archives / sv_files / sv_symbols                                */
/* ------------------------------------------------------------------ */

static void drop_unchanged(struct sv_summary *s, const struct mm_args *args)
{
	int dst = 0;

	if (!args->diff || args->show_unchanged)
		return;

	for (int i = 0; i < s->nr_entries; i++) {
		struct sv_entry *e = s->entries[i];
		if (e->size_diff == 0) {
			/* Free skeleton + entry. */
			for (int j = 0; j < e->nr_types; j++) {
				struct sv_type *st = e->types[j];
				for (int k = 0; k < st->nr_sections; k++) {
					free(st->sections[k]->name);
					free(st->sections[k]->abbrev_name);
					free(st->sections[k]);
				}
				free(st->sections);
				free(st->name);
				free(st);
			}
			free(e->types);
			free(e->name);
			free(e->abbrev_name);
			free(e);
			continue;
		}
		s->entries[dst++] = e;
	}
	s->nr_entries = dst;
}

void sv_archives(const struct memmap *mm, const struct mm_args *args,
		 struct sv_summary *out)
{
	memset(out, 0, sizeof(*out));

	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = mm->types[i];
		for (int j = 0; j < t->nr_sections; j++) {
			struct mm_section *s = t->sections[j];
			for (int k = 0; k < s->nr_archives; k++) {
				struct mm_archive *a = s->archives[k];
				struct sv_entry *e = sv_summary_get_entry(
				    out, a->name, a->abbrev_name, mm);
				add_to_cell(e, t->name, s->name, a->size,
					    a->size_diff);
			}
		}
	}

	drop_unchanged(out, args);
}

void sv_files(const struct memmap *mm, const struct mm_args *args,
	      struct sv_summary *out)
{
	memset(out, 0, sizeof(*out));

	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = mm->types[i];
		for (int j = 0; j < t->nr_sections; j++) {
			struct mm_section *s = t->sections[j];
			for (int k = 0; k < s->nr_archives; k++) {
				struct mm_archive *a = s->archives[k];
				for (int l = 0; l < a->nr_objs; l++) {
					struct mm_object *o = a->objs[l];
					struct sbuf key = SBUF_INIT;
					struct sv_entry *e;

					if (args->unify) {
						sbuf_addstr(&key,
							    o->abbrev_name);
					} else {
						sbuf_addstr(&key, a->name);
						sbuf_addch(&key, ':');
						sbuf_addstr(&key, o->name);
					}
					e = sv_summary_get_entry(
					    out, key.buf, o->abbrev_name, mm);
					sbuf_release(&key);
					add_to_cell(e, t->name, s->name,
						    o->size, o->size_diff);
				}
			}
		}
	}

	drop_unchanged(out, args);
}

void sv_symbols(const struct memmap *mm, const struct mm_args *args,
		struct sv_summary *out)
{
	int matched = 0;

	memset(out, 0, sizeof(*out));

	for (int i = 0; i < mm->nr_types; i++) {
		struct mm_type *t = mm->types[i];
		for (int j = 0; j < t->nr_sections; j++) {
			struct mm_section *s = t->sections[j];
			for (int k = 0; k < s->nr_archives; k++) {
				struct mm_archive *a = s->archives[k];
				if (strcmp(a->abbrev_name,
					   args->archive_details) != 0)
					continue;
				matched = 1;
				for (int l = 0; l < a->nr_objs; l++) {
					struct mm_object *o = a->objs[l];
					for (int m = 0; m < o->nr_syms; m++) {
						struct mm_symbol *sy =
						    o->syms[m];
						struct sbuf key = SBUF_INIT;
						struct sv_entry *e;

						if (args->unify) {
							sbuf_addstr(
							    &key,
							    sy->abbrev_name);
						} else {
							sbuf_addstr(&key,
								    a->name);
							sbuf_addch(&key, ':');
							sbuf_addstr(&key,
								    o->name);
							sbuf_addch(&key, ':');
							sbuf_addstr(&key,
								    sy->name);
						}
						e = sv_summary_get_entry(
						    out, key.buf, sy->name, mm);
						sbuf_release(&key);
						add_to_cell(e, t->name, s->name,
							    sy->size,
							    sy->size_diff);
					}
				}
			}
		}
	}

	if (!matched)
		die("archive '%s' not found", args->archive_details);

	drop_unchanged(out, args);
}

/* ------------------------------------------------------------------ */
/* sort + filter                                                       */
/* ------------------------------------------------------------------ */

static int g_sort_diff;
static int g_sort_reverse;

/* g_sort_reverse == 1 means descending (Python's reverse=True). */
static int direction_cmp(int64_t va, int64_t vb)
{
	if (va < vb)
		return g_sort_reverse ? 1 : -1;
	if (va > vb)
		return g_sort_reverse ? -1 : 1;
	return 0;
}

static int cmp_sv_entry(const void *a, const void *b)
{
	const struct sv_entry *ea = *(const struct sv_entry *const *)a;
	const struct sv_entry *eb = *(const struct sv_entry *const *)b;
	int64_t va = g_sort_diff ? ea->size_diff : ea->size;
	int64_t vb = g_sort_diff ? eb->size_diff : eb->size;
	return direction_cmp(va, vb);
}

static int cmp_sv_type(const void *a, const void *b)
{
	const struct sv_type *ta = *(const struct sv_type *const *)a;
	const struct sv_type *tb = *(const struct sv_type *const *)b;
	int64_t va = g_sort_diff ? ta->size_diff : ta->size;
	int64_t vb = g_sort_diff ? tb->size_diff : tb->size;
	return direction_cmp(va, vb);
}

static int cmp_sv_section(const void *a, const void *b)
{
	const struct sv_section *sa = *(const struct sv_section *const *)a;
	const struct sv_section *sb = *(const struct sv_section *const *)b;
	int64_t va = g_sort_diff ? sa->size_diff : sa->size;
	int64_t vb = g_sort_diff ? sb->size_diff : sb->size;
	return direction_cmp(va, vb);
}

void sv_sort(struct sv_summary *s, const struct mm_args *args)
{
	/*
	 * Only the row order (entries) is sorted -- per-entry types and
	 * sections are columns in the rendered table, so reordering
	 * them would shuffle headers for one entry out of sync with
	 * the rest.  Upstream's Rich Table uses the first row to define
	 * columns, so we keep the skeleton (i.e. memmap insertion)
	 * order across all entries.
	 */
	g_sort_diff = args->sort_diff;
	g_sort_reverse = args->sort_reverse;

	if (s->nr_entries > 1)
		mm_stable_sort((void **)s->entries, s->nr_entries,
			       cmp_sv_entry);
}

void sv_sort_columns(struct sv_summary *s, const struct mm_args *args)
{
	g_sort_diff = args->sort_diff;
	g_sort_reverse = args->sort_reverse;

	for (int i = 0; i < s->nr_entries; i++) {
		struct sv_entry *e = s->entries[i];
		if (e->nr_types > 1)
			mm_stable_sort((void **)e->types, e->nr_types,
				       cmp_sv_type);
		for (int j = 0; j < e->nr_types; j++) {
			struct sv_type *st = e->types[j];
			if (st->nr_sections > 1)
				mm_stable_sort((void **)st->sections,
					       st->nr_sections, cmp_sv_section);
		}
	}
}

void sv_filter(struct sv_summary *s, const struct mm_args *args)
{
	int dst = 0;

	if (!args->nr_filter)
		return;

	for (int i = 0; i < s->nr_entries; i++) {
		struct sv_entry *e = s->entries[i];
		const char *name = args->abbrev ? e->abbrev_name : e->name;
		int keep = 0;

		for (int j = 0; j < args->nr_filter; j++) {
			char pat[512];
			snprintf(pat, sizeof(pat), "*%s*", args->filter[j]);
			if (glob_match(pat, name)) {
				keep = 1;
				break;
			}
		}

		if (!keep) {
			for (int j = 0; j < e->nr_types; j++) {
				struct sv_type *st = e->types[j];
				for (int k = 0; k < st->nr_sections; k++) {
					free(st->sections[k]->name);
					free(st->sections[k]->abbrev_name);
					free(st->sections[k]);
				}
				free(st->sections);
				free(st->name);
				free(st);
			}
			free(e->types);
			free(e->name);
			free(e->abbrev_name);
			free(e);
			continue;
		}
		s->entries[dst++] = e;
	}
	s->nr_entries = dst;
}

void sv_release(struct sv_summary *s)
{
	for (int i = 0; i < s->nr_entries; i++) {
		struct sv_entry *e = s->entries[i];
		for (int j = 0; j < e->nr_types; j++) {
			struct sv_type *st = e->types[j];
			for (int k = 0; k < st->nr_sections; k++) {
				free(st->sections[k]->name);
				free(st->sections[k]->abbrev_name);
				free(st->sections[k]);
			}
			free(st->sections);
			free(st->name);
			free(st);
		}
		free(e->types);
		free(e->name);
		free(e->abbrev_name);
		free(e);
	}
	free(s->entries);
	memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ */
/* Archive dependencies                                                */
/* ------------------------------------------------------------------ */

static struct dep_archive *dep_get_archive(struct dep_entry *e,
					   const char *name,
					   const char *abbrev_name,
					   int64_t size)
{
	for (int i = 0; i < e->nr_archives; i++)
		if (!strcmp(e->archives[i]->name, name))
			return e->archives[i];
	{
		struct dep_archive *a = calloc(1, sizeof(*a));
		if (!a)
			die_errno("calloc");
		a->name = sbuf_strdup(name);
		a->abbrev_name = sbuf_strdup(abbrev_name);
		a->size = size;
		ALLOC_GROW(e->archives, e->nr_archives + 1, e->alloc_archives);
		e->archives[e->nr_archives++] = a;
		return a;
	}
}

static struct dep_entry *dep_get_entry(struct dep_summary *s, const char *name,
				       const char *abbrev_name, int64_t size)
{
	for (int i = 0; i < s->nr_entries; i++)
		if (!strcmp(s->entries[i]->name, name))
			return s->entries[i];
	{
		struct dep_entry *e = calloc(1, sizeof(*e));
		if (!e)
			die_errno("calloc");
		e->name = sbuf_strdup(name);
		e->abbrev_name = sbuf_strdup(abbrev_name);
		e->size = size;
		ALLOC_GROW(s->entries, s->nr_entries + 1, s->alloc_entries);
		s->entries[s->nr_entries++] = e;
		return e;
	}
}

/* Look up an archive entry's size from the per-archive summary. */
static int find_archive_size(const struct sv_summary *archives,
			     const char *name, int64_t *out_size,
			     const char **abbrev)
{
	for (int i = 0; i < archives->nr_entries; i++) {
		if (!strcmp(archives->entries[i]->name, name)) {
			*out_size = archives->entries[i]->size;
			*abbrev = archives->entries[i]->abbrev_name;
			return 1;
		}
	}
	return 0;
}

/* Build a sorted, deduplicated symbol-name set from the ELF table for
 * STT_FUNC and STT_OBJECT entries excluding SHN_ABS. */
static void build_live_sym_set(const struct elf_symbols *syms,
			       const char ***out_names, int *out_nr)
{
	const char **names = NULL;
	int nr = 0, alloc = 0;

	for (int i = 0; i < syms->nr; i++) {
		const struct elf_symbol *s = &syms->s[i];
		if (s->type != STT_FUNC && s->type != STT_OBJECT)
			continue;
		if (s->shndx == SHN_ABS)
			continue;
		ALLOC_GROW(names, nr + 1, alloc);
		names[nr++] = s->name;
	}

	*out_names = names;
	*out_nr = nr;
}

static int has_name(const char **arr, int nr, const char *name)
{
	for (int i = 0; i < nr; i++)
		if (!strcmp(arr[i], name))
			return 1;
	return 0;
}

void dep_build(const struct map_file *mf, const struct memmap *mm,
	       const struct elf_symbols *syms, const struct mm_args *args,
	       struct dep_summary *out)
{
	struct sv_summary archives;
	const char **live = NULL;
	int nr_live = 0;

	memset(out, 0, sizeof(*out));

	if (!syms)
		die("Displaying archives dependencies requires an ELF file");
	if (mf->nr_xrefs == 0)
		die("The cross-reference table is not available");

	sv_archives(mm, args, &archives);
	build_live_sym_set(syms, &live, &nr_live);

	for (int x = 0; x < mf->nr_xrefs; x++) {
		const struct map_xref *xr = &mf->xrefs[x];
		const char *def_arch;
		int64_t def_size;
		const char *def_abbrev;

		if (!xr->nr_refs)
			continue;
		def_arch = xr->refs[0].archive;

		/* Skip symbols that didn't make it into the linked image. */
		if (!has_name(live, nr_live, xr->symbol))
			continue;
		if (!find_archive_size(&archives, def_arch, &def_size,
				       &def_abbrev))
			continue;

		for (int r = 1; r < xr->nr_refs; r++) {
			const char *ref_arch = xr->refs[r].archive;
			int64_t ref_size;
			const char *ref_abbrev;
			struct dep_entry *e;
			struct dep_archive *da;

			if (!strcmp(ref_arch, def_arch))
				continue;
			if (!find_archive_size(&archives, ref_arch, &ref_size,
					       &ref_abbrev))
				continue;

			/*
			 * Convention:
			 *   forward (default): ref_arch -> def_arch
			 *     (each entry is the user, archives[] = providers)
			 *   reverse:            def_arch -> ref_arch
			 *     (each entry is the provider, archives[] = users)
			 */
			if (args->dep_reverse) {
				e = dep_get_entry(out, def_arch, def_abbrev,
						  def_size);
				da = dep_get_archive(e, ref_arch, ref_abbrev,
						     ref_size);
			} else {
				e = dep_get_entry(out, ref_arch, ref_abbrev,
						  ref_size);
				da = dep_get_archive(e, def_arch, def_abbrev,
						     def_size);
			}

			ALLOC_GROW(da->symbols, da->nr_symbols + 1,
				   da->alloc_symbols);
			da->symbols[da->nr_symbols++] = sbuf_strdup(xr->symbol);
		}
	}

	free(live);
	sv_release(&archives);
}

void dep_filter(struct dep_summary *s, const struct mm_args *args)
{
	int dst = 0;

	if (!args->nr_filter)
		return;

	for (int i = 0; i < s->nr_entries; i++) {
		struct dep_entry *e = s->entries[i];
		const char *name = args->abbrev ? e->abbrev_name : e->name;
		int keep = 0;

		for (int j = 0; j < args->nr_filter; j++) {
			char pat[512];
			snprintf(pat, sizeof(pat), "*%s*", args->filter[j]);
			if (glob_match(pat, name)) {
				keep = 1;
				break;
			}
		}

		if (!keep) {
			for (int j = 0; j < e->nr_archives; j++) {
				struct dep_archive *a = e->archives[j];
				for (int k = 0; k < a->nr_symbols; k++)
					free(a->symbols[k]);
				free(a->symbols);
				free(a->name);
				free(a->abbrev_name);
				free(a);
			}
			free(e->archives);
			free(e->name);
			free(e->abbrev_name);
			free(e);
			continue;
		}
		s->entries[dst++] = e;
	}
	s->nr_entries = dst;
}

void dep_release(struct dep_summary *s)
{
	for (int i = 0; i < s->nr_entries; i++) {
		struct dep_entry *e = s->entries[i];
		for (int j = 0; j < e->nr_archives; j++) {
			struct dep_archive *a = e->archives[j];
			for (int k = 0; k < a->nr_symbols; k++)
				free(a->symbols[k]);
			free(a->symbols);
			free(a->name);
			free(a->abbrev_name);
			free(a);
		}
		free(e->archives);
		free(e->name);
		free(e->abbrev_name);
		free(e);
	}
	free(s->entries);
	memset(s, 0, sizeof(*s));
}
