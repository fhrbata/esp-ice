/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fs.c
 * @brief Portable filesystem helpers (mkdirp, rmtree).
 */
#include "fs.h"
#include "ice.h"

int mkdirp(const char *dir)
{
	struct sbuf sb = SBUF_INIT;
	char *p;
	int rc = 0;

	if (!dir || !*dir)
		return 0;

	sbuf_addstr(&sb, dir);

	/*
	 * Find the first "creatable" position.  Drive prefixes like "C:"
	 * on Windows and a leading root separator cannot be mkdir()'d --
	 * skip past them so we never call mkdir("C:") or mkdir("").
	 */
	p = sb.buf;
#ifdef _WIN32
	if (sb.len >= 2 && isalpha((unsigned char)p[0]) && p[1] == ':')
		p += 2;
#endif
	if (*p == '/' || *p == '\\')
		p++;

	for (; *p; p++) {
		if (*p != '/' && *p != '\\')
			continue;
		char c = *p;
		*p = '\0';
		if (mkdir(sb.buf, 0755) != 0 && errno != EEXIST) {
			*p = c;
			rc = -1;
			goto done;
		}
		*p = c;
	}

	if (mkdir(sb.buf, 0755) != 0 && errno != EEXIST)
		rc = -1;

done:
	sbuf_release(&sb);
	return rc;
}

int mkdirp_for_file(const char *path)
{
	struct sbuf dir = SBUF_INIT;
	const char *sep;
	const char *bs;
	int rc = 0;

	sep = strrchr(path, '/');
	bs = strrchr(path, '\\');
	if (bs && (!sep || bs > sep))
		sep = bs;

	if (!sep || sep == path)
		return 0;

	sbuf_add(&dir, path, sep - path);
	rc = mkdirp(dir.buf);
	sbuf_release(&dir);
	return rc;
}

int write_file_atomic(const char *path, const void *data, size_t len)
{
	struct sbuf tmp = SBUF_INIT;
	FILE *fp;
	int rc = -1;

	sbuf_addf(&tmp, "%s.tmp", path);

	fp = fopen(tmp.buf, "wb");
	if (!fp)
		goto done;
	if (fwrite(data, 1, len, fp) != len) {
		fclose(fp);
		goto unlink_tmp;
	}
	if (fclose(fp) != 0)
		goto unlink_tmp;

	if (rename(tmp.buf, path) != 0)
		goto unlink_tmp;

	rc = 0;
	goto done;

unlink_tmp:
	unlink(tmp.buf);
done:
	sbuf_release(&tmp);
	return rc;
}

struct rmtree_ctx {
	const char *path;
	int verbose;
	int rc;
};

static int rmtree_cb(const char *name, void *ud)
{
	struct rmtree_ctx *ctx = ud;
	struct sbuf child = SBUF_INIT;

	sbuf_addf(&child, "%s/%s", ctx->path, name);

	if (ctx->verbose)
		printf("Removing: %s\n", child.buf);

	if (is_directory(child.buf)) {
		if (rmtree(child.buf, ctx->verbose) < 0)
			ctx->rc = -1;
		if (rmdir(child.buf) < 0) {
			warn_errno("cannot remove '%s'", child.buf);
			ctx->rc = -1;
		}
	} else if (unlink(child.buf) < 0) {
		warn_errno("cannot remove '%s'", child.buf);
		ctx->rc = -1;
	}

	sbuf_release(&child);
	return 0;
}

int rmtree(const char *path, int verbose)
{
	struct rmtree_ctx ctx = {.path = path, .verbose = verbose, .rc = 0};

	if (dir_foreach(path, rmtree_cb, &ctx) < 0) {
		warn_errno("cannot open '%s'", path);
		return -1;
	}
	return ctx.rc;
}

int find_in_path(const char *name)
{
	const char *p = getenv("PATH");
	struct sbuf buf = SBUF_INIT;

	if (!p || !*p)
		return 0;

	while (*p) {
		const char *end = p;
		while (*end && *end != ':')
			end++;

		sbuf_reset(&buf);
		if (end != p) {
			sbuf_add(&buf, p, (size_t)(end - p));
			sbuf_addch(&buf, '/');
		}
		sbuf_addstr(&buf, name);

		if (!access(buf.buf, X_OK)) {
			sbuf_release(&buf);
			return 1;
		}

		if (!*end)
			break;
		p = end + 1;
	}

	sbuf_release(&buf);
	return 0;
}
