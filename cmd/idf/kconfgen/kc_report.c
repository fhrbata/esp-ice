/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_report.c
 * @brief Deferred diagnostic reporting for the Kconfig processor.
 *
 * Ported from the cconfig/ prototype: accumulate non-fatal messages
 * during parse/eval/I/O, then flush them sorted so the user sees
 * problems grouped by source location rather than interleaved with
 * work the processor is doing.
 */
#include "kc_report.h"
#include "ice.h"

#include <stdarg.h>

void kc_report_init(struct kc_report *rpt) { memset(rpt, 0, sizeof(*rpt)); }

void kc_report_release(struct kc_report *rpt)
{
	for (size_t i = 0; i < rpt->count; i++)
		free(rpt->msgs[i].text);
	free(rpt->msgs);
	memset(rpt, 0, sizeof(*rpt));
}

static void report_add(struct kc_report *rpt, enum kc_report_level level,
		       const char *file, int line, const char *fmt, va_list ap)
{
	struct sbuf sb = SBUF_INIT;
	struct kc_report_msg *msg;

	ALLOC_GROW(rpt->msgs, rpt->count + 1, rpt->alloc);

	sbuf_vaddf(&sb, fmt, ap);

	msg = &rpt->msgs[rpt->count];
	msg->level = level;
	msg->file = file;
	msg->line = line;
	msg->text = sbuf_detach(&sb);
	msg->seq = rpt->count;
	rpt->count++;

	if (level == KC_REPORT_ERROR)
		rpt->error_count++;
}

void kc_report_error(struct kc_report *rpt, const char *file, int line,
		     const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	report_add(rpt, KC_REPORT_ERROR, file, line, fmt, ap);
	va_end(ap);
}

void kc_report_warning(struct kc_report *rpt, const char *file, int line,
		       const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	report_add(rpt, KC_REPORT_WARNING, file, line, fmt, ap);
	va_end(ap);
}

void kc_report_info(struct kc_report *rpt, const char *file, int line,
		    const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	report_add(rpt, KC_REPORT_INFO, file, line, fmt, ap);
	va_end(ap);
}

/*
 * qsort cmp: file ASC, line ASC, severity ASC (errors before
 * warnings before info), then insertion order.  NULL @c file sorts
 * before any path so context-free diagnostics lead the block.
 */
static int msg_cmp(const void *va, const void *vb)
{
	const struct kc_report_msg *a = va;
	const struct kc_report_msg *b = vb;
	int r;

	if (a->file && b->file) {
		r = strcmp(a->file, b->file);
		if (r)
			return r;
	} else if (!a->file && b->file) {
		return -1;
	} else if (a->file && !b->file) {
		return 1;
	}

	if (a->line < b->line)
		return -1;
	if (a->line > b->line)
		return 1;

	if (a->level < b->level)
		return -1;
	if (a->level > b->level)
		return 1;

	if (a->seq < b->seq)
		return -1;
	if (a->seq > b->seq)
		return 1;

	return 0;
}

static const char *level_str(enum kc_report_level level)
{
	switch (level) {
	case KC_REPORT_ERROR:
		return "error";
	case KC_REPORT_WARNING:
		return "warning";
	case KC_REPORT_INFO:
		return "info";
	}
	return "unknown";
}

int kc_report_flush(struct kc_report *rpt)
{
	int errors = 0;

	qsort(rpt->msgs, rpt->count, sizeof(rpt->msgs[0]), msg_cmp);

	for (size_t i = 0; i < rpt->count; i++) {
		const struct kc_report_msg *msg = &rpt->msgs[i];

		if (msg->level == KC_REPORT_ERROR)
			errors++;

		if (msg->file)
			fprintf(stderr, "%s: %s:%d: %s\n",
				level_str(msg->level), msg->file, msg->line,
				msg->text);
		else
			fprintf(stderr, "%s: %s\n", level_str(msg->level),
				msg->text);
	}

	return errors;
}

int kc_report_has_errors(const struct kc_report *rpt)
{
	return rpt->error_count > 0;
}
