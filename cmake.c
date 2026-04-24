/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmake.c
 * @brief Project lifecycle glue for the dispatcher: resolve profile,
 *        enforce configured/built preconditions, populate runtime
 *        state under @c _project.* before a leaf handler runs.
 *
 * @b{ice init} is the single owner of the cmake configure-time state
 * (generator, -D flags, build-dir layout, build.ninja fixups); every
 * other cmake-based command shells out to @b{cmake --build} or
 * consumes artefacts in the build directory.  Commands declare what
 * they need via @c cmd_desc.needs and the dispatcher calls
 * setup_project() to bring the process to that state -- load the
 * profile, stat the @c .ice/configured / @c .ice/built markers, parse
 * @c flasher_args.json into @c _project.flash-file, and so on.
 */
#include "ice.h"

void complete_profile_names(void)
{
	struct svec seen = SVEC_INIT;
	int seen_default = 0;

	for (int i = 0; i < config.nr; i++) {
		const char *key = config.entries[i].key;
		const char *p, *dot;
		struct sbuf nm = SBUF_INIT;
		int duplicate = 0;

		if (strncmp(key, "project.", 8) != 0)
			continue;
		p = key + 8;
		dot = strchr(p, '.');
		if (!dot)
			continue;

		sbuf_add(&nm, p, dot - p);
		for (size_t j = 0; j < seen.nr; j++) {
			if (!strcmp(seen.v[j], nm.buf)) {
				duplicate = 1;
				break;
			}
		}
		if (!duplicate) {
			printf("%s\n", nm.buf);
			if (!strcmp(nm.buf, "default"))
				seen_default = 1;
			svec_push(&seen, nm.buf);
		}
		sbuf_release(&nm);
	}
	svec_clear(&seen);

	if (!seen_default)
		printf("default\n");
}

/*
 * Prepend the active profile's IDF tool directories to PATH and set
 * IDF_PATH / IDF_COMPONENT_MANAGER so any cmake/ninja child inherits
 * the right toolchain without ever sourcing @b{export.sh}.
 *
 * The IDF_COMPONENT_MANAGER=0 export is global (not just during init)
 * because ninja later spawns sub-cmakes (bootloader, ULP, ...) that
 * re-read project.cmake; without the env var they'd reintroduce the
 * Python component manager we explicitly opt out of.
 */
static void setup_tooling_env(void)
{
	const char *idf_path = config_get("_project.idf-path");

	if (idf_path && *idf_path) {
		setup_tool_env(idf_path);
		setenv("IDF_PATH", idf_path, 1);
	}

	setenv("IDF_COMPONENT_MANAGER", "0", 1);
}

/*
 * Parse @b{<build>/flasher_args.json} into @c _project.chip and
 * @c _project.flash-file entries so @b{ice flash} / @b{ice monitor}
 * don't have to re-read it.  Best-effort: a missing or unparseable
 * file leaves the keys absent, and the caller must check
 * config_has() before using them.
 */
static void populate_flash_info(const char *build_dir)
{
	struct sbuf path = SBUF_INIT;
	struct sbuf buf = SBUF_INIT;

	sbuf_addf(&path, "%s/flasher_args.json", build_dir);
	if (sbuf_read_file(&buf, path.buf) < 0)
		goto done;

	struct json_value *root = json_parse(buf.buf, buf.len);
	if (!root)
		goto done;

	const char *chip = json_as_string(
	    json_get(json_get(root, "extra_esptool_args"), "chip"));
	if (chip && *chip)
		config_add(&config, "_project.chip", chip,
			   CONFIG_SCOPE_PROJECT);

	struct json_value *files = json_get(root, "flash_files");
	if (files && files->type == JSON_OBJECT) {
		for (int i = 0; i < files->u.object.nr; i++) {
			const char *offset = files->u.object.members[i].key;
			const char *rel =
			    json_as_string(files->u.object.members[i].value);
			struct sbuf entry = SBUF_INIT;

			if (!offset || !rel)
				continue;
			sbuf_addf(&entry, "%s=%s/%s", offset, build_dir, rel);
			config_add(&config, "_project.flash-file", entry.buf,
				   CONFIG_SCOPE_PROJECT);
			sbuf_release(&entry);
		}
	}
	json_free(root);

done:
	sbuf_release(&buf);
	sbuf_release(&path);
}

