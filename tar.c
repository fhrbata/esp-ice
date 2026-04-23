/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tar.c
 * @brief Tar archive extractor: ustar + GNU long names + pax paths.
 */

#include <stdint.h>

#include "ice.h"
#include "reader.h"
#include "tar.h"

#define TAR_BLOCK 512

/* Ustar header field offsets (POSIX 1003.1-1988, 512 bytes total). */
#define H_MODE 100
#define H_SIZE 124
#define H_CHKSUM 148
#define H_TYPE 156
#define H_LINK 157
#define H_PREFIX 345

#define NAME_MAX_LEN 4096

struct tar_entry {
	char name[NAME_MAX_LEN];
	char linkname[NAME_MAX_LEN];
	uint64_t size;
	mode_t mode;
	char typeflag;
};

/* ---- header parsing --------------------------------------------------- */

/*
 * Tar encodes all numeric fields as NUL-or-space-terminated octal ASCII.
 * For sizes >= 8 GiB the high bit of the first byte is set and the rest
 * of the field is a big-endian base-256 value (GNU extension).
 */
static int parse_octal(const uint8_t *s, size_t n, uint64_t *out)
{
	if (n > 0 && (s[0] & 0x80)) {
		uint64_t v = s[0] & 0x7f;
		for (size_t i = 1; i < n; i++)
			v = (v << 8) | s[i];
		*out = v;
		return 0;
	}

	uint64_t v = 0;
	size_t i = 0;
	while (i < n && (s[i] == ' ' || s[i] == 0))
		i++;
	for (; i < n; i++) {
		if (s[i] == 0 || s[i] == ' ')
			break;
		if (s[i] < '0' || s[i] > '7')
			return -1;
		v = v * 8 + (uint64_t)(s[i] - '0');
	}
	*out = v;
	return 0;
}

static int is_zero_block(const uint8_t *hdr)
{
	for (int i = 0; i < TAR_BLOCK; i++)
		if (hdr[i])
			return 0;
	return 1;
}

static int verify_checksum(const uint8_t *hdr)
{
	uint64_t stored;
	if (parse_octal(hdr + H_CHKSUM, 8, &stored) < 0)
		return -1;

	uint64_t sum = 0;
	for (int i = 0; i < TAR_BLOCK; i++) {
		if (i >= H_CHKSUM && i < H_CHKSUM + 8)
			sum += ' ';
		else
			sum += hdr[i];
	}
	return sum == stored ? 0 : -1;
}

static int parse_header(const uint8_t *hdr, struct tar_entry *e)
{
	uint64_t mode, size;

	if (verify_checksum(hdr) < 0)
		return -1;
	if (parse_octal(hdr + H_MODE, 8, &mode) < 0)
		return -1;
	if (parse_octal(hdr + H_SIZE, 12, &size) < 0)
		return -1;

	/* Full path = prefix + "/" + name when prefix is non-empty. */
	char name100[101], prefix155[156];
	memcpy(name100, hdr, 100);
	name100[100] = 0;
	memcpy(prefix155, hdr + H_PREFIX, 155);
	prefix155[155] = 0;

	if (prefix155[0])
		snprintf(e->name, sizeof(e->name), "%s/%s", prefix155, name100);
	else
		snprintf(e->name, sizeof(e->name), "%s", name100);

	memcpy(e->linkname, hdr + H_LINK, 100);
	e->linkname[100] = 0;

	e->mode = (mode_t)(mode & 07777);
	e->size = size;
	e->typeflag = (char)hdr[H_TYPE];
	return 0;
}

/* Advance the reader past the 0..511-byte pad that rounds entry data
 * up to a 512-byte boundary. */
static int skip_padding(struct reader *r, uint64_t size)
{
	uint64_t pad = (TAR_BLOCK - (size % TAR_BLOCK)) % TAR_BLOCK;
	return pad ? reader_skip(r, (size_t)pad) : 0;
}

/* Read entry data into a NUL-terminated buffer and step past padding.
 * Used for L/K/x headers whose data IS the override string (or pax blob). */
static int read_entry_to_buf(struct reader *r, uint64_t size, char *out,
			     size_t out_size)
{
	if (size + 1 > out_size)
		return -1;
	if (reader_read_exact(r, out, (size_t)size) < 0)
		return -1;
	out[size] = 0;
	return skip_padding(r, size);
}

/* ---- pax extended header parsing -------------------------------------- */

/*
 * pax records have the form:
 *    "%d %s=%s\n", <length>, <keyword>, <value>
 * where <length> is the decimal byte count of the entire record
 * including the length field, the space, '=', value, and trailing \n.
 * We only care about 'path' and 'linkpath' keys.
 */
