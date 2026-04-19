/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file install.c
 * @brief `ice install` -- download and install ESP-IDF tools from a
 *        tools.json manifest.
 *
 * Reads the manifest, picks the recommended version of each tool for
 * the current platform, downloads the archive, verifies SHA-256, and
 * extracts it under <tools-path>/tools/<name>/<version>/.
 */
#include "ice.h"

#include "fs.h"
#include "http.h"
#include "json.h"
#include "tar.h"
#include "vendor/sha256/sha256.h"

/* ------------------------------------------------------------------ */
/* Manual / options                                                    */
/* ------------------------------------------------------------------ */

/* clang-format off */
static const struct cmd_manual tools_install_manual = {
	.name = "ice tools install",
	.summary = "download and install tools from a manifest",

	.description =
	H_PARA("Read an ESP-IDF @b{tools.json} manifest and install the "
	       "recommended version of each tool for the current platform.  "
	       "By default only tools with @b{\"install\": \"always\"} are "
	       "fetched; pass @b{--tool <name>} to install a specific tool "
	       "(including on-request tools).")
	H_PARA("Archives are downloaded to @b{<tools-path>/dist/}, verified "
	       "against the SHA-256 digest in the manifest, and extracted to "
	       "@b{<tools-path>/tools/<name>/<version>/}.  Tools that are "
	       "already installed are skipped unless @b{--force} is given.")
	H_PARA("The tools path defaults to @b{~/.ice} and can be "
	       "overridden with the @b{tools.path} config key."),

	.examples =
	H_EXAMPLE("ice install tools/tools.json")
	H_EXAMPLE("ice install --tool xtensa-esp-elf tools/tools.json")
	H_EXAMPLE("ice install --force tools/tools.json"),
};
/* clang-format on */

static const char *opt_tool;
static const char *opt_target;
static const char *opt_path;
static int opt_force;

static const struct option cmd_install_opts[] = {
    OPT_STRING(0, "target", &opt_target, "chip",
	       "only install tools that support this chip target", NULL),
    OPT_STRING(0, "tool", &opt_tool, "name",
	       "install a specific tool (includes on_request tools)", NULL),
    OPT_STRING(0, "path", &opt_path, "dir",
	       "tools install directory (default: ~/.ice)", NULL),
    OPT_BOOL(0, "force", &opt_force,
	     "re-download and overwrite existing installations"),
    OPT_END(),
};

const struct cmd_desc cmd_tools_install_desc = {
    .name = "install",
    .fn = cmd_install,
    .opts = cmd_install_opts,
    .manual = &tools_install_manual,
};

/* ------------------------------------------------------------------ */
/* Platform detection                                                  */
/* ------------------------------------------------------------------ */

/**
 * Map the compile-time ICE_PLATFORM_OS / ICE_PLATFORM_ARCH defines
 * (set by the Makefile from the compiler triple) to the platform key
 * used inside tools.json version entries.
 */
static const char *platform_key(void)
{
	static char key[32];

	if (key[0])
		return key;

	if (!strcmp(ICE_PLATFORM_OS, "linux")) {
		const char *arch = !strcmp(ICE_PLATFORM_ARCH, "i386")
				       ? "i686"
				       : ICE_PLATFORM_ARCH;
		snprintf(key, sizeof(key), "linux-%s", arch);
	} else if (!strcmp(ICE_PLATFORM_OS, "macos")) {
		if (!strcmp(ICE_PLATFORM_ARCH, "arm64"))
			snprintf(key, sizeof(key), "macos-arm64");
		else
			snprintf(key, sizeof(key), "macos");
	} else if (!strcmp(ICE_PLATFORM_OS, "win")) {
		if (!strcmp(ICE_PLATFORM_ARCH, "amd64"))
			snprintf(key, sizeof(key), "win64");
		else if (!strcmp(ICE_PLATFORM_ARCH, "i386"))
			snprintf(key, sizeof(key), "win32");
		else
			snprintf(key, sizeof(key), "win-arm64");
	} else {
		die("unsupported platform: %s-%s", ICE_PLATFORM_OS,
		    ICE_PLATFORM_ARCH);
	}

	return key;
}

/* ------------------------------------------------------------------ */
/* Tools directory                                                     */
/* ------------------------------------------------------------------ */

static const char *tools_dir(void)
{
	if (opt_path && *opt_path)
		return opt_path;
	return ice_home();
}

/* ------------------------------------------------------------------ */
/* SHA-256 helpers                                                     */
/* ------------------------------------------------------------------ */

static int sha256_file(const char *path, unsigned char hash[SHA256_BLOCK_SIZE])
{
	FILE *fp;
	SHA256_CTX ctx;
	unsigned char buf[8192];
	size_t n;

	fp = fopen(path, "rb");
	if (!fp)
		return -1;

	sha256_init(&ctx);
	for (;;) {
		n = fread(buf, 1, sizeof(buf), fp);
		if (n > 0)
			sha256_update(&ctx, buf, n);
		if (n < sizeof(buf))
			break;
	}

	if (ferror(fp)) {
		fclose(fp);
		return -1;
	}

	sha256_final(&ctx, hash);
	fclose(fp);
	return 0;
}

