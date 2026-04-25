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
 * Two collection modes are supported, matching esptool:
 *
 *   - @b sections (default) walks the ELF section headers and keeps
 *     SHT_PROGBITS / SHT_INIT_ARRAY / SHT_FINI_ARRAY /
 *     SHT_PREINIT_ARRAY entries with sh_addr != 0 and sh_size != 0.
 *     SHT_NOBITS sections (.bss, leading .dram0.dummy padding) are
 *     naturally excluded.  Adjacent same-class sections are merged
 *     before layout.  Equivalent to esptool's default behaviour.
 *
 *   - @b segments walks PT_LOAD program headers (esptool's
 *     @c --use-segments mode).  IDF linker scripts coalesce a
 *     leading NOBITS @c .dram0.dummy with the real DRAM data into
 *     one PT_LOAD whose @c p_filesz includes the dummy padding;
 *     the section table is consulted to trim the leading/trailing
 *     NOBITS ranges and emit only the PROGBITS sub-range.
 *
 * Layout, BIN_IROM_ALIGN flash placement, ESP32-specific 0x24-byte
 * trailing pad, @c .flash.appdesc front-loading, checksum and
 * digest emission all match esptool exactly so the resulting bin is
 * byte-identical for IDF inputs.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "binary.h"
#include "elf2image.h"
#include "ice.h"
#include "vendor/sha256/sha256.h"

/* ELF section type values from the ELF spec. */
#define SHT_PROGBITS 1
#define SHT_NOBITS 8
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_PREINIT_ARRAY 16

/* esptool keeps these section types as image content (NOBITS goes to
 * a separate list it does not emit).  We match the same set. */
static bool is_progbits_loadable(uint32_t sh_type)
{
	return sh_type == SHT_PROGBITS || sh_type == SHT_INIT_ARRAY ||
	       sh_type == SHT_FINI_ARRAY || sh_type == SHT_PREINIT_ARRAY;
}

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
 * Backed either by the original ELF data (via @c data) or by a
 * heap-allocated zero-pad buffer (via @c owned).  Set @c owned so
 * layout() can free synthesised padding segments when we are done.
 *
 * @c name points into the ELF section header string table for
 * section-mode collection; NULL for segment-mode and synthetic pads.
 */
struct img_seg {
	uint32_t vaddr;
	const uint8_t *data;
	uint32_t size;
	uint32_t padded_size; /* size rounded up to 4 bytes */
	bool is_flash;
	uint8_t *owned; /* NULL for ELF-backed; non-NULL for pads */
	const char *name;
};

/* ------------------------------------------------------------------ */
/* Collection: sections (default) and segments (--use-segments)       */
/* ------------------------------------------------------------------ */

static void append_seg(struct img_seg **segs, size_t *n, size_t *cap,
		       struct img_seg s)
{
	ALLOC_GROW(*segs, *n + 1, *cap);
	(*segs)[(*n)++] = s;
}

/*
 * esptool _read_sections: include sh_type in {PROGBITS, *_ARRAY},
 * skip sh_addr == 0 or sh_size == 0.  No SHF_ALLOC check -- the
 * sh_addr == 0 filter is what excludes .debug_*, .symtab, .strtab,
 * etc. on ESP since real load addresses never start at 0.
 */
