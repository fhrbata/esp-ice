/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/size/size.c
 * @brief CLI dispatch for `ice idf size`.
 *
 * Mirrors esp-idf-size's main.py: parse the command line, load the
 * linker map (and optionally the .elf and project_description.json),
 * apply the requested memmap operations (diff, unify, remove_unused,
 * ignore_flash_size, sort), then dispatch to the formatter selected
 * by --format and the view selectors (--archives, --files,
 * --archive-details, --archive-dependencies).
 *
 * Sources around it:
 *   memmap.c / memmap_ops.c -- data model + load + ops
 *   views.c                 -- per-archive / per-file / per-symbol
 *   format_table.c          -- box-drawing summary / pivot tables
 *   format_csv.c            -- CSV emitter (reuses table builder)
 *   format_json.c           -- raw + json2 emitters
 *   format_tree.c           -- indented tree
 *   format_dot.c            -- Graphviz DOT for archive deps
 */
#include "ice.h"

#include "format.h"
#include "memmap.h"
#include "size.h"
#include "views.h"

/* ------------------------------------------------------------------ */
/* Manual / completion                                                 */
/* ------------------------------------------------------------------ */

/* clang-format off */
static const struct cmd_manual idf_size_manual = {
	.name = "ice idf size",
	.summary = "analyse firmware memory usage",

	.description =
	H_PARA("Reads a GCC/LD linker map file and prints memory usage "
	       "broken down by chip memory region (IRAM, DRAM, Flash, "
	       "...) and by output section (@b{.text}, @b{.data}, "
	       "@b{.bss}, @b{.rodata}, ...).")
	H_PARA("With @b{--archives}, @b{--files}, or "
	       "@b{--archive-details NAME}, contributions are pivoted "
	       "by archive, object file, or per-symbol within a single "
	       "archive.  @b{--archive-dependencies} (alias "
	       "@b{--archive-deps}) walks the linker's Cross Reference "
	       "Table and emits one entry per archive with the libraries "
	       "it pulls in (or the libraries that pull it in, with "
	       "@b{--dep-reverse}).")
	H_PARA("Output formats: @b{table} (default), @b{tree}, @b{csv}, "
	       "@b{json2}, @b{raw}, @b{dot}.  @b{raw} dumps the full "
	       "memory map; @b{json2} the public summary schema "
	       "(version 1.2).  @b{dot} is only valid with "
	       "@b{--archive-dependencies}.")
	H_PARA("@b{--diff REF.map} compares two builds: positive deltas "
	       "mean the current build is bigger.  @b{--unify} collapses "
	       "section / archive / object / symbol names by their "
	       "abbreviated form, useful when comparing builds across "
	       "esp-idf versions where toolchain paths differ."),

	.examples =
	H_EXAMPLE("ice idf size build/hello-world.map")
	H_EXAMPLE("ice idf size --target esp32s3 build/app.map")
	H_EXAMPLE("ice idf size --archives --format table build/app.map")
	H_EXAMPLE("ice idf size --diff build/old.map build/new.map")
	H_EXAMPLE("ice idf size --archive-dependencies --format dot "
		  "build/app.map | dot -Tsvg -o deps.svg"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice cmake size",
	       "Invoke ESP-IDF's native size target (different format)."),
};
/* clang-format on */

/* ------------------------------------------------------------------ */
/* Option storage                                                      */
/* ------------------------------------------------------------------ */

static const char *opt_target;
static const char *opt_format = "table";
static int opt_archives;
static int opt_archive_deps;
static int opt_dep_symbols;
static int opt_dep_reverse;
static const char *opt_archive_details;
static int opt_files;
static const char *opt_diff;
static int opt_no_abbrev;
static int opt_unify;
static int opt_show_unused;
static int opt_show_unchanged;
static int opt_use_flash_size;
static const char *opt_sort = "1";
static int opt_sort_diff;
static int opt_sort_reverse_flag; /* 1 = ascending; 0 = descending (default) */
static struct svec opt_filter = SVEC_INIT;
static const char *opt_output_file;
static int opt_quiet;
static int opt_no_color;

/* ------------------------------------------------------------------ */
/* Option table                                                        */
/* ------------------------------------------------------------------ */

