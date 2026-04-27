/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for kc_report -- the deferred diagnostic collector used
 * by the Kconfig processor.  Exercises the three severity levels,
 * counting semantics, and the sort ordering applied by
 * kc_report_flush.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ice.h"
#include "kc_report.h"
#include "tap.h"

/*
 * MSVC/MinGW stdio uses text mode for stderr by default; fprintf's
 * "\n" becomes "\r\n" on disk.  Real output is still one logical line
 * feed per message — normalize so strcmp against "\n" expectations
 * matches on Windows.
 */
static void squash_crlf(char *s)
{
	char *w = s;

	for (const char *r = s; *r;) {
		if (r[0] == '\r' && r[1] == '\n') {
			r += 2;
			*w++ = '\n';
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

/*
 * Run @p body with stderr redirected to a scratch file, then slurp
 * the file's contents and return them (caller frees).  The original
 * stderr fd is saved and restored with dup/dup2 so subsequent tests
 * can still emit failure diagnostics via tap_check.
 */
static char *capture_stderr(void (*body)(void *), void *arg)
{
	fflush(stderr);
	int saved = dup(fileno(stderr));
	if (saved < 0)
		die_errno("dup stderr");

	FILE *cap = fopen("cap.err", "w+");
	if (!cap)
		die_errno("fopen cap.err");
	fflush(stderr);
	if (dup2(fileno(cap), fileno(stderr)) < 0)
		die_errno("dup2 stderr");

	body(arg);
	fflush(stderr);

	if (dup2(saved, fileno(stderr)) < 0)
		die_errno("dup2 restore");
	close(saved);
	fclose(cap);

	FILE *f = fopen("cap.err", "rb");
	if (!f)
		die_errno("fopen cap.err for read");
	struct sbuf sb = SBUF_INIT;
	char buf[512];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		sbuf_add(&sb, buf, n);
	fclose(f);
	{
		char *out = sbuf_detach(&sb);

		squash_crlf(out);
		return out;
	}
}

static void do_flush(void *ud) { kc_report_flush((struct kc_report *)ud); }

int main(void)
{
	/* A fresh report has no messages and no errors. */
	{
		struct kc_report rpt;
		kc_report_init(&rpt);
		tap_check(rpt.count == 0);
		tap_check(rpt.error_count == 0);
		tap_check(kc_report_has_errors(&rpt) == 0);
		kc_report_release(&rpt);
		tap_done("fresh report is empty");
	}

	/* kc_report_error bumps error_count; warning/info do not. */
	{
		struct kc_report rpt;
		kc_report_init(&rpt);
		kc_report_warning(&rpt, "a.kconfig", 1, "warn %d", 1);
		kc_report_error(&rpt, "a.kconfig", 2, "err %s", "X");
		kc_report_info(&rpt, "a.kconfig", 3, "info");
		tap_check(rpt.count == 3);
		tap_check(rpt.error_count == 1);
		tap_check(kc_report_has_errors(&rpt) == 1);
		kc_report_release(&rpt);
		tap_done("error count tracks errors, not warnings/info");
	}

	/* kc_report_flush sorts by file:line and emits expected lines. */
	{
		struct kc_report rpt;
		kc_report_init(&rpt);
		/* Add out of order on purpose. */
		kc_report_warning(&rpt, "b.kconfig", 5, "msg-b5");
		kc_report_warning(&rpt, "a.kconfig", 10, "msg-a10");
		kc_report_warning(&rpt, "a.kconfig", 2, "msg-a2");

		char *out = capture_stderr(do_flush, &rpt);

		const char *want[] = {
		    "warning: a.kconfig:2: msg-a2\n"
		    "warning: a.kconfig:10: msg-a10\n"
		    "warning: b.kconfig:5: msg-b5\n",
		};
		tap_check(strcmp(out, want[0]) == 0);
		free(out);
		kc_report_release(&rpt);
		tap_done("flush sorts by file:line ASC");
	}

	/* NULL-file messages sort before any located messages. */
	{
		struct kc_report rpt;
		kc_report_init(&rpt);
		kc_report_warning(&rpt, "a.kconfig", 1, "with-loc");
		kc_report_warning(&rpt, NULL, 0, "no-loc");

		char *out = capture_stderr(do_flush, &rpt);

		const char *want = "warning: no-loc\n"
				   "warning: a.kconfig:1: with-loc\n";
		tap_check(strcmp(out, want) == 0);
		free(out);
		kc_report_release(&rpt);
		tap_done("NULL-file messages lead the sorted block");
	}

	/* Insertion order is preserved for messages at the same site. */
	{
		struct kc_report rpt;
		kc_report_init(&rpt);
		kc_report_warning(&rpt, "x.kconfig", 7, "first");
		kc_report_warning(&rpt, "x.kconfig", 7, "second");
		kc_report_warning(&rpt, "x.kconfig", 7, "third");

		char *out = capture_stderr(do_flush, &rpt);
		const char *want = "warning: x.kconfig:7: first\n"
				   "warning: x.kconfig:7: second\n"
				   "warning: x.kconfig:7: third\n";
		tap_check(strcmp(out, want) == 0);
		free(out);
		kc_report_release(&rpt);
		tap_done("insertion order preserved for co-located messages");
	}

	/* Severity tiebreaker: errors precede warnings precede info at
	 * the same file:line. */
	{
		struct kc_report rpt;
		kc_report_init(&rpt);
		kc_report_info(&rpt, "x.kconfig", 1, "i");
		kc_report_warning(&rpt, "x.kconfig", 1, "w");
		kc_report_error(&rpt, "x.kconfig", 1, "e");

		char *out = capture_stderr(do_flush, &rpt);
		const char *want = "error: x.kconfig:1: e\n"
				   "warning: x.kconfig:1: w\n"
				   "info: x.kconfig:1: i\n";
		tap_check(strcmp(out, want) == 0);
		free(out);
		kc_report_release(&rpt);
		tap_done("severity breaks ties at the same location");
	}

	return tap_result();
}
