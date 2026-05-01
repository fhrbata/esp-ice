/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file format_dot.c
 * @brief Graphviz DOT emitter for archive dependencies.
 *
 * Matches the upstream "strict digraph" layout:
 *   - boxes (darkorange3) for archives that have dependencies
 *   - ellipses (azure) for archives that only appear as dependencies
 *   - one edge per (user, provider) pair (or reversed when --dep-reverse)
 *   - edge label lists symbols when --dep-symbols is set
 */
#include "ice.h"

#include "format.h"
#include "size.h"
#include "views.h"

static int seen_has(const char **seen, int nr, const char *name)
{
	for (int i = 0; i < nr; i++)
		if (!strcmp(seen[i], name))
			return 1;
	return 0;
}

void fmt_dot_deps(const struct map_file *mf, const struct memmap *mm,
		  const struct elf_symbols *syms, const struct mm_args *args)
{
	struct dep_summary d;
	const char **seen = NULL;
	int nr_seen = 0, alloc_seen = 0;

	dep_build(mf, mm, syms, args, &d);
	dep_filter(&d, args);

	fprintf(args->out, "strict digraph {\n");

	for (int i = 0; i < d.nr_entries; i++) {
		struct dep_entry *e = d.entries[i];
		const char *e_name = args->abbrev ? e->abbrev_name : e->name;

		if (!seen_has(seen, nr_seen, e->name)) {
			fprintf(args->out,
				"\"%s\" [shape=box, label=\"%s (%lld)\", "
				"style=filled, fillcolor=\"darkorange3\"]\n",
				e->name, e_name, (long long)e->size);
			ALLOC_GROW(seen, nr_seen + 1, alloc_seen);
			seen[nr_seen++] = e->name;
		}

		for (int j = 0; j < e->nr_archives; j++) {
			struct dep_archive *da = e->archives[j];
			const char *da_name =
			    args->abbrev ? da->abbrev_name : da->name;

			if (!seen_has(seen, nr_seen, da->name)) {
				/* Determine shape: box if it also appears as
				 * an entry (i.e. has its own dependencies). */
				int is_box = 0;
				for (int k = 0; k < d.nr_entries; k++) {
					if (!strcmp(d.entries[k]->name,
						    da->name)) {
						is_box = 1;
						break;
					}
				}
				fprintf(
				    args->out,
				    "\"%s\" [shape=%s, label=\"%s (%lld)\", "
				    "style=filled, fillcolor=\"%s\"]\n",
				    da->name, is_box ? "box" : "ellipse",
				    da_name, (long long)da->size,
				    is_box ? "darkorange3" : "azure");
				ALLOC_GROW(seen, nr_seen + 1, alloc_seen);
				seen[nr_seen++] = da->name;
			}

			fprintf(args->out, "\"%s\" -> \"%s\"",
				args->dep_reverse ? da->name : e->name,
				args->dep_reverse ? e->name : da->name);

			if (args->dep_symbols) {
				fputs(" [label=\"", args->out);
				for (int k = 0; k < da->nr_symbols; k++) {
					if (k)
						fputc('\n', args->out);
					fputs(da->symbols[k], args->out);
				}
				fputs("\"]", args->out);
			}
			fputc('\n', args->out);
		}
	}

	fprintf(args->out, "}\n");

	free(seen);
	dep_release(&d);
}
