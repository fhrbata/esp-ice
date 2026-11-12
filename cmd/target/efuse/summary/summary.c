/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/target/efuse/summary/summary.c
 * @brief `ice target efuse summary` -- list the chip's eFuse fields.
 *
 * Collecting and printing are separate steps: one row per field with
 * its decoded value and protection state, then a text renderer that
 * reads only those rows.
 */
#include <ctype.h>
#include <inttypes.h>

#include "cmd/target/efuse/efuse.h"
#include "ice.h"
#include "serial.h"

/* clang-format off */
static const struct cmd_manual target_efuse_summary_manual = {
	.name = "ice target efuse summary",
	.summary = "list the eFuse fields of a connected chip",

	.description =
	H_PARA("Read every eFuse of the chip and list its fields, one per "
	       "line: the name, the block it lives in, its width in bits, "
	       "whether it can still be read and written (@b{R/W}; a "
	       "@b{-} marks a burned read or write protection), and its "
	       "value.")
	H_PARA("Values up to 32 bits are shown as numbers, MAC addresses "
	       "as @b{aa:bb:cc:dd:ee:ff}, and longer fields as bytes in "
	       "eFuse order.  A read-protected field reads back as zeros "
	       "on the chip, so its value is not shown.")
	H_PARA("Give one or more @b{<pattern>}s to list only the matching "
	       "fields; @b{*} and @b{?} work as in the shell, and case does "
	       "not matter.  Nothing is written to the chip.  It is reset "
	       "into its application when the command ends."),

	.examples =
	H_EXAMPLE("ice target efuse summary")
	H_EXAMPLE("ice target efuse summary mac 'key_purpose_*'")
	H_EXAMPLE("ice target efuse summary --port /dev/ttyACM0"),

	.extras =
	H_SECTION("SEE ALSO")
	H_ITEM("ice target probe",
	       "List the serial ports to pick @b{--port} from."),
};
/* clang-format on */

static const struct option cmd_target_efuse_summary_opts[] = {
    EFUSE_COMMON_OPTS,
    OPT_POSITIONAL("[<pattern>...]", NULL),
    OPT_END(),
};

int cmd_target_efuse_summary(int argc, const char **argv);

const struct cmd_desc cmd_target_efuse_summary_desc = {
    .name = "summary",
    .fn = cmd_target_efuse_summary,
    .opts = cmd_target_efuse_summary_opts,
    .manual = &target_efuse_summary_manual,
};

/* A field never spans more than one block (keys and user data are
 * whole 256-bit blocks), so one block's worth of bytes holds any value. */
#define MAX_FIELD_BYTES ((size_t)ESP_LOADER_EFUSE_MAX_WORDS_PER_BLOCK * 4)

/** One listed field: everything the renderer prints. */
struct summary_row {
	const char *name;
	unsigned block; /* block of the field's first bit range */
	size_t bits;
	int readable;
	int writable;
	uint8_t value[MAX_FIELD_BYTES]; /* LSB-first */
};

static int matches(const char *name, const struct svec *pats)
{
	if (pats->nr == 0)
		return 1;
	for (size_t i = 0; i < pats->nr; i++)
		if (glob_match(pats->v[i], name))
			return 1;
	return 0;
}

static int collect_rows(const struct efuse_session *s, const struct svec *pats,
			struct summary_row **out)
{
	struct summary_row *rows;
	int n = 0;

	rows = calloc(s->fields->count, sizeof(*rows));
	if (!rows)
		die_errno("calloc");
	for (uint16_t i = 0; i < s->fields->count; i++) {
		const esp_loader_efuse_field_info_t *f = &s->fields->fields[i];
		struct summary_row *r;

		if (!matches(f->name, pats))
			continue;
		r = &rows[n++];
		r->name = f->name;
		r->block = f->field[0] ? f->field[0]->efuse_block : 0;
		r->bits = efuse_field_bits(f);
		if (r->bits > 8 * MAX_FIELD_BYTES)
			die("eFuse field %s is %zu bits, more than a block",
			    f->name, r->bits);
		r->readable = !efuse_field_read_protected(s, f);
		r->writable = !efuse_field_write_protected(s, f);
		efuse_field_value(s, f, r->value);
	}
	*out = rows;
	return n;
}

static void format_value(const struct summary_row *r, struct sbuf *out)
{
	size_t bytes = (r->bits + 7) / 8;

	if (!r->readable) {
		sbuf_addstr(out, "(read-protected)");
	} else if (r->bits <= 32) {
		uint32_t v = 0;

		for (size_t i = 0; i < bytes; i++)
			v |= (uint32_t)r->value[i] << (8 * i);
		sbuf_addf(out, "%" PRIu32, v);
	} else if (r->bits == 48 && strstr(r->name, "MAC")) {
		/* The field table lists a MAC's bit ranges so that its bytes
		 * decode in display order, as espefuse prints them. */
		for (size_t i = 0; i < bytes; i++)
			sbuf_addf(out, "%s%02x", i ? ":" : "", r->value[i]);
	} else {
		for (size_t i = 0; i < bytes; i++)
			sbuf_addf(out, "%s%02x", i ? " " : "", r->value[i]);
	}
}

static void print_rows(const struct efuse_session *s,
		       const struct summary_row *rows, int n)
{
	int wname = 4;
	struct sbuf val = SBUF_INIT;

	for (int i = 0; i < n; i++)
		if ((int)strlen(rows[i].name) > wname)
			wname = (int)strlen(rows[i].name);

	printf("@b{%s} revision v%u.%u\n\n",
	       ice_chip_name(ice_chip_from_esf(s->chip)), s->revision / 100,
	       s->revision % 100);
	printf("@b{%-*s  %-5s %4s  %-3s  %s}\n", wname, "NAME", "BLOCK", "BITS",
	       "R/W", "VALUE");

	for (int i = 0; i < n; i++) {
		const struct summary_row *r = &rows[i];

		sbuf_reset(&val);
		format_value(r, &val);
		printf("%-*s  BLK%-2u %4zu  %c/%c  %s\n", wname, r->name,
		       r->block, r->bits, r->readable ? 'R' : '-',
		       r->writable ? 'W' : '-', val.buf);
	}
	sbuf_release(&val);
}

int cmd_target_efuse_summary(int argc, const char **argv)
{
	struct efuse_session s;
	struct summary_row *rows = NULL;
	struct svec pats = SVEC_INIT;
	int n, rc = 1;

	argc = parse_options(argc, argv, &cmd_target_efuse_summary_desc);

	/* Field names are upper case; match patterns without regard to
	 * case by upper-casing them once. */
	for (int i = 0; i < argc; i++) {
		char *upper = sbuf_strdup(argv[i]);

		for (char *c = upper; *c; c++)
			*c = (char)toupper((unsigned char)*c);
		svec_push(&pats, upper);
		free(upper);
	}

	if (efuse_session_open(&s) != 0)
		goto out;

	n = collect_rows(&s, &pats, &rows);
	if (n == 0) {
		err("no eFuse field matches the given pattern");
		goto out;
	}
	print_rows(&s, rows, n);
	rc = 0;
out:
	free(rows);
	svec_clear(&pats);
	efuse_session_close(&s);
	return rc;
}
