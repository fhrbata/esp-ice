/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/init/init.c
 * @brief `ice init` -- bind a project to an ESP-IDF + chip target.
 *
 * Required positionals: @b{<chip>} @b{<idf>}.
 * Optional positional:  @b{[<name>]} -- project profile, defaults to
 * "default".  Each profile gets its own build directory and sdkconfig
 * so multi-target builds (e.g. esp32 + esp32s3 in the same project)
 * stay isolated.
 *
 * Every @b{ice init} call:
 *   1. Validates the chip and the IDF path (must contain tools/tools.json).
 *   2. Persists the profile under @b{[project "<name>"]} in @b{.iceconfig}.
 *   3. Installs (or skips if already installed) the IDF's tools.
 *   4. Renames the profile's @b{sdkconfig} to @b{sdkconfig.old}.
 *   5. Wipes the profile's build directory.
 *   6. Runs cmake from scratch.
 *
 * No partial / incremental modes; for a soft rebuild use @b{ice clean}
 * + @b{ice build}, which keeps the cmake configuration intact.
 */
#include <dirent.h>

#include "ice.h"

/* ------------------------------------------------------------------ */
/* Manual / options                                                    */
/* ------------------------------------------------------------------ */

/* clang-format off */
static const struct cmd_manual init_manual = {
	.name = "ice init",
	.summary = "bind project to an ESP-IDF + chip target",

	.description =
	H_PARA("Bind the current project to an ESP-IDF source tree and "
	       "chip target.  Always wipes the profile's build directory, "
	       "renames any existing @b{sdkconfig} to @b{sdkconfig.old}, "
	       "installs the IDF's tools, and runs cmake from scratch.")
	H_PARA("Both @b{<chip>} and @b{<idf>} are required.  An optional "
	       "third positional @b{[<name>]} selects the project profile "
	       "(default: @b{default}).  Use named profiles to keep build "
	       "artefacts for different chips or sdkconfig sets in "
	       "isolated build directories within the same project.")
	H_PARA("@b{<idf>} can be a bare name (e.g. @b{v5.4}, resolved to "
	       "@b{~/.ice/checkouts/v5.4/}) or any path to an existing "
	       "ESP-IDF tree.  Create named checkouts with @b{ice repo "
	       "checkout}.")
	H_PARA("Profile state is persisted to @b{.iceconfig} under "
	       "@b{[project \"<name>\"]} so subsequent ice commands can "
	       "look up what was configured."),

	.examples =
	H_EXAMPLE("ice init esp32 v5.4")
	H_EXAMPLE("ice init esp32s3 v5.5 production")
	H_EXAMPLE("ice init --preview linux v5.4")
	H_EXAMPLE("ice init -s sdkconfig.prod -S sdkconfig.defaults esp32 v5.4 prod"),

	.extras =
	H_SECTION("CONFIG")
	H_ITEM("project.<name>.chip",
	       "Chip target for profile @b{<name>}.")
	H_ITEM("project.<name>.idf-path",
	       "ESP-IDF source path for profile @b{<name>}.")
	H_ITEM("project.<name>.sdkconfig",
	       "Sdkconfig path "
	       "(default: @b{sdkconfig} for @b{default}, "
	       "@b{sdkconfig.<name>} otherwise).")
	H_ITEM("project.<name>.sdkconfig-defaults",
	       "Defaults files (multi-value).")
	H_ITEM("project.<name>.build-dir",
	       "Build directory "
	       "(default: @b{build} for @b{default}, "
	       "@b{build/<name>} otherwise).")
	H_ITEM("project.<name>.generator",
	       "cmake generator (default: @b{Ninja}).")
	H_ITEM("project.<name>.define",
	       "Extra @b{-D<key>=<value>} entries (multi-value).")

	H_SECTION("SEE ALSO")
	H_ITEM("ice repo checkout",
	       "Create a named ESP-IDF checkout to bind with @b{<idf>}.")
	H_ITEM("ice build",
	       "Build the project after init.")
	H_ITEM("ice clean",
	       "Soft rebuild: remove built artefacts but keep cmake "
	       "configuration intact."),
};
/* clang-format on */

static int opt_preview;
static const char *opt_sdkconfig;
static struct svec opt_sdkconfig_defaults;
static const char *opt_build_dir;
static const char *opt_generator;
static struct svec opt_defines;

