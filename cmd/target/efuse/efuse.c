/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/efuse/efuse.c
 * @brief `ice target efuse` -- namespace dispatcher and shared helpers.
 */
#include "cmd/target/efuse/efuse.h"
#include "ice.h"
#include "serial.h"

/* ------------------------------------------------------------------ */
/* ice target efuse -- namespace dispatcher                            */
/* ------------------------------------------------------------------ */

static const struct option cmd_target_efuse_opts[] = {OPT_END()};

/* clang-format off */
static const struct cmd_manual target_efuse_manual = {
	.name = "ice target efuse",
	.summary = "read the eFuses of a connected chip (espefuse replacement)",

	.description =
	H_PARA("eFuses are one-time-programmable bits that hold a chip's MAC "
	       "address, calibration data, keys and security settings.  "
	       "These commands read them through the chip's ROM bootloader "
	       "or the flasher stub, the way @b{espefuse} does.")
	H_PARA("Field names and layouts come from ESP-IDF's eFuse tables, "
	       "chosen for the chip and its silicon revision."),

	.examples =
	H_EXAMPLE("ice target efuse summary")
	H_EXAMPLE("ice target efuse summary 'KEY_PURPOSE_*'"),
};
/* clang-format on */

extern const struct cmd_desc cmd_target_efuse_summary_desc;

static const struct cmd_desc *const target_efuse_subs[] = {
    &cmd_target_efuse_summary_desc,
    NULL,
};

const struct cmd_desc cmd_target_efuse_desc = {
    .name = "efuse",
    .opts = cmd_target_efuse_opts,
    .manual = &target_efuse_manual,
    .subcommands = target_efuse_subs,
};

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

const char *efuse_opt_port;
int efuse_opt_baud = 115200;
int efuse_opt_no_stub;

int efuse_session_open(struct efuse_session *s)
{
	enum ice_chip chip;
	esp_loader_error_t lerr;

	memset(s, 0, sizeof(*s));
	if (esf_open(&s->loader, &s->sport, efuse_opt_port, efuse_opt_baud,
		     efuse_opt_no_stub, &s->autoport) != 0)
		return -1;
	s->connected = 1;

	s->chip = esp_loader_get_target(&s->loader);
	chip = ice_chip_from_esf(s->chip);

	lerr = esp_loader_get_chip_revision(&s->loader, &s->revision);
	if (lerr != ESP_LOADER_SUCCESS) {
		err("cannot read the revision of %s (esp-loader err %d)",
		    ice_chip_name(chip), lerr);
		return -1;
	}

	if (esp_loader_efuse_get_field_info(s->chip, s->revision, &s->fields) !=
	    ESP_LOADER_SUCCESS) {
		err("no eFuse table for %s revision v%u.%u",
		    ice_chip_name(chip), s->revision / 100, s->revision % 100);
		return -1;
	}

	lerr = esp_loader_efuse_read_image(&s->loader, &s->image);
	if (lerr != ESP_LOADER_SUCCESS) {
		err("cannot read the eFuses (esp-loader err %d)", lerr);
		return -1;
	}
	return 0;
}

void efuse_session_close(struct efuse_session *s)
{
	if (s->connected) {
		esp_loader_reset_target(&s->loader);
		esp_loader_deinit(&s->loader);
		s->connected = 0;
	}
	free(s->autoport);
	s->autoport = NULL;
}

static int image_bit(const esp_loader_efuse_image_t *img, unsigned block,
		     unsigned bit)
{
	const esp_loader_efuse_block_t *b;

	if (block >= img->block_count)
		return 0;
	b = &img->blocks[block];
	if (bit / 32 >= b->word_count)
		return 0;
	return (b->words[bit / 32] >> (bit % 32)) & 1u;
}

size_t efuse_field_bits(const esp_loader_efuse_field_info_t *f)
{
	size_t bits = 0;

	for (size_t i = 0; f->field[i]; i++)
		bits += f->field[i]->bit_count;
	return bits;
}

void efuse_field_value(const struct efuse_session *s,
		       const esp_loader_efuse_field_info_t *f, uint8_t *dst)
{
	size_t out = 0;

	memset(dst, 0, (efuse_field_bits(f) + 7) / 8);
	for (size_t i = 0; f->field[i]; i++) {
		const esp_loader_efuse_desc_t *d = f->field[i];

		for (unsigned b = 0; b < d->bit_count; b++, out++)
			if (image_bit(&s->image, d->efuse_block,
				      d->bit_start + b))
				dst[out / 8] |= (uint8_t)(1u << (out % 8));
	}
}

int efuse_field_write_protected(const struct efuse_session *s,
				const esp_loader_efuse_field_info_t *f)
{
	return f->wr_dis_bit != ESP_LOADER_EFUSE_BIT_NONE &&
	       image_bit(&s->image, 0, f->wr_dis_bit);
}

int efuse_field_read_protected(const struct efuse_session *s,
			       const esp_loader_efuse_field_info_t *f)
{
	return f->rd_dis_bit != ESP_LOADER_EFUSE_BIT_NONE &&
	       image_bit(&s->image, 0, f->rd_dis_bit);
}