static void collect_sections(const void *elf_data, size_t elf_len,
			     enum bin_chip chip, struct img_seg **segs,
			     size_t *n_io)
{
	struct elf_sections sec;
	size_t cap = 0;

	*segs = NULL;
	*n_io = 0;

	elf_read_sections(elf_data, elf_len, &sec);

	for (int i = 0; i < sec.nr; i++) {
		const struct elf_section *s = &sec.s[i];

		if (!is_progbits_loadable(s->type))
			continue;
		if (s->addr == 0 || s->size == 0)
			continue;
		if (s->offset + s->size > elf_len)
			die("e2i: section '%s' extends past end of ELF",
			    s->name);
		if (s->addr > UINT32_MAX || s->size > UINT32_MAX)
			die("e2i: section '%s' size/addr > 32 bits", s->name);

		enum bin_seg_type t = bin_classify(chip, (uint32_t)s->addr);

		if (t == BIN_SEG_UNKNOWN)
			die("e2i: section '%s' at 0x%08x is not mapped "
			    "by chip %s",
			    s->name, (unsigned)s->addr, bin_chip_name(chip));

		struct img_seg im = {
		    .vaddr = (uint32_t)s->addr,
		    .data = (const uint8_t *)elf_data + s->offset,
		    .size = (uint32_t)s->size,
		    .padded_size = ((uint32_t)s->size + 3u) & ~3u,
		    .is_flash = is_flash_type(t),
		    .owned = NULL,
		    .name = s->name,
		};

		append_seg(segs, n_io, &cap, im);
	}

	elf_sections_release(&sec);
}

/*
 * Within a PT_LOAD covering [seg_addr, seg_addr+seg_filesz), find the
 * lowest and highest PROGBITS-class section so we can trim leading
 * NOBITS padding (e.g. .dram0.dummy) that the linker materialises as
 * zero bytes inside p_filesz.  If no PROGBITS sections fall inside,
 * out_addr is set to 0 and out_size to 0 -- caller drops the segment.
 */
static void progbits_extent(const struct elf_sections *secs, uint64_t seg_addr,
			    uint64_t seg_filesz, uint64_t *out_addr,
			    uint64_t *out_offset, uint64_t *out_size)
{
	const struct elf_section *first = NULL, *last = NULL;
	uint64_t seg_end = seg_addr + seg_filesz;

	for (int i = 0; i < secs->nr; i++) {
		const struct elf_section *s = &secs->s[i];

		if (!is_progbits_loadable(s->type))
			continue;
		if (s->size == 0)
			continue;
		if (s->addr < seg_addr || s->addr + s->size > seg_end)
			continue;

		if (first == NULL || s->addr < first->addr)
			first = s;
		if (last == NULL || s->addr + s->size > last->addr + last->size)
			last = s;
	}

	if (first == NULL) {
		*out_addr = 0;
		*out_offset = 0;
		*out_size = 0;
		return;
	}

	*out_addr = first->addr;
	*out_offset = first->offset;
	*out_size = (last->addr + last->size) - first->addr;
}

/*
 * esptool _read_segments: walk PT_LOADs, keep p_paddr != 0 and
 * p_filesz > 0.  Note esptool uses p_paddr (LMA), not p_vaddr;
 * IDF builds set them equal, but other toolchains may not.
 *
 * Then, if the ELF carries a section table, intersect each PT_LOAD
 * with PROGBITS sections to trim NOBITS sub-ranges.  Without a
 * section table (synthetic test ELFs) the PT_LOAD is taken whole.
 */
static void collect_segments(const void *elf_data, size_t elf_len,
			     enum bin_chip chip, struct img_seg **segs,
			     size_t *n_io)
{
	struct elf_segments ph;
	struct elf_sections sec;
	size_t cap = 0;

	*segs = NULL;
	*n_io = 0;

	elf_read_segments(elf_data, elf_len, &ph);
	elf_read_sections(elf_data, elf_len, &sec);

	for (int i = 0; i < ph.nr; i++) {
		const struct elf_segment *p = &ph.s[i];

		if (p->type != 1) /* PT_LOAD */
			continue;
		if (p->paddr == 0)
			continue;
		if (p->filesz == 0)
			continue;
		if ((uint64_t)p->offset + p->filesz > elf_len)
			die("e2i: PT_LOAD segment extends past end of ELF");

		uint64_t addr = p->paddr;
		uint64_t offset = p->offset;
		uint64_t size = p->filesz;

		if (sec.nr > 0) {
			uint64_t a, off, sz;

			progbits_extent(&sec, p->paddr, p->filesz, &a, &off,
					&sz);
			if (sz == 0)
				continue;
			addr = a;
			offset = off;
			size = sz;
		}

		if (addr > UINT32_MAX)
			die("e2i: PT_LOAD addr > 32 bits (chip=%s)",
			    bin_chip_name(chip));

		enum bin_seg_type t = bin_classify(chip, (uint32_t)addr);

		if (t == BIN_SEG_UNKNOWN)
			die("e2i: PT_LOAD at addr 0x%08x is not mapped "
			    "by chip %s",
			    (unsigned)addr, bin_chip_name(chip));

		struct img_seg im = {
		    .vaddr = (uint32_t)addr,
		    .data = (const uint8_t *)elf_data + offset,
		    .size = (uint32_t)size,
		    .padded_size = ((uint32_t)size + 3u) & ~3u,
		    .is_flash = is_flash_type(t),
		    .owned = NULL,
		    .name = NULL,
		};

		append_seg(segs, n_io, &cap, im);
	}

	elf_segments_release(&ph);
	elf_sections_release(&sec);
}

