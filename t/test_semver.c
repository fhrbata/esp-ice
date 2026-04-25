/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for semver.c -- parser, comparator, and constraint
 * matcher.  Cases are drawn from the canonical python-semanticversion
 * and mixology corpora and adapted for ice's TAP harness.
 */
#include "ice.h"
#include "semver.h"
#include "tap.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Helper for a one-shot "does string v satisfy constraint s?" query.
 * Returns  1 if match,
 *          0 if no match,
 *         -1 on parse error (either spec or version). */
static int c_match(const char *spec, const char *ver)
{
	struct semver_constraint *c;
	struct semver_version v = SEMVER_VERSION_INIT;
	int ok;

	c = semver_constraint_parse(spec);
	if (!c)
		return -1;

	if (semver_parse(&v, ver) < 0) {
		semver_constraint_free(c);
		return -1;
	}

	ok = semver_constraint_matches(c, &v);
	semver_release(&v);
	semver_constraint_free(c);
	return ok;
}

/* Helper: compare two version strings; returns -1/0/1 or -2 on parse
 * error. */
static int v_cmp(const char *a, const char *b)
{
	struct semver_version va = SEMVER_VERSION_INIT;
	struct semver_version vb = SEMVER_VERSION_INIT;
	int r;

	if (semver_parse(&va, a) < 0)
		return -2;
	if (semver_parse(&vb, b) < 0) {
		semver_release(&va);
		return -2;
	}

	r = semver_cmp(&va, &vb);
	semver_release(&va);
	semver_release(&vb);

	if (r < 0)
		return -1;
	if (r > 0)
		return 1;
	return 0;
}

