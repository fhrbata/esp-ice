/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file configdep.c
 * @brief Sdkconfig.h dependency optimizer (compiler wrapper).
 *
 * This command wraps a compiler invocation to optimise incremental
 * builds in ESP-IDF projects.  It:
 *  1. Runs the real compiler (forwarding all arguments).
 *  2. Locates the dependency file produced by the compiler (-MF flag).
 *  3. Parses the dependency file; removes sdkconfig.h if present.
 *  4. Scans every prerequisite for CONFIG_* references.
 *  5. Rewrites the dependency file with granular per-option dummy
 *     files (.cdep), so that a source is only rebuilt when the
 *     specific options it uses change.
 */
#include "ice.h"

/* clang-format off */
static const struct cmd_manual configdep_manual = {
	.name = "ice idf configdep",
	.summary = "sdkconfig-aware compiler wrapper",

	.description =
	H_PARA("Compiler wrapper that rewrites GCC-style dependency "
	       "files (@b{-MF}) so a translation unit only rebuilds "
	       "when the specific @b{CONFIG_*} options it actually "
	       "references change -- not every time @b{sdkconfig} is "
	       "touched.")
	H_PARA("Not intended for direct use: the cmake toolchain is "
	       "configured to invoke the real compiler through this "
	       "wrapper.  The command is documented here so the build "
	       "system's behaviour is discoverable.")
	H_PARA("Mechanism: the wrapper first runs the real compiler "
	       "with the arguments it was given.  If a @b{-MF} "
	       "dependency file was produced, it parses that file, "
	       "removes @b{sdkconfig.h} from the prerequisite list, "
	       "scans the remaining prerequisite files for "
	       "@b{CONFIG_*} identifiers, and rewrites the dependency "
	       "file with one per-option @b{.cdep} marker file per "
	       "referenced option.  Those marker files are touched "
	       "only when the corresponding option changes, so Make / "
	       "Ninja recompile precisely the affected sources."),

	.examples =
	H_EXAMPLE("ice idf configdep cc -c foo.c -o foo.o -MF foo.d"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice menuconfig",
	       "Edit @b{sdkconfig} interactively; subsequent builds "
	       "pick up only the actually-affected sources."),
};
/* clang-format on */

static const struct option cmd_configdep_opts[] = {OPT_END()};

const struct cmd_desc cmd_configdep_desc = {
    .name = "configdep",
    .fn = cmd_configdep,
    .opts = cmd_configdep_opts,
    .manual = &configdep_manual,
};

/* ------------------------------------------------------------------ */
/*  Parsed dependency file                                            */
/* ------------------------------------------------------------------ */

struct depfile {
	char *fn;     /**< Dep file path (borrowed from argv). */
	char *target; /**< Make target (heap). */
	size_t target_len;
	char *sdkconfig_dir; /**< Dir prefix of sdkconfig.h, or NULL. */
	size_t sdkconfig_dir_len;
	int sdkconfig; /**< Non-zero if sdkconfig.h was found. */
	char **deps;   /**< Dependency paths (heap strings). */
	int n_deps, alloc_deps;
};

static void depfile_release(struct depfile *d)
{
	for (int i = 0; i < d->n_deps; i++)
		free(d->deps[i]);
	free(d->deps);
	free(d->target);
	free(d->sdkconfig_dir);
}

/**
 * Check whether a dependency path is sdkconfig.h and, if so, record
 * the directory prefix for later .cdep path construction.
 *
 * Returns 1 if the dep was sdkconfig.h (consumed), 0 otherwise.
 */
static int check_sdkconfig(struct depfile *d, const char *s, size_t len)
{
	static const char suffix[] = "/sdkconfig.h";
	static const char name[] = "sdkconfig.h";
	size_t slen = sizeof(suffix) - 1;
	size_t nlen = sizeof(name) - 1;

	if (d->sdkconfig)
		return 0;

	if (len == nlen && !memcmp(s, name, nlen)) {
		d->sdkconfig = 1;
		return 1;
	}

	if (len > slen && !memcmp(s + len - slen, suffix, slen)) {
		d->sdkconfig = 1;
		d->sdkconfig_dir = sbuf_strndup(s, len - slen);
		d->sdkconfig_dir_len = len - slen;
		return 1;
	}

	return 0;
}

