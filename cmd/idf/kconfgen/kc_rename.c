/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_rename.c
 * @brief sdkconfig.rename table loader and translator.
 *
 * A rename file is one entry per line:
 *
 *   CONFIG_OLD CONFIG_NEW          -- plain rename
 *   CONFIG_OLD !CONFIG_NEW         -- rename with bool value inversion
 *   # comment                      -- ignored
 *   <blank>                        -- ignored
 *
 * Multiple calls to kc_load_rename() accumulate.  Duplicates (same
 * old/new/invert triple) are deduped on load so rename files passed
 * through both @c --sdkconfig-rename and the
 * @c COMPONENT_SDKCONFIG_RENAMES env var don't double-register.
 *
 * Once loaded, kc_rename_translate() is called by the sdkconfig reader
 * on every incoming CONFIG_* line to migrate deprecated names to their
 * current identity before the value is applied to the symbol table.
 */
#include "ice.h"
#include "kc_ast.h"
#include "kc_io.h"
#include "kc_private.h"

int kc_rename_translate(const struct kc_ctx *ctx, char **name_inout,
			char **val_inout)
{
	for (size_t i = 0; i < ctx->n_renames; i++) {
		const struct kc_rename *r = &ctx->renames[i];
		if (strcmp(r->old_name, *name_inout) != 0)
			continue;
		free(*name_inout);
		*name_inout = sbuf_strdup(r->new_name);
		if (r->invert && val_inout && *val_inout) {
			const char *flipped = NULL;
			if (!strcmp(*val_inout, "y"))
				flipped = "n";
			else if (!strcmp(*val_inout, "n"))
				flipped = "y";
			if (flipped) {
				free(*val_inout);
				*val_inout = sbuf_strdup(flipped);
			}
		}
		return 1;
	}
	return 0;
}

void kc_load_rename(struct kc_ctx *ctx, const char *path)
{
	struct sbuf sb = SBUF_INIT;
	if (sbuf_read_file(&sb, path) < 0)
		die_errno("cannot read rename '%s'", path);

	size_t pos = 0;
	char *line;
	int lineno = 0;
	while ((line = sbuf_getline(sb.buf, sb.len, &pos)) != NULL) {
		lineno++;
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line || *line == '#')
			continue;

		/* Expect: CONFIG_OLD CONFIG_NEW   or   CONFIG_OLD !CONFIG_NEW
		 */
		char *old_start = line;
		char *space = old_start;
		while (*space && *space != ' ' && *space != '\t')
			space++;
		if (!*space)
			continue;
		*space = '\0';
		char *new_start = space + 1;
		while (*new_start == ' ' || *new_start == '\t')
			new_start++;
		if (!*new_start)
			continue;

		int invert = 0;
		if (*new_start == '!') {
			invert = 1;
			new_start++;
		}

		if (strncmp(old_start, KC_CONFIG_PREFIX,
			    KC_CONFIG_PREFIX_LEN) != 0 ||
		    strncmp(new_start, KC_CONFIG_PREFIX,
			    KC_CONFIG_PREFIX_LEN) != 0)
			continue; /* not a CONFIG_* rename; skip silently */

		const char *old_short = old_start + KC_CONFIG_PREFIX_LEN;
		const char *new_short = new_start + KC_CONFIG_PREFIX_LEN;

		/*
		 * Self-rename is a configuration error: a deprecated-alias
		 * entry that points at itself can't express any migration.
		 * Python kconfgen rejects these with a specific RuntimeError
		 * message the upstream test suite matches on; mirror the
		 * exact wording so drop-in compatibility holds.
		 */
		if (!strcmp(old_short, new_short)) {
			die("RuntimeError: Error in %s (line %d): Replacement "
			    "name is the same as original name (%s).",
			    path, lineno, old_short);
		}

		/*
		 * Skip duplicates.  ESP-IDF's build passes rename files both
		 * via --sdkconfig-rename and via the
		 * COMPONENT_SDKCONFIG_RENAMES env var, and several components
		 * intentionally re-list the same line in per-target rename
		 * files -- loading each entry blindly would produce duplicate
		 * deprecated-alias #define lines in sdkconfig.h.  Python
		 * kconfgen dedupes on load.
		 */
		int dup = 0;
		for (size_t i = 0; i < ctx->n_renames; i++) {
			const struct kc_rename *r0 = &ctx->renames[i];
			if (r0->invert == invert &&
			    !strcmp(r0->old_name, old_short) &&
			    !strcmp(r0->new_name, new_short)) {
				dup = 1;
				break;
			}
		}
		if (dup)
			continue;

		ALLOC_GROW(ctx->renames, ctx->n_renames + 1,
			   ctx->alloc_renames);
		struct kc_rename *r = &ctx->renames[ctx->n_renames++];
		r->old_name = sbuf_strdup(old_short);
		r->new_name = sbuf_strdup(new_short);
		r->invert = invert;
	}
	sbuf_release(&sb);
}
