/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file semver.h
 * @brief Semantic versioning parser and constraint matcher.
 *
 * Minimal SemVer 2.0.0 support sized for what @c pubgrub needs:
 * parse a version, parse a constraint spec, ask "does this version
 * satisfy this constraint?".  No range-algebra is exposed; the solver
 * operates on concrete per-package version lists and filters each
 * against a constraint via @c semver_constraint_matches().
 *
 * Version format:
 *
 *   MAJOR.MINOR.PATCH[~REVISION][-PRERELEASE][+BUILD]
 *
 * @c MAJOR / @c MINOR / @c PATCH are required unsigned integers.
 * @c ~REVISION is an IDF-component-manager extension -- a fourth
 * ordered numeric field used by some Espressif packages.  It is zero
 * when absent and sorts immediately after @c PATCH.  @c PRERELEASE
 * and @c BUILD are dot-separated identifiers; build metadata is
 * preserved for formatting but ignored in precedence comparisons.
 *
 * Constraint syntax (single predicates):
 *
 *   *              any version
 *   1.2.3          exactly 1.2.3 (wildcards expand: 1.2 = >=1.2.0,<1.3.0)
 *   ==1.2.3  =1.2.3   exactly 1.2.3
 *   !=1.2.3        any version except 1.2.3
 *   >=1.2.3  >1.2.3
 *   <=1.2.3  <1.2.3
 *   ^1.2.3         compatible: >=1.2.3, bump-limited by the most
 *                  significant non-zero digit (^0.2.3 = >=0.2.3,<0.3.0;
 *                  ^0.0.3 = >=0.0.3,<0.0.4)
 *   ~1.2.3         patch-level: >=1.2.3, <1.3.0
 *   ~=1.2.3        same as ~
 *
 * Predicates are joined with ',' (conjunction).  Whitespace between
 * predicates is also treated as conjunction.  Disjunction is written
 * with '||' and spans whole constraints (e.g. "1.0.0 || >=2.0,<3.0").
 *
 * Pre-release handling: a constraint that does not explicitly mention
 * a prerelease identifier does not match prerelease versions.  That is,
 * @c ^1.0 matches 1.5.0 and 1.9.9 but not 2.0.0-alpha, and @c >=1.5.0
 * does not match 2.0.0-alpha either.  Constraints that do mention a
 * prerelease (e.g. @c >=1.0.0-rc or @c ^1.0.0-beta) opt into matching
 * prerelease versions of the same base.
 */
#ifndef SEMVER_H
#define SEMVER_H

#include <stddef.h>

struct sbuf;

/* ------------------------------------------------------------------ */
/* Version                                                             */
/* ------------------------------------------------------------------ */

struct semver_version {
	unsigned major;
	unsigned minor;
	unsigned patch;
	unsigned revision; /**< IDF extension (1.2.3~5); 0 if absent. */

	char **prerelease; /**< Dot-split identifiers; NULL if none. */
	size_t prerelease_nr;

	char **build; /**< Dot-split identifiers; NULL if none. */
	size_t build_nr;
};

/** Static initializer for an empty (0.0.0) version. */
#define SEMVER_VERSION_INIT {0, 0, 0, 0, NULL, 0, NULL, 0}

/**
 * @brief Parse a SemVer string into @p out.
 *
 * On failure @p out is left untouched and needs no release.
 *
 * @return 0 on success; -1 on malformed input.
 */
int semver_parse(struct semver_version *out, const char *s);

/** Release memory owned by @p v.  Does not free @p v itself. */
void semver_release(struct semver_version *v);

/**
 * @brief Compare two versions by SemVer 2.0.0 precedence.
 *
 * Build metadata is ignored.  Numeric prerelease identifiers sort
 * below alphabetic ones; an absent prerelease sorts above any
 * prerelease of the same @c M.m.p.rev.
 *
 * @return -1 if @p a < @p b, 0 if equal, 1 if @p a > @p b.
 */
int semver_cmp(const struct semver_version *a, const struct semver_version *b);

/** Append a formatted version to @p out (does not reset the sbuf). */
void semver_format(struct sbuf *out, const struct semver_version *v);

/* ------------------------------------------------------------------ */
/* Constraint                                                          */
/* ------------------------------------------------------------------ */

struct semver_constraint;

/**
 * @brief Parse a constraint spec.
 *
 * Examples: @c "*", @c "^1.2", @c ">=1.0,<2.0", @c "1.0 || 2.0".
 * Returns NULL on malformed input.  Caller owns the result and must
 * call @c semver_constraint_free().
 */
struct semver_constraint *semver_constraint_parse(const char *s);

/** Free a constraint returned by @c semver_constraint_parse(). */
void semver_constraint_free(struct semver_constraint *c);

/** @return 1 if @p v satisfies @p c, 0 otherwise. */
int semver_constraint_matches(const struct semver_constraint *c,
			      const struct semver_version *v);

/** Append a canonical form of @p c to @p out. */
void semver_constraint_format(struct sbuf *out,
			      const struct semver_constraint *c);

#endif /* SEMVER_H */
