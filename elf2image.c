/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf2image.c
 * @brief ELF → ESP flash-image conversion engine.
 *
 * Ported from esptool's @c esptool/bin_image.py
 * (@c ESP32FirmwareImage.save()).  Chip identity, per-chip memory
 * maps, flash-parameter encoding, and image-format constants live
 * in @ref binary.h so @c ice @c image @c info can share them; this
 * file contains only the write-side machinery on top (PT_LOAD
 * collection, IROM_ALIGN layout with RAM gap-fill, image emission,
 * optional ELF-SHA256 patching).
 *
 * The goal is a functional, bootable image -- not necessarily
 * byte-identical to @c esptool @c elf2image output, because esptool
 * uses ELF sections by default while this engine uses PT_LOAD
 * program headers (a.k.a. esptool's @c --use-segments mode).  Both
 * forms are accepted by the ROM bootloader and produce the same
 * end-to-end behaviour.
 *
 * Known simplifications vs full esptool parity:
 *   - PT_LOAD segments only (no per-ELF-section granularity).
 *   - MMU page size fixed at 64 KB (BIN_IROM_ALIGN).
 *   - No bootloader-image specials (ram_only_header, secure_pad,
 *     bootdesc reordering).
 *   - No ESP8266 (different image format).
 *   - ELF-SHA256 patching requires an explicit offset; the app-desc
 *     auto-detect path is not implemented (IDF passes
 *     --elf-sha256-offset 0xb0 explicitly anyway).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "binary.h"
#include "elf2image.h"
#include "ice.h"
#include "vendor/sha256/sha256.h"

/* ------------------------------------------------------------------ */
/* LE helpers and zero-fill                                           */
/* ------------------------------------------------------------------ */

static void put_u8(struct sbuf *out, uint8_t v) { sbuf_addch(out, (int)v); }

static void put_le16(struct sbuf *out, uint16_t v)
{
	uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};

	sbuf_add(out, b, sizeof b);
}

static void put_le32(struct sbuf *out, uint32_t v)
{
	uint8_t b[4] = {
	    (uint8_t)v,
	    (uint8_t)(v >> 8),
	    (uint8_t)(v >> 16),
	    (uint8_t)(v >> 24),
	};

	sbuf_add(out, b, sizeof b);
}

static void put_zeros(struct sbuf *out, size_t n)
{
	sbuf_grow(out, n);
	memset(out->buf + out->len, 0, n);
	sbuf_setlen(out, out->len + n);
}

static bool is_flash_type(enum bin_seg_type t)
{
	return t == BIN_SEG_DROM || t == BIN_SEG_IROM;
}

/* ------------------------------------------------------------------ */
/* Internal per-image-segment descriptor                              */
/* ------------------------------------------------------------------ */

/*
 * Backed either by the original ELF PT_LOAD data (via @c data) or by
 * a heap-allocated zero-pad buffer (via @c owned).  Set @c owned so
 * layout() can free synthesised padding segments when we are done.
 */
struct img_seg {
	uint32_t vaddr;
	const uint8_t *data;
	uint32_t size;
	uint32_t padded_size; /* size rounded up to 4 bytes */
	bool is_flash;
	uint8_t *owned; /* NULL for ELF-backed; non-NULL for pads */
};

static void collect_loads(const void *elf_data, size_t elf_len,
			  enum bin_chip chip, struct img_seg **segs, size_t *n)
{
	struct elf_segments ph;
	size_t cap = 0;

	*segs = NULL;
	*n = 0;

	elf_read_segments(elf_data, elf_len, &ph);

	for (int i = 0; i < ph.nr; i++) {
		const struct elf_segment *p = &ph.s[i];

		if (p->type != 1) /* PT_LOAD */
			continue;
		if (p->filesz == 0)
			continue;

		if ((uint64_t)p->offset + p->filesz > elf_len)
			die("e2i: PT_LOAD segment extends past end of ELF");

		if (p->vaddr > UINT32_MAX)
			die("e2i: PT_LOAD vaddr > 32 bits (chip=%s)",
			    bin_chip_name(chip));

		enum bin_seg_type t = bin_classify(chip, (uint32_t)p->vaddr);

		if (t == BIN_SEG_UNKNOWN)
			die("e2i: PT_LOAD at vaddr 0x%08x is not mapped "
			    "by chip %s",
			    (unsigned)p->vaddr, bin_chip_name(chip));

		ALLOC_GROW(*segs, *n + 1, cap);
		struct img_seg *s = &(*segs)[(*n)++];

		s->vaddr = (uint32_t)p->vaddr;
		s->data = (const uint8_t *)elf_data + p->offset;
		s->size = (uint32_t)p->filesz;
		s->padded_size = (s->size + 3u) & ~3u;
		s->is_flash = is_flash_type(t);
		s->owned = NULL;

		if (*n > BIN_MAX_SEGS)
			die("e2i: too many PT_LOAD segments (%zu, max %u)", *n,
			    BIN_MAX_SEGS);
	}

	elf_segments_release(&ph);
}

