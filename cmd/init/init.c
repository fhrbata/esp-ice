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
 *   2. Persists the profile under @b{[project "<name>"]} in @b{.ice/config}.
 *   3. Installs (or skips if already installed) the IDF's tools.
 *   4. Renames the profile's @b{sdkconfig} to @b{sdkconfig.old}.
 *   5. Wipes the profile's build directory.
 *   6. Runs cmake from scratch.
 *
 * No partial / incremental modes; for a soft rebuild use @b{ice clean}
 * + @b{ice build}, which keeps the cmake configuration intact.
 */
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
	H_PARA("Profile state is persisted to @b{.ice/config} under "
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

/*
 * Read the checkout's HEAD file and format a short label for the
 * completion description.  Cheap: one stat + one read (no git fork,
 * no packed-refs walk), so latency stays negligible even with many
 * checkouts.  Returns 1 on success with @p out populated.
 */
static int checkout_head_label(const char *checkout, struct sbuf *out)
{
	struct sbuf head_path = SBUF_INIT;
	struct sbuf content = SBUF_INIT;
	int ok = 0;

	sbuf_addf(&head_path, "%s/.git/HEAD", checkout);
	if (sbuf_read_file(&content, head_path.buf) >= 0) {
		sbuf_rtrim(&content);
		if (!strncmp(content.buf, "ref: refs/heads/", 16)) {
			sbuf_addf(out, "on branch %s", content.buf + 16);
			ok = 1;
		} else if (content.len >= 7) {
			sbuf_addf(out, "at %.7s", content.buf);
			ok = 1;
		}
	}
	sbuf_release(&head_path);
	sbuf_release(&content);
	return ok;
}

static int complete_idf_cb(const char *name, void *ud)
{
	const char *base = ud;
	struct sbuf full = SBUF_INIT;
	struct sbuf label = SBUF_INIT;

	sbuf_addf(&full, "%s/%s", base, name);
	if (checkout_head_label(full.buf, &label))
		complete_emit(name, label.buf);
	else
		complete_emit(name, NULL);
	sbuf_release(&full);
	sbuf_release(&label);
	return 0;
}

/** Slot 0 (chip): emit every supported and preview chip. */
static void complete_chip(void)
{
	const char *s;

	for (const char *const *t = ice_supported_targets; *t; t++)
		complete_emit(*t, ice_chip_summary(*t));
	for (const char *const *t = ice_preview_targets; *t; t++) {
		s = ice_chip_summary(*t);
		complete_emit(*t, s ? s : "(preview)");
	}
}

/** Slot 1 (idf): emit names of @b{~/.ice/checkouts/} entries. */
static void complete_idf(void)
{
	struct sbuf path = SBUF_INIT;

	sbuf_addf(&path, "%s/checkouts", ice_home());
	if (access(path.buf, F_OK) == 0)
		dir_foreach(path.buf, complete_idf_cb, path.buf);
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
    OPT_POSITIONAL_OPT("name", complete_profile_names),
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

struct wipe_ctx {
	const char *build_dir;
	int n_entries;
};

static int wipe_check_entry(const char *name, void *ud)
{
	struct wipe_ctx *ctx = ud;

	ctx->n_entries++;
	if (!strcmp(name, "CMakeLists.txt") || !strcmp(name, ".git") ||
	    !strcmp(name, ".svn"))
		die("refusing to clean '%s': contains '%s'", ctx->build_dir,
		    name);
	return 0;
}

/**
 * Wipe the contents of @p build_dir.
 *
 * Refuse only if @p build_dir looks like a source tree
 * (@b{CMakeLists.txt}, @b{.git}, @b{.svn}) -- guards against a
 * misconfigured build-dir pointing at the project itself.
 */
static int wipe_build_dir(const char *build_dir)
{
	struct wipe_ctx ctx = {.build_dir = build_dir};

	if (access(build_dir, F_OK) != 0)
		return 0;

	if (dir_foreach(build_dir, wipe_check_entry, &ctx) < 0)
		die_errno("cannot open '%s'", build_dir);

	if (ctx.n_entries == 0)
		return 0;

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
	const char *chip = config_get("_project.chip");
	const char *sdkconfig = config_get("_project.sdkconfig");
	struct config_entry **entries;
	int n;

	if (chip)
		svec_pushf(out, "IDF_TARGET=%s", chip);
	if (sdkconfig)
		svec_pushf(out, "SDKCONFIG=%s", sdkconfig);

	n = config_get_all("_project.sdkconfig-defaults", &entries);
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

	n = config_get_all("_project.define", &entries);
	for (int i = 0; i < n; i++)
		svec_push(out, entries[i]->value);
	free(entries);
}

static void venv_python(struct sbuf *out);
static void setup_venv(void);

/** Run cmake's configure step on the active profile. */
static int cmake_configure(void)
{
	const char *build_dir = config_get("_project.build-dir");
	const char *generator = config_get("_project.generator");
	struct svec defines = SVEC_INIT;
	struct svec args = SVEC_INIT;
	struct process proc = PROCESS_INIT;
	int rc;

	if (access("CMakeLists.txt", F_OK) != 0)
		die("no @b{CMakeLists.txt} in current directory");

	build_define_set(&defines);

	mkdir(build_dir, 0755);

	svec_push(&args, "cmake");
	svec_push(&args, "-G");
	svec_push(&args, generator);
	svec_push(&args, "-B");
	svec_push(&args, build_dir);
	/*
	 * Short-circuit IDF's `__build_check_python` -- the ice venv does
	 * not have IDF's pip packages installed (kconfgen, idf-component-
	 * manager, ...) because the tools they provide are intercepted at
	 * the Python-level via sitecustomize.py.  The property is consulted
	 * in project.cmake before the check runs, so setting it via -D
	 * skips the import-style probe.
	 */
	svec_push(&args, "-DPYTHON_DEPS_CHECKED=1");
	/*
	 * Pin every Python interpreter cmake and IDF might spawn to the
	 * ice-managed venv so their invocations route through
	 * sitecustomize.py (see setup_venv() for the mechanism).  We have
	 * to set two variables because ESP-IDF and cmake name the
	 * interpreter differently:
	 *
	 *   Python3_EXECUTABLE  cmake's find_package(Python3) result,
	 *                       consumed by generic rules.
	 *   PYTHON              IDF's `project.cmake` build property
	 *                       (set_default(PYTHON "python") in
	 *                       tools/cmake/build.cmake); used as
	 *                       ${python} in all ESP-IDF COMMAND lines
	 *                       including `${python} -m kconfgen`.
	 *
	 * Without the PYTHON override IDF would fall back to searching
	 * PATH for "python", which on a machine with a previous
	 * esp-idf/install.sh run resolves to ~/.espressif/python_env/...
	 * and bypasses our sitecustomize entirely.
	 */
	{
		struct sbuf py = SBUF_INIT;

		venv_python(&py);
		svec_pushf(&args, "-DPython3_EXECUTABLE=%s", py.buf);
		svec_pushf(&args, "-DPYTHON=%s", py.buf);
		sbuf_release(&py);
	}
	for (size_t i = 0; i < defines.nr; i++)
		svec_pushf(&args, "-D%s", defines.v[i]);

	proc.argv = args.v;
	rc = process_run_progress(&proc, "Configuring", "init-configure", NULL);

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
/* Python venv + sitecustomize.py -- dispatch IDF's host Python        */
/* scripts to their native ice equivalents                             */
/*                                                                     */
/*   ldgen.py            -> ice idf ldgen                              */
/*   gen_esp32part.py    -> ice idf partition-table  (generate form)   */
/*   gen_esp32part.py    -> exit 0                   (display form)    */
/*   gen_crt_bundle.py   -> ice idf crt-bundle                         */
/*   esptool.py          -> ice image create         (elf2image only)  */
/*   python -m esptool   -> ice image create         (elf2image only)  */
/*   python -m kconfgen  -> ice idf kconfgen                           */
/*                                                                     */
/* IDF wires these scripts into its cmake rules (and thus into         */
/* build.ninja, or Makefiles when -G "Unix Makefiles") as              */
/* `${Python3_EXECUTABLE} <script>`.  We create a venv once under      */
/* @c{ice_home()/venv/}, install a sitecustomize.py into its site-     */
/* packages, and point cmake at the venv's interpreter (under         */
/* @c{bin/} on POSIX, @c{Scripts/} on Windows) via                     */
/* -DPython3_EXECUTABLE.  Python's site.py runs sitecustomize at       */
/* interpreter startup, which inspects sys.argv and execs the ice      */
/* binary for any recognised script -- transparently to cmake, ninja,  */
/* and IDF, and regardless of which generator cmake writes files for.  */
/*                                                                     */
/* Python3_EXECUTABLE is baked into CMakeCache.txt, so every cmake     */
/* re-configure (including bootloader / ULP sub-projects triggered by  */
/* ninja) picks up the same interpreter.  Unrecognised invocations     */
/* (-c, -m <other>, cmake's own Python3 probing) fall through to the   */
/* real venv python without interception.                              */
/* ------------------------------------------------------------------ */

/** @c{<ice_home>/venv} -- venv is shared across projects and IDF trees. */
static void venv_dir(struct sbuf *out)
{
	sbuf_addf(out, "%s/venv", ice_home());
}

/**
 * Absolute path to the venv's Python interpreter -- the value we hand
 * cmake via @c -DPython3_EXECUTABLE.  The relative path differs per
 * OS (@c bin/python3 vs @c Scripts/python.exe), encoded in
 * PLATFORM_VENV_PYTHON_REL.
 */
static void venv_python(struct sbuf *out)
{
	sbuf_addf(out, "%s/venv/%s", ice_home(), PLATFORM_VENV_PYTHON_REL);
}

/** Pick an interpreter to bootstrap the venv with.  Returns NULL if none. */
static const char *host_python(void)
{
	if (find_in_path("python3"))
		return "python3";
	if (find_in_path("python"))
		return "python";
	return NULL;
}

/*
 * Ask the venv python for its site-packages directory and fill @p out.
 * Returns 0 on success, -1 on spawn / read / exit failure.  Using
 * site.getsitepackages()[0] avoids hard-coding the Python minor version
 * in the path -- the venv's python is the single source of truth.
 */
static int venv_site_packages(const char *py, struct sbuf *out)
{
	const char *argv[] = {
	    py,
	    "-c",
	    "import site; print(site.getsitepackages()[0])",
	    NULL,
	};
	struct process proc = PROCESS_INIT;
	char buf[4096];
	size_t len = 0;

	proc.argv = argv;
	proc.pipe_out = 1;
	if (process_start(&proc) < 0)
		return -1;

	for (;;) {
		ssize_t n = pipe_read_timed(proc.out, buf + len,
					    sizeof(buf) - 1 - len, 1000);
		if (n <= 0)
			break;
		len += (size_t)n;
		if (len >= sizeof(buf) - 1)
			break;
	}
	if (process_finish(&proc) != 0 || len == 0)
		return -1;

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		len--;
	sbuf_add(out, buf, len);
	return 0;
}

/*
 * Write `sitecustomize.py` under @p site_packages.  The ice absolute
 * path is embedded verbatim (POSIX paths contain neither quotes nor
 * backslashes in practice, so no escaping is needed).  Called every
 * @c{ice init} so the path stays fresh if the binary moves.
 */
static void write_sitecustomize(const char *site_packages, const char *ice_path)
{
	struct sbuf path = SBUF_INIT;
	struct sbuf content = SBUF_INIT;

	sbuf_addf(&path, "%s/sitecustomize.py", site_packages);
	sbuf_addf(
	    &content,
	    "# Generated by `ice init`.  Intercepts ESP-IDF host Python\n"
	    "# scripts at interpreter startup and hands them off to the\n"
	    "# native ice equivalents.  Do not edit -- rewritten on\n"
	    "# every `ice init`.\n"
	    "import os\n"
	    "import sys\n"
	    "\n"
	    "_ICE = \"%s\"\n"
	    "\n"
	    "\n"
	    "def _esptool_dispatch(args):\n"
	    "    \"\"\"Map `esptool <args>` to `ice image create`.\"\"\"\n"
	    "    if \"elf2image\" not in args:\n"
	    "        return\n"
	    "    i = args.index(\"elf2image\")\n"
	    "    before, after = args[:i], args[i + 1:]\n"
	    "    chip = None\n"
	    "    kept = []\n"
	    "    j = 0\n"
	    "    while j < len(before):\n"
	    "        if before[j] == \"--chip\" and j + 1 < len(before):\n"
	    "            chip = before[j + 1]\n"
	    "            j += 2\n"
	    "        else:\n"
	    "            kept.append(before[j])\n"
	    "            j += 1\n"
	    "    new = [_ICE, \"image\", \"create\"]\n"
	    "    if chip:\n"
	    "        new += [\"--chip\", chip]\n"
	    "    new += kept + after\n"
	    "    os.execv(_ICE, new)\n"
	    "\n"
	    "\n"
	    "def _main():\n"
	    "    if not sys.argv:\n"
	    "        return\n"
	    "    base = os.path.basename(sys.argv[0])\n"
	    "    if base == \"ldgen.py\":\n"
	    "        os.execv(_ICE, [_ICE, \"idf\", \"ldgen\", "
	    "*sys.argv[1:]])\n"
	    "    elif base == \"gen_esp32part.py\":\n"
	    "        # IDF calls gen_esp32part.py twice per target:\n"
	    "        #   generate  (csv -> bin) -- has a .csv argument\n"
	    "        #   display   (bin)        -- no .csv argument\n"
	    "        # Route generate through ice; treat display as no-op.\n"
	    "        # Use os._exit so we bail without unwinding through\n"
	    "        # site.py (SystemExit during site init is fatal).\n"
	    "        if any(a.endswith(\".csv\") for a in sys.argv[1:]):\n"
	    "            os.execv(_ICE,\n"
	    "                     [_ICE, \"idf\", \"partition-table\",\n"
	    "                      *sys.argv[1:]])\n"
	    "        else:\n"
	    "            os._exit(0)\n"
	    "    elif base == \"gen_crt_bundle.py\":\n"
	    "        os.execv(_ICE,\n"
	    "                 [_ICE, \"idf\", \"crt-bundle\", *sys.argv[1:]])\n"
	    "    elif base == \"esptool.py\":\n"
	    "        _esptool_dispatch(sys.argv[1:])\n"
	    "    elif sys.argv[0] == \"-m\" and \"elf2image\" in "
	    "sys.argv[1:]:\n"
	    "        # `python -m esptool ...`: Python consumes the module\n"
	    "        # name, so sys.argv is [\"-m\", <user args...>] and\n"
	    "        # \"esptool\" is not visible.  Use `elf2image`'s\n"
	    "        # presence as the disambiguator -- that matches\n"
	    "        # patch_ninja's exact coverage.\n"
	    "        _esptool_dispatch(sys.argv[1:])\n"
	    "    elif sys.argv[0] == \"-m\" and \"--kconfig\" in "
	    "sys.argv[1:]:\n"
	    "        # `python -m kconfgen ...`: same argv-eating quirk as\n"
	    "        # esptool above.  `--kconfig` is kconfgen's mandatory\n"
	    "        # root flag and doesn't appear in any other IDF-invoked\n"
	    "        # python module, so it is a safe disambiguator.\n"
	    "        os.execv(_ICE,\n"
	    "                 [_ICE, \"idf\", \"kconfgen\", *sys.argv[1:]])\n"
	    "\n"
	    "\n"
	    "try:\n"
	    "    _main()\n"
	    "except BaseException as _exc:\n"
	    "    sys.stderr.write(\"ice sitecustomize: \" + repr(_exc) + "
	    "\"\\n\")\n",
	    ice_path);

	if (write_file_atomic(path.buf, content.buf, content.len) < 0)
		die_errno("write '%s'", path.buf);

	sbuf_release(&path);
	sbuf_release(&content);
}

/*
 * Create @c{<ice_home>/venv} on first init and write sitecustomize.py
 * into its site-packages.  Subsequent calls skip venv creation but
 * always refresh sitecustomize.py so the embedded ice path tracks the
 * currently-running binary.  Dies on any step that must succeed.
 */
static void setup_venv(void)
{
	struct sbuf venv = SBUF_INIT;
	struct sbuf py = SBUF_INIT;
	struct sbuf site = SBUF_INIT;
	const char *exe = process_exe();

	venv_dir(&venv);
	venv_python(&py);

	if (access(py.buf, X_OK) != 0) {
		const char *host = host_python();
		struct svec cmd = SVEC_INIT;
		struct process proc = PROCESS_INIT;
		int rc;

		if (!host)
			die("no host python found on PATH "
			    "(install python3 to bootstrap @c{~/.ice/venv})");

		svec_push(&cmd, host);
		svec_push(&cmd, "-m");
		svec_push(&cmd, "venv");
		svec_push(&cmd, venv.buf);

		proc.argv = cmd.v;
		rc = process_run_progress(&proc, "Creating Python venv",
					  "init-venv", NULL);
		svec_clear(&cmd);
		if (rc)
			die("failed to create @c{%s}", venv.buf);
	}

	if (venv_site_packages(py.buf, &site) < 0)
		die("cannot query site-packages from @c{%s}", py.buf);

	write_sitecustomize(site.buf, exe ? exe : "ice");

	sbuf_release(&venv);
	sbuf_release(&py);
	sbuf_release(&site);
}

/*
 * Build an in-memory @c struct config holding the existing on-disk
 * entries at LOCAL scope plus the newly-staged profile entries -- the
 * serialised form of what @b{.ice/config} should contain if cmake
 * succeeds.  Caller owns @p out and must config_release() it.
 */
static void stage_local_config(struct config *out, const char *name,
			       const char *chip, const char *idf_path,
			       const char *sdkconfig,
			       const struct svec *sdkconfig_defaults,
			       const char *build_dir, const char *generator,
			       const struct svec *defines)
{
	struct sbuf key = SBUF_INIT;

	config_load_file(out, CONFIG_SCOPE_LOCAL, local_config_path());

	/* Scalar entries. */
	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.chip", name);
	config_set(out, key.buf, chip, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.idf-path", name);
	config_set(out, key.buf, idf_path, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.sdkconfig", name);
	config_set(out, key.buf, sdkconfig, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.build-dir", name);
	config_set(out, key.buf, build_dir, CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.generator", name);
	config_set(out, key.buf, generator, CONFIG_SCOPE_LOCAL);

	/* Multi-values (clear + re-add so re-init replaces, not appends). */
	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.sdkconfig-defaults", name);
	config_unset(out, key.buf, CONFIG_SCOPE_LOCAL);
	for (size_t i = 0; i < sdkconfig_defaults->nr; i++)
		config_add(out, key.buf, sdkconfig_defaults->v[i],
			   CONFIG_SCOPE_LOCAL);

	sbuf_reset(&key);
	sbuf_addf(&key, "project.%s.define", name);
	config_unset(out, key.buf, CONFIG_SCOPE_LOCAL);
	for (size_t i = 0; i < defines->nr; i++)
		config_add(out, key.buf, defines->v[i], CONFIG_SCOPE_LOCAL);

	sbuf_release(&key);
}

/*
 * Populate the process-wide @c _project.* keys that cmake_configure()
 * reads -- equivalent to what setup_project() would do, minus the
 * configured-marker check (which cannot succeed mid-init).  Scope is
 * PROJECT so a re-run of config_load_profile() elsewhere would
 * replace these cleanly.
 */
static void activate_staged_profile(const char *chip, const char *idf_path,
				    const char *sdkconfig,
				    const char *build_dir,
				    const char *generator,
				    const struct svec *sdkconfig_defaults,
				    const struct svec *defines)
{
	struct sbuf env = SBUF_INIT;
	struct sbuf log_dir = SBUF_INIT;

	config_add(&config, "_project.chip", chip, CONFIG_SCOPE_PROJECT);
	config_add(&config, "_project.idf-path", idf_path,
		   CONFIG_SCOPE_PROJECT);
	config_add(&config, "_project.sdkconfig", sdkconfig,
		   CONFIG_SCOPE_PROJECT);
	config_add(&config, "_project.build-dir", build_dir,
		   CONFIG_SCOPE_PROJECT);
	config_add(&config, "_project.generator", generator,
		   CONFIG_SCOPE_PROJECT);
	for (size_t i = 0; i < sdkconfig_defaults->nr; i++)
		config_add(&config, "_project.sdkconfig-defaults",
			   sdkconfig_defaults->v[i], CONFIG_SCOPE_PROJECT);
	for (size_t i = 0; i < defines->nr; i++)
		config_add(&config, "_project.define", defines->v[i],
			   CONFIG_SCOPE_PROJECT);

	/*
	 * Route the cmake-configure log under this init call into the
	 * per-profile log dir so it's reachable via @b{ice log} the
	 * same way @b{ice build} logs are.  Without this, init's log
	 * would land in ~/.ice/logs/ (the standalone fallback) and
	 * @b{ice log} wouldn't find it.
	 */
	sbuf_addf(&log_dir, "%s/.ice/logs", build_dir);
	config_add(&config, "_project.log-dir", log_dir.buf,
		   CONFIG_SCOPE_PROJECT);
	sbuf_release(&log_dir);

	setup_tool_env(idf_path);
	sbuf_addf(&env, "IDF_PATH=%s", idf_path);
	putenv(sbuf_detach(&env));
	putenv((char *)"IDF_COMPONENT_MANAGER=0");
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
	struct sbuf lock_path = SBUF_INIT;
	struct sbuf marker = SBUF_INIT;
	struct sbuf log_dir = SBUF_INIT;
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

	/* Install tools for this IDF by spawning "ice tools install"
	 * under process_run_progress so its output lands in ~/.ice/logs/
	 * and joins init's other phases behind a single spinner.  The
	 * --target filter keeps the set narrowed to the chosen chip.
	 * Tools are idempotent and their state is keyed by IDF tree, not
	 * by ice profile, so no lock needed around this step. */
	{
		struct svec cmd = SVEC_INIT;
		struct process proc = PROCESS_INIT;
		const char *exe = process_exe();

		svec_push(&cmd, exe ? exe : "ice");
		svec_push(&cmd, "tools");
		svec_push(&cmd, "install");
		svec_push(&cmd, "--target");
		svec_push(&cmd, chip);
		svec_push(&cmd, manifest.buf);

		proc.argv = cmd.v;
		rc = process_run_progress(&proc, "Installing tools",
					  "init-install", NULL);
		svec_clear(&cmd);
	}
	sbuf_release(&manifest);
	if (rc)
		goto out;

	/* Create ~/.ice/venv (if missing) and refresh its sitecustomize.py
	 * so cmake's Python3_EXECUTABLE can safely route IDF's host scripts
	 * through ice -- see the section comment on setup_venv() above. */
	setup_venv();

	/* Wipe the profile's build dir and back up its sdkconfig. */
	backup_sdkconfig(sdkconfig);
	rc = wipe_build_dir(build_dir);
	if (rc)
		goto out;

	/* Pre-create the per-profile log dir so the spawned cmake run
	 * has somewhere to land its output even if configure fails. */
	sbuf_addf(&log_dir, "%s/.ice/logs", build_dir);
	if (mkdirp(log_dir.buf) < 0)
		die_errno("cannot create '%s'", log_dir.buf);

	/*
	 * Stage-then-commit: take .ice/config.lock up front, run cmake,
	 * and touch `<build>/.ice/configured` + rewrite `.ice/config`
	 * atomically ONLY if cmake succeeds.  On failure the on-disk
	 * config and the `configured` marker are left untouched, so a
	 * partial-init state the rest of ice would key off of never
	 * exists.
	 */
	sbuf_addf(&lock_path, "%s.lock", local_config_path());
	if (lock_acquire(lock_path.buf, 2000) < 0)
		die_errno("lock '%s' (remove if no ice is running)",
			  lock_path.buf);

	/* Populate _project.* so cmake_configure reads the staged values
	 * without us having to dual-write into the on-disk config first. */
	activate_staged_profile(chip, idf_path, sdkconfig, build_dir, generator,
				&opt_sdkconfig_defaults, &opt_defines);

	putenv(envstr);

	rc = cmake_configure();
	if (rc != 0) {
		/* Failure: release lock, leave `.ice/config` untouched. */
		lock_release(lock_path.buf);
		goto out;
	}

	/* Success: touch configured marker. */
	sbuf_addf(&marker, "%s/.ice/configured", build_dir);
	if (write_file_atomic(marker.buf, "", 0) < 0)
		die_errno("cannot touch '%s'", marker.buf);

	/* Write `.ice/config` via atomic rename of the held lockfile. */
	{
		struct config staged = CONFIG_INIT;
		struct sbuf rendered = SBUF_INIT;
		FILE *fp;
		size_t written;

		stage_local_config(&staged, name, chip, idf_path, sdkconfig,
				   &opt_sdkconfig_defaults, build_dir,
				   generator, &opt_defines);
		config_render_ini(&staged, CONFIG_SCOPE_LOCAL, &rendered);

		fp = fopen(lock_path.buf, "wb");
		if (!fp)
			die_errno("open '%s'", lock_path.buf);
		written = fwrite(rendered.buf, 1, rendered.len, fp);
		if (fclose(fp) != 0 || written != rendered.len)
			die_errno("write '%s'", lock_path.buf);
		if (rename(lock_path.buf, local_config_path()) != 0)
			die_errno("rename '%s' -> '%s'", lock_path.buf,
				  local_config_path());
		lock_forget(lock_path.buf);

		sbuf_release(&rendered);
		config_release(&staged);
	}

	/* Unlink any stale `built` marker: the previous build is no
	 * longer consistent with the just-rewritten config / cache. */
	sbuf_reset(&marker);
	sbuf_addf(&marker, "%s/.ice/built", build_dir);
	unlink(marker.buf);

out:
	free(idf_path);
	sbuf_release(&sdkconfig_buf);
	sbuf_release(&build_dir_buf);
	sbuf_release(&lock_path);
	sbuf_release(&marker);
	sbuf_release(&log_dir);
	return rc;
}
