/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for chip.c -- the centralized chip identity table.
 */
#include "chip.h"
#include "tap.h"

#include <stddef.h>
#include <string.h>

static int in_list(const char *const *list, const char *name)
{
	for (; *list; list++)
		if (!strcmp(*list, name))
			return 1;
	return 0;
}

int main(void)
{
	/* ice_chip_name: known chips return human name */
	{
		tap_check(strcmp(ice_chip_name(ICE_CHIP_ESP32), "ESP32") == 0);
		tap_check(strcmp(ice_chip_name(ICE_CHIP_ESP32C6), "ESP32-C6") ==
			  0);
		tap_check(strcmp(ice_chip_name(ICE_CHIP_ESP32S3), "ESP32-S3") ==
			  0);
		tap_check(strcmp(ice_chip_name(ICE_CHIP_ESP32P4), "ESP32-P4") ==
			  0);
		tap_done("ice_chip_name returns human-readable names");
	}

	/* ice_chip_name: unknown/invalid returns "unknown", never NULL */
	{
		tap_check(strcmp(ice_chip_name(ICE_CHIP_UNKNOWN), "unknown") ==
			  0);
		tap_check(
		    strcmp(ice_chip_name((enum ice_chip)999), "unknown") == 0);
		tap_done(
		    "ice_chip_name returns \"unknown\" for invalid values");
	}

	/* ice_chip_idf_name: known chips return IDF target string */
	{
		tap_check(strcmp(ice_chip_idf_name(ICE_CHIP_ESP32), "esp32") ==
			  0);
		tap_check(strcmp(ice_chip_idf_name(ICE_CHIP_ESP32C6),
				 "esp32c6") == 0);
		tap_check(strcmp(ice_chip_idf_name(ICE_CHIP_ESP32S3),
				 "esp32s3") == 0);
		tap_check(strcmp(ice_chip_idf_name(ICE_CHIP_ESP32H21),
				 "esp32h21") == 0);
		tap_done("ice_chip_idf_name returns IDF target strings");
	}

	/* ice_chip_idf_name: unknown returns NULL */
	{
		tap_check(ice_chip_idf_name(ICE_CHIP_UNKNOWN) == NULL);
		tap_check(ice_chip_idf_name((enum ice_chip)999) == NULL);
		tap_done("ice_chip_idf_name returns NULL for unknown/invalid");
	}

	/* ice_chip_from_idf_name: round-trips with ice_chip_idf_name */
	{
		tap_check(ice_chip_from_idf_name("esp32") == ICE_CHIP_ESP32);
		tap_check(ice_chip_from_idf_name("esp32c6") ==
			  ICE_CHIP_ESP32C6);
		tap_check(ice_chip_from_idf_name("esp32s3") ==
			  ICE_CHIP_ESP32S3);
		tap_check(ice_chip_from_idf_name("esp32p4") ==
			  ICE_CHIP_ESP32P4);
		tap_check(ice_chip_from_idf_name("esp32h21") ==
			  ICE_CHIP_ESP32H21);
		tap_done("ice_chip_from_idf_name resolves known IDF names");
	}

	/* ice_chip_from_idf_name: rejects NULL, empty, and garbage */
	{
		tap_check(ice_chip_from_idf_name(NULL) == ICE_CHIP_UNKNOWN);
		tap_check(ice_chip_from_idf_name("") == ICE_CHIP_UNKNOWN);
		tap_check(ice_chip_from_idf_name("bogus") == ICE_CHIP_UNKNOWN);
		tap_check(ice_chip_from_idf_name("ESP32") ==
			  ICE_CHIP_UNKNOWN); /* case-sensitive */
		tap_done(
		    "ice_chip_from_idf_name returns UNKNOWN for invalid input");
	}

	/* Full round-trip: idf_name → enum → idf_name */
	{
		const char *names[] = {"esp32",	  "esp32s2", "esp32s3",
				       "esp32c2", "esp32c3", "esp32c6",
				       "esp32h2", "esp32p4"};
		int ok = 1;

		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			enum ice_chip c = ice_chip_from_idf_name(names[i]);
			if (c == ICE_CHIP_UNKNOWN ||
			    strcmp(ice_chip_idf_name(c), names[i]) != 0)
				ok = 0;
		}
		tap_check(ok);
		tap_done(
		    "ice_chip_from_idf_name / ice_chip_idf_name round-trip");
	}

	/* ice_chip_summary: supported chips have non-NULL summaries */
	{
		tap_check(ice_chip_summary("esp32") != NULL);
		tap_check(ice_chip_summary("esp32c6") != NULL);
		tap_check(ice_chip_summary("esp32p4") != NULL);
		tap_done(
		    "ice_chip_summary returns non-NULL for supported chips");
	}

	/* ice_chip_summary: unknown input returns NULL */
	{
		tap_check(ice_chip_summary(NULL) == NULL);
		tap_check(ice_chip_summary("") == NULL);
		tap_check(ice_chip_summary("bogus") == NULL);
		tap_done("ice_chip_summary returns NULL for unknown chips");
	}

	/* ice_supported_targets: NULL-terminated, contains expected chips */
	{
		tap_check(ice_supported_targets[0] != NULL); /* not empty */
		tap_check(in_list(ice_supported_targets, "esp32"));
		tap_check(in_list(ice_supported_targets, "esp32c6"));
		tap_check(in_list(ice_supported_targets, "esp32s3"));
		tap_check(in_list(ice_supported_targets, "esp32p4"));
		tap_check(!in_list(ice_supported_targets,
				   "esp32h21")); /* preview only */
		tap_check(!in_list(ice_supported_targets,
				   "linux")); /* preview only */
		tap_done("ice_supported_targets contains supported chips, not "
			 "preview");
	}

	/* ice_preview_targets: NULL-terminated, contains expected chips */
	{
		tap_check(ice_preview_targets[0] != NULL);
		tap_check(in_list(ice_preview_targets, "linux"));
		tap_check(in_list(ice_preview_targets, "esp32h21"));
		tap_check(!in_list(ice_preview_targets,
				   "esp32c6")); /* supported only */
		tap_done("ice_preview_targets contains preview chips, not "
			 "supported");
	}

	/* Every entry in both lists resolves via ice_chip_from_idf_name */
	{
		int ok = 1;

		for (const char *const *t = ice_supported_targets; *t; t++)
			if (ice_chip_from_idf_name(*t) == ICE_CHIP_UNKNOWN)
				ok = 0;
		for (const char *const *t = ice_preview_targets; *t; t++)
			if (ice_chip_from_idf_name(*t) == ICE_CHIP_UNKNOWN)
				ok = 0;
		tap_check(ok);
		tap_done(
		    "every list entry resolves via ice_chip_from_idf_name");
	}

	return tap_result();
}
