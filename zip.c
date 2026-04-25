/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal ZIP archive extractor (see zip.h).
 *
 * Layout summary of the PKZIP structures we read:
 *
 *   [local file header][filename][extra][data]   repeated per entry
 *   [central directory header][filename][extra][comment]  ...
 *   [end of central directory (EOCD)]
 *
 * The EOCD sits at the end of the file (within the last 64 KiB +
 * header).  It points at the start of the central directory, whose
 * entries describe every file in the archive.  Each central-directory
 * entry carries the compression method, CRC-32, sizes, and the offset
 * of the entry's local header -- which we seek to, skip past, and
 * decompress / copy the data into place.
 */
#include "zip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "error.h"
#include "fs.h"
#include "sbuf.h"

#define EOCD_SIG 0x06054b50u
#define CDFH_SIG 0x02014b50u
#define LFH_SIG 0x04034b50u

#define EOCD_MIN_SIZE 22
#define EOCD_MAX_BACK 0xffffu /* max comment length */

static uint16_t rd16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/*
 * Find the EOCD record by scanning backwards from the end of the
 * buffer.  Returns the byte offset of the EOCD signature, or -1 if
 * not found.  The scan window is at most 64 KiB (the maximum comment
 * length) plus the EOCD header size.
 */
static ssize_t find_eocd(const unsigned char *buf, size_t len)
{
	size_t start;
	size_t stop;
	size_t max_back = (size_t)EOCD_MAX_BACK + EOCD_MIN_SIZE;

	if (len < EOCD_MIN_SIZE)
		return -1;
	start = len - EOCD_MIN_SIZE;
	stop = (len > max_back) ? (len - max_back) : 0;
	for (size_t off = start;; off--) {
		if (rd32(buf + off) == EOCD_SIG)
			return (ssize_t)off;
		if (off == stop)
			break;
	}
	return -1;
}

/*
 * Reject filenames that would escape the destination directory.
 * Allows normal forward-slash components but refuses "..", leading
 * "/", and embedded "/../" segments.  Backslashes and ':' (drive
 * prefixes) are also rejected: Windows fopen() honours '\\' as a
 * separator, so an entry named "..\\evil" must not slip past the
 * "/"-only scan.
 */
static int safe_path(const unsigned char *name, size_t len)
{
	size_t i = 0;

	if (len == 0)
		return 0;
	if (name[0] == '/')
		return 0;

	for (size_t k = 0; k < len; k++) {
		if (name[k] == '\\' || name[k] == ':')
			return 0;
	}

	while (i < len) {
		size_t j = i;
		while (j < len && name[j] != '/')
			j++;
		if (j == i + 2 && name[i] == '.' && name[i + 1] == '.')
			return 0;
		i = j + 1;
	}
	return 1;
}

/*
 * Copy @p src_len bytes from @p src into @p out_path, verifying the
 * running CRC-32 against @p expected_crc.  Creates parent directories
 * via mkdirp_for_file().
 */
static int extract_stored(const unsigned char *src, uint32_t src_len,
			  uint32_t expected_crc, const char *out_path)
{
	FILE *fp;
	uLong crc;

	if (mkdirp_for_file(out_path) < 0)
		return -1;
	fp = fopen(out_path, "wb");
	if (!fp)
		return -1;
	if (src_len && fwrite(src, 1, src_len, fp) != src_len) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	crc = crc32(0, Z_NULL, 0);
	crc = crc32(crc, src, src_len);
	return (uint32_t)crc == expected_crc ? 0 : -1;
}

/*
 * Decompress a raw DEFLATE stream of @p comp_len bytes into
 * @p out_path, verifying uncompressed size and CRC-32.
 */
static int extract_deflate(const unsigned char *src, uint32_t comp_len,
			   uint32_t uncomp_size, uint32_t expected_crc,
			   const char *out_path)
{
	z_stream zs;
	unsigned char out[8192];
	FILE *fp = NULL;
	uLong crc = crc32(0, Z_NULL, 0);
	uint32_t total = 0;
	int ret;
	int rc = -1;

	if (mkdirp_for_file(out_path) < 0)
		return -1;
	fp = fopen(out_path, "wb");
	if (!fp)
		return -1;

	memset(&zs, 0, sizeof(zs));
	if (inflateInit2(&zs, -15) != Z_OK)
		goto done;

	zs.next_in = (Bytef *)src;
	zs.avail_in = comp_len;

	do {
		zs.next_out = out;
		zs.avail_out = sizeof(out);
		ret = inflate(&zs, Z_NO_FLUSH);
		if (ret != Z_OK && ret != Z_STREAM_END) {
			inflateEnd(&zs);
			goto done;
		}
		{
			size_t n = sizeof(out) - zs.avail_out;
			if (n && fwrite(out, 1, n, fp) != n) {
				inflateEnd(&zs);
				goto done;
			}
			crc = crc32(crc, out, (uInt)n);
			total += (uint32_t)n;
		}
	} while (ret != Z_STREAM_END);

	inflateEnd(&zs);
	if (total == uncomp_size && (uint32_t)crc == expected_crc)
		rc = 0;
done:
	fclose(fp);
	return rc;
}