/*
 * Load @c <build>/config.env -- the JSON dump cmake writes during
 * configure with everything ESP-IDF's Kconfig tree needs for
 * @c $(VAR) / @c $VAR interpolation (IDF_TARGET, IDF_ENV_FPGA,
 * COMPONENT_KCONFIGS, COMPONENT_SDKCONFIG_RENAMES, the two
 * COMPONENT_KCONFIGS_*_SOURCE_FILE paths, ...).
 *
 * Each entry is exposed two ways so both in-process callers (the
 * Kconfig parser, menuconfig's argv builder) and spawned children
 * (cmake, ninja, anything the ice process might re-exec) see the
 * same values:
 *
 *   - @c _project.env.<NAME> under @c CONFIG_SCOPE_PROJECT so
 *     @c config_get("_project.env.IDF_TARGET") and friends work
 *     from any PROJECT_CONFIGURED command.
 *   - @c setenv(NAME, value, 1) into the process env so
 *     @c kc_parse_file's @c getenv-based @c $(VAR) substitution
 *     and any child process inherit them without a per-caller
 *     @c --env plumb.
 *
 * Best-effort: a missing file is fine (non-IDF ice projects don't
 * have a config.env), and unparseable contents are silently skipped
 * rather than aborting commands that don't actually need Kconfig.
 */
static void populate_kconfig_env(const char *build_dir)
{
	struct sbuf path = SBUF_INIT;
	struct sbuf buf = SBUF_INIT;

	sbuf_addf(&path, "%s/config.env", build_dir);
	if (sbuf_read_file(&buf, path.buf) < 0)
		goto done;

	struct json_value *root = json_parse(buf.buf, buf.len);
	if (!root || json_type(root) != JSON_OBJECT) {
		json_free(root);
		goto done;
	}

	for (int i = 0; i < root->u.object.nr; i++) {
		const struct json_member *m = &root->u.object.members[i];
		const char *val = NULL;
		char numbuf[32];

		switch (json_type(m->value)) {
		case JSON_STRING:
			val = json_as_string(m->value);
			break;
		case JSON_BOOL:
			val = json_as_bool(m->value) ? "y" : "n";
			break;
		case JSON_NUMBER:
			snprintf(numbuf, sizeof(numbuf), "%lld",
				 (long long)json_as_number(m->value));
			val = numbuf;
			break;
		case JSON_NULL:
			val = "";
			break;
		default:
			continue; /* arrays / objects: skip */
		}
		if (!val)
			val = "";

		struct sbuf key = SBUF_INIT;
		sbuf_addf(&key, "_project.env.%s", m->key);
		config_add(&config, key.buf, val, CONFIG_SCOPE_PROJECT);
		sbuf_release(&key);

		setenv(m->key, val, 1);
	}
	json_free(root);

done:
	sbuf_release(&buf);
	sbuf_release(&path);
}

/*
 * Resolve the active profile name.  Precedence: CLI (--profile / env
 * ICE_PROFILE seed parse_options already folded into global_profile) >
 * config @c project.default-profile > "default".  Never returns NULL.
 */
static const char *resolve_profile_name(void)
{
	const char *name = global_profile;

	if (name && *name)
		return name;

	name = config_get("project.default-profile");
	if (name && *name)
		return name;

	return "default";
}

