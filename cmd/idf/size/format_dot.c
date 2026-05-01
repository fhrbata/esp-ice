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
 *
 * The textual DOT output also embeds @x{...} terminal-color tokens that
 * the platform fprintf shim expands on a tty and strips when piped.
 * Pipes into `dot -Tsvg` get clean text; dumping to a terminal gets
 * basic syntax highlighting (keywords yellow, string literals green,
 * numbers cyan, fillcolor names bold).
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

	fprintf(args->out, "@y{strict digraph} {\n");

	for (int i = 0; i < d.nr_entries; i++) {
		struct dep_entry *e = d.entries[i];
		const char *e_name = args->abbrev ? e->abbrev_name : e->name;

		if (!seen_has(seen, nr_seen, e->name)) {
			fprintf(args->out,
				"@g{\"%s\"} [@y{shape}=@b{box}, "
				"@y{label}=@g{\"%s (@c{%lld})\"}, "
				"@y{style}=@b{filled}, "
				"@y{fillcolor}=@g{\"darkorange3\"}]\n",
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
				fprintf(args->out,
					"@g{\"%s\"} [@y{shape}=@b{%s}, "
					"@y{label}=@g{\"%s (@c{%lld})\"}, "
					"@y{style}=@b{filled}, "
					"@y{fillcolor}=@g{\"%s\"}]\n",
					da->name, is_box ? "box" : "ellipse",
					da_name, (long long)da->size,
					is_box ? "darkorange3" : "azure");
				ALLOC_GROW(seen, nr_seen + 1, alloc_seen);
				seen[nr_seen++] = da->name;
			}

			fprintf(args->out, "@g{\"%s\"} -> @g{\"%s\"}",
				args->dep_reverse ? da->name : e->name,
				args->dep_reverse ? e->name : da->name);

			if (args->dep_symbols) {
				/* Build the label as one fputs so the
				 * @y{...} / @g{...} blocks open and close
				 * inside a single expand_colors pass.
				 * Splitting across multiple stdio calls
				 * leaks state because the depth stack does
				 * not persist between them. */
				struct sbuf lbl = SBUF_INIT;
				sbuf_addstr(&lbl, " [@y{label}=@g{\"");
				for (int k = 0; k < da->nr_symbols; k++) {
					if (k)
						sbuf_addch(&lbl, '\n');
					sbuf_addstr(&lbl, da->symbols[k]);
				}
				sbuf_addstr(&lbl, "\"}]");
				fputs(lbl.buf, args->out);
				sbuf_release(&lbl);
			}
			fputc('\n', args->out);
		}
	}

	fprintf(args->out, "}\n");

	free(seen);
	dep_release(&d);
}
