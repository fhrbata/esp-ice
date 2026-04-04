/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sbuf.c
 * @brief Dynamic NUL-terminated string buffer implementation.
 */
#include "ice.h"

char sbuf_empty[] = "";

void sbuf_init(struct sbuf *sb)
{
	sb->buf = sbuf_empty;
	sb->len = 0;
	sb->alloc = 0;
}

void sbuf_release(struct sbuf *sb)
{
	if (sb->alloc)
		free(sb->buf);
	sbuf_init(sb);
}

char *sbuf_detach(struct sbuf *sb)
{
	char *buf;

	if (!sb->alloc) {
		size_t len = sb->len + 1;
		buf = malloc(len);
		if (!buf)
			die_errno("malloc");
		memcpy(buf, sb->buf, len);
	} else {
		buf = sb->buf;
	}

	sbuf_init(sb);
	return buf;
}

void sbuf_grow(struct sbuf *sb, size_t extra)
{
	size_t need = sb->len + extra + 1; /* +1 for NUL */

	if (need <= sb->alloc)
		return;

	size_t newalloc = sb->alloc ? sb->alloc * 2 : 64;
	while (newalloc < need)
		newalloc *= 2;

	char *newbuf = malloc(newalloc);
	if (!newbuf)
		die_errno("malloc(%zu)", newalloc);

	if (sb->len)
		memcpy(newbuf, sb->buf, sb->len);
	newbuf[sb->len] = '\0';

	if (sb->alloc)
		free(sb->buf);

	sb->buf = newbuf;
	sb->alloc = newalloc;
}

void sbuf_add(struct sbuf *sb, const void *data, size_t len)
{
	sbuf_grow(sb, len);
	memcpy(sb->buf + sb->len, data, len);
	sb->len += len;
	sb->buf[sb->len] = '\0';
}

void sbuf_addstr(struct sbuf *sb, const char *s)
{
	sbuf_add(sb, s, strlen(s));
}

void sbuf_vaddf(struct sbuf *sb, const char *fmt, va_list ap)
{
	va_list ap2;
	int n;

	va_copy(ap2, ap);
	n = vsnprintf(sb->buf + sb->len, sb->alloc - sb->len, fmt, ap2);
	va_end(ap2);

	if (n < 0)
		die("vsnprintf failed");

	if ((size_t)n < sb->alloc - sb->len) {
		sb->len += n;
		return;
	}

	sbuf_grow(sb, n);
	n = vsnprintf(sb->buf + sb->len, sb->alloc - sb->len, fmt, ap);
	if (n < 0)
		die("vsnprintf failed");
	sb->len += n;
}

void sbuf_addf(struct sbuf *sb, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	sbuf_vaddf(sb, fmt, ap);
	va_end(ap);
}

ssize_t sbuf_read_file(struct sbuf *sb, const char *path)
{
	FILE *fp;
	long size;

	fp = fopen(path, "rb");
	if (!fp)
		return -1;

	if (fseek(fp, 0, SEEK_END)) {
		fclose(fp);
		return -1;
	}

	size = ftell(fp);
	if (size < 0 || fseek(fp, 0, SEEK_SET)) {
		fclose(fp);
		return -1;
	}

	sbuf_grow(sb, size);

	if (fread(sb->buf + sb->len, 1, size, fp) != (size_t)size) {
		fclose(fp);
		return -1;
	}

	sb->len += size;
	sb->buf[sb->len] = '\0';
	fclose(fp);
	return size;
}

void sbuf_rtrim(struct sbuf *sb)
{
	while (sb->len && isspace((unsigned char)sb->buf[sb->len - 1]))
		sb->len--;
	sb->buf[sb->len] = '\0';
}