static const struct option cmd_idf_size_opts[] = {
    OPT_STRING('t', "target", &opt_target, "chip",
	       "target chip override (e.g. esp32s3)", NULL),
    OPT_STRING(0, "format", &opt_format, "fmt",
	       "output format: table, text, tree, csv, json2, raw, dot", NULL),
    OPT_BOOL(0, "archives", &opt_archives, "print per-archive sizes"),
    OPT_BOOL(0, "archive-dependencies", &opt_archive_deps,
	     "show archive dependency graph"),
    OPT_BOOL(0, "archive-deps", &opt_archive_deps,
	     "alias for --archive-dependencies"),
    OPT_BOOL(0, "dep-symbols", &opt_dep_symbols,
	     "include symbol list in archive deps"),
    OPT_BOOL(0, "dep-syms", &opt_dep_symbols, "alias for --dep-symbols"),
    OPT_BOOL(0, "dep-reverse", &opt_dep_reverse,
	     "show reverse dependencies (def -> users)"),
    OPT_BOOL(0, "dep-rev", &opt_dep_reverse, "alias for --dep-reverse"),
    OPT_STRING(0, "archive-details", &opt_archive_details, "ARCHIVE",
	       "print per-symbol sizes for one archive", NULL),
    OPT_BOOL(0, "files", &opt_files, "print per-file sizes"),
    OPT_STRING(0, "diff", &opt_diff, "MAP",
	       "compare against another linker map file", NULL),
    OPT_BOOL(0, "no-abbrev", &opt_no_abbrev,
	     "use full names instead of abbreviations"),
    OPT_BOOL(0, "unify", &opt_unify,
	     "aggregate by abbreviated name across the tree"),
    OPT_BOOL(0, "show-unused", &opt_show_unused,
	     "show unused memory types and sections"),
    OPT_BOOL(0, "show-unchanged", &opt_show_unchanged,
	     "show unchanged entries when --diff is used"),
    OPT_BOOL(0, "use-flash-size", &opt_use_flash_size,
	     "report flash totals from the link map (rarely accurate)"),
    OPT_STRING('s', "sort", &opt_sort, "COL",
	       "sort by column index/name (default: 1)", NULL),
    OPT_BOOL(0, "sort-diff", &opt_sort_diff, "sort by diff value, not size"),
    OPT_BOOL(0, "sort-reverse", &opt_sort_reverse_flag,
	     "sort ascending (default is descending)"),
    OPT_STRING_LIST('F', "filter", &opt_filter, "PATTERN",
		    "filter archives/files/symbols by glob (repeatable)", NULL),
    OPT_STRING('o', "output-file", &opt_output_file, "FILE",
	       "write output to FILE instead of stdout", NULL),
    OPT_BOOL('q', "quiet", &opt_quiet, "suppress non-essential output"),
    OPT_BOOL(0, "no-color", &opt_no_color, "disable ANSI color escapes"),
    OPT_END(),
};

int cmd_idf_size(int argc, const char **argv);