int main(void)
{
	/* Basic version parsing. */
	{
		struct semver_version v = SEMVER_VERSION_INIT;
		tap_check(semver_parse(&v, "1.2.3") == 0);
		tap_check(v.major == 1 && v.minor == 2 && v.patch == 3);
		tap_check(v.revision == 0);
		tap_check(v.prerelease_nr == 0);
		tap_check(v.build_nr == 0);
		semver_release(&v);
		tap_done("parse bare 1.2.3");
	}

	/* Extended field: IDF revision (~N). */
	{
		struct semver_version v = SEMVER_VERSION_INIT;
		tap_check(semver_parse(&v, "1.2.3~5") == 0);
		tap_check(v.major == 1 && v.minor == 2 && v.patch == 3);
		tap_check(v.revision == 5);
		semver_release(&v);
		tap_done("parse version with ~revision");
	}

	/* Pre-release identifiers. */
	{
		struct semver_version v = SEMVER_VERSION_INIT;
		tap_check(semver_parse(&v, "1.0.0-alpha.1") == 0);
		tap_check(v.prerelease_nr == 2);
		tap_check(strcmp(v.prerelease[0], "alpha") == 0);
		tap_check(strcmp(v.prerelease[1], "1") == 0);
		semver_release(&v);
		tap_done("parse version with prerelease");
	}

	/* Build metadata. */
	{
		struct semver_version v = SEMVER_VERSION_INIT;
		tap_check(semver_parse(&v, "1.0.0+build.7") == 0);
		tap_check(v.build_nr == 2);
		tap_check(strcmp(v.build[0], "build") == 0);
		tap_check(strcmp(v.build[1], "7") == 0);
		semver_release(&v);
		tap_done("parse version with build metadata");
	}

	/* All fields together. */
	{
		struct semver_version v = SEMVER_VERSION_INIT;
		tap_check(semver_parse(&v, "1.2.3~4-rc.1+meta") == 0);
		tap_check(v.major == 1 && v.minor == 2 && v.patch == 3);
		tap_check(v.revision == 4);
		tap_check(v.prerelease_nr == 2);
		tap_check(v.build_nr == 1);
		semver_release(&v);
		tap_done("parse full version M.m.p~r-pre+build");
	}

	/* Invalid inputs. */
	{
		struct semver_version v = SEMVER_VERSION_INIT;
		tap_check(semver_parse(&v, "") < 0);
		tap_check(semver_parse(&v, "1") < 0);
		tap_check(semver_parse(&v, "1.2") < 0);
		tap_check(semver_parse(&v, "01.0.0") < 0); /* leading zero */
		tap_check(semver_parse(&v, "1.0.0-") <
			  0); /* empty prerelease */
		tap_check(semver_parse(&v, "1.0.0-01") <
			  0); /* numeric leading zero */
		tap_check(semver_parse(&v, "1.0.0 ") < 0); /* trailing junk */
		tap_done("reject malformed versions");
	}

	/* M.m.p ordering. */
	{
		tap_check(v_cmp("1.0.0", "2.0.0") == -1);
		tap_check(v_cmp("2.0.0", "1.0.0") == 1);
		tap_check(v_cmp("1.0.0", "1.0.0") == 0);
		tap_check(v_cmp("1.2.3", "1.2.4") == -1);
		tap_check(v_cmp("1.2.3", "1.3.0") == -1);
		tap_done("cmp: M.m.p ordering");
	}

	/* Revision ordering. */
	{
		tap_check(v_cmp("1.2.3", "1.2.3~1") == -1);
		tap_check(v_cmp("1.2.3~1", "1.2.3~2") == -1);
		tap_check(v_cmp("1.2.3~5", "1.2.3~5") == 0);
		tap_done("cmp: revision ordering (~N extension)");
	}

	/* Pre-release ordering per SemVer 2.0.0. */
	{
		/* Any prerelease sorts below no-prerelease of same base. */
		tap_check(v_cmp("1.0.0-alpha", "1.0.0") == -1);
		tap_check(v_cmp("1.0.0", "1.0.0-alpha") == 1);

		/* alpha < beta (lexical ASCII on alphanumeric identifiers). */
		tap_check(v_cmp("1.0.0-alpha", "1.0.0-beta") == -1);

		/* Numeric identifier sorts below alphanumeric. */
		tap_check(v_cmp("1.0.0-1", "1.0.0-alpha") == -1);

		/* Fewer shared prerelease identifiers sort below more. */
		tap_check(v_cmp("1.0.0-alpha", "1.0.0-alpha.1") == -1);

		/* Numeric prerelease compared numerically. */
		tap_check(v_cmp("1.0.0-alpha.2", "1.0.0-alpha.10") == -1);
		tap_done("cmp: SemVer 2.0.0 prerelease ordering");
	}

	/* Build metadata ignored in comparison. */
	{
		tap_check(v_cmp("1.0.0+a", "1.0.0+b") == 0);
		tap_check(v_cmp("1.0.0+a", "1.0.0") == 0);
		tap_done("cmp: build metadata ignored");
	}

	/* Format round-trip. */
	{
		struct semver_version v = SEMVER_VERSION_INIT;
		struct sbuf sb = SBUF_INIT;
		tap_check(semver_parse(&v, "1.2.3~4-rc.1+meta") == 0);
		semver_format(&sb, &v);
		tap_check(strcmp(sb.buf, "1.2.3~4-rc.1+meta") == 0);
		sbuf_release(&sb);
		semver_release(&v);
		tap_done("format round-trip");
	}

	/* Constraint: wildcard. */
	{
		tap_check(c_match("*", "0.0.1") == 1);
		tap_check(c_match("*", "99.0.0") == 1);
		/* Natural policy: '*' excludes prereleases. */
		tap_check(c_match("*", "1.0.0-alpha") == 0);
		tap_done("constraint: * matches non-prerelease any");
	}

	/* Constraint: exact. */
	{
		tap_check(c_match("1.2.3", "1.2.3") == 1);
		tap_check(c_match("==1.2.3", "1.2.3") == 1);
		tap_check(c_match("=1.2.3", "1.2.3") == 1);
		tap_check(c_match("1.2.3", "1.2.4") == 0);
		tap_done("constraint: exact match");
	}

	/* Constraint: partial (wildcard) versions. */
	{
		/* "1.2" expands to >=1.2.0, <1.3.0. */
		tap_check(c_match("1.2", "1.2.0") == 1);
		tap_check(c_match("1.2", "1.2.99") == 1);
		tap_check(c_match("1.2", "1.3.0") == 0);

		/* "1" expands to >=1.0.0, <2.0.0. */
		tap_check(c_match("1", "1.0.0") == 1);
		tap_check(c_match("1", "1.99.99") == 1);
		tap_check(c_match("1", "2.0.0") == 0);
		tap_done("constraint: partial wildcard expansion");
	}

	/* Constraint: comparison operators. */
	{
		tap_check(c_match(">=1.0.0", "1.0.0") == 1);
		tap_check(c_match(">=1.0.0", "0.9.9") == 0);
		tap_check(c_match(">1.0.0", "1.0.0") == 0);
		tap_check(c_match(">1.0.0", "1.0.1") == 1);
		tap_check(c_match("<=2.0.0", "2.0.0") == 1);
		tap_check(c_match("<=2.0.0", "2.0.1") == 0);
		tap_check(c_match("<2.0.0", "1.9.9") == 1);
		tap_check(c_match("!=1.5.0", "1.5.0") == 0);
		tap_check(c_match("!=1.5.0", "1.6.0") == 1);
		tap_done("constraint: comparison operators");
	}

	/* Constraint: conjunction with ','. */
	{
		tap_check(c_match(">=1.0.0,<2.0.0", "1.5.0") == 1);
		tap_check(c_match(">=1.0.0,<2.0.0", "0.5.0") == 0);
		tap_check(c_match(">=1.0.0,<2.0.0", "2.0.0") == 0);
		tap_check(c_match(">=1.0.0 <2.0.0", "1.5.0") ==
			  1); /* space too */
		tap_done("constraint: conjunction (comma or space)");
	}

	/* Constraint: caret. */
	{
		tap_check(c_match("^1.2.3", "1.2.3") == 1);
		tap_check(c_match("^1.2.3", "1.9.9") == 1);
		tap_check(c_match("^1.2.3", "2.0.0") == 0);
		tap_check(c_match("^1.2.3", "1.2.2") == 0);

		/* ^0.2.3 -> >=0.2.3, <0.3.0. */
		tap_check(c_match("^0.2.3", "0.2.3") == 1);
		tap_check(c_match("^0.2.3", "0.2.99") == 1);
		tap_check(c_match("^0.2.3", "0.3.0") == 0);

		/* ^0.0.3 -> >=0.0.3, <0.0.4. */
		tap_check(c_match("^0.0.3", "0.0.3") == 1);
		tap_check(c_match("^0.0.3", "0.0.4") == 0);
		tap_done("constraint: caret");
	}

	/* Constraint: tilde (python-semanticversion variant). */
	{
		/* ~1.2.3 -> >=1.2.3, <1.3.0 */
		tap_check(c_match("~1.2.3", "1.2.3") == 1);
		tap_check(c_match("~1.2.3", "1.2.99") == 1);
		tap_check(c_match("~1.2.3", "1.3.0") == 0);

		/* ~1.2 -> >=1.2.0, <2.0.0 (partial: bump major). */
		tap_check(c_match("~1.2", "1.2.0") == 1);
		tap_check(c_match("~1.2", "1.99.99") == 1);
		tap_check(c_match("~1.2", "2.0.0") == 0);

		/* ~= acts like ~. */
		tap_check(c_match("~=1.2.3", "1.2.99") == 1);
		tap_check(c_match("~=1.2.3", "1.3.0") == 0);
		tap_done("constraint: tilde");
	}

	/* Constraint: disjunction with '||'. */
	{
		tap_check(c_match("1.0.0 || 2.0.0", "1.0.0") == 1);
		tap_check(c_match("1.0.0 || 2.0.0", "2.0.0") == 1);
		tap_check(c_match("1.0.0 || 2.0.0", "1.5.0") == 0);
		tap_check(c_match(">=1.0,<1.5 || >=2.0", "1.2.0") == 1);
		tap_check(c_match(">=1.0,<1.5 || >=2.0", "2.5.0") == 1);
		tap_check(c_match(">=1.0,<1.5 || >=2.0", "1.7.0") == 0);
		tap_done("constraint: disjunction (||)");
	}

	/* Pre-release filtering under the natural policy. */
	{
		/* Constraints without a prerelease spec reject prerelease
		 * candidates. */
		tap_check(c_match("^1.0", "2.0.0-alpha") == 0);
		tap_check(c_match(">=1.0.0", "1.5.0-alpha") == 0);
		tap_check(c_match("<2.0.0", "1.9.9-alpha") == 0);

		/* A constraint that explicitly names a prerelease with the
		 * same base admits it. */
		tap_check(c_match(">=1.0.0-rc", "1.0.0-rc") == 1);
		tap_check(c_match(">=1.0.0-rc", "1.0.0-rc.2") == 1);
		/* But still not a prerelease of a different base. */
		tap_check(c_match(">=1.0.0-rc", "2.0.0-alpha") == 0);
		tap_done("constraint: prerelease filtering (natural policy)");
	}

	/* Invalid constraints return NULL from the parser. */
	{
		tap_check(semver_constraint_parse("") == NULL);
		tap_check(semver_constraint_parse(">=") == NULL);
		tap_check(semver_constraint_parse("^") == NULL);
		tap_check(semver_constraint_parse("bogus") == NULL);
		tap_done("constraint: reject malformed input");
	}

	return tap_result();
}