/* ------------------------------------------------------------------ */
/* BIN_IROM_ALIGN placement                                           */
/* ------------------------------------------------------------------ */

/*
 * Required file position of the segment DATA (not header) for a
 * flash segment at @p vaddr:
 *
 *   data_pos % BIN_IROM_ALIGN == vaddr % BIN_IROM_ALIGN
 *
 * The segment header lives BIN_SEG_HDR_LEN bytes before the data.
 * Given a current file position @p cur and the target @p vaddr,
 * return how many bytes of padding (at the header level) must
 * precede the header so data alignment works out.
 */
static uint32_t pad_needed(uint32_t cur, uint32_t vaddr)
{
	uint32_t req_data_mod = vaddr % BIN_IROM_ALIGN;
	uint32_t req_hdr_mod =
	    (req_data_mod + BIN_IROM_ALIGN - BIN_SEG_HDR_LEN) % BIN_IROM_ALIGN;
	uint32_t cur_mod = cur % BIN_IROM_ALIGN;

	if (cur_mod <= req_hdr_mod)
		return req_hdr_mod - cur_mod;
	return BIN_IROM_ALIGN - cur_mod + req_hdr_mod;
}

/*
 * Insert zero-pad segments (or split a RAM segment) so each flash
 * segment's data lands on its required BIN_IROM_ALIGN offset.
 * Mirrors esptool bin_image.py:898-924.
 */
static void layout(struct img_seg **segs_io, size_t *n_io, uint32_t header_size)
{
	struct img_seg *segs = *segs_io;
	size_t n = *n_io;
	uint32_t pos = header_size;
	size_t ram_cursor = 0;

	struct img_seg *out = NULL;
	size_t out_n = 0;
	size_t out_cap = 0;

	while (ram_cursor < n && segs[ram_cursor].is_flash)
		ram_cursor++;

	for (size_t i = 0; i < n; i++) {
		if (!segs[i].is_flash)
			continue;

		uint32_t gap = pad_needed(pos, segs[i].vaddr);

		while (gap > 0) {
			while (ram_cursor < n && segs[ram_cursor].is_flash)
				ram_cursor++;

			if (ram_cursor < n && gap > BIN_SEG_HDR_LEN) {
				struct img_seg *src = &segs[ram_cursor];
				uint32_t avail = src->padded_size;
				uint32_t cap_data = gap - BIN_SEG_HDR_LEN;

				if (avail <= cap_data) {
					ALLOC_GROW(out, out_n + 1, out_cap);
					out[out_n++] = *src;
					pos += BIN_SEG_HDR_LEN + avail;
					gap -= BIN_SEG_HDR_LEN + avail;
					ram_cursor++;
				} else {
					struct img_seg head = *src;

					head.size = cap_data;
					head.padded_size = cap_data;
					ALLOC_GROW(out, out_n + 1, out_cap);
					out[out_n++] = head;
					src->data += cap_data;
					src->vaddr += cap_data;
					src->size -= cap_data;
					src->padded_size -= cap_data;
					pos += BIN_SEG_HDR_LEN + cap_data;
					gap -= BIN_SEG_HDR_LEN + cap_data;
				}
			} else {
				uint32_t pad_len = (gap < BIN_SEG_HDR_LEN)
						       ? 0u
						       : gap - BIN_SEG_HDR_LEN;
				struct img_seg pad = {0};

				pad.vaddr = 0;
				pad.size = pad_len;
				pad.padded_size = pad_len;
				pad.is_flash = false;
				if (pad_len > 0) {
					pad.owned = malloc(pad_len);
					if (pad.owned == NULL)
						die_errno("malloc(%u)",
							  pad_len);
					memset(pad.owned, 0, pad_len);
					pad.data = pad.owned;
				} else {
					pad.data = NULL;
				}
				ALLOC_GROW(out, out_n + 1, out_cap);
				out[out_n++] = pad;
				pos += BIN_SEG_HDR_LEN + pad_len;
				gap = 0;
			}
		}

		ALLOC_GROW(out, out_n + 1, out_cap);
		out[out_n++] = segs[i];
		pos += BIN_SEG_HDR_LEN + segs[i].padded_size;
	}

	while (ram_cursor < n) {
		if (segs[ram_cursor].is_flash) {
			ram_cursor++;
			continue;
		}
		ALLOC_GROW(out, out_n + 1, out_cap);
		out[out_n++] = segs[ram_cursor++];
	}

	if (out_n > BIN_MAX_SEGS)
		die("e2i: layout produced %zu image segments (max %u); "
		    "too many flash regions for this chip",
		    out_n, BIN_MAX_SEGS);

	free(segs);
	*segs_io = out;
	*n_io = out_n;
}

/* ------------------------------------------------------------------ */
/* Write pass + checksum + digest                                     */
/* ------------------------------------------------------------------ */