static void hex_encode(const unsigned char *in, size_t len, char *out)
{
	for (size_t i = 0; i < len; i++)
		sprintf(out + i * 2, "%02x", in[i]);
	out[len * 2] = '\0';
}

/* ------------------------------------------------------------------ */
/* Target helpers                                                      */
/* ------------------------------------------------------------------ */

static int is_known_target(const char *target)
{
	for (const char *const *t = ice_supported_targets; *t; t++)
		if (!strcmp(target, *t))
			return 1;
	for (const char *const *t = ice_preview_targets; *t; t++)
		if (!strcmp(target, *t))
			return 1;
	return 0;
}

/**
 * Check whether a tool's "supported_targets" JSON array includes
 * @p target.  Returns true if the array contains "all" or @p target.
 */
static int tool_supports_target(struct json_value *tool, const char *target)
{
	struct json_value *arr = json_get(tool, "supported_targets");
	int n = json_array_size(arr);

	for (int i = 0; i < n; i++) {
		const char *t = json_as_string(json_array_at(arr, i));
		if (t && (!strcmp(t, "all") || !strcmp(t, target)))
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */

static struct json_value *find_recommended(struct json_value *versions)
{
	int n = json_array_size(versions);

	for (int i = 0; i < n; i++) {
		struct json_value *v = json_array_at(versions, i);
		const char *status = json_as_string(json_get(v, "status"));
		if (status && !strcmp(status, "recommended"))
			return v;
	}
	return NULL;
}

/**
 * Return a pointer to the last path component (the filename) of @p url.
 * If there is no '/' the whole string is returned.
 */
static const char *url_basename(const char *url)
{
	const char *p = strrchr(url, '/');
	return p ? p + 1 : url;
}

/* ------------------------------------------------------------------ */
/* Progress callbacks                                                  */
/* ------------------------------------------------------------------ */

/** Erase the current line and return the cursor to column 0. */
static void erase_line(void) { fputs_p("\033[2K\r", stderr); }

struct install_progress {
	const char *name;
	const char *version;
	int files;
};

static void download_progress(size_t total, size_t now, void *ctx)
{
	struct install_progress *p = ctx;

	if (!total)
		fprintf(stderr, "\r  @b{%s} %s - downloading %zu bytes",
			p->name, p->version, now);
	else
		fprintf(
		    stderr,
		    "\r  @b{%s} %s - downloading @b{%3d%%}  %zu / %zu bytes",
		    p->name, p->version, (int)(now * 100 / total), now, total);
}

static void extract_progress(const char *entry, void *ctx)
{
	struct install_progress *p = ctx;

	if (p->files == 0)
		erase_line();
	p->files++;
	fprintf(stderr, "\r  @b{%s} %s - extracting @b{%d} files", p->name,
		p->version, p->files);
	(void)entry;
}

/* ------------------------------------------------------------------ */
/* Single-tool installer                                               */
/* ------------------------------------------------------------------ */

static int install_tool(struct json_value *tool, const char *platform,
			const char *base_dir, int force)
{
	const char *name = json_as_string(json_get(tool, "name"));
	double strip = json_as_number(json_get(tool, "strip_container_dirs"));
	struct json_value *versions = json_get(tool, "versions");
	struct json_value *ver;
	struct json_value *dl;
	const char *ver_name;
	const char *url;
	const char *expected_sha;
	struct sbuf dest = SBUF_INIT;
	struct sbuf archive = SBUF_INIT;
	unsigned char hash[SHA256_BLOCK_SIZE];
	char hex[SHA256_BLOCK_SIZE * 2 + 1];
	int ret = -1;

	if (!name) {
		warn("tool entry without a name, skipping");
		goto out;
	}

	if (strip > 0) {
		warn("%s: strip_container_dirs not yet supported, skipping",
		     name);
		goto out;
	}

	ver = find_recommended(versions);
	if (!ver) {
		warn("%s: no recommended version, skipping", name);
		goto out;
	}

	ver_name = json_as_string(json_get(ver, "name"));
	if (!ver_name) {
		warn("%s: version entry has no name, skipping", name);
		goto out;
	}

	dl = json_get(ver, platform);
	if (!dl)
		dl = json_get(ver, "any");
	if (!dl) {
		warn("%s: no download for platform '%s', skipping", name,
		     platform);
		goto out;
	}

	url = json_as_string(json_get(dl, "url"));
	expected_sha = json_as_string(json_get(dl, "sha256"));
	if (!url || !expected_sha) {
		warn("%s: download entry missing url or sha256, skipping",
		     name);
		goto out;
	}

	/* Check for .zip archives (not yet supported) */
	if (strstr(url, ".zip")) {
		warn("%s: .zip extraction not yet supported, skipping", name);
		goto out;
	}

	/* Already installed? */
	sbuf_addf(&dest, "%s/tools/%s/%s", base_dir, name, ver_name);
	if (!force && !access(dest.buf, F_OK)) {
		fprintf(stderr, "  @g{%s %s} - already installed\n", name,
			ver_name);
		ret = 0;
		goto out;
	}

	struct install_progress progress = {.name = name, .version = ver_name};

	/* Download */
	sbuf_addf(&archive, "%s/dist/%s", base_dir, url_basename(url));
	if (mkdirp_for_file(archive.buf) < 0) {
		warn_errno("cannot create dist directory for %s", name);
		goto out;
	}

	if (http_download(url, archive.buf, download_progress, &progress) < 0) {
		erase_line();
		fprintf(stderr, "  @b{%s} %s - @r{failed}\n", name, ver_name);
		warn("download failed: %s", name);
		goto out;
	}

	/* Verify SHA-256 */
	if (sha256_file(archive.buf, hash) < 0) {
		erase_line();
		fprintf(stderr, "  @b{%s} %s - @r{failed}\n", name, ver_name);
		warn_errno("cannot read downloaded archive for %s", name);
		goto cleanup_archive;
	}
	hex_encode(hash, SHA256_BLOCK_SIZE, hex);
	if (strcmp(hex, expected_sha) != 0) {
		erase_line();
		fprintf(stderr, "  @b{%s} %s - @r{failed}\n", name, ver_name);
		warn("%s: SHA-256 mismatch: expected %s, got %s", name,
		     expected_sha, hex);
		goto cleanup_archive;
	}

	/* Extract */
	if (mkdirp(dest.buf) < 0) {
		erase_line();
		fprintf(stderr, "  @b{%s} %s - @r{failed}\n", name, ver_name);
		warn_errno("cannot create install directory for %s", name);
		goto cleanup_archive;
	}
	if (tar_extract_progress(archive.buf, dest.buf, extract_progress,
				 &progress) < 0) {
		erase_line();
		fprintf(stderr, "  @b{%s} %s - @r{failed}\n", name, ver_name);
		warn("extraction failed: %s", name);
		goto cleanup_archive;
	}
	erase_line();
	fprintf(stderr, "  @g{%s %s} - done\n", name, ver_name);

	ret = 0;

cleanup_archive:
	if (ret < 0)
		unlink(archive.buf);
out:
	sbuf_release(&dest);
	sbuf_release(&archive);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Command entry point                                                 */
/* ------------------------------------------------------------------ */

int install_from_manifest(const char *manifest_path, const char *target_filter,
			  const char *tool_filter, int force)
{
	struct sbuf manifest = SBUF_INIT;
	struct json_value *root;
	struct json_value *tools;
	const char *platform;
	const char *base_dir;
	int n;
	int installed = 0, skipped = 0, failed = 0;
	int tool_found = 0;

	if (target_filter && !is_known_target(target_filter))
		die("'%s' is not a known target", target_filter);

	if (sbuf_read_file(&manifest, manifest_path) < 0)
		die_errno("cannot read '%s'", manifest_path);

	root = json_parse(manifest.buf, manifest.len);
	if (!root)
		die("failed to parse '%s'", manifest_path);

	/* Validate schema version */
	double ver = json_as_number(json_get(root, "version"));
	if (ver < 1) {
		die("missing or invalid 'version' field in '%s'",
		    manifest_path);
	}

	tools = json_get(root, "tools");
	if (!tools) {
		die("no 'tools' array in '%s'", manifest_path);
	}

	platform = platform_key();
	base_dir = tools_dir();
	n = json_array_size(tools);

	fprintf(stderr, "Platform: @b{%s}  Tools path: @b{%s}\n", platform,
		base_dir);

	for (int i = 0; i < n; i++) {
		struct json_value *tool = json_array_at(tools, i);
		const char *name = json_as_string(json_get(tool, "name"));
		const char *install = json_as_string(json_get(tool, "install"));

		if (!name)
			continue;

		if (tool_filter) {
			if (strcmp(name, tool_filter) != 0)
				continue;
			tool_found = 1;
		} else {
			if (!install || strcmp(install, "always") != 0) {
				skipped++;
				continue;
			}
		}

		if (target_filter &&
		    !tool_supports_target(tool, target_filter)) {
			skipped++;
			continue;
		}

		int rc = install_tool(tool, platform, base_dir, force);
		if (rc < 0)
			failed++;
		else
			installed++;
	}

	if (tool_filter && !tool_found)
		die("tool '%s' not found in manifest", tool_filter);

	fprintf(stderr, "Done: @b{%d} installed, %d skipped, %d failed\n",
		installed, skipped, failed);

	json_free(root);
	sbuf_release(&manifest);
	return failed ? 1 : 0;
}

int cmd_install(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_tools_install_desc);
	if (argc != 1)
		die("expected exactly one argument: path to tools.json");

	return install_from_manifest(argv[0], opt_target, opt_tool, opt_force);
}