/* ------------------------------------------------------------------ */
/* Per-slot completion                                                 */
/* ------------------------------------------------------------------ */

static int complete_dir_cb(const char *name, void *ud)
{
	(void)ud;
	printf("%s\n", name);
	return 0;
}

/** Slot 0 (chip): emit every supported and preview chip. */
static void complete_chip(void)
{
	for (const char *const *t = ice_supported_targets; *t; t++)
		printf("%s\n", *t);
	for (const char *const *t = ice_preview_targets; *t; t++)
		printf("%s\n", *t);
}

/** Slot 1 (idf): emit names of @b{~/.ice/checkouts/} entries. */
static void complete_idf(void)
{
	struct sbuf path = SBUF_INIT;

	sbuf_addf(&path, "%s/checkouts", ice_home());
	if (access(path.buf, F_OK) == 0)
		dir_foreach(path.buf, complete_dir_cb, NULL);
	sbuf_release(&path);
}

static const struct option cmd_init_opts[] = {
    OPT_STRING('s', "sdkconfig", &opt_sdkconfig, "file",
	       "sdkconfig path (default: sdkconfig[.<name>])", NULL),
    OPT_STRING_LIST('S', "sdkconfig-defaults", &opt_sdkconfig_defaults, "file",
		    "sdkconfig defaults file (repeatable)", NULL),
    OPT_STRING('b', "build-dir", &opt_build_dir, "dir",
	       "build directory (default: build[/<name>])", NULL),
    OPT_STRING('g', "generator", &opt_generator, "name",
	       "cmake generator (default: Ninja)", NULL),
    OPT_STRING_LIST('d', "define", &opt_defines, "key=val",
		    "extra cmake -D<key>=<value> entry (repeatable)", NULL),
    OPT_BOOL(0, "preview", &opt_preview, "allow preview chip targets"),
    OPT_POSITIONAL("chip", complete_chip),
    OPT_POSITIONAL("idf", complete_idf),
    OPT_POSITIONAL("[<name>]", complete_profile_names),
    OPT_END(),
};

const struct cmd_desc cmd_init_desc = {
    .name = "init",
    .fn = cmd_init,
    .opts = cmd_init_opts,
    .manual = &init_manual,
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int in_list(const char *target, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(target, *list))
			return 1;
	return 0;
}

/**
 * Resolve the @b{<idf>} positional to a real filesystem path.
 *
 * A bare name (no separator, no leading @b{.} / @b{~} / @b{/}) maps to
 * @b{~/.ice/checkouts/<name>/}, mirroring @b{ice repo checkout}'s
 * shorthand.  Anything else is taken verbatim.  Returns a malloc'd
 * string the caller owns.
 */
static char *resolve_idf_arg(const char *arg)
{
	struct sbuf p = SBUF_INIT;
	int bare;

	bare = *arg && arg[0] != '/' && arg[0] != '.' && arg[0] != '~' &&
	       !strchr(arg, '/');

	if (bare)
		sbuf_addf(&p, "%s/checkouts/%s", ice_home(), arg);
	else
		sbuf_addstr(&p, arg);
	return sbuf_detach(&p);
}

/**
 * Wipe the contents of @p build_dir.
 *
 * Safety checks (kept verbatim from the old `ice fullclean`): refuse
 * to clean a directory that does not have @b{CMakeCache.txt} or that
 * contains source-tree markers (@b{CMakeLists.txt}, @b{.git},
 * @b{.svn}) -- guards against a misconfigured build-dir.
 */
