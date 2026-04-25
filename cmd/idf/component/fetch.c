/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Primitives used by the component-manager hooks to materialise
 * downloaded components on disk.  See fetch.h for the contract.
 */
#include "fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cmd/idf/component/lockfile.h"
#include "error.h"
#include "fs.h"
#include "git.h"
#include "http.h"
#include "json.h"
#include "sbuf.h"
#include "vendor/sha256/sha256.h"
#include "zip.h"

void fetch_build_name(struct sbuf *out, const char *name)
{
	if (!name)
		return;
	for (const char *p = name; *p; p++) {
		if (*p == '/')
			sbuf_addstr(out, "__");
		else
			sbuf_addch(out, (unsigned char)*p);
	}
}

int fetch_compute_sha256(const char *path, char out_hex[65])
{
	FILE *fp;
	SHA256_CTX ctx;
	unsigned char buf[8192];
	unsigned char digest[SHA256_BLOCK_SIZE];
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
	fclose(fp);

	sha256_final(&ctx, digest);

	for (size_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
		static const char hex[] = "0123456789abcdef";
		out_hex[i * 2] = hex[(digest[i] >> 4) & 0xf];
		out_hex[i * 2 + 1] = hex[digest[i] & 0xf];
	}
	out_hex[64] = '\0';
	return 0;
}

