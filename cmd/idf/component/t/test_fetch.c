/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for cmd/idf/component/fetch.c primitives.
 * Skips the network-bound @c fetch_download -- integration tests in a
 * follow-up phase can exercise that against a local file:// server.
 */
#include "cmd/idf/component/fetch.h"
#include "cmd/idf/component/lockfile.h"
#include "ice.h"
#include "tap.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Drop @p len bytes of @p data into @p path; mkdirp the parent first. */
static void write_file(const char *path, const char *data)
{
	FILE *fp;
	tap_check(mkdirp_for_file(path) == 0);
	fp = fopen(path, "wb");
	tap_check(fp != NULL);
	if (data && *data)
		fputs(data, fp);
	fclose(fp);
}

static void build(struct sbuf *out, const char *name)
{
	sbuf_reset(out);
	fetch_build_name(out, name);
}

int main(void)
{
	/* Name normalisation: slash -> double underscore. */
	{
		struct sbuf sb = SBUF_INIT;
		build(&sb, "example/cmp");
		tap_check(!strcmp(sb.buf, "example__cmp"));
		build(&sb, "espressif/cjson");
		tap_check(!strcmp(sb.buf, "espressif__cjson"));
		build(&sb, "idf");
		tap_check(!strcmp(sb.buf, "idf"));
		/* Multi-slash names (not used in practice, but must not drop
		 * segments). */
		build(&sb, "a/b/c");
		tap_check(!strcmp(sb.buf, "a__b__c"));
		build(&sb, "");
		tap_check(!strcmp(sb.buf, ""));
		sbuf_release(&sb);
		tap_done("fetch_build_name: slash -> __");
	}

	/* sha256: known vectors.  "abc" -> ba7816bf8f01cfea4141... */
	{
		const char *path = "abc.bin";
		FILE *fp = fopen(path, "wb");
		tap_check(fp != NULL);
		fputs("abc", fp);
		fclose(fp);

		char hex[65];
		tap_check(fetch_compute_sha256(path, hex) == 0);
		tap_check(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a"
				       "396177a9cb410ff61f20015ad"));
		tap_done("fetch_compute_sha256: classic 'abc' vector");
	}

	/* sha256: empty file -> e3b0c44298fc1c149afbf4c8996fb92427ae41e4... */
	{
		const char *path = "empty.bin";
		FILE *fp = fopen(path, "wb");
		tap_check(fp != NULL);
		fclose(fp);

		char hex[65];
		tap_check(fetch_compute_sha256(path, hex) == 0);
		tap_check(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e"
				       "4649b934ca495991b7852b855"));
		tap_done("fetch_compute_sha256: empty file");
	}

	/* Missing file -> error. */
	{
		char hex[65];
		tap_check(fetch_compute_sha256("no_such_file", hex) == -1);
		tap_done("fetch_compute_sha256: missing file returns -1");
	}

	/* Verify: matching digest succeeds, case-insensitive. */
	{
		const char *path =
		    "abc.bin"; /* still around from earlier test */
		tap_check(fetch_verify_sha256(
			      path, "ba7816bf8f01cfea414140de5dae2223b00361a396"
				    "177a9cb410ff61f20015ad") == 0);
		tap_check(fetch_verify_sha256(
			      path, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396"
				    "177A9CB410FF61F20015AD") == 0);
		/* Mismatch. */
		tap_check(fetch_verify_sha256(
			      path, "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
				    "adbeefdeadbeefdeadbeef") == -1);
		/* NULL expected. */
		tap_check(fetch_verify_sha256(path, NULL) == -1);
		tap_done("fetch_verify_sha256: match, case, mismatch, NULL");
	}

	/* fetch_component: IDF / LOCAL sources are no-ops -- nothing
	 * downloaded, nothing extracted, no directory created. */
	{
		struct lockfile_entry e_idf = {
		    .name = (char *)"idf",
		    .src_type = LOCKFILE_SRC_IDF,
		};
		struct lockfile_entry e_local = {
		    .name = (char *)"helper",
		    .src_type = LOCKFILE_SRC_LOCAL,
		};
		struct stat st;

		tap_check(fetch_component(&e_idf, "noop_dir") == 0);
		tap_check(fetch_component(&e_local, "noop_dir") == 0);
		tap_check(stat("noop_dir", &st) != 0);
		tap_done("fetch_component: IDF/LOCAL are no-ops");
	}

	/* fetch_component: UNKNOWN / NULL inputs reject cleanly. */
	{
		struct lockfile_entry e_unknown = {
		    .name = (char *)"x",
		    .src_type = LOCKFILE_SRC_UNKNOWN,
		};
		tap_check(fetch_component(&e_unknown, "noop_dir") == -1);
		tap_check(fetch_component(NULL, "noop_dir") == -1);
		{
			struct lockfile_entry e = {.name = NULL};
			tap_check(fetch_component(&e, "noop_dir") == -1);
		}
		tap_done("fetch_component: UNKNOWN / NULL rejected");
	}

	/*
	 * fetch_compute_dirhash: empty tree -> sha256 of empty input (the
	 * outer hash sees no path/file bytes, so the result is the standard
	 * "e3b0..." digest).
	 */
	{
		const char *root = "dir_empty";
		char hex[65];
		tap_check(mkdirp(root) == 0);
		tap_check(fetch_compute_dirhash(root, hex) == 0);
		tap_check(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41"
				       "e4649b934ca495991b7852b855"));
		tap_done("fetch_compute_dirhash: empty directory");
	}

	/*
	 * Recursion + sort order: top-level @c a.txt and nested
	 * @c sub/b.txt.  Hash is precomputed against the python tool's
	 * hash_dir() so any drift between implementations breaks the test.
	 */
	{
		const char *root = "dir_recursive";
		char hex[65];
		write_file("dir_recursive/a.txt", "hello");
		write_file("dir_recursive/sub/b.txt", "world");
		tap_check(fetch_compute_dirhash(root, hex) == 0);
		tap_check(!strcmp(hex, "aa53ebe0074a0f07cb17d9fe1e0a05f94b181f"
				       "747bd24fbb7ad642f8fa7efe3c"));
		tap_done("fetch_compute_dirhash: recursion + sorted order");
	}

	/*
	 * @c .component_hash and @c CHECKSUMS.json are skipped at any
	 * depth -- adding them must not change the digest.  Reuse the
	 * single-file digest computed via the python algorithm.
	 */
	{
		const char *root = "dir_excluded";
		char hex[65];
		write_file("dir_excluded/a.txt", "hello");
		tap_check(fetch_compute_dirhash(root, hex) == 0);
		tap_check(!strcmp(hex, "7ce533eb8ea1f2e8b3cfe38fbffe1c92f04094"
				       "2a2bb1b9c8fa6031670e542ce1"));

		write_file("dir_excluded/.component_hash", "IGNORED");
		write_file("dir_excluded/CHECKSUMS.json", "IGNORED");
		tap_check(fetch_compute_dirhash(root, hex) == 0);
		tap_check(!strcmp(hex, "7ce533eb8ea1f2e8b3cfe38fbffe1c92f04094"
				       "2a2bb1b9c8fa6031670e542ce1"));
		tap_done("fetch_compute_dirhash: skips marker files");
	}

	return tap_result();
}