static int wipe_build_dir(const char *build_dir)
{
	DIR *dir;
	struct dirent *de;
	int has_cache = 0;
	int n_entries = 0;

	if (access(build_dir, F_OK) != 0)
		return 0;

	dir = opendir(build_dir);
	if (!dir)
		die_errno("cannot open '%s'", build_dir);

	while ((de = readdir(dir)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		n_entries++;
		if (!strcmp(de->d_name, "CMakeCache.txt"))
			has_cache = 1;
		if (!strcmp(de->d_name, "CMakeLists.txt") ||
		    !strcmp(de->d_name, ".git") ||
		    !strcmp(de->d_name, ".svn")) {
			closedir(dir);
			die("refusing to clean '%s': contains '%s'", build_dir,
			    de->d_name);
		}
	}
	closedir(dir);

	if (n_entries == 0)
		return 0;
	if (!has_cache)
		die("'%s' does not look like a cmake build directory "
		    "(no CMakeCache.txt); delete it manually",
		    build_dir);

	return rmtree(build_dir, global_verbose) < 0 ? -1 : 0;
}

/** Rename @p path to @p path.old if @p path exists. */
static void backup_sdkconfig(const char *sdkconfig)
{
	struct sbuf old = SBUF_INIT;

	if (access(sdkconfig, F_OK) != 0)
		goto done;
	sbuf_addf(&old, "%s.old", sdkconfig);
	if (rename(sdkconfig, old.buf) < 0)
		warn_errno("rename '%s' -> '%s'", sdkconfig, old.buf);
done:
	sbuf_release(&old);
}

/** Build the cmake @b{-D<key>=<value>} set from the active profile.
 *  Caller owns @p out and must svec_clear() it after use. */
static void build_define_set(struct svec *out)
{
	const char *chip = config_get("project.chip");
	const char *sdkconfig = config_get("project.sdkconfig");
	struct config_entry **entries;
	int n;

	if (chip)
		svec_pushf(out, "IDF_TARGET=%s", chip);
	if (sdkconfig)
		svec_pushf(out, "SDKCONFIG=%s", sdkconfig);

	n = config_get_all("project.sdkconfig-defaults", &entries);
	if (n > 0) {
		struct sbuf e = SBUF_INIT;

		sbuf_addstr(&e, "SDKCONFIG_DEFAULTS=");
		for (int i = 0; i < n; i++) {
			if (i > 0)
				sbuf_addch(&e, ';');
			sbuf_addstr(&e, entries[i]->value);
		}
		svec_push(out, e.buf);
		sbuf_release(&e);
	}
	free(entries);

	n = config_get_all("project.define", &entries);
	for (int i = 0; i < n; i++)
		svec_push(out, entries[i]->value);
	free(entries);
}

/** Run cmake's configure step on the active profile. */
static int cmake_configure(void)
{
	const char *build_dir = config_get("project.build-dir");
	const char *generator = config_get("project.generator");
	struct svec defines = SVEC_INIT;
	struct svec args = SVEC_INIT;
	struct sbuf logdir = SBUF_INIT;
	struct process proc = PROCESS_INIT;
	int rc;

	if (access("CMakeLists.txt", F_OK) != 0)
		die("no @b{CMakeLists.txt} in current directory");

	build_define_set(&defines);

	sbuf_addf(&logdir, "%s/log", build_dir);
	mkdir(build_dir, 0755);
	mkdir(logdir.buf, 0755);
	sbuf_release(&logdir);

	svec_push(&args, "cmake");
	svec_push(&args, "-G");
	svec_push(&args, generator);
	svec_push(&args, "-B");
	svec_push(&args, build_dir);
	for (size_t i = 0; i < defines.nr; i++)
		svec_pushf(&args, "-D%s", defines.v[i]);

	proc.argv = args.v;
	rc = process_run(&proc);

	if (rc) {
		struct sbuf cache = SBUF_INIT;

		sbuf_addf(&cache, "%s/CMakeCache.txt", build_dir);
		unlink(cache.buf);
		sbuf_release(&cache);
	}

	svec_clear(&args);
	svec_clear(&defines);
	return rc;
}

/* ------------------------------------------------------------------ */
/* build.ninja patching -- post-configure fixups                       */
/*   gen_esp32part.py   -> ice idf partition-table                     */
/*   esptool elf2image  -> ice image create                            */
/*                                                                     */
/* IDF's cmake files wire these scripts into build.ninja COMMAND       */
/* lines.  We rewrite them to call the native ice equivalents so no    */
/* Python is needed at build time.  This is a temporary hack that      */
/* lives here (rather than being replayed on every build) because      */
/* cmake configure is the only thing that regenerates build.ninja, so  */
/* doing the patch once after init is sufficient.  Re-running ice init */
/* re-applies it.                                                      */
/* ------------------------------------------------------------------ */

static const char *mem_find(const char *p, const char *end, const char *needle,
			    size_t nlen)
{
	for (; p + nlen <= end; p++)
		if (!memcmp(p, needle, nlen))
			return p;
	return NULL;
}

static void patch_command_line(struct sbuf *out, const char *line, size_t len)
{
	static const char needle[] = "gen_esp32part.py";
	static const size_t nlen = sizeof(needle) - 1;
	const char *p = line;
	const char *end = line + len;
	int occurrence = 0;

	while (p < end) {
		const char *found = mem_find(p, end, needle, nlen);
		if (!found) {
			sbuf_add(out, p, end - p);
			return;
		}

		const char *script_start = found;
		while (script_start > p && script_start[-1] != ' ')
			script_start--;

		const char *python_start = script_start;
		if (python_start > p)
			python_start--;
		while (python_start > p && python_start[-1] != ' ')
			python_start--;

		sbuf_add(out, p, python_start - p);

		/*
		 * IDF calls gen_esp32part.py twice per COMMAND line:
		 *   1st call: generate  -- replace with ice idf partition-table
		 *   2nd call: display   -- replace with true
		 */
		if (occurrence == 0) {
			const char *exe = process_exe();

			sbuf_addstr(out, exe ? exe : "ice");
			sbuf_addstr(out, " idf partition-table");
		} else {
			sbuf_addstr(out, "true");
		}
		occurrence++;

		p = found + nlen;
	}
}

static void patch_ninja_partition(const char *build_dir)
{
	static const char needle[] = "gen_esp32part.py";
	static const size_t nlen = sizeof(needle) - 1;
	struct sbuf ninja_path = SBUF_INIT;
	struct sbuf content = SBUF_INIT;
	struct sbuf out = SBUF_INIT;
	const char *p, *end, *nl;
	int modified = 0;

	sbuf_addf(&ninja_path, "%s/build.ninja", build_dir);

	if (sbuf_read_file(&content, ninja_path.buf) < 0)
		goto done;

	p = content.buf;
	end = content.buf + content.len;

	while (p < end) {
		nl = memchr(p, '\n', end - p);
		if (!nl)
			nl = end;

		size_t line_len = (size_t)(nl - p);

		if (line_len > 11 && !memcmp(p, "  COMMAND =", 11) &&
		    mem_find(p, nl, needle, nlen)) {
			patch_command_line(&out, p, line_len);
			modified = 1;
		} else {
			sbuf_add(&out, p, line_len);
		}

		if (nl < end)
			sbuf_addch(&out, '\n');
		p = nl + (nl < end ? 1 : 0);
	}

	if (modified)
		write_file_atomic(ninja_path.buf, out.buf, out.len);

done:
	sbuf_release(&ninja_path);
	sbuf_release(&content);
	sbuf_release(&out);
}

/*
 * Walk back @p n whitespace-delimited tokens from @p p toward
 * @p line_start, returning a pointer to the start of the token @p n
 * positions back.
 */
static const char *back_n_tokens(const char *p, const char *line_start, int n)
{
	while (n-- > 0) {
		while (p > line_start && p[-1] == ' ')
			p--;
		while (p > line_start && p[-1] != ' ')
			p--;
	}
	return p;
}

/*
 * Replace the esptool elf2image invocation on a COMMAND line with
 * the native `ice image create` equivalent.  IDF's COMMAND line has
 * the form:
 *
 *   cd <dir> && <python> -m esptool --chip <chip> elf2image <args> \
 *       -o <out.bin> <in.elf> && cmake -E echo "..." && ...
 *
 * We rewrite `<python> -m esptool` to `ice image create`, then
 * re-emit the captured `--chip <chip>` argument (which lives between
 * `esptool` and `elf2image`) plus everything after `elf2image`.
 */
static void patch_elf2image_line(struct sbuf *out, const char *line, size_t len)
{
	static const char e2i_needle[] = "elf2image";
	static const size_t e2ilen = sizeof(e2i_needle) - 1;
	static const char esptool_needle[] = "esptool";
	static const size_t elen = sizeof(esptool_needle) - 1;
	static const char chip_needle[] = "--chip";
	static const size_t chip_nlen = sizeof(chip_needle) - 1;

	const char *end = line + len;
	const char *etool = mem_find(line, end, esptool_needle, elen);

	if (!etool)
		goto passthrough;

	const char *e2i = mem_find(etool + elen, end, e2i_needle, e2ilen);
	if (!e2i)
		goto passthrough;

	const char *etool_start = etool;
	while (etool_start > line && etool_start[-1] != ' ')
		etool_start--;
	const char *invoke_start = back_n_tokens(etool_start, line, 2);

	const char *chip_arg =
	    mem_find(etool + elen, e2i, chip_needle, chip_nlen);
	const char *chip_val_start = NULL;
	const char *chip_val_end = NULL;
	if (chip_arg) {
		chip_val_start = chip_arg + chip_nlen;
		while (chip_val_start < e2i && *chip_val_start == ' ')
			chip_val_start++;
		chip_val_end = chip_val_start;
		while (chip_val_end < e2i && *chip_val_end != ' ')
			chip_val_end++;
	}

	sbuf_add(out, line, invoke_start - line);

	{
		const char *exe = process_exe();

		sbuf_addstr(out, exe ? exe : "ice");
	}
	sbuf_addstr(out, " image create");
	if (chip_val_start && chip_val_end > chip_val_start) {
		sbuf_addstr(out, " --chip ");
		sbuf_add(out, chip_val_start, chip_val_end - chip_val_start);
	}

	sbuf_add(out, e2i + e2ilen, end - (e2i + e2ilen));
	return;

passthrough:
	sbuf_add(out, line, len);
}

static void patch_ninja_elf2image(const char *build_dir)
{
	static const char esptool_needle[] = "esptool";
	static const char e2i_needle[] = "elf2image";
	static const size_t elen = sizeof(esptool_needle) - 1;
	static const size_t e2ilen = sizeof(e2i_needle) - 1;

	struct sbuf ninja_path = SBUF_INIT;
	struct sbuf content = SBUF_INIT;
	struct sbuf out = SBUF_INIT;
	const char *p, *end, *nl;
	int modified = 0;

	sbuf_addf(&ninja_path, "%s/build.ninja", build_dir);

	if (sbuf_read_file(&content, ninja_path.buf) < 0)
		goto done;

	p = content.buf;
	end = content.buf + content.len;

	while (p < end) {
		nl = memchr(p, '\n', end - p);
		if (!nl)
			nl = end;

		size_t line_len = (size_t)(nl - p);

		if (line_len > 11 && !memcmp(p, "  COMMAND =", 11) &&
		    mem_find(p, nl, esptool_needle, elen) &&
		    mem_find(p, nl, e2i_needle, e2ilen)) {
			patch_elf2image_line(&out, p, line_len);
			modified = 1;
		} else {
			sbuf_add(&out, p, line_len);
		}

		if (nl < end)
			sbuf_addch(&out, '\n');
		p = nl + (nl < end ? 1 : 0);
	}

	if (modified)
		write_file_atomic(ninja_path.buf, out.buf, out.len);

done:
	sbuf_release(&ninja_path);
	sbuf_release(&content);
	sbuf_release(&out);
}

/** Persist the profile under @b{[project "<name>"]} in @b{.iceconfig}.
 *
 *  Only writes to the file -- the process-wide config is not touched.
 *  load_profile() re-reads .iceconfig fresh, so it picks up what we
 *  just wrote without an in-memory mirror. */
static void persist_profile(const char *name, const char *chip,
			    const char *idf_path, const char *sdkconfig,
			    const struct svec *sdkconfig_defaults,
			    const char *build_dir, const char *generator,
			    const struct svec *defines)
{
	struct config c = CONFIG_INIT;
	const char *path = local_config_path();
	struct sbuf key = SBUF_INIT;

	if (!path)
		die("cannot locate project @b{.iceconfig}");

	config_load_file(&c, CONFIG_SCOPE_LOCAL, path);

	/* Scalar entries. */
	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.chip", name);
	config_set(&c, key.buf, chip, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.idf-path", name);
	config_set(&c, key.buf, idf_path, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.sdkconfig", name);
	config_set(&c, key.buf, sdkconfig, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.build-dir", name);
	config_set(&c, key.buf, build_dir, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.generator", name);
	config_set(&c, key.buf, generator, CONFIG_SCOPE_LOCAL);

	/* Multi-values (clear + re-add so re-init replaces, not appends). */
	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.sdkconfig-defaults", name);
	config_unset(&c, key.buf, CONFIG_SCOPE_LOCAL);
	for (size_t i = 0; i < sdkconfig_defaults->nr; i++)
		config_add(&c, key.buf, sdkconfig_defaults->v[i],
			   CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.define", name);
	config_unset(&c, key.buf, CONFIG_SCOPE_LOCAL);
	for (size_t i = 0; i < defines->nr; i++)
		config_add(&c, key.buf, defines->v[i], CONFIG_SCOPE_LOCAL);

	if (config_write_file(&c, CONFIG_SCOPE_LOCAL, path))
		die_errno("cannot write '%s'", path);
	config_release(&c);
	sbuf_release(&key);
}

/* ------------------------------------------------------------------ */
/* Command entry                                                       */
/* ------------------------------------------------------------------ */

int cmd_init(int argc, const char **argv)
{
	/*
	 * Handshake with esp-idf/tools/cmake/project.cmake: when this env
	 * var is set, project.cmake renames an existing sdkconfig to
	 * sdkconfig.old before __target_init runs, so a stale IDF_TARGET
	 * from the old sdkconfig is not consulted for consistency checks.
	 *
	 * putenv() stores the pointer, not a copy, so the storage must
	 * outlive the call -- a function-static array provides that.
	 */
	static char envstr[] = "_IDF_PY_SET_TARGET_ACTION=1";
	const char *chip;
	const char *name;
	char *idf_path = NULL;
	struct sbuf manifest = SBUF_INIT;
	struct sbuf sdkconfig_buf = SBUF_INIT;
	struct sbuf build_dir_buf = SBUF_INIT;
	const char *sdkconfig;
	const char *build_dir;
	const char *generator;
	int rc;

	argc = parse_options(argc, argv, &cmd_init_desc);

	if (argc < 2)
		die("usage: ice init <chip> <idf> [<name>]");
	if (argc > 3)
		die("too many arguments");

	chip = argv[0];
	idf_path = resolve_idf_arg(argv[1]);
	name = (argc >= 3) ? argv[2] : "default";

	/* Validate chip up front so we error before any state mutation. */
	if (!in_list(chip, ice_supported_targets)) {
		if (in_list(chip, ice_preview_targets)) {
			if (!opt_preview)
				die("'%s' is a preview target; "
				    "pass --preview to use it",
				    chip);
		} else {
			die("'%s' is not a supported target", chip);
		}
	}

	/* Validate the IDF path. */
	sbuf_addf(&manifest, "%s/tools/tools.json", idf_path);
	if (access(manifest.buf, F_OK) != 0)
		die("'%s' does not look like an ESP-IDF tree (no "
		    "tools/tools.json)",
		    idf_path);

	/* Auto-default sdkconfig and build-dir from profile name. */
	sdkconfig = opt_sdkconfig;
	if (!sdkconfig) {
		if (!strcmp(name, "default")) {
			sdkconfig = "sdkconfig";
		} else {
			sbuf_addf(&sdkconfig_buf, "sdkconfig.%s", name);
			sdkconfig = sdkconfig_buf.buf;
		}
	}

	build_dir = opt_build_dir;
	if (!build_dir) {
		if (!strcmp(name, "default")) {
			build_dir = "build";
		} else {
			sbuf_addf(&build_dir_buf, "build/%s", name);
			build_dir = build_dir_buf.buf;
		}
	}

	generator = opt_generator ? opt_generator : "Ninja";

	/* Persist the profile to .iceconfig, then refresh the in-memory
	 * config from disk so the load_profile() below sees what we just
	 * wrote -- without having to dual-write into the global config. */
	persist_profile(name, chip, idf_path, sdkconfig,
			&opt_sdkconfig_defaults, build_dir, generator,
			&opt_defines);
	config_reload_local();

	/* Install (or skip if already installed) tools for this IDF.
	 * Filtering by chip keeps target-specific tool sets minimal. */
	rc = install_from_manifest(manifest.buf, chip, NULL, 0);
	sbuf_release(&manifest);
	if (rc) {
		free(idf_path);
		return rc;
	}

	/* Wipe the profile's build dir and back up its sdkconfig. */
	backup_sdkconfig(sdkconfig);
	rc = wipe_build_dir(build_dir);
	if (rc) {
		free(idf_path);
		return rc;
	}

	/*
	 * Read the just-persisted profile back into process state via
	 * the same load_profile() that build/flash/etc. use.  This sets
	 * up PATH so the cmake configure below can find ninja and the
	 * cross-compiler, and populates the project.* keys that
	 * build_define_set consumes.
	 */
	load_profile(name);

	putenv(envstr);

	rc = cmake_configure();
	if (rc == 0) {
		const char *bdir = config_get("project.build-dir");

		patch_ninja_partition(bdir);
		patch_ninja_elf2image(bdir);
	}

	free(idf_path);
	sbuf_release(&sdkconfig_buf);
	sbuf_release(&build_dir_buf);
	return rc;
}
