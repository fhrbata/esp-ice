/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file memmap.c
 * @brief Hierarchical memory map: load + ops.
 *
 * Mirrors esp-idf-size's memorymap module.  The layered structure is
 * memory type -> section -> archive -> object -> symbol.  Numbers are
 * carried as int64_t to support diff results going negative.
 *
 * Pipeline (mm_load):
 *
 *   1.  Filter linker memory regions (drop "*default*").
 *   2.  Split each linker region across chip memory-type boundaries
 *       so every fragment is owned by exactly one chip type.
 *   3.  Filter map output sections (drop empty / NOLOAD; if an ELF is
 *       available, restrict to SHF_ALLOC sections; otherwise fall back
 *       to the name-suffix heuristic used by upstream).
 *   4.  Annotate each input section with its leaf "symbols": real
 *       ELF symbols for STT_FUNC / STT_OBJECT entries when an ELF is
 *       provided, else one synthetic symbol per input section.
 *   5.  Split map sections at split-region boundaries, redistributing
 *       input sections (and within them, symbols).
 *   6.  For each (split) section, build an archives/objects/symbols
 *       sub-tree and attach it to the section.
 *   7.  Compute every memory type's total size from the regions
 *       (DIRAM-style aliases are detected so physical capacity is not
 *       counted twice).
 *   8.  Walk regions in address order; for every (split) section, find
 *       the containing region and accumulate the section into that
 *       memory type's `used` plus its `sections` list.
 *
 * Section names that overflow into a preceding region get an
 * "_overflow" suffix to disambiguate from the unsplit half (matches
 * upstream, which has run into this on small-RAM boards).
 *
 * After mm_load(), the operations mm_diff(), mm_unify(),
 * mm_remove_unused(), mm_ignore_flash_size(), mm_trim() and mm_sort()
 * post-process the tree.  All ownership is on the heap; mm_release()
 * frees everything.
 */
#include "ice.h"

#include "memmap.h"
#include "size.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

void mm_stable_sort(void **arr, int nr, int (*cmp)(const void *, const void *))
{
	for (int i = 1; i < nr; i++) {
		void *cur = arr[i];
		int j = i - 1;
		while (j >= 0 && cmp(&arr[j], &cur) > 0) {
			arr[j + 1] = arr[j];
			j--;
		}
		arr[j + 1] = cur;
	}
}

char *mm_basename(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *bs = strrchr(path, '\\');
	if (bs && bs > slash)
		slash = bs;
	return sbuf_strdup(slash ? slash + 1 : path);
}

char *mm_abbrev_section(const char *name)
{
	const char *last = strrchr(name, '.');
	struct sbuf sb = SBUF_INIT;

	/*
	 * Match the upstream split('.')[ -1 ] semantics: use the final
	 * dot-segment, prefixed with a single '.'.  Names without a
	 * dot become ".name".
	 */
	if (last && last != name) {
		sbuf_addstr(&sb, last);
	} else if (last == name) {
		/* Starts with '.' and has no other dot -- keep as-is. */
		sbuf_addstr(&sb, name);
	} else {
		sbuf_addch(&sb, '.');
		sbuf_addstr(&sb, name);
	}
	return sbuf_detach(&sb);
}

/* ------------------------------------------------------------------ */
/* find/add helpers                                                   */
/* ------------------------------------------------------------------ */

struct mm_type *mm_find_type(const struct memmap *mm, const char *name)
{
	for (int i = 0; i < mm->nr_types; i++)
		if (!strcmp(mm->types[i]->name, name))
			return mm->types[i];
	return NULL;
}

struct mm_type *mm_get_type(struct memmap *mm, const char *name)
{
	struct mm_type *t = mm_find_type(mm, name);
	if (t)
		return t;
	t = calloc(1, sizeof(*t));
	if (!t)
		die_errno("calloc");
	t->name = sbuf_strdup(name);
	ALLOC_GROW(mm->types, mm->nr_types + 1, mm->alloc_types);
	mm->types[mm->nr_types++] = t;
	return t;
}

static struct mm_section *find_section(const struct mm_type *t,
				       const char *name)
{
	for (int i = 0; i < t->nr_sections; i++)
		if (!strcmp(t->sections[i]->name, name))
			return t->sections[i];
	return NULL;
}