static int hex_eq_ci(const char *a, const char *b)
{
	while (*a && *b) {
		int ca = (unsigned char)*a, cb = (unsigned char)*b;
		if (ca >= 'A' && ca <= 'Z')
			ca += 32;
		if (cb >= 'A' && cb <= 'Z')
			cb += 32;
		if (ca != cb)
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

int fetch_verify_sha256(const char *path, const char *expected_hex)
{
	char got[65];

	if (!expected_hex)
		return -1;
	if (fetch_compute_sha256(path, got) < 0)
		return -1;
	return hex_eq_ci(got, expected_hex) ? 0 : -1;
}

int fetch_download(const char *url, const char *dest_path)
{
	if (mkdirp_for_file(dest_path) < 0)
		return -1;
	if (http_download(url, dest_path, NULL, NULL) < 0) {
		/* http_download typically leaves no file behind on failure,
		 * but be defensive: best-effort unlink. */
		(void)remove(dest_path);
		return -1;
	}
	return 0;
}

/* ==================================================================== */
/* Orchestrator: fetch_component()                                       */
/* ==================================================================== */

/* Append "/" to @p url unless it already ends with one. */
static void ensure_trailing_slash(struct sbuf *url)
{
	if (url->len == 0 || url->buf[url->len - 1] != '/')
		sbuf_addch(url, '/');
}

/*
 * Resolve the storage URL by fetching @c <registry_url>/api/ and
 * reading @c components_base_url from the response.
 *
 * On success, @p storage_url contains a normalized URL with a
 * trailing slash.  On failure @p storage_url is left empty.
 */
static int discover_storage_url(const char *registry_url,
				struct sbuf *storage_url)
{
	struct sbuf url = SBUF_INIT;
	struct sbuf body = SBUF_INIT;
	struct json_value *root = NULL;
	const char *base;
	int rc = -1;

	sbuf_addstr(&url, registry_url);
	ensure_trailing_slash(&url);
	sbuf_addstr(&url, "api/");

	if (http_get(url.buf, &body) != 200)
		goto out;

	root = json_parse(body.buf, body.len);
	if (!root)
		goto out;

	base = json_as_string(json_get(root, "components_base_url"));
	if (!base)
		goto out;

	sbuf_addstr(storage_url, base);
	ensure_trailing_slash(storage_url);
	rc = 0;
out:
	if (root)
		json_free(root);
	sbuf_release(&url);
	sbuf_release(&body);
	return rc;
}

/*
 * Fetch @c <storage>/components/<name>.json and write the relative
 * download path of the version that matches @p target_version into
 * @p url_rel.  Empty @p url_rel on failure.
 */
static int find_version_url(const char *storage_url, const char *name,
			    const char *target_version, struct sbuf *url_rel)
{
	struct sbuf url = SBUF_INIT;
	struct sbuf body = SBUF_INIT;
	struct json_value *root = NULL;
	struct json_value *versions;
	int rc = -1;

	sbuf_addstr(&url, storage_url);
	sbuf_addf(&url, "components/%s.json", name);

	if (http_get(url.buf, &body) != 200)
		goto out;

	root = json_parse(body.buf, body.len);
	if (!root)
		goto out;

	versions = json_get(root, "versions");
	{
		int n = json_array_size(versions);
		for (int i = 0; i < n; i++) {
			struct json_value *v = json_array_at(versions, i);
			const char *vs = json_as_string(json_get(v, "version"));
			if (vs && !strcmp(vs, target_version)) {
				const char *u =
				    json_as_string(json_get(v, "url"));
				if (u) {
					sbuf_addstr(url_rel, u);
					rc = 0;
				}
				break;
			}
		}
	}
out:
	if (root)
		json_free(root);
	sbuf_release(&url);
	sbuf_release(&body);
	return rc;
}

static int fetch_service(const struct lockfile_entry *entry,
			 const char *managed_components_dir)
{
	struct sbuf storage = SBUF_INIT;
	struct sbuf url_rel = SBUF_INIT;
	struct sbuf dl_url = SBUF_INIT;
	struct sbuf build_name = SBUF_INIT;
	struct sbuf tmp_zip = SBUF_INIT;
	struct sbuf dest = SBUF_INIT;
	int rc = -1;

	if (!entry->registry_url || !entry->version)
		goto out;

	if (discover_storage_url(entry->registry_url, &storage) < 0)
		goto out;

	if (find_version_url(storage.buf, entry->name, entry->version,
			     &url_rel) < 0)
		goto out;

	sbuf_addstr(&dl_url, storage.buf);
	sbuf_addstr(&dl_url, url_rel.buf);

	fetch_build_name(&build_name, entry->name);

	if (mkdirp(managed_components_dir) < 0)
		goto out;

	sbuf_addf(&tmp_zip, "%s/.%s.zip", managed_components_dir,
		  build_name.buf);
	if (fetch_download(dl_url.buf, tmp_zip.buf) < 0)
		goto out;

	/* Pin to the digest carried in the lockfile -- without this
	 * the lockfile only pins the version number, not the bytes. */
	if (entry->component_hash &&
	    fetch_verify_sha256(tmp_zip.buf, entry->component_hash) != 0) {
		warn("%s@%s: archive sha256 mismatch (expected %s)",
		     entry->name, entry->version, entry->component_hash);
		goto out;
	}

	sbuf_addf(&dest, "%s/%s", managed_components_dir, build_name.buf);
	if (access(dest.buf, F_OK) == 0)
		(void)rmtree(dest.buf, 0); /* idempotent: replace existing */

	if (zip_extract_all(tmp_zip.buf, dest.buf) < 0)
		goto out;

	rc = 0;
out:
	if (tmp_zip.len)
		(void)remove(tmp_zip.buf);
	sbuf_release(&storage);
	sbuf_release(&url_rel);
	sbuf_release(&dl_url);
	sbuf_release(&build_name);
	sbuf_release(&tmp_zip);
	sbuf_release(&dest);
	return rc;
}

static int fetch_git(const struct lockfile_entry *entry,
		     const char *managed_components_dir)
{
	struct sbuf build_name = SBUF_INIT;
	struct sbuf dest = SBUF_INIT;
	int rc = -1;

	if (!entry->git_url)
		goto out;

	fetch_build_name(&build_name, entry->name);

	if (mkdirp(managed_components_dir) < 0)
		goto out;

	sbuf_addf(&dest, "%s/%s", managed_components_dir, build_name.buf);
	if (access(dest.buf, F_OK) == 0)
		(void)rmtree(dest.buf, 0); /* idempotent: replace existing */

	{
		const char *clone[] = {"git",	       "clone",	 "--quiet",
				       entry->git_url, dest.buf, NULL};
		if (git_run(NULL, clone) != 0)
			goto out;
	}

	if (entry->git_ref) {
		const char *checkout[] = {"git", "checkout", "--quiet",
					  entry->git_ref, NULL};
		if (git_run(dest.buf, checkout) != 0)
			goto out;
	}

	rc = 0;
out:
	sbuf_release(&build_name);
	sbuf_release(&dest);
	return rc;
}

int fetch_component(const struct lockfile_entry *entry,
		    const char *managed_components_dir)
{
	if (!entry || !entry->name || !managed_components_dir)
		return -1;

	switch (entry->src_type) {
	case LOCKFILE_SRC_IDF:
	case LOCKFILE_SRC_LOCAL:
		return 0; /* nothing to fetch -- already on disk */
	case LOCKFILE_SRC_SERVICE:
		return fetch_service(entry, managed_components_dir);
	case LOCKFILE_SRC_GIT:
		return fetch_git(entry, managed_components_dir);
	case LOCKFILE_SRC_UNKNOWN:
	default:
		return -1;
	}
}
