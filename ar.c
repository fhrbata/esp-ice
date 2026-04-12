/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ar.c
 * @brief AR (static library) archive reader implementation.
 *
 * Parses the Unix AR format directly from a memory buffer.
 * Handles GNU long-name string tables (//) and BSD extended
 * names (#1/len).  Symbol tables and other special members are
 * silently skipped.
 *
 * Format reference: see ar(5) and the GNU binutils source.
 *
 * Archive layout:
 *   "!<arch>\n"                        global header (8 bytes)
 *   [ member header (60 bytes) ]       per-member header
 *   [ member data (size bytes)  ]      file contents
 *   [ padding byte if size is odd ]    2-byte alignment
 *   ...
 *
 * Member header fields (all ASCII, space-padded):
 *   ar_name[16]  ar_date[12]  ar_uid[6]  ar_gid[6]
 *   ar_mode[8]   ar_size[10]  ar_fmag[2] = "`\n"
 */
#include "ice.h"

#define AR_MAGIC     "!<arch>\n"
#define AR_MAGIC_LEN 8
#define AR_HDR_LEN   60
#define AR_FMAG      "`\n"

/**
 * Parse a space-padded decimal number from a fixed-width field.
 */
static size_t parse_ar_decimal(const char *s, int width)
{
	size_t v = 0;
	int i;

	for (i = 0; i < width && s[i] >= '0' && s[i] <= '9'; i++)
		v = v * 10 + (s[i] - '0');
	return v;
}

void ar_reader_init(struct ar_reader *r, const void *buf, size_t len)
{
	if (len < AR_MAGIC_LEN ||
	    memcmp(buf, AR_MAGIC, AR_MAGIC_LEN) != 0)
		die("not an AR archive (bad magic)");

	r->buf = buf;
	r->len = len;
	r->pos = AR_MAGIC_LEN;
	r->strtab = NULL;
	r->strtab_len = 0;
}

int ar_reader_next(struct ar_reader *r, struct ar_member *m)
{
	const char *hdr;
	const unsigned char *data;
	size_t size;

	for (;;) {
		/* Align to 2-byte boundary (AR padding). */
		if (r->pos & 1)
			r->pos++;

		if (r->pos >= r->len)
			return 0;

		if (r->pos + AR_HDR_LEN > r->len)
			die("AR archive truncated "
			    "(incomplete header at offset %zu)", r->pos);

		hdr = (const char *)(r->buf + r->pos);

		if (memcmp(hdr + 58, AR_FMAG, 2) != 0)
			die("AR member at offset %zu: "
			    "bad header magic", r->pos);

		size = parse_ar_decimal(hdr + 48, 10);
		data = r->buf + r->pos + AR_HDR_LEN;

		if (r->pos + AR_HDR_LEN + size > r->len)
			die("AR member at offset %zu: "
			    "data extends past end of archive", r->pos);

		r->pos += AR_HDR_LEN + size;

		/*
		 * Handle special members that start with '/'.
		 * These are never returned to the caller.
		 */
		if (hdr[0] == '/') {
			if (hdr[1] == '/' && hdr[2] == ' ') {
				/* GNU long-name string table. */
				r->strtab = (const char *)data;
				r->strtab_len = size;
				continue;
			}
			if (hdr[1] >= '0' && hdr[1] <= '9') {
				/* GNU long name: /offset -- fall through to name resolution. */
				break;
			}
			/*
			 * Symbol table ("/"), 64-bit symbol table
			 * ("/SYM64/"), or other special member -- skip.
			 */
			continue;
		}

		/* BSD long name: #1/len */
		if (hdr[0] == '#' && hdr[1] == '1' && hdr[2] == '/') {
			size_t name_len = parse_ar_decimal(hdr + 3, 13);
			const char *nul;

			if (name_len > size)
				die("AR: BSD name length %zu "
				    "exceeds member size %zu",
				    name_len, size);

			/* BSD names may be NUL-padded; find actual end. */
			nul = memchr(data, '\0', name_len);
			if (nul)
				name_len = (size_t)(nul - (const char *)data);

			m->name = sbuf_strndup((const char *)data, name_len);
			m->data = data + name_len;
			m->size = size - (size_t)((const unsigned char *)m->data - data);
			return 1;
		}

		/* Short name: terminated by '/' or end of field. */
		break;
	}

	/* Resolve member name. */
	if (hdr[0] == '/' && hdr[1] >= '0' && hdr[1] <= '9') {
		/* GNU long name: /offset into string table. */
		size_t off = parse_ar_decimal(hdr + 1, 15);
		const char *s, *end;

		if (!r->strtab || off >= r->strtab_len)
			die("AR: long name offset %zu "
			    "exceeds string table", off);

		s = r->strtab + off;
		end = memchr(s, '/', r->strtab_len - off);
		if (!end)
			end = r->strtab + r->strtab_len;
		m->name = sbuf_strndup(s, (size_t)(end - s));
	} else {
		/* Short name: find '/' terminator or trim trailing spaces. */
		const char *slash = memchr(hdr, '/', 16);
		int name_len;

		if (slash)
			name_len = (int)(slash - hdr);
		else
			name_len = 16;

		while (name_len > 0 && hdr[name_len - 1] == ' ')
			name_len--;

		m->name = sbuf_strndup(hdr, (size_t)name_len);
	}

	m->data = data;
	m->size = size;
	return 1;
}