struct mm_section *mm_type_get_section(struct mm_type *t, const char *name)
{
	struct mm_section *s = find_section(t, name);
	if (s)
		return s;
	s = calloc(1, sizeof(*s));
	if (!s)
		die_errno("calloc");
	s->name = sbuf_strdup(name);
	s->abbrev_name = mm_abbrev_section(name);
	ALLOC_GROW(t->sections, t->nr_sections + 1, t->alloc_sections);
	t->sections[t->nr_sections++] = s;
	return s;
}

static struct mm_archive *find_archive(const struct mm_section *s,
				       const char *name)
{
	for (int i = 0; i < s->nr_archives; i++)
		if (!strcmp(s->archives[i]->name, name))
			return s->archives[i];
	return NULL;
}

struct mm_archive *mm_section_get_archive(struct mm_section *s,
					  const char *name)
{
	struct mm_archive *a = find_archive(s, name);
	if (a)
		return a;
	a = calloc(1, sizeof(*a));
	if (!a)
		die_errno("calloc");
	a->name = sbuf_strdup(name);
	a->abbrev_name = mm_basename(name);
	ALLOC_GROW(s->archives, s->nr_archives + 1, s->alloc_archives);
	s->archives[s->nr_archives++] = a;
	return a;
}

static struct mm_object *find_object(const struct mm_archive *a,
				     const char *name)
{
	for (int i = 0; i < a->nr_objs; i++)
		if (!strcmp(a->objs[i]->name, name))
			return a->objs[i];
	return NULL;
}

struct mm_object *mm_archive_get_object(struct mm_archive *a, const char *name)
{
	struct mm_object *o = find_object(a, name);
	if (o)
		return o;
	o = calloc(1, sizeof(*o));
	if (!o)
		die_errno("calloc");
	o->name = sbuf_strdup(name);
	o->abbrev_name = mm_basename(name);
	ALLOC_GROW(a->objs, a->nr_objs + 1, a->alloc_objs);
	a->objs[a->nr_objs++] = o;
	return o;
}

static struct mm_symbol *find_symbol(const struct mm_object *o,
				     const char *name)
{
	for (int i = 0; i < o->nr_syms; i++)
		if (!strcmp(o->syms[i]->name, name))
			return o->syms[i];
	return NULL;
}

struct mm_symbol *mm_object_get_symbol(struct mm_object *o, const char *name)
{
	struct mm_symbol *s = find_symbol(o, name);
	if (s)
		return s;
	s = calloc(1, sizeof(*s));
	if (!s)
		die_errno("calloc");
	s->name = sbuf_strdup(name);
	s->abbrev_name = sbuf_strdup(name);
	ALLOC_GROW(o->syms, o->nr_syms + 1, o->alloc_syms);
	o->syms[o->nr_syms++] = s;
	return s;
}

/* ------------------------------------------------------------------ */
/* Region splitting -- shared between size totals and section assign  */
/* ------------------------------------------------------------------ */

struct split_region {
	const char *name;
	uint64_t origin;
	uint64_t length;
	const char *attrs;
	const struct chip_mem_range *type;
};

static int split_regions(const struct map_file *mf,
			 const struct chip_info *chip,
			 struct split_region **out)
{
	struct split_region *sr = NULL;
	int nr = 0, alloc = 0;

