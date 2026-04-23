/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for yaml.c -- the small in-house YAML DOM reader.
 */
#include "ice.h"
#include "tap.h"

int main(void)
{
	/* Block mapping with string, bool, and nested list values. */
	{
		const char *src = "re: \"warning: foo\"\n"
				  "hint: 'use bar'\n"
				  "match_to_output: True\n";
		struct yaml_value *root = yaml_parse(src, strlen(src));

		tap_check(root != NULL);
		tap_check(yaml_type(root) == YAML_MAP);
		tap_check(strcmp(yaml_as_string(yaml_get(root, "re")),
				 "warning: foo") == 0);
		tap_check(strcmp(yaml_as_string(yaml_get(root, "hint")),
				 "use bar") == 0);
		tap_check(yaml_as_bool(yaml_get(root, "match_to_output")) == 1);
		tap_check(yaml_get(root, "missing") == NULL);
		yaml_free(root);
		tap_done("parse flat mapping with string/bool values");
	}

	/* Flow sequence with quoted strings and escapes. */
	{
		const char *src = "items: ['a', 'b', \"c\\nd\"]\n";
		struct yaml_value *root = yaml_parse(src, strlen(src));
		struct yaml_value *items;

		tap_check(root != NULL);
		items = yaml_get(root, "items");
		tap_check(yaml_seq_size(items) == 3);
		tap_check(strcmp(yaml_as_string(yaml_seq_at(items, 0)), "a") ==
			  0);
		tap_check(
		    strcmp(yaml_as_string(yaml_seq_at(items, 2)), "c\nd") == 0);
		yaml_free(root);
		tap_done("parse flow sequence with quoted strings + escapes");
	}

	/* Block sequence of block mappings -- the hints.yml shape. */
	{
		const char *src = "-\n"
				  "    re: \"err: foo\"\n"
				  "    hint: \"fix foo\"\n"
				  "-\n"
				  "    re: \"err: bar\"\n"
				  "    hint: \"fix bar\"\n";
		struct yaml_value *root = yaml_parse(src, strlen(src));
		struct yaml_value *e0, *e1;

		tap_check(root != NULL);
		tap_check(yaml_type(root) == YAML_SEQ);
		tap_check(yaml_seq_size(root) == 2);
		e0 = yaml_seq_at(root, 0);
		e1 = yaml_seq_at(root, 1);
		tap_check(yaml_type(e0) == YAML_MAP);
		tap_check(strcmp(yaml_as_string(yaml_get(e0, "re")),
				 "err: foo") == 0);
		tap_check(strcmp(yaml_as_string(yaml_get(e1, "hint")),
				 "fix bar") == 0);
		yaml_free(root);
		tap_done("parse block sequence of block mappings");
	}

	/* Nested: mapping containing a sequence containing mappings. */
	{
		const char *src = "re: \"err {}\"\n"
				  "variables:\n"
				  "    -\n"
				  "        re_variables: ['x']\n"
				  "        hint_variables: ['y']\n"
				  "    -\n"
				  "        re_variables: ['p']\n"
				  "        hint_variables: ['q']\n";
		struct yaml_value *root = yaml_parse(src, strlen(src));
		struct yaml_value *vars, *entry;

		tap_check(root != NULL);
		vars = yaml_get(root, "variables");
		tap_check(yaml_seq_size(vars) == 2);
		entry = yaml_seq_at(vars, 1);
		tap_check(strcmp(yaml_as_string(yaml_seq_at(
				     yaml_get(entry, "re_variables"), 0)),
				 "p") == 0);
		yaml_free(root);
		tap_done("parse nested map/seq/map structure");
	}

	/* Comments: own-line and end-of-line. */
	{
		const char *src = "# leading comment\n"
				  "key1: value1  # trailing\n"
				  "# middle\n"
				  "key2: value2\n";
		struct yaml_value *root = yaml_parse(src, strlen(src));

		tap_check(root != NULL);
		tap_check(strcmp(yaml_as_string(yaml_get(root, "key1")),
				 "value1") == 0);
		tap_check(strcmp(yaml_as_string(yaml_get(root, "key2")),
				 "value2") == 0);
		yaml_free(root);
		tap_done("comments ignored on own and end of lines");
	}

	/* Boolean variants: true, True, TRUE, false, False, FALSE. */
	{
		const char *src = "a: true\n"
				  "b: True\n"
				  "c: TRUE\n"
				  "d: false\n"
				  "e: False\n"
				  "f: FALSE\n";
		struct yaml_value *root = yaml_parse(src, strlen(src));

		tap_check(yaml_as_bool(yaml_get(root, "a")) == 1);
		tap_check(yaml_as_bool(yaml_get(root, "b")) == 1);
		tap_check(yaml_as_bool(yaml_get(root, "c")) == 1);
		tap_check(yaml_as_bool(yaml_get(root, "d")) == 0);
		tap_check(yaml_as_bool(yaml_get(root, "e")) == 0);
		tap_check(yaml_as_bool(yaml_get(root, "f")) == 0);
		tap_check(yaml_type(yaml_get(root, "d")) == YAML_BOOL);
		yaml_free(root);
		tap_done("boolean case variants parse as YAML_BOOL");
	}

	/* Escape handling inside double-quoted strings. */
	{
		const char *src = "re: \"err \\\\w+ regex\"\n";
		struct yaml_value *root = yaml_parse(src, strlen(src));

		tap_check(strcmp(yaml_as_string(yaml_get(root, "re")),
				 "err \\w+ regex") == 0);
		yaml_free(root);
		tap_done("double-quoted \\\\ yields single backslash");
	}

	/* Single-quoted '' escape for literal apostrophe. */
	{
		const char *src = "msg: 'it''s fine'\n";
		struct yaml_value *root = yaml_parse(src, strlen(src));

		tap_check(strcmp(yaml_as_string(yaml_get(root, "msg")),
				 "it's fine") == 0);
		yaml_free(root);
		tap_done("single-quoted '' is a literal apostrophe");
	}

	/* Empty flow sequence. */
	{
		const char *src = "v: []\n";
		struct yaml_value *root = yaml_parse(src, strlen(src));

		tap_check(yaml_type(yaml_get(root, "v")) == YAML_SEQ);
		tap_check(yaml_seq_size(yaml_get(root, "v")) == 0);
		yaml_free(root);
		tap_done("empty flow sequence parses as empty YAML_SEQ");
	}

	/* Invalid input: unterminated double quote -> NULL. */
	{
		const char *bad = "key: \"unterminated\n";
		tap_check(yaml_parse(bad, strlen(bad)) == NULL);
		tap_done("unterminated string returns NULL");
	}

	/* Invalid input: stray content after mapping value. */
	{
		const char *bad = "a: 1 extra junk [ unclosed\n";
		/* Plain scalar swallows the entire run, including '['.  That
		 * is tolerated, not a parse error.  So re-verify with a truly
		 * broken construct: unbalanced flow seq. */
		struct yaml_value *y = yaml_parse(bad, strlen(bad));
		yaml_free(y);

		const char *bad2 = "a: [1, 2\n";
		tap_check(yaml_parse(bad2, strlen(bad2)) == NULL);
		tap_done("unterminated flow sequence returns NULL");
	}

	/* Keys with colon in value: 'err: foo' parses without confusion. */
	{
		const char *src = "re: \"error: not found\"\n";
		struct yaml_value *root = yaml_parse(src, strlen(src));

		tap_check(root != NULL);
		tap_check(strcmp(yaml_as_string(yaml_get(root, "re")),
				 "error: not found") == 0);
		yaml_free(root);
		tap_done("quoted value containing colon parses correctly");
	}

	return tap_result();
}