static int parse_pax(const char *data, size_t data_len, char *path,
		     size_t path_sz, char *linkpath, size_t linkpath_sz)
{
	const char *p = data;
	const char *end = data + data_len;

	while (p < end) {
		const char *rec = p;
		long rec_len = 0;

		while (p < end && *p >= '0' && *p <= '9') {
			rec_len = rec_len * 10 + (*p - '0');
			p++;
		}
		if (p == end || *p != ' ' || rec_len <= 0 ||
		    rec + rec_len > end)
			return -1;
		p++; /* past the space separating length from keyword */

		const char *rec_end = rec + rec_len;
		/* Record ends with '\n'; strip it when reading the value. */
		const char *eq = memchr(p, '=', (size_t)(rec_end - 1 - p));
		if (!eq) {
			p = rec_end;
			continue;
		}

		size_t key_len = (size_t)(eq - p);
		const char *val = eq + 1;
		size_t val_len = (size_t)(rec_end - 1 - val);

		if (key_len == 4 && memcmp(p, "path", 4) == 0) {
			if (val_len + 1 > path_sz)
				return -1;
			memcpy(path, val, val_len);
			path[val_len] = 0;
		} else if (key_len == 8 && memcmp(p, "linkpath", 8) == 0) {
			if (val_len + 1 > linkpath_sz)
				return -1;
			memcpy(linkpath, val, val_len);
			linkpath[val_len] = 0;
		}

		p = rec_end;
	}
	return 0;
}

/* ---- path safety ------------------------------------------------------ */

/* Reject absolute paths and any '..' components (tar-slip protection). */
static int is_safe_name(const char *name)
{
	if (!name || !*name)
		return 0;
	if (name[0] == '/' || name[0] == '\\')
		return 0;
	/* Reject drive-letter absolute paths ("C:foo") on all platforms:
	 * they'd escape dest on Windows, and no POSIX user has a tar
	 * entry legitimately starting with "<letter>:". */
	if (isalpha((unsigned char)name[0]) && name[1] == ':')
		return 0;

	const char *p = name;
	while (*p) {
		if (p[0] == '.' && p[1] == '.' &&
		    (p[2] == 0 || p[2] == '/' || p[2] == '\\'))
			return 0;
		while (*p && *p != '/' && *p != '\\')
			p++;
		while (*p == '/' || *p == '\\')
			p++;
	}
	return 1;
}

/* ---- extraction ------------------------------------------------------- */

static int extract_regular(const char *dest, const char *name, mode_t mode,
			   struct reader *r, uint64_t size)
{
	struct sbuf path = SBUF_INIT;
	FILE *fp = NULL;
	int rc = -1;
	uint8_t buf[8192];
	uint64_t left = size;

	sbuf_addf(&path, "%s/%s", dest, name);
	if (mkdirp_for_file(path.buf) < 0)
		goto out;

	fp = fopen(path.buf, "wb");
	if (!fp)
		goto out;

	while (left > 0) {
		size_t chunk = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
		ssize_t n = r->read(r, buf, chunk);
		if (n <= 0)
			goto out;
		if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n)
			goto out;
		left -= (uint64_t)n;
	}

	if (fclose(fp) != 0) {
		fp = NULL;
		goto out;
	}
	fp = NULL;

	/* Apply mode bits after close (POSIX); chmod is a no-op on
	 * Windows for execute bits but harmless.  Non-fatal: some
	 * filesystems (FAT) don't support chmod. */
	(void)chmod(path.buf, mode);
	rc = 0;
out:
	if (fp)
		fclose(fp);
	sbuf_release(&path);
	return rc;
}

static int extract_directory(const char *dest, const char *name, mode_t mode)
{
	struct sbuf path = SBUF_INIT;
	sbuf_addf(&path, "%s/%s", dest, name);
	int rc = mkdirp(path.buf);
	if (rc == 0)
		(void)chmod(path.buf, mode); /* best effort */
	sbuf_release(&path);
	return rc;
}

static int extract_symlink(const char *dest, const char *name,
			   const char *target)
{
	struct sbuf path = SBUF_INIT;
	int rc;

	sbuf_addf(&path, "%s/%s", dest, name);
	mkdirp_for_file(path.buf);
	unlink(path.buf); /* so re-extraction isn't blocked by EEXIST */
	rc = symlink(target, path.buf);
	sbuf_release(&path);
	return rc;
}

static int extract_hardlink(const char *dest, const char *name,
			    const char *target)
{
	struct sbuf src = SBUF_INIT, dst = SBUF_INIT;
	int rc;

	sbuf_addf(&src, "%s/%s", dest, target);
	sbuf_addf(&dst, "%s/%s", dest, name);
	mkdirp_for_file(dst.buf);
	unlink(dst.buf);
	rc = link(src.buf, dst.buf);
	sbuf_release(&src);
	sbuf_release(&dst);
	return rc;
}

/* ---- main loop -------------------------------------------------------- */

static int ends_with(const char *s, const char *suffix)
{
	size_t sl = strlen(s), pl = strlen(suffix);
	return pl <= sl && strcmp(s + sl - pl, suffix) == 0;
}