	for (int i = 0; i < mf->nr_regions; i++) {
		const struct map_region *reg = &mf->regions[i];
		uint64_t origin = reg->origin;
		uint64_t remaining = reg->length;

		if (!strcmp(reg->name, "*default*"))
			continue;

		while (remaining) {
			int found = 0;

			for (int j = 0; chip->ranges[j].name; j++) {
				const struct chip_mem_range *cr =
				    &chip->ranges[j];
				uint64_t addr = cr->primary_addr;
				uint64_t len = cr->length;
				uint64_t avail, used;

				if (addr <= origin && origin < addr + len) {
					found = 1;
				} else if (cr->secondary_addr) {
					addr = cr->secondary_addr;
					if (addr <= origin &&
					    origin < addr + len)
						found = 1;
				}
				if (!found)
					continue;

				avail = len - (origin - addr);
				used = remaining < avail ? remaining : avail;

				ALLOC_GROW(sr, nr + 1, alloc);
				sr[nr].name = reg->name;
				sr[nr].origin = origin;
				sr[nr].length = used;
				sr[nr].attrs = reg->attrs;
				sr[nr].type = cr;
				nr++;

				origin += used;
				remaining -= used;
				break;
			}

			if (!found) {
				/*
				 * Reserved zero-size segments (e.g.
				 * rtc_fast_reserved_seg on esp32) sit at
				 * the exact end of an existing range.
				 * Glue them onto whichever fragment ends
				 * there.
				 */
				int fixed = 0;
				for (int k = 0; k < nr; k++) {
					uint64_t end =
					    sr[k].origin + sr[k].length;
					if (origin + remaining == end) {
						ALLOC_GROW(sr, nr + 1, alloc);
						sr[nr].name = reg->name;
						sr[nr].origin = origin;
						sr[nr].length = remaining;
						sr[nr].attrs = reg->attrs;
						sr[nr].type = sr[k].type;
						nr++;
						fixed = 1;
						break;
					}
				}
				if (!fixed)
					warn("cannot assign region '%s' to "
					     "any memory type",
					     reg->name);
				break;
			}
		}
	}

	*out = sr;
	return nr;
}

static int region_addr_cmp(const void *a, const void *b)
{
	const struct split_region *ra = a;
	const struct split_region *rb = b;
	if (ra->origin < rb->origin)
		return -1;
	if (ra->origin > rb->origin)
		return 1;
	return 0;
}

/*
 * Walk split regions and assign each chip memory type its physical
 * total, taking aliases into account.
 */
static void compute_type_sizes(struct memmap *mm, const struct split_region *sr,
			       int nr)
{
	for (int i = 0; i < nr; i++) {
		struct mm_type *t = mm_get_type(mm, sr[i].type->name);
		uint64_t type_offset = 0;
		int is_alias = 0;

		if (sr[i].type->secondary_addr) {
			uint64_t p = sr[i].type->primary_addr;
			uint64_t s = sr[i].type->secondary_addr;
			type_offset = p > s ? p - s : s - p;
		}

		for (int k = 0; k < i; k++) {
			uint64_t a, b, off;
			if (strcmp(sr[k].type->name, sr[i].type->name) != 0)
				continue;
			a = sr[k].origin;
			b = sr[i].origin;
			off = a > b ? a - b : b - a;
			if (off == type_offset &&
			    sr[k].length == sr[i].length) {
				is_alias = 1;
				break;
			}
		}
		if (!is_alias)
			t->size += (int64_t)sr[i].length;
	}
}

/* ------------------------------------------------------------------ */
/* Section filtering                                                  */
/* ------------------------------------------------------------------ */

static int has_suffix(const char *s, size_t len, const char *suffix)
{
	size_t suf_len = strlen(suffix);
	return len >= suf_len && !memcmp(s + len - suf_len, suffix, suf_len);
}

static int section_passes_name_filter(const char *name, size_t len)
{
	if (has_suffix(name, len, "dummy") ||
	    has_suffix(name, len, "reserved_for_iram") ||
	    has_suffix(name, len, "noload"))
		return 0;

	return has_suffix(name, len, ".text") ||
	       has_suffix(name, len, ".data") ||
	       has_suffix(name, len, ".bss") ||
	       has_suffix(name, len, ".rodata") ||
	       has_suffix(name, len, "noinit") ||
	       has_suffix(name, len, ".vectors") ||
	       strstr(name, ".flash") != NULL ||
	       strstr(name, ".eh_frame") != NULL;
}