static void write_segs(struct sbuf *out, struct img_seg *segs, size_t n,
		       uint32_t *checksum_io, const uint8_t *elf_hash,
		       uint32_t elf_sha256_offset)
{
	uint32_t cksum = *checksum_io;

	for (size_t i = 0; i < n; i++) {
		struct img_seg *s = &segs[i];
		uint32_t start_off = (uint32_t)out->len;

		put_le32(out, s->vaddr);
		put_le32(out, s->padded_size);

		if (s->size > 0)
			sbuf_add(out, s->data, s->size);
		if (s->padded_size > s->size)
			put_zeros(out, s->padded_size - s->size);

		/* Patch app_elf_sha256 in-place if the 32-byte field
		 * lives inside this segment's data. */
		if (elf_sha256_offset != 0 && elf_hash != NULL) {
			uint32_t data_start = start_off + BIN_SEG_HDR_LEN;
			uint32_t data_end = data_start + s->padded_size;

			if (elf_sha256_offset + BIN_DIGEST_LEN <= data_end &&
			    elf_sha256_offset >= data_start) {
				memcpy(out->buf + elf_sha256_offset, elf_hash,
				       BIN_DIGEST_LEN);
			}
		}

		/* XOR over this segment's data (post-patch). */
		for (uint32_t j = 0; j < s->padded_size; j++)
			cksum ^=
			    (uint8_t)out->buf[start_off + BIN_SEG_HDR_LEN + j];
	}

	*checksum_io = cksum;
}

static void sha256_buf(const uint8_t *data, size_t n, uint8_t *hash)
{
	SHA256_CTX ctx;

	sha256_init(&ctx);
	sha256_update(&ctx, data, n);
	sha256_final(&ctx, hash);
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                 */
/* ------------------------------------------------------------------ */

void e2i_build(const void *elf, size_t elf_len, enum bin_chip chip,
	       const struct e2i_config *cfg, struct sbuf *out)
{
	if ((unsigned)chip >= (unsigned)BIN_CHIP_MAX)
		die("e2i: invalid chip (%d)", (int)chip);
	if (cfg == NULL)
		die("e2i: config must not be NULL");

	/* Validate flash params up front so the error message comes
	 * before any output is produced. */
	uint8_t fm_byte = bin_flash_mode_byte(cfg->flash_mode);
	uint8_t fs_byte = bin_flash_size_byte(cfg->flash_size);
	uint8_t ff_byte = bin_flash_freq_byte(chip, cfg->flash_freq);

	/* Read entry point from ELF Ehdr and collect PT_LOAD segments. */
	struct elf_segments ph;

	elf_read_segments(elf, elf_len, &ph);
	uint32_t entry = (uint32_t)ph.entry;

	elf_segments_release(&ph);

	struct img_seg *segs;
	size_t n_segs;

	collect_loads(elf, elf_len, chip, &segs, &n_segs);
	if (n_segs == 0)
		die("e2i: ELF has no PT_LOAD segments with data");

	uint8_t elf_hash[BIN_DIGEST_LEN];
	bool want_elf_patch = cfg->elf_sha256_offset != 0;

	if (want_elf_patch)
		sha256_buf(elf, elf_len, elf_hash);

	sbuf_reset(out);

	uint32_t header_size = BIN_HDR_LEN + BIN_EXT_HDR_LEN;

	layout(&segs, &n_segs, header_size);

	/* --- Common header (8 bytes) --- */
	put_u8(out, BIN_IMAGE_MAGIC);
	put_u8(out, (uint8_t)n_segs);
	put_u8(out, fm_byte);
	put_u8(out, (uint8_t)(fs_byte | ff_byte));
	put_le32(out, entry);

	/* --- Extended header (16 bytes) --- */
	put_u8(out, 0xEE); /* WP pin (disabled) */
	put_u8(out, 0);	   /* SPI drv: clk/q */
	put_u8(out, 0);	   /* SPI drv: d/cs */
	put_u8(out, 0);	   /* SPI drv: hd/wp */
	put_le16(out, bin_chip_id(chip));
	put_u8(out, 0); /* min_rev (legacy byte) */
	put_le16(out, cfg->min_rev_full);
	put_le16(out, cfg->max_rev_full);
	put_zeros(out, 4); /* reserved */
	put_u8(out, cfg->append_sha256 ? 1u : 0u);

	/* --- Segments + running checksum --- */
	uint32_t cksum = BIN_CHECKSUM_MAGIC;

	write_segs(out, segs, n_segs, &cksum, want_elf_patch ? elf_hash : NULL,
		   cfg->elf_sha256_offset);

	/*
	 * Checksum trailer: pad zeros until the next byte position is
	 * 15 mod 16, then write the 1-byte XOR checksum.  Result:
	 * out->len % 16 == 0 after.
	 */
	size_t pad = (size_t)(16u - ((out->len + 1u) % 16u)) % 16u;

	put_zeros(out, pad);
	put_u8(out, (uint8_t)cksum);

	/* --- Optional SHA-256 digest over everything so far --- */
	if (cfg->append_sha256) {
		uint8_t image_hash[BIN_DIGEST_LEN];

		sha256_buf((const uint8_t *)out->buf, out->len, image_hash);
		sbuf_add(out, image_hash, sizeof image_hash);
	}

	for (size_t i = 0; i < n_segs; i++)
		free(segs[i].owned);
	free(segs);
}