/* ------------------------------------------------------------------ */
/* Sort, merge, reorder: match esptool ordering for byte equality     */
/* ------------------------------------------------------------------ */

/*
 * Stable insertion sort by vaddr.  Lists are tiny (< 20 segments
 * for IDF builds), so O(n^2) is fine and avoids pulling in qsort
 * comparator boilerplate.
 */
static void sort_by_vaddr(struct img_seg *segs, size_t n)
{
	for (size_t i = 1; i < n; i++) {
		struct img_seg key = segs[i];
		size_t j = i;

		while (j > 0 && segs[j - 1].vaddr > key.vaddr) {
			segs[j] = segs[j - 1];
			j--;
		}
		segs[j] = key;
	}
}

/*
 * esptool merge_adjacent_segments: collapse same-class entries that
 * are contiguous in vaddr (and, here, also contiguous in the source
 * file -- if not, we can't memcpy them as one block).  Names follow
 * the first segment, mirroring esptool's "elem.data += next.data"
 * which keeps elem's name field.
 */
static void merge_adjacent(struct img_seg *segs, size_t *n_io)
{
	size_t n = *n_io;

	if (n < 2)
		return;

	size_t out = 0;

	for (size_t i = 1; i < n; i++) {
		struct img_seg *prev = &segs[out];
		struct img_seg *cur = &segs[i];

		bool can_merge = prev->is_flash == cur->is_flash &&
				 prev->vaddr + prev->size == cur->vaddr &&
				 prev->owned == NULL && cur->owned == NULL &&
				 prev->data + prev->size == cur->data;

		if (can_merge) {
			prev->size += cur->size;
			prev->padded_size = (prev->size + 3u) & ~3u;
		} else {
			out++;
			segs[out] = *cur;
		}
	}

	*n_io = out + 1;
}

/*
 * esptool moves any segment whose name contains ".flash.appdesc"
 * to the front of flash_segments after sorting.  For typical IDF
 * builds the appdesc is already at the lowest flash address, so this
 * is a no-op; we mirror the logic for parity with esptool on
 * unusual link configurations.
 */
static bool is_appdesc(const struct img_seg *s)
{
	return s->name && strstr(s->name, ".flash.appdesc") != NULL;
}