static int section_passes_elf_filter(const char *name,
				     const struct elf_sections *secs)
{
	for (int i = 0; i < secs->nr; i++)
		if (secs->s[i].size && (secs->s[i].flags & SHF_ALLOC) &&
		    !strcmp(secs->s[i].name, name))
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Pipeline-internal section/input/symbol structs                     */
/*                                                                    */
/* These are working copies kept on the side because we mutate sizes  */
/* during splitting.  The final tree uses mm_section / mm_archive /   */
/* mm_object / mm_symbol nodes built at step 6.                       */
/* ------------------------------------------------------------------ */

struct ws_symbol {
	const char *name;
	uint64_t address;
	uint64_t size;
	int is_func; /**< STT_FUNC -- append "()" to the displayed name. */
};

struct ws_input {
	const char *name;
	uint64_t address;
	uint64_t size;
	uint64_t fill;
	const char *archive;
	const char *object;
	struct ws_symbol *syms;
	int nr_syms, alloc_syms;
};

struct ws_section {
	const char *name;
	uint64_t address;
	uint64_t size;
	struct ws_input *inputs;
	int nr_inputs, alloc_inputs;
};

static void ws_input_add_synthetic_symbol(struct ws_input *isec)
{
	struct ws_symbol *sy;
	ALLOC_GROW(isec->syms, isec->nr_syms + 1, isec->alloc_syms);
	sy = &isec->syms[isec->nr_syms++];
	memset(sy, 0, sizeof(*sy));
	sy->name = isec->name;
	sy->address = isec->address;
	sy->size = isec->size;
}

/*
 * Convert mf->sections (after filtering) into our private working
 * sections, copying input sections and dropping zero-size inputs.
 */
static int build_ws_sections(const struct map_file *mf,
			     const struct elf_sections *elf_secs,
			     struct ws_section **out)
{
	struct ws_section *ws = NULL;
	int nr = 0, alloc = 0;

	for (int i = 0; i < mf->nr_sections; i++) {
		const struct map_section *sec = &mf->sections[i];
		size_t nlen = strlen(sec->name);
		struct ws_section *w;

		if (!sec->size)
			continue;

		if (has_suffix(sec->name, nlen, "dummy") ||
		    has_suffix(sec->name, nlen, "reserved_for_iram") ||
		    has_suffix(sec->name, nlen, "noload"))
			continue;

		if (elf_secs) {
			if (!section_passes_elf_filter(sec->name, elf_secs))
				continue;
		} else {
			if (!section_passes_name_filter(sec->name, nlen))
				continue;
		}

		ALLOC_GROW(ws, nr + 1, alloc);
		w = &ws[nr];
		memset(w, 0, sizeof(*w));
		w->name = sec->name;
		w->address = sec->address;
		w->size = sec->size;
		nr++;

		for (int j = 0; j < sec->nr_inputs; j++) {
			const struct map_input *in = &sec->inputs[j];
			struct ws_input *wi;

			if (!in->size && !in->fill)
				continue;

			ALLOC_GROW(w->inputs, w->nr_inputs + 1,
				   w->alloc_inputs);
			wi = &w->inputs[w->nr_inputs++];
			memset(wi, 0, sizeof(*wi));
			wi->name = in->name;
			wi->address = in->address;
			wi->size = in->size;
			wi->fill = in->fill;
			wi->archive = in->archive;
			wi->object = in->object;
		}
	}

	*out = ws;
	return nr;
}

static void ws_release(struct ws_section *ws, int nr)
{
	for (int i = 0; i < nr; i++) {
		for (int j = 0; j < ws[i].nr_inputs; j++)
			free(ws[i].inputs[j].syms);
		free(ws[i].inputs);
	}
	free(ws);
}

/* ------------------------------------------------------------------ */
/* ELF symbol -> input section assignment                             */
/* ------------------------------------------------------------------ */

struct sym_ptr_arr {
	const struct elf_symbol **s;
	int nr;
};

static int sym_addr_cmp(const void *a, const void *b)
{
	const struct elf_symbol *sa = *(const struct elf_symbol *const *)a;
	const struct elf_symbol *sb = *(const struct elf_symbol *const *)b;
	if (sa->value < sb->value)
		return -1;
	if (sa->value > sb->value)
		return 1;
	return 0;
}

/*
 * Place STT_FUNC / STT_OBJECT symbols into their host input section
 * (matching the upstream behavior so per-archive details look the
 * same).  Input sections that don't get any ELF symbols receive a
 * synthetic "self" symbol so reports never show holes.
 *
 * Cross-buffer pointer use: the symbols array is a pre-sorted view
 * into the elf_symbols already owned by the caller; we only borrow
 * pointers and never free them here.
 */
static void assign_elf_symbols(struct ws_section *ws, int nr_ws,
			       const struct elf_symbols *syms)
{
	struct sym_ptr_arr arr;
	struct ws_input **isecs = NULL;
	int nr_isecs = 0, alloc_isecs = 0;
	int next = 0;

	arr.nr = 0;
	arr.s = NULL;
	if (syms) {
		arr.s = calloc(syms->nr, sizeof(*arr.s));
		if (!arr.s)
			die_errno("calloc");
		for (int i = 0; i < syms->nr; i++) {
			const struct elf_symbol *s = &syms->s[i];
			if (!s->size)
				continue;
			if (s->type != STT_FUNC && s->type != STT_OBJECT)
				continue;
			arr.s[arr.nr++] = s;
		}
		qsort(arr.s, arr.nr, sizeof(*arr.s), sym_addr_cmp);
	}

	/* Flatten input sections in address-sorted order. */
	for (int i = 0; i < nr_ws; i++) {
		for (int j = 0; j < ws[i].nr_inputs; j++) {
			ALLOC_GROW(isecs, nr_isecs + 1, alloc_isecs);
			isecs[nr_isecs++] = &ws[i].inputs[j];
		}
	}
	{
		/* Stable sort by address (input sections within a section
		 * are already address-ordered; this puts cross-section
		 * inputs in global order). */
		for (int i = 1; i < nr_isecs; i++) {
			struct ws_input *cur = isecs[i];
			int j = i - 1;
			while (j >= 0 && isecs[j]->address > cur->address) {
				isecs[j + 1] = isecs[j];
				j--;
			}
			isecs[j + 1] = cur;
		}
	}

	for (int si = 0; si < arr.nr; si++) {
		const struct elf_symbol *sym = arr.s[si];
		uint64_t addr = sym->value;
		uint64_t end = addr + sym->size;
		struct ws_input *isec = NULL;

		while (next < nr_isecs) {
			isec = isecs[next];
			if (addr < isec->address + isec->size)
				break;

			if (!isec->nr_syms)
				ws_input_add_synthetic_symbol(isec);
			next++;
			isec = NULL;
		}

		if (!isec)
			break;
		if (addr < isec->address)
			continue;
		if (end > isec->address + isec->size)
			continue;

		ALLOC_GROW(isec->syms, isec->nr_syms + 1, isec->alloc_syms);
		{
			struct ws_symbol *ds = &isec->syms[isec->nr_syms++];
			ds->name = sym->name;
			ds->address = addr;
			ds->size = sym->size;
			ds->is_func = (sym->type == STT_FUNC);
		}
	}
	free(arr.s);

	/*
	 * Don't backfill trailing input sections that the symbol walk
	 * never touched -- upstream lets those come through with an
	 * empty symbol list, which means the input contributes to
	 * archive/object totals but doesn't add a symbol entry.  This
	 * matters for sections like .flash.tdata whose contents are
	 * input-section-named only and have no STT_FUNC/STT_OBJECT
	 * coverage in the ELF.
	 */
	free(isecs);
}

static void synthetic_symbols_only(struct ws_section *ws, int nr_ws)
{
	for (int i = 0; i < nr_ws; i++)
		for (int j = 0; j < ws[i].nr_inputs; j++)
			ws_input_add_synthetic_symbol(&ws[i].inputs[j]);
}

/* ------------------------------------------------------------------ */
/* Section split at memory-type boundaries                            */
/* ------------------------------------------------------------------ */

/*
 * Split a single section if it crosses any region boundary.  Result is
 * pushed onto *out as one or more sections, each fitting into exactly
 * one region.  Input sections (and their symbols) are split too.
 */
static void split_one_section(struct ws_section *src,
			      const struct split_region *regions,
			      int nr_regions, struct ws_section **out, int *nr,
			      int *alloc)
{
	int matched = -1;

	for (int j = 0; j < nr_regions; j++) {
		uint64_t rstart = regions[j].origin;
		uint64_t rend = rstart + regions[j].length;

		if (src->address < rstart || src->address >= rend)
			continue;

		if (src->address + src->size <= rend) {
			matched = j;
			break;
		}

		/* Crosses split_addr = rend.  Split into head + tail. */
		{
			uint64_t split = rend;
			uint64_t head_size = split - src->address;
			struct ws_section head;
			struct ws_section tail;

			memset(&head, 0, sizeof(head));
			memset(&tail, 0, sizeof(tail));
			head.name = src->name;
			head.address = src->address;
			head.size = head_size;
			tail.name = src->name;
			tail.address = split;
			tail.size = src->size - head_size;

			for (int k = 0; k < src->nr_inputs; k++) {
				struct ws_input *isec = &src->inputs[k];
				uint64_t iend =
				    isec->address + isec->size + isec->fill;

				if (iend <= split) {
					ALLOC_GROW(head.inputs,
						   head.nr_inputs + 1,
						   head.alloc_inputs);
					head.inputs[head.nr_inputs++] = *isec;
					/* ownership transferred */
					memset(isec, 0, sizeof(*isec));
				} else if (isec->address >= split) {
					ALLOC_GROW(tail.inputs,
						   tail.nr_inputs + 1,
						   tail.alloc_inputs);
					tail.inputs[tail.nr_inputs++] = *isec;
					memset(isec, 0, sizeof(*isec));
				} else {
					struct ws_input h = *isec;
					struct ws_input t = *isec;
					uint64_t hsize;

					hsize = split - isec->address;
					if (hsize > isec->size)
						hsize = isec->size;
					h.size = hsize;
					h.fill = split - (h.address + h.size);
					h.syms = NULL;
					h.nr_syms = h.alloc_syms = 0;

					t.address = split;
					t.size = isec->size - hsize;
					t.fill = isec->fill - h.fill;
					t.syms = NULL;
					t.nr_syms = t.alloc_syms = 0;

					/* Split symbols. */
					for (int m = 0; m < isec->nr_syms;
					     m++) {
						struct ws_symbol *sy =
						    &isec->syms[m];
						uint64_t syend =
						    sy->address + sy->size;
						if (syend <= split) {
							ALLOC_GROW(
							    h.syms,
							    h.nr_syms + 1,
							    h.alloc_syms);
							h.syms[h.nr_syms++] =
							    *sy;
						} else if (sy->address >=
							   split) {
							ALLOC_GROW(
							    t.syms,
							    t.nr_syms + 1,
							    t.alloc_syms);
							t.syms[t.nr_syms++] =
							    *sy;
						} else {
							struct ws_symbol h2 =
							    *sy;
							struct ws_symbol t2 =
							    *sy;
							h2.size =
							    split - sy->address;
							t2.address = split;
							t2.size =
							    sy->size - h2.size;
							ALLOC_GROW(
							    h.syms,
							    h.nr_syms + 1,
							    h.alloc_syms);
							h.syms[h.nr_syms++] =
							    h2;
							ALLOC_GROW(
							    t.syms,
							    t.nr_syms + 1,
							    t.alloc_syms);
							t.syms[t.nr_syms++] =
							    t2;
						}
					}
					free(isec->syms);
					memset(isec, 0, sizeof(*isec));

					ALLOC_GROW(head.inputs,
						   head.nr_inputs + 1,
						   head.alloc_inputs);
					head.inputs[head.nr_inputs++] = h;
					ALLOC_GROW(tail.inputs,
						   tail.nr_inputs + 1,
						   tail.alloc_inputs);
					tail.inputs[tail.nr_inputs++] = t;
				}
			}

			ALLOC_GROW(*out, *nr + 1, *alloc);
			(*out)[(*nr)++] = head;

			/* Recurse on tail (may need further splitting). */
			free(src->inputs);
			*src = tail;
			split_one_section(src, regions, nr_regions, out, nr,
					  alloc);
			return;
		}
	}

	(void)matched;

	/* Fits as-is (or no region matched). */
	ALLOC_GROW(*out, *nr + 1, *alloc);
	(*out)[(*nr)++] = *src;
	memset(src, 0, sizeof(*src));
}

static int split_ws_sections(struct ws_section *src, int nr_src,
			     const struct split_region *regions, int nr_regions,
			     struct ws_section **out)
{
	struct ws_section *dst = NULL;
	int nr = 0, alloc = 0;

	/*
	 * Walk last-to-first to match upstream's stack-pop traversal
	 * (esp_idf_size.memorymap._split_map_sections).  This is the
	 * order memory_map.sections gets populated in, and downstream
	 * consumers (the JSON dump, archives/files columns) lock the
	 * column / key order to that traversal.
	 */
	for (int i = nr_src - 1; i >= 0; i--)
		split_one_section(&src[i], regions, nr_regions, &dst, &nr,
				  &alloc);

	*out = dst;
	return nr;
}

/* ------------------------------------------------------------------ */
/* Build mm tree                                                      */
/* ------------------------------------------------------------------ */

static void section_attach_archives(struct mm_section *mm_s,
				    const struct ws_section *ws)
{
	for (int i = 0; i < ws->nr_inputs; i++) {
		const struct ws_input *isec = &ws->inputs[i];
		struct mm_archive *a;
		struct mm_object *o;

		if (!isec->archive || !*isec->archive)
			continue;

		a = mm_section_get_archive(mm_s, isec->archive);
		a->size += (int64_t)isec->size;

		o = mm_archive_get_object(a, isec->object);
		o->size += (int64_t)isec->size;

		for (int j = 0; j < isec->nr_syms; j++) {
			struct mm_symbol *sym;
			const struct ws_symbol *ws = &isec->syms[j];
			if (ws->is_func) {
				size_t n = strlen(ws->name);
				char *buf = malloc(n + 3);
				if (!buf)
					die_errno("malloc");
				memcpy(buf, ws->name, n);
				buf[n] = '(';
				buf[n + 1] = ')';
				buf[n + 2] = '\0';
				sym = mm_object_get_symbol(o, buf);
				free(buf);
			} else {
				sym = mm_object_get_symbol(o, ws->name);
			}
			sym->size += (int64_t)ws->size;
		}
	}
}

/*
 * Walk regions in address order; for every (already-split) section,
 * find its containing region and accumulate.
 */
static void assign_sections_to_types(struct memmap *mm,
				     const struct ws_section *ws, int nr_ws,
				     struct split_region *regions,
				     int nr_regions)
{
	if (nr_regions <= 0)
		return;

	qsort(regions, (size_t)nr_regions, sizeof(regions[0]), region_addr_cmp);

	for (int i = 0; i < nr_ws; i++) {
		const struct ws_section *src = &ws[i];
		const struct split_region *prev = NULL;
		int placed = 0;

		for (int j = 0; j < nr_regions; j++) {
			uint64_t addr = regions[j].origin;
			uint64_t len = regions[j].length;
			struct mm_type *t;
			struct mm_section *mm_s;

			if (addr <= src->address && src->address < addr + len) {
				t = mm_get_type(mm, regions[j].type->name);
				t->used += (int64_t)src->size;
				mm_s = mm_type_get_section(t, src->name);
				mm_s->size += (int64_t)src->size;
				section_attach_archives(mm_s, src);
				placed = 1;
				break;
			}
			if (addr > src->address && prev) {
				/*
				 * Section sits before this region but no
				 * earlier region claimed it -- attribute to
				 * the preceding region with an "_overflow"
				 * suffix so it doesn't collide with the head.
				 */
				struct sbuf nb = SBUF_INIT;
				t = mm_get_type(mm, prev->type->name);
				sbuf_addstr(&nb, src->name);
				sbuf_addstr(&nb, "_overflow");
				warn("%s overflow detected: section '%s' "
				     "(addr 0x%llx, size %llu) does not fit "
				     "into any region; assigning to %s",
				     prev->type->name, src->name,
				     (unsigned long long)src->address,
				     (unsigned long long)src->size, prev->name);
				t->used += (int64_t)src->size;
				mm_s = mm_type_get_section(t, nb.buf);
				mm_s->size += (int64_t)src->size;
				section_attach_archives(mm_s, src);
				sbuf_release(&nb);
				placed = 1;
				break;
			}
			prev = &regions[j];
		}

		if (!placed)
			warn("cannot assign section '%s' (addr 0x%llx) "
			     "to any memory type",
			     src->name, (unsigned long long)src->address);
	}
}

/* ------------------------------------------------------------------ */
/* Image size                                                         */
/* ------------------------------------------------------------------ */

static int64_t compute_image_size(const struct elf_sections *elf_secs,
				  const struct ws_section *ws, int nr_ws)
{
	int64_t total = 0;

	if (elf_secs) {
		for (int i = 0; i < elf_secs->nr; i++) {
			const struct elf_section *s = &elf_secs->s[i];
			if (!s->size)
				continue;
			if (!(s->flags & SHF_ALLOC))
				continue;
			if (s->type != SHT_PROGBITS)
				continue;
			total += (int64_t)s->size;
		}
		return total;
	}

	/* Fallback: every filtered section minus .bss / noinit. */
	for (int i = 0; i < nr_ws; i++) {
		const char *n = ws[i].name;
		size_t nlen = strlen(n);
		if (has_suffix(n, nlen, ".bss") ||
		    has_suffix(n, nlen, "noinit"))
			continue;
		total += (int64_t)ws[i].size;
	}
	return total;
}

/* ------------------------------------------------------------------ */
/* Public load                                                        */
/* ------------------------------------------------------------------ */

void mm_load(struct memmap *out, struct map_file *mf,
	     const struct chip_info *chip, const char *target,
	     const char *project_path, const struct elf_sections *elf_secs,
	     const struct elf_symbols *elf_syms, int load_symbols)
{
	struct split_region *regions = NULL;
	int nr_regions = 0;
	struct ws_section *ws = NULL;
	struct ws_section *split = NULL;
	int nr_ws = 0;
	int nr_split = 0;

	memset(out, 0, sizeof(*out));
	out->target = sbuf_strdup(target ? target : "");
	out->target_diff = sbuf_strdup("");
	out->project_path = sbuf_strdup(project_path ? project_path : "");
	out->project_path_diff = sbuf_strdup("");

	/* Pre-create one mm_type per chip range alias (in chip-info order),
	 * matching upstream so the JSON memory_types preserves that
	 * insertion order. */
	for (int i = 0; chip->ranges[i].name; i++)
		mm_get_type(out, chip->ranges[i].name);

	nr_regions = split_regions(mf, chip, &regions);
	compute_type_sizes(out, regions, nr_regions);

	nr_ws = build_ws_sections(mf, elf_secs, &ws);

	/*
	 * Match upstream's three-way choice:
	 *   load_symbols=False: no symbols added at all (cheap path,
	 *     used by tree / json2 / table summaries that don't need
	 *     per-symbol detail).
	 *   load_symbols=True + ELF: real STT_FUNC / STT_OBJECT entries.
	 *   load_symbols=True, no ELF: one synthetic symbol per input
	 *     section so reports never show holes.
	 */
	if (load_symbols) {
		if (elf_syms)
			assign_elf_symbols(ws, nr_ws, elf_syms);
		else
			synthetic_symbols_only(ws, nr_ws);
	}

	/*
	 * Compute image_size from the *unsplit* ws list -- split_ws_sections
	 * mutates its input (zeroing entries it has consumed via tail
	 * recursion), so anything that needs ws.name must happen first.
	 */
	out->image_size = compute_image_size(elf_secs, ws, nr_ws);

	nr_split = split_ws_sections(ws, nr_ws, regions, nr_regions, &split);

	if (nr_split > 0)
		assign_sections_to_types(out, split, nr_split, regions,
					 nr_regions);

	ws_release(ws, nr_ws);
	ws_release(split, nr_split);
	free(regions);
}

/* ------------------------------------------------------------------ */
/* Release                                                            */
/* ------------------------------------------------------------------ */

static void sym_free(struct mm_symbol *s)
{
	free(s->name);
	free(s->abbrev_name);
	free(s);
}

static void obj_free(struct mm_object *o)
{
	for (int i = 0; i < o->nr_syms; i++)
		sym_free(o->syms[i]);
	free(o->syms);
	free(o->name);
	free(o->abbrev_name);
	free(o);
}

static void arch_free(struct mm_archive *a)
{
	for (int i = 0; i < a->nr_objs; i++)
		obj_free(a->objs[i]);
	free(a->objs);
	free(a->name);
	free(a->abbrev_name);
	free(a);
}

static void sec_free(struct mm_section *s)
{
	for (int i = 0; i < s->nr_archives; i++)
		arch_free(s->archives[i]);
	free(s->archives);
	free(s->name);
	free(s->abbrev_name);
	free(s);
}

static void type_free(struct mm_type *t)
{
	for (int i = 0; i < t->nr_sections; i++)
		sec_free(t->sections[i]);
	free(t->sections);
	free(t->name);
	free(t);
}

void mm_release(struct memmap *out)
{
	for (int i = 0; i < out->nr_types; i++)
		type_free(out->types[i]);
	free(out->types);
	free(out->target);
	free(out->target_diff);
	free(out->project_path);
	free(out->project_path_diff);
	memset(out, 0, sizeof(*out));
}