int zip_extract_all(const char *zip_path, const char *dest_dir)
{
	struct sbuf buf = SBUF_INIT;
	struct sbuf dst = SBUF_INIT;
	const unsigned char *data;
	size_t len;
	ssize_t eocd_off;
	const unsigned char *eocd;
	uint16_t total_entries;
	uint32_t cd_size;
	uint32_t cd_off;
	const unsigned char *cd;
	const unsigned char *cd_end;
	int rc = -1;

	if (sbuf_read_file(&buf, zip_path) < 0)
		goto out;

	data = (const unsigned char *)buf.buf;
	len = buf.len;

	eocd_off = find_eocd(data, len);
	if (eocd_off < 0)
		goto out;
	eocd = data + eocd_off;

	total_entries = rd16(eocd + 10);
	cd_size = rd32(eocd + 12);
	cd_off = rd32(eocd + 16);

	/* ZIP64 sentinel -- central directory truncated at 32 bits.
	 * Reject rather than misread. */
	if (total_entries == 0xffff || cd_size == 0xffffffffu ||
	    cd_off == 0xffffffffu)
		goto out;

	if ((size_t)cd_off > len || (size_t)cd_off + cd_size > len)
		goto out;

	if (mkdirp(dest_dir) < 0)
		goto out;

	cd = data + cd_off;
	cd_end = cd + cd_size;

	for (uint16_t i = 0; i < total_entries; i++) {
		uint16_t method, fname_len, extra_len, comment_len;
		uint32_t crc_expect, comp_size, uncomp_size, lfh_off;
		const unsigned char *fname;
		const unsigned char *lfh;
		uint16_t lfh_fname_len, lfh_extra_len;
		const unsigned char *payload;

		if (cd + 46 > cd_end || rd32(cd) != CDFH_SIG)
			goto out;

		method = rd16(cd + 10);
		crc_expect = rd32(cd + 16);
		comp_size = rd32(cd + 20);
		uncomp_size = rd32(cd + 24);
		fname_len = rd16(cd + 28);
		extra_len = rd16(cd + 30);
		comment_len = rd16(cd + 32);
		lfh_off = rd32(cd + 42);

		if (cd + 46 + fname_len + extra_len + comment_len > cd_end)
			goto out;
		fname = cd + 46;

		sbuf_reset(&dst);
		sbuf_addf(&dst, "%s/", dest_dir);
		sbuf_add(&dst, fname, fname_len);

		if (fname_len > 0 && fname[fname_len - 1] == '/') {
			/* Directory entry: create it, no data payload. */
			if (!safe_path(fname, fname_len - 1))
				goto out;
			/* Strip trailing slash so mkdirp sees a clean path. */
			if (dst.len && dst.buf[dst.len - 1] == '/')
				sbuf_setlen(&dst, dst.len - 1);
			if (mkdirp(dst.buf) < 0)
				goto out;
			goto advance;
		}

		if (!safe_path(fname, fname_len))
			goto out;

		/* Seek into the local header to find the data start. */
		if ((size_t)lfh_off + 30 > len)
			goto out;
		lfh = data + lfh_off;
		if (rd32(lfh) != LFH_SIG)
			goto out;
		lfh_fname_len = rd16(lfh + 26);
		lfh_extra_len = rd16(lfh + 28);
		payload = lfh + 30 + lfh_fname_len + lfh_extra_len;
		if (payload < data || payload + comp_size > data + len)
			goto out;

		if (method == 0) {
			if (comp_size != uncomp_size)
				goto out;
			if (extract_stored(payload, comp_size, crc_expect,
					   dst.buf) < 0)
				goto out;
		} else if (method == 8) {
			if (extract_deflate(payload, comp_size, uncomp_size,
					    crc_expect, dst.buf) < 0)
				goto out;
		} else {
			/* Unsupported compression method. */
			goto out;
		}

	advance:
		cd += 46 + fname_len + extra_len + comment_len;
	}

	rc = 0;
out:
	sbuf_release(&dst);
	sbuf_release(&buf);
	return rc;
}