static void appdesc_to_front(struct img_seg *segs, size_t n)
{
	size_t first_flash = SIZE_MAX;

	for (size_t i = 0; i < n; i++) {
		if (segs[i].is_flash) {
			first_flash = i;
			break;
		}
	}
	if (first_flash == SIZE_MAX)
		return;

	size_t hit = SIZE_MAX;

	for (size_t i = first_flash; i < n; i++) {
		if (segs[i].is_flash && is_appdesc(&segs[i])) {
			hit = i;
			break;
		}
	}
	if (hit == SIZE_MAX || hit == first_flash)
		return;

	struct img_seg tmp = segs[hit];

	for (size_t i = hit; i > first_flash; i--)
		segs[i] = segs[i - 1];
	segs[first_flash] = tmp;
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

/*
 * ESP32-only: the IDF 2nd-stage bootloader fails to map the final MMU
 * page if a flash segment ends within the first 0x24 bytes of an
 * IROM_ALIGN page.  esptool save_flash_segment pads the data out to
 * byte 0x24 of the page in that case; we replicate the same fix-up
 * before writing the segment header so the size field is accurate.
 */
static uint32_t esp32_flash_pad(enum bin_chip chip, bool is_flash,
				uint32_t pos_after_header_and_data)
{
	if (chip != BIN_CHIP_ESP32 || !is_flash)
		return 0;

	uint32_t rem = pos_after_header_and_data % BIN_IROM_ALIGN;

	if (rem >= 0x24)
		return 0;
	return 0x24 - rem;
}

static void write_segs(struct sbuf *out, struct img_seg *segs, size_t n,
		       enum bin_chip chip, uint32_t *checksum_io,
		       const uint8_t *elf_hash, uint32_t elf_sha256_offset)
{
	uint32_t cksum = *checksum_io;

	for (size_t i = 0; i < n; i++) {
		struct img_seg *s = &segs[i];
		uint32_t start_off = (uint32_t)out->len;
		uint32_t pos_end = start_off + BIN_SEG_HDR_LEN + s->padded_size;
		uint32_t extra = esp32_flash_pad(chip, s->is_flash, pos_end);
		uint32_t total_size = s->padded_size + extra;

		put_le32(out, s->vaddr);
		put_le32(out, total_size);

		if (s->size > 0)
			sbuf_add(out, s->data, s->size);
		if (s->padded_size > s->size)
			put_zeros(out, s->padded_size - s->size);
		if (extra > 0)
			put_zeros(out, extra);

		/* Patch app_elf_sha256 in-place if the 32-byte field
		 * lives inside this segment's data. */
		if (elf_sha256_offset != 0 && elf_hash != NULL) {
			uint32_t data_start = start_off + BIN_SEG_HDR_LEN;
			uint32_t data_end = data_start + total_size;

			if (elf_sha256_offset + BIN_DIGEST_LEN <= data_end &&
			    elf_sha256_offset >= data_start) {
				memcpy(out->buf + elf_sha256_offset, elf_hash,
				       BIN_DIGEST_LEN);
			}
		}

		/* XOR over this segment's data (post-patch). */
		for (uint32_t j = 0; j < total_size; j++)
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

	/* Read entry point from ELF Ehdr. */
	struct elf_segments ph;

	elf_read_segments(elf, elf_len, &ph);
	uint32_t entry = (uint32_t)ph.entry;

	elf_segments_release(&ph);

	struct img_seg *segs = NULL;
	size_t n_segs = 0;

	if (cfg->use_segments) {
		collect_segments(elf, elf_len, chip, &segs, &n_segs);
	} else {
		collect_sections(elf, elf_len, chip, &segs, &n_segs);
		/* Fall back to PT_LOAD walk if the ELF has no usable
		 * sections (e.g. synthetic tests, or a stripped ELF). */
		if (n_segs == 0) {
			free(segs);
			collect_segments(elf, elf_len, chip, &segs, &n_segs);
		}
	}

	if (n_segs == 0)
		die("e2i: ELF has no loadable PROGBITS sections or "
		    "PT_LOAD segments");

	sort_by_vaddr(segs, n_segs);
	merge_adjacent(segs, &n_segs);
	appdesc_to_front(segs, n_segs);

	if (n_segs > BIN_MAX_SEGS)
		die("e2i: %zu image segments before layout (max %u); "
		    "linker script likely produces too many regions",
		    n_segs, BIN_MAX_SEGS);

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

	write_segs(out, segs, n_segs, chip, &cksum,
		   want_elf_patch ? elf_hash : NULL, cfg->elf_sha256_offset);

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