static void add_dep(struct depfile *d, const char *s, size_t len)
{
	if (!len || check_sdkconfig(d, s, len))
		return;

	ALLOC_GROW(d->deps, d->n_deps + 1, d->alloc_deps);
	d->deps[d->n_deps++] = sbuf_strndup(s, len);
}

/**
 * Read and parse a compiler-generated dependency (.d) file.
 *
 * The format is:  target: dep1 dep2 ... depN
 * with backslash-newline continuations.  sdkconfig.h is detected
 * and removed; all other dependencies are stored in d->deps.
 */
static int parse_depfile(struct depfile *d)
{
	struct sbuf sb = SBUF_INIT;
	char *buf, *end, *colon, *c, *dep;
	char last;

	if (sbuf_read_file(&sb, d->fn) < 0) {
		warn_errno("cannot read '%s'", d->fn);
		sbuf_release(&sb);
		return -1;
	}

	buf = sb.buf;
	end = buf + sb.len;

	colon = memchr(buf, ':', sb.len);
	if (!colon) {
		warn("no target in '%s'", d->fn);
		sbuf_release(&sb);
		return -1;
	}

	d->target = sbuf_strndup(buf, colon - buf);
	d->target_len = colon - buf;

	c = colon + 1;
	dep = c;
	last = 0;

	while (c < end) {
		if ((*c == '\n' || *c == '\r') && last == '\\') {
			dep = c + 1;
		} else if ((*c == ' ' || *c == '\n' || *c == '\r') &&
			   last != '\\') {
			add_dep(d, dep, c - dep);
			dep = c + 1;
		}
		last = *c++;
	}
	add_dep(d, dep, c - dep);

	sbuf_release(&sb);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  CONFIG_* scanner                                                  */
/* ------------------------------------------------------------------ */

struct sdk_opts {
	char **opts; /**< Unique option names (without CONFIG_). */
	int n_opts, alloc_opts;
};

static void sdk_opts_release(struct sdk_opts *cfg)
{
	for (int i = 0; i < cfg->n_opts; i++)
		free(cfg->opts[i]);
	free(cfg->opts);
}

static void add_opt(struct sdk_opts *cfg, const char *s, size_t len)
{
	if (!len)
		return;

	for (int i = 0; i < cfg->n_opts; i++) {
		if (strlen(cfg->opts[i]) == len &&
		    !memcmp(cfg->opts[i], s, len))
			return;
	}

	ALLOC_GROW(cfg->opts, cfg->n_opts + 1, cfg->alloc_opts);
	cfg->opts[cfg->n_opts++] = sbuf_strndup(s, len);
}

/**
 * Scan a single file for CONFIG_* references.  Missing files are
 * silently ignored (they may have been removed between compilation
 * and this pass).
 */
static int scan_file(struct sdk_opts *cfg, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	const char *c, *end;

	if (sbuf_read_file(&sb, path) < 0) {
		if (errno == ENOENT) {
			sbuf_release(&sb);
			return 0;
		}
		warn_errno("cannot read '%s'", path);
		sbuf_release(&sb);
		return -1;
	}

	c = sb.buf;
	end = c + sb.len;

	while (c < end) {
		c = memchr(c, 'C', end - c);
		if (!c || (size_t)(end - c) < 7)
			break;

		if (memcmp(c, "CONFIG_", 7) != 0) {
			c++;
			continue;
		}

		c += 7;
		const char *opt = c;
		while (c < end && (isupper((unsigned char)*c) ||
				   isdigit((unsigned char)*c) || *c == '_'))
			c++;

		add_opt(cfg, opt, c - opt);
	}

	sbuf_release(&sb);
	return 0;
}

static int scan_deps(struct sdk_opts *cfg, struct depfile *d)
{
	for (int i = 0; i < d->n_deps; i++) {
		if (scan_file(cfg, d->deps[i]))
			return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Dep-file rewriter                                                 */
/* ------------------------------------------------------------------ */

/**
 * Create an empty file at @p path, creating parent directories as
 * needed.
 */
static int touch_file(const char *path)
{
	FILE *fp;

	mkdirp_for_file(path);

	fp = fopen(path, "wb");
	if (!fp) {
		warn_errno("cannot create '%s'", path);
		return -1;
	}
	fclose(fp);
	return 0;
}

/**
 * Rewrite the dependency file: keep the original target and deps
 * (minus sdkconfig.h), and append a .cdep entry for each CONFIG_*
 * option found in the source files.
 *
 * Option names are lowercased and underscores become directory
 * separators, e.g. CONFIG_MY_OPTION -> my/option.cdep.
 */
static int fix_depfile(struct depfile *d, struct sdk_opts *cfg)
{
	struct sbuf out = SBUF_INIT;
	struct sbuf path = SBUF_INIT;
	int rv = -1;
	FILE *fp;

	sbuf_add(&out, d->target, d->target_len);
	sbuf_addch(&out, ':');

	for (int i = 0; i < d->n_deps; i++) {
		sbuf_addstr(&out, " \\" EOL " ");
		sbuf_addstr(&out, d->deps[i]);
	}

	for (int i = 0; i < cfg->n_opts; i++) {
		sbuf_reset(&path);
		if (d->sdkconfig_dir) {
			sbuf_add(&path, d->sdkconfig_dir, d->sdkconfig_dir_len);
			sbuf_addch(&path, '/');
		}

		for (const char *n = cfg->opts[i]; *n; n++) {
			if (*n == '_')
				sbuf_addch(&path, '/');
			else
				sbuf_addch(&path, tolower((unsigned char)*n));
		}
		sbuf_addstr(&path, ".cdep");

		if (access(path.buf, F_OK) && touch_file(path.buf))
			goto out;

		sbuf_addstr(&out, " \\" EOL " ");
		sbuf_addstr(&out, path.buf);
	}

	sbuf_addstr(&out, EOL);

	fp = fopen(d->fn, "wb");
	if (!fp) {
		warn_errno("cannot open '%s' for writing", d->fn);
		goto out;
	}

	if (fwrite(out.buf, 1, out.len, fp) != out.len) {
		warn("write error on '%s'", d->fn);
		fclose(fp);
		goto out;
	}

	fclose(fp);
	rv = 0;
out:
	sbuf_release(&out);
	sbuf_release(&path);
	return rv;
}

/* ------------------------------------------------------------------ */
/*  Command entry point                                               */
/* ------------------------------------------------------------------ */

static const char *find_mf(int argc, const char **argv)
{
	for (int i = 0; i < argc - 1; i++) {
		if (!strcmp(argv[i], "-MF"))
			return argv[i + 1];
	}
	return NULL;
}

int cmd_configdep(int argc, const char **argv)
{
	struct depfile d = {0};
	struct sdk_opts cfg = {0};
	const char *dep_fn;
	int rc;

	/*
	 * All args after argv[0] are forwarded verbatim to the real
	 * compiler, so we can't call parse_options here -- flags
	 * like -c, -MF etc. are not ours.  Intercept --help/-h manually
	 * at the head of the argv before forwarding takes over.
	 */
	if (argc >= 2 &&
	    (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		print_manual(configdep_manual.name, &cmd_configdep_desc);
		return EXIT_SUCCESS;
	}

	/* argv[0] = "configdep", argv[1..] = compiler command */
	if (argc < 2)
		die("usage: ice idf configdep <cmd> <arg...>");

	/* Step 1: run the compiler. */
	struct process proc = PROCESS_INIT;
	proc.argv = argv + 1;
	rc = process_run(&proc);
	if (rc)
		return rc < 0 ? EXIT_FAILURE : rc;

	/* Step 2: find dependency file path from -MF flag. */
	dep_fn = find_mf(argc, argv);
	if (!dep_fn)
		return EXIT_SUCCESS;

	/* Step 3: parse the dependency file. */
	d.fn = (char *)dep_fn;
	if (parse_depfile(&d))
		goto fail;

	if (!d.sdkconfig) {
		depfile_release(&d);
		return EXIT_SUCCESS;
	}

	/* Step 4: scan deps for CONFIG_* references. */
	if (scan_deps(&cfg, &d))
		goto fail;

	/* Step 5: rewrite dep file with per-option .cdep deps. */
	if (fix_depfile(&d, &cfg))
		goto fail;

	depfile_release(&d);
	sdk_opts_release(&cfg);
	return EXIT_SUCCESS;

fail:
	depfile_release(&d);
	sdk_opts_release(&cfg);
	return EXIT_FAILURE;
}
