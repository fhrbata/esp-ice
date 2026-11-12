/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for the host-side eFuse decoding in
 * cmd/target/efuse/efuse.c: field values and protection bits are read
 * from an image, never from a chip, so a hand-built image covers them.
 */
#include "cmd/target/efuse/efuse.h"
#include "ice.h"
#include "tap.h"

static struct efuse_session session_with_blocks(void)
{
	struct efuse_session s;

	memset(&s, 0, sizeof(s));
	s.image.block_count = 3;
	for (uint8_t b = 0; b < 3; b++) {
		s.image.blocks[b].block = b;
		s.image.blocks[b].word_count = 8;
	}
	return s;
}

int main(void)
{
	/* A field inside one word: bits 4..11 of BLK1 word 0. */
	{
		struct efuse_session s = session_with_blocks();
		const esp_loader_efuse_desc_t d = {1, 4, 8};
		const esp_loader_efuse_desc_t *field[] = {&d, NULL};
		const esp_loader_efuse_field_info_t f = {
		    "F", field, ESP_LOADER_EFUSE_BIT_NONE,
		    ESP_LOADER_EFUSE_BIT_NONE};
		uint8_t v[1];

		s.image.blocks[1].words[0] = 0xa5u << 4;
		efuse_field_value(&s, &f, v);
		tap_check(efuse_field_bits(&f) == 8);
		tap_check(v[0] == 0xa5);
		tap_done("decodes a field inside one word");
	}

	/* A field across a word boundary: bits 28..35 of BLK2. */
	{
		struct efuse_session s = session_with_blocks();
		const esp_loader_efuse_desc_t d = {2, 28, 8};
		const esp_loader_efuse_desc_t *field[] = {&d, NULL};
		const esp_loader_efuse_field_info_t f = {
		    "F", field, ESP_LOADER_EFUSE_BIT_NONE,
		    ESP_LOADER_EFUSE_BIT_NONE};
		uint8_t v[1];

		s.image.blocks[2].words[0] = 0xcu << 28; /* low nibble */
		s.image.blocks[2].words[1] = 0x3u;	 /* high nibble */
		efuse_field_value(&s, &f, v);
		tap_check(v[0] == 0x3c);
		tap_done("decodes a field across a word boundary");
	}

	/* Several ranges concatenate LSB-first in table order. */
	{
		struct efuse_session s = session_with_blocks();
		const esp_loader_efuse_desc_t lo = {1, 0, 4};
		const esp_loader_efuse_desc_t hi = {2, 8, 4};
		const esp_loader_efuse_desc_t *field[] = {&lo, &hi, NULL};
		const esp_loader_efuse_field_info_t f = {
		    "F", field, ESP_LOADER_EFUSE_BIT_NONE,
		    ESP_LOADER_EFUSE_BIT_NONE};
		uint8_t v[1];

		s.image.blocks[1].words[0] = 0x9u;
		s.image.blocks[2].words[0] = 0x6u << 8;
		efuse_field_value(&s, &f, v);
		tap_check(efuse_field_bits(&f) == 8);
		tap_check(v[0] == 0x69);
		tap_done("concatenates multi-range fields LSB-first");
	}

	/* Protection bits are positions in BLK0. */
	{
		struct efuse_session s = session_with_blocks();
		const esp_loader_efuse_desc_t d = {1, 0, 1};
		const esp_loader_efuse_desc_t *field[] = {&d, NULL};
		const esp_loader_efuse_field_info_t f = {"F", field, 3, 33};
		const esp_loader_efuse_field_info_t none = {
		    "N", field, ESP_LOADER_EFUSE_BIT_NONE,
		    ESP_LOADER_EFUSE_BIT_NONE};

		tap_check(!efuse_field_write_protected(&s, &f));
		tap_check(!efuse_field_read_protected(&s, &f));
		s.image.blocks[0].words[0] = 1u << 3;
		s.image.blocks[0].words[1] = 1u << 1;
		tap_check(efuse_field_write_protected(&s, &f));
		tap_check(efuse_field_read_protected(&s, &f));
		tap_check(!efuse_field_write_protected(&s, &none));
		tap_check(!efuse_field_read_protected(&s, &none));
		tap_done("WR_DIS / RD_DIS bits come from BLK0");
	}

	/* Bits outside the image's blocks or words read as zero. */
	{
		struct efuse_session s = session_with_blocks();
		const esp_loader_efuse_desc_t d = {7, 0, 8};
		const esp_loader_efuse_desc_t *field[] = {&d, NULL};
		const esp_loader_efuse_field_info_t f = {
		    "F", field, ESP_LOADER_EFUSE_BIT_NONE,
		    ESP_LOADER_EFUSE_BIT_NONE};
		uint8_t v[1] = {0xff};

		efuse_field_value(&s, &f, v);
		tap_check(v[0] == 0);
		tap_done("missing blocks decode as zero");
	}

	return tap_result();
}
