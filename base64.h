/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file base64.h
 * @brief Base64 decoder.  Tolerates whitespace; stops at '=' padding.
 */
#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>

struct sbuf;

/*
 * Decode the base64 text in @p src (length @p len) into @p out.
 * Whitespace bytes (\n, \r, ' ', \t) are skipped, matching the
 * line-wrapping that PEM blocks and UART core-dump streams insert.
 * Decoding stops at the first '=' padding byte.  Returns 0 on
 * success, -1 if any non-base64, non-whitespace byte is encountered.
 */
int base64_decode(const char *src, size_t len, struct sbuf *out);

#endif /* BASE64_H */
