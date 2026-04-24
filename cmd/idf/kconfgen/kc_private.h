/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_private.h
 * @brief Internal helpers shared across the kconfgen I/O split.
 *
 * Not part of the public kc_io / kc_ast / kc_eval surface -- callers
 * outside @c cmd/idf/kconfgen/ must not include this file.  The
 * declarations live here so the post-split writer, reader, and
 * rename translator can share a single definition without exposing
 * internals to the rest of the tree.
 */
#ifndef KC_PRIVATE_H
#define KC_PRIVATE_H

struct kc_ctx;

#define KC_CONFIG_PREFIX "CONFIG_"
#define KC_CONFIG_PREFIX_LEN (sizeof(KC_CONFIG_PREFIX) - 1)

/**
 * @brief Apply a rename entry (if any) to the @p name_inout /
 *        @p val_inout pair coming from an sdkconfig line.
 *
 * On a hit, reallocates @c *name_inout to the canonical (post-rename)
 * name and flips the bool value when the matching entry carries
 * @c invert.  Caller owns the memory on both sides of the swap.
 * Returns 1 when a rename applied, 0 otherwise.
 */
int kc_rename_translate(const struct kc_ctx *ctx, char **name_inout,
			char **val_inout);

#endif /* KC_PRIVATE_H */