static struct reader *open_reader_for_path(const char *path)
{
	if (ends_with(path, ".tar.xz") || ends_with(path, ".txz"))
		return reader_open_xz(path);
	if (ends_with(path, ".tar.gz") || ends_with(path, ".tgz"))
		return reader_open_gzip(path);
	return reader_open_plain(path);
}

int tar_extract(const char *src, const char *dest_dir)
{
	return tar_extract_progress(src, dest_dir, NULL, NULL);
}

int tar_extract_progress(const char *src, const char *dest_dir,
			 tar_progress_fn progress, void *ctx)
{
	struct reader *r = open_reader_for_path(src);
	if (!r) {
		err_errno("open '%s'", src);
		return -1;
	}

	/* Long-name overrides.  An L / K / x record modifies the NEXT
	 * real entry only; we clear these after each non-extension header. */
	char gnu_name[NAME_MAX_LEN] = "";
	char gnu_linkname[NAME_MAX_LEN] = "";
	char pax_path[NAME_MAX_LEN] = "";
	char pax_linkpath[NAME_MAX_LEN] = "";

	uint8_t hdr[TAR_BLOCK];
	int rc = 0;
	int zero_blocks = 0;

	for (;;) {
		if (reader_read_exact(r, hdr, TAR_BLOCK) < 0) {
			err("unexpected end of tar stream");
			rc = -1;
			break;
		}

		if (is_zero_block(hdr)) {
			/* End-of-archive marker is two consecutive zero
			 * blocks; tolerate just one since some tools omit
			 * the second when output is padded. */
			if (++zero_blocks >= 1)
				break;
			continue;
		}
		zero_blocks = 0;

		struct tar_entry e;
		if (parse_header(hdr, &e) < 0) {
			err("corrupt tar header");
			rc = -1;
			break;
		}

		/* Resolve effective name/linkname: pax beats GNU beats ustar.
		 */
		const char *name = e.name;
		const char *linkname = e.linkname;
		if (gnu_name[0])
			name = gnu_name;
		if (pax_path[0])
			name = pax_path;
		if (gnu_linkname[0])
			linkname = gnu_linkname;
		if (pax_linkpath[0])
			linkname = pax_linkpath;

		switch (e.typeflag) {
		case 'L':
			if (read_entry_to_buf(r, e.size, gnu_name,
					      sizeof(gnu_name)) < 0) {
				rc = -1;
				goto done;
			}
			continue;
		case 'K':
			if (read_entry_to_buf(r, e.size, gnu_linkname,
					      sizeof(gnu_linkname)) < 0) {
				rc = -1;
				goto done;
			}
			continue;
		case 'x': {
			char paxbuf[8192];
			if (read_entry_to_buf(r, e.size, paxbuf,
					      sizeof(paxbuf)) < 0) {
				rc = -1;
				goto done;
			}
			if (parse_pax(paxbuf, (size_t)e.size, pax_path,
				      sizeof(pax_path), pax_linkpath,
				      sizeof(pax_linkpath)) < 0) {
				rc = -1;
				goto done;
			}
			continue;
		}
		case 'g':
			/* Global pax header applies until end of archive;
			 * we don't carry any keys that need it. */
			if (reader_skip(r, (size_t)e.size) < 0 ||
			    skip_padding(r, e.size) < 0) {
				rc = -1;
				goto done;
			}
			continue;

		case '0':
		case '\0':
			if (!is_safe_name(name)) {
				warn("skipping unsafe entry: %s", name);
				if (reader_skip(r, (size_t)e.size) < 0 ||
				    skip_padding(r, e.size) < 0) {
					rc = -1;
					goto done;
				}
			} else if (extract_regular(dest_dir, name, e.mode, r,
						   e.size) < 0 ||
				   skip_padding(r, e.size) < 0) {
				err("extract failed: %s", name);
				rc = -1;
				goto done;
			} else if (progress) {
				progress(name, ctx);
			}
			break;

		case '5':
			if (is_safe_name(name)) {
				extract_directory(dest_dir, name, e.mode);
				if (progress)
					progress(name, ctx);
			}
			break;

		case '1':
			if (is_safe_name(name) && is_safe_name(linkname) &&
			    linkname[0]) {
				extract_hardlink(dest_dir, name, linkname);
				if (progress)
					progress(name, ctx);
			}
			break;

		case '2':
			if (is_safe_name(name) && linkname[0]) {
				extract_symlink(dest_dir, name, linkname);
				if (progress)
					progress(name, ctx);
			}
			break;

		default:
			/* Devices ('3','4'), fifos ('6'), and any unknown
			 * typeflag: drain data and keep going. */
			if (e.size > 0) {
				if (reader_skip(r, (size_t)e.size) < 0 ||
				    skip_padding(r, e.size) < 0) {
					rc = -1;
					goto done;
				}
			}
			break;
		}

		/* Overrides apply to one entry only. */
		gnu_name[0] = 0;
		gnu_linkname[0] = 0;
		pax_path[0] = 0;
		pax_linkpath[0] = 0;
	}

done:
	reader_close(r);
	return rc;
}