void setup_project(enum project_need needs)
{
	const char *name;
	const char *build_dir;
	struct sbuf marker = SBUF_INIT;

	if (needs == PROJECT_NONE)
		return;

	/*
	 * No .ice/config in cwd means we're not inside an ice project.
	 * Same shape as git refusing to run outside a work tree.
	 */
	if (access(local_config_path(), F_OK) != 0) {
		hint("run @b{ice init <chip> <idf>} to bind, or cd to an "
		     "existing ice project");
		die("not an ice project (no @b{.ice/config} in cwd)");
	}

	name = resolve_profile_name();
	config_load_profile(name);

	build_dir = config_get("_project.build-dir");
	if (!build_dir || !*build_dir) {
		if (!strcmp(name, "default")) {
			hint("run @b{ice init <chip> <idf>} first");
			die("project not initialised");
		}
		hint("run @b{ice init <chip> <idf> %s} first", name);
		die("profile '%s' not configured", name);
	}

	/*
	 * PATH / IDF env set unconditionally for any PROJECT_CONFIGURED+
	 * command so child cmake/ninja invocations see the right
	 * toolchain.  PROJECT_NONE returned early above.
	 */
	setup_tooling_env();

	/*
	 * `<build>/.ice/configured` is touched atomically by @b{ice
	 * init} after cmake succeeds.  Absence means either init has
	 * never run for this profile or it failed partway through --
	 * either way the right recovery is to re-run init.
	 */
	sbuf_reset(&marker);
	sbuf_addf(&marker, "%s/.ice/configured", build_dir);
	if (access(marker.buf, F_OK) != 0) {
		hint("run @b{ice init <chip> <idf>%s%s} first",
		     strcmp(name, "default") ? " " : "",
		     strcmp(name, "default") ? name : "");
		sbuf_release(&marker);
		die("profile '%s' not configured", name);
	}

	populate_flash_info(build_dir);
	populate_kconfig_env(build_dir);
	config_add(&config, "_project.configured", "1", CONFIG_SCOPE_PROJECT);

	/*
	 * Runtime-only state keys consumed by @c process_run_progress
	 * (log-dir) and @c ice log (log-dir).  Written at PROJECT scope
	 * -- the same scope config_load_profile() just cleared -- so
	 * config_add() yields single-value semantics.  Set before the
	 * PROJECT_BUILT branch so @c project_build() (the
	 * @c core.build-always path) logs into the profile's log-dir,
	 * not the @c ~/.ice/logs fallback.
	 */
	{
		struct sbuf log_dir = SBUF_INIT;

		sbuf_addf(&log_dir, "%s/.ice/logs", build_dir);
		config_add(&config, "_project.log-dir", log_dir.buf,
			   CONFIG_SCOPE_PROJECT);
		sbuf_release(&log_dir);
	}

	/*
	 * PROJECT_BUILT: @c core.build-always = true (the default)
	 * gives the idf.py-style coupled flow -- always run ninja,
	 * regardless of the @c .ice/built marker.  Ninja's own
	 * up-to-date check keeps no-op rebuilds cheap, and the marker
	 * reflects a fresh build rather than whatever stale state was
	 * on disk.  Power users can flip the key to @c false to get the
	 * decoupled shape where @b{ice flash} / @b{ice size} / ... refuse
	 * to act on an unbuilt project.
	 */
	if (needs >= PROJECT_BUILT) {
		int build_always = 0;

		config_get_bool("core.build-always", &build_always);
		if (build_always) {
			int rc = project_build();

			if (rc != 0) {
				sbuf_release(&marker);
				exit(rc);
			}
		} else {
			sbuf_reset(&marker);
			sbuf_addf(&marker, "%s/.ice/built", build_dir);
			if (access(marker.buf, F_OK) != 0) {
				hint("run @b{ice build} first, or re-enable "
				     "@b{core.build-always} (default: true)");
				sbuf_release(&marker);
				die("project not built");
			}
		}
		config_add(&config, "_project.built", "1",
			   CONFIG_SCOPE_PROJECT);
	}

	sbuf_release(&marker);
}