const struct cmd_desc cmd_idf_size_desc = {
    .name = "size",
    .fn = cmd_idf_size,
    .opts = cmd_idf_size_opts,
    .manual = &idf_size_manual,
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * Try to load <map_dir>/project_description.json.  Returns NULL when
 * the file isn't present or doesn't match the map file's basename
 * (multi-executable projects emit one .map per exe; we want only the
 * matching project_description).
 */
static struct json_value *load_proj_desc(const char *map_path,
					 char **out_target)
{
	struct sbuf path = SBUF_INIT;
	struct sbuf body = SBUF_INIT;
	const char *slash;
	size_t dirlen;
	struct json_value *root;
	const char *target_str;
	const char *app_elf_str;
	const char *map_base;

	*out_target = NULL;

	slash = strrchr(map_path, '/');
	if (!slash)
		slash = strrchr(map_path, '\\');
	dirlen = slash ? (size_t)(slash - map_path) : 0;

	if (dirlen) {
		sbuf_add(&path, map_path, dirlen);
		sbuf_addch(&path, '/');
	}
	sbuf_addstr(&path, "project_description.json");

	if (sbuf_read_file(&body, path.buf) < 0) {
		sbuf_release(&path);
		sbuf_release(&body);
		return NULL;
	}

	root = json_parse(body.buf, body.len);
	sbuf_release(&body);
	sbuf_release(&path);
	if (!root)
		return NULL;

	/* Match map basename against app_elf basename. */
	map_base = slash ? slash + 1 : map_path;
	app_elf_str = json_as_string(json_get(root, "app_elf"));
	if (app_elf_str && *app_elf_str) {
		const char *elf_slash = strrchr(app_elf_str, '/');
		if (!elf_slash)
			elf_slash = strrchr(app_elf_str, '\\');
		const char *elf_base = elf_slash ? elf_slash + 1 : app_elf_str;
		const char *map_dot = strrchr(map_base, '.');
		const char *elf_dot = strrchr(elf_base, '.');
		size_t mlen =
		    map_dot ? (size_t)(map_dot - map_base) : strlen(map_base);
		size_t elen =
		    elf_dot ? (size_t)(elf_dot - elf_base) : strlen(elf_base);
		if (mlen != elen || strncmp(map_base, elf_base, mlen) != 0) {
			json_free(root);
			return NULL;
		}
	}

	target_str = json_as_string(json_get(root, "target"));
	if (target_str)
		*out_target = sbuf_strdup(target_str);

	return root;
}

/*
 * Try to load a sibling .elf file if project_description.json is
 * present.  Returns NULL if no ELF is available; the caller treats
 * this as "no ELF info".
 */
static struct sbuf *load_elf(const char *map_path,
			     const struct json_value *proj_desc)
{
	struct sbuf path = SBUF_INIT;
	struct sbuf *body;
	const char *slash;
	size_t dirlen;
	const char *app_elf_str;
	ssize_t n;

	if (!proj_desc)
		return NULL;
	app_elf_str = json_as_string(json_get(proj_desc, "app_elf"));
	if (!app_elf_str || !*app_elf_str)
		return NULL;

	slash = strrchr(map_path, '/');
	if (!slash)
		slash = strrchr(map_path, '\\');
	dirlen = slash ? (size_t)(slash - map_path) : 0;

	if (dirlen) {
		sbuf_add(&path, map_path, dirlen);
		sbuf_addch(&path, '/');
	}
	sbuf_addstr(&path, app_elf_str);

	body = calloc(1, sizeof(*body));
	if (!body)
		die_errno("calloc");

	n = sbuf_read_file(body, path.buf);
	sbuf_release(&path);
	if (n < 0) {
		sbuf_release(body);
		free(body);
		return NULL;
	}
	return body;
}

/*
 * One-shot end-to-end loader.  Reads the .map and (best-effort)
 * locates an .elf and project_description.json, then builds a
 * memmap.  out_buf, out_elf_buf, out_secs, out_syms, out_mf are
 * populated so the caller can release them once it's done.
 */
struct loaded {
	struct sbuf map_buf;
	struct sbuf *elf_buf;
	struct map_file mf;
	struct elf_sections secs;
	struct elf_symbols syms;
	int has_secs;
	int has_syms;
};

static void loaded_load(struct loaded *L, const char *map_path,
			const char *target_override, struct memmap *out_mm,
			int load_symbols)
{
	struct json_value *proj_desc;
	char *target_from_proj = NULL;
	const char *target;
	const struct chip_info *chip;
	char *project_path;

	memset(L, 0, sizeof(*L));
	sbuf_init(&L->map_buf);

	if (sbuf_read_file(&L->map_buf, map_path) < 0)
		die_errno("cannot read '%s'", map_path);

	map_read(L->map_buf.buf, L->map_buf.len, &L->mf);

	proj_desc = load_proj_desc(map_path, &target_from_proj);
	L->elf_buf = load_elf(map_path, proj_desc);

	if (L->elf_buf) {
		elf_read_sections(L->elf_buf->buf, L->elf_buf->len, &L->secs);
		L->has_secs = 1;
		/*
		 * Always read the symbol table when an ELF is available --
		 * archive-dependency filtering needs to know which symbols
		 * survived the link, even when load_symbols is false (the
		 * latter only controls whether per-symbol entries land in
		 * the memmap tree).
		 */
		elf_read_symbols(L->elf_buf->buf, L->elf_buf->len, &L->syms);
		L->has_syms = 1;
	}

	target = target_override
		     ? target_override
		     : (target_from_proj ? target_from_proj : L->mf.target);
	if (!target)
		die("cannot determine chip target; use --target");

	chip = chip_find(target);
	if (!chip)
		die("unknown target '%s'", target);

	/* Use absolute-ish path for project_path -- we can't easily
	 * canonicalize without realpath, so just store what was given. */
	project_path = sbuf_strdup(map_path);

	mm_load(out_mm, &L->mf, chip, target, project_path,
		L->has_secs ? &L->secs : NULL, L->has_syms ? &L->syms : NULL,
		load_symbols);

	free(project_path);
	free(target_from_proj);
	json_free(proj_desc);
}

static void loaded_release(struct loaded *L)
{
	if (L->has_syms)
		elf_symbols_release(&L->syms);
	if (L->has_secs)
		elf_sections_release(&L->secs);
	if (L->elf_buf) {
		sbuf_release(L->elf_buf);
		free(L->elf_buf);
	}
	map_release(&L->mf);
	sbuf_release(&L->map_buf);
}

/* ------------------------------------------------------------------ */
/* Args wiring                                                         */
/* ------------------------------------------------------------------ */

static void args_init(struct mm_args *a, const char *map_path)
{
	(void)map_path;
	memset(a, 0, sizeof(*a));
	a->format = opt_format;
	a->archives = opt_archives;
	a->archive_deps = opt_archive_deps;
	a->dep_symbols = opt_dep_symbols;
	a->dep_reverse = opt_dep_reverse;
	a->archive_details = opt_archive_details;
	a->files = opt_files;
	a->diff = opt_diff;
	a->no_abbrev = opt_no_abbrev;
	a->abbrev = !opt_no_abbrev;
	a->unify = opt_unify;
	a->show_unused = opt_show_unused;
	a->show_unchanged = opt_show_unchanged;
	a->use_flash_size = opt_use_flash_size;
	a->target_override = opt_target;
	a->sort = opt_sort;
	a->sort_diff = opt_sort_diff;
	/* Upstream calls the flag --sort-reverse but uses store_false
	 * (i.e. by default reverse=True meaning DESCENDING; passing the
	 * flag flips it to ASCENDING).  Match that behaviour: the flag
	 * stored in opt_sort_reverse_flag is "ascending requested", so
	 * args.sort_reverse (the flag the comparators consume to mean
	 * "swap order") is the opposite. */
	a->sort_reverse = !opt_sort_reverse_flag;
	a->filter = opt_filter.v;
	a->nr_filter = (int)opt_filter.nr;
	a->output_file = opt_output_file;
	a->quiet = opt_quiet;
	a->no_color = opt_no_color;
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static void validate_args(struct mm_args *a)
{
	if (a->no_abbrev && a->unify) {
		warn("--no-abbrev cannot be combined with --unify; ignoring "
		     "--no-abbrev");
		a->no_abbrev = 0;
		a->abbrev = 1;
	}
	if (a->archive_deps && a->diff) {
		warn("--diff cannot be combined with --archive-dependencies; "
		     "ignoring --diff");
		a->diff = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static int format_is_table(const char *f)
{
	return !strcmp(f, "table") || !strcmp(f, "text");
}

static int format_is_csv(const char *f) { return !strcmp(f, "csv"); }
static int format_is_tree(const char *f) { return !strcmp(f, "tree"); }
static int format_is_dot(const char *f) { return !strcmp(f, "dot"); }
static int format_is_raw(const char *f) { return !strcmp(f, "raw"); }
static int format_is_json2(const char *f) { return !strcmp(f, "json2"); }

int cmd_idf_size(int argc, const char **argv)
{
	struct loaded cur, ref;
	struct memmap mm_cur;
	struct memmap mm_ref;
	struct mm_args args;
	int load_symbols;
	FILE *out;

	argc = parse_options(argc, argv, &cmd_idf_size_desc);
	if (argc < 1)
		die("no map file; see 'ice idf size --help'");

	args_init(&args, argv[0]);
	validate_args(&args);

	/* Open the output stream early so we can fail fast. */
	out = stdout;
	if (args.output_file) {
		out = fopen(args.output_file, "w");
		if (!out)
			die_errno("cannot open '%s'", args.output_file);
	}
	args.out = out;

	/*
	 * --quiet routes everything to /dev/null while still running the
	 * pipeline -- handy for "did this map parse?" exit-code checks.
	 * Upstream does this via Rich's log layer; we just swap the
	 * output stream.  Note that --output-file wins if both are set,
	 * matching upstream where set_console takes file_stdout first.
	 */
	if (args.quiet && !args.output_file) {
		fclose(out);
		out = fopen("/dev/null", "w");
		if (!out)
			die_errno("/dev/null");
		args.out = out;
	}

	/*
	 * load_symbols is expensive for large ELFs; only enable it when
	 * something downstream actually consumes per-symbol data.
	 * Matches upstream's check exactly.
	 */
	load_symbols =
	    (args.archive_details != NULL) || format_is_raw(args.format);

	loaded_load(&cur, argv[0], args.target_override, &mm_cur, load_symbols);

	if (!args.show_unused)
		mm_remove_unused(&mm_cur);
	if (!args.use_flash_size)
		mm_ignore_flash_size(&mm_cur);

	memset(&ref, 0, sizeof(ref));
	memset(&mm_ref, 0, sizeof(mm_ref));

	if (args.diff) {
		loaded_load(&ref, args.diff, args.target_override, &mm_ref,
			    load_symbols);
		if (!args.show_unused)
			mm_remove_unused(&mm_ref);
		if (!args.use_flash_size)
			mm_ignore_flash_size(&mm_ref);

		if (strcmp(mm_cur.target, mm_ref.target) != 0)
			warn("targets differ: current=%s reference=%s",
			     mm_cur.target, mm_ref.target);

		mm_diff(&mm_cur, &mm_ref);
	}

	if (args.unify)
		mm_unify(&mm_cur);

	/*
	 * Tree walks the underlying memmap directly, and json2-summary
	 * dumps it as-is, so both need a destructive global sort.  Every
	 * other path either reuses memmap insertion order (raw, table
	 * archives/files/symbols column layouts, sv_summary skeletons)
	 * or sorts a side structure locally (table summary rows,
	 * sv_summary entries).
	 */
	{
		int summary_view = !args.archives && !args.archive_details &&
				   !args.files && !args.archive_deps;
		if (format_is_tree(args.format) ||
		    (format_is_json2(args.format) && summary_view))
			mm_sort(&mm_cur, &args);
	}

	if (format_is_dot(args.format)) {
		if (!args.archive_deps)
			die("the dot format is only valid with "
			    "--archive-dependencies");
		fmt_dot_deps(&cur.mf, &mm_cur, cur.has_syms ? &cur.syms : NULL,
			     &args);
	} else if (args.archive_deps) {
		if (format_is_table(args.format) ||
		    format_is_csv(args.format)) {
			if (format_is_csv(args.format))
				fmt_csv_deps(&cur.mf, &mm_cur,
					     cur.has_syms ? &cur.syms : NULL,
					     &args);
			else
				fmt_table_deps(&cur.mf, &mm_cur,
					       cur.has_syms ? &cur.syms : NULL,
					       &args);
		} else if (format_is_tree(args.format)) {
			fmt_tree_deps(&cur.mf, &mm_cur,
				      cur.has_syms ? &cur.syms : NULL, &args);
		} else if (format_is_raw(args.format) ||
			   format_is_json2(args.format)) {
			fmt_json_deps(&cur.mf, &mm_cur,
				      cur.has_syms ? &cur.syms : NULL, &args);
		} else {
			die("unsupported format '%s' for "
			    "--archive-dependencies",
			    args.format);
		}
	} else if (format_is_raw(args.format)) {
		fmt_json_raw(&mm_cur, &args);
	} else if (args.archives) {
		if (format_is_table(args.format))
			fmt_table_archives(&mm_cur, &args);
		else if (format_is_csv(args.format))
			fmt_csv_archives(&mm_cur, &args);
		else if (format_is_tree(args.format)) {
			mm_trim(&mm_cur, &args);
			fmt_tree_memmap(&mm_cur, &args);
		} else if (format_is_json2(args.format))
			fmt_json2_archives(&mm_cur, &args);
		else
			die("unsupported format '%s' for --archives",
			    args.format);
	} else if (args.archive_details) {
		if (format_is_table(args.format))
			fmt_table_symbols(&mm_cur, &args);
		else if (format_is_csv(args.format))
			fmt_csv_symbols(&mm_cur, &args);
		else if (format_is_tree(args.format)) {
			mm_trim(&mm_cur, &args);
			fmt_tree_memmap(&mm_cur, &args);
		} else if (format_is_json2(args.format))
			fmt_json2_symbols(&mm_cur, &args);
		else
			die("unsupported format '%s' for --archive-details",
			    args.format);
	} else if (args.files) {
		if (format_is_table(args.format))
			fmt_table_files(&mm_cur, &args);
		else if (format_is_csv(args.format))
			fmt_csv_files(&mm_cur, &args);
		else if (format_is_tree(args.format)) {
			mm_trim(&mm_cur, &args);
			fmt_tree_memmap(&mm_cur, &args);
		} else if (format_is_json2(args.format))
			fmt_json2_files(&mm_cur, &args);
		else
			die("unsupported format '%s' for --files", args.format);
	} else {
		/* Default summary. */
		if (format_is_table(args.format)) {
			mm_trim(&mm_cur, &args);
			fmt_table_summary(&mm_cur, &args);
		} else if (format_is_csv(args.format)) {
			mm_trim(&mm_cur, &args);
			fmt_csv_summary(&mm_cur, &args);
		} else if (format_is_tree(args.format)) {
			mm_trim(&mm_cur, &args);
			fmt_tree_memmap(&mm_cur, &args);
		} else if (format_is_json2(args.format))
			fmt_json2_summary(&mm_cur, &args);
		else
			die("unknown format '%s'", args.format);
	}

	mm_release(&mm_cur);
	if (args.diff) {
		mm_release(&mm_ref);
		loaded_release(&ref);
	}
	loaded_release(&cur);
	svec_clear(&opt_filter);

	if (out != stdout)
		fclose(out);

	return 0;
}
