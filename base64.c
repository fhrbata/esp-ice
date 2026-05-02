/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file base64.c
 * @brief Base64 decoder.  Tolerates whitespace; stops at '=' padding.
 */
#include "base64.h"
#include "ice.h"

#include <stdint.h>
#include <string.h>

int base64_decode(const char *src, size_t len, struct sbuf *out)
{
	static int8_t tab[256];
	static int init;

	if (!init) {
		memset(tab, -1, sizeof(tab));
		for (int i = 0; i < 26; i++) {
			tab['A' + i] = (int8_t)i;
			tab['a' + i] = (int8_t)(26 + i);
		}
		for (int i = 0; i < 10; i++)
			tab['0' + i] = (int8_t)(52 + i);
		tab[(unsigned char)'+'] = 62;
		tab[(unsigned char)'/'] = 63;
		init = 1;
	}

	uint32_t buf = 0;
	int bits = 0;

	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)src[i];
		if (c == '=')
			break;
		if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
			continue;
		int8_t v = tab[c];
		if (v < 0)
			return -1;
		buf = (buf << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			sbuf_addch(out, (char)((buf >> bits) & 0xFF));
		}
	}
	return 0;
}
