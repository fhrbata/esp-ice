/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_report.h
 * @brief Deferred diagnostic reporting for the Kconfig processor.
 *
 * The parser, evaluator, and I/O layer accumulate non-fatal
 * diagnostics on a @p kc_report attached to the context.  Callers
 * flush the collected set at end of run via @c kc_report_flush(),
 * which prints them to stderr sorted by file:line (insertion-order
 * preserved as the tiebreaker).
 *
 * Fatal errors still use @c die() for immediate termination -- only
 * diagnostics that can be surfaced usefully alongside others go
 * through this path.
 */
#ifndef KC_REPORT_H
#define KC_REPORT_H

#include <stddef.h>

struct kc_ctx;

enum kc_report_level {
	KC_REPORT_ERROR,
	KC_REPORT_WARNING,
	KC_REPORT_INFO,
};

struct kc_report_msg {
	enum kc_report_level level;
	const char *file; /**< Borrowed; must outlive the report. */
	int line;
	char *text;
	size_t seq; /**< Insertion order, used as sort tiebreaker. */
};

struct kc_report {
	struct kc_report_msg *msgs;
	size_t count;
	size_t alloc;
	int error_count;
};

void kc_report_init(struct kc_report *rpt);
void kc_report_release(struct kc_report *rpt);

/**
 * @brief Accumulate a diagnostic at the given severity.
 *
 * @p file may be NULL to indicate "no source location".  The format
 * string is printf-style; the rendered text is copied into the
 * report.  Errors bump @c error_count; @c kc_report_has_errors
 * reports the final tally.
 */
void kc_report_error(struct kc_report *rpt, const char *file, int line,
		     const char *fmt, ...);
void kc_report_warning(struct kc_report *rpt, const char *file, int line,
		       const char *fmt, ...);
void kc_report_info(struct kc_report *rpt, const char *file, int line,
		    const char *fmt, ...);

/**
 * @brief Print every accumulated diagnostic to stderr, sorted by
 *        file:line.  Returns the error count.
 *
 * Not idempotent: calling twice prints twice.  The report retains
 * its entries; release with @c kc_report_release.
 */
int kc_report_flush(struct kc_report *rpt);

int kc_report_has_errors(const struct kc_report *rpt);

#endif /* KC_REPORT_H */
