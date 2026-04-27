/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file vt100.h
 * @brief Inner-terminal vt100 emulator -- platform-free pure C.
 *
 * vt100 sits between the chip's serial byte stream and tui.c's
 * composer.  It feeds bytes through a Paul Williams DEC-ANSI parser
 * state machine, mutates an in-memory cell grid, and accumulates any
 * device-reply bytes (DSR, etc.) into a reply sbuf the caller drains
 * back to the chip.
 *
 * Phase 2 scope: real putc / cursor / clears / SGR.  Implicit scroll
 * past the bottom margin is still phase 3 (the cursor clamps for now);
 * DSR synth is phase 4; renderer integration is phase 6.
 *
 * Usage:
 *   struct vt100 *V = vt100_new(rows, cols);
 *   vt100_input(V, buf, n);
 *   struct sbuf *r = vt100_reply(V);
 *   if (r->len) { write_back(r->buf, r->len); sbuf_reset(r); }
 *   const struct vt100_cell *cell = vt100_cell(V, row, col);
 *   vt100_free(V);
 *
 * No I/O, no syscalls, no #ifdef -- like sbuf and yaml at the root.
 */
#ifndef VT100_H
#define VT100_H

#include <stddef.h>
#include <stdint.h>

#include "sbuf.h"

struct vt100; /* opaque */

/* ------------------------------------------------------------------ */
/*  Cell + SGR types                                                  */
/* ------------------------------------------------------------------ */

/** Sentinel "no explicit color" used for unset fg/bg in @ref vt100_sgr. */
#define VT100_DEFAULT_COLOR 0xff

/** SGR attribute bits, packed into @ref vt100_sgr::attrs. */
#define VT100_ATTR_BOLD (1u << 0)
#define VT100_ATTR_UNDERLINE (1u << 1)
#define VT100_ATTR_REVERSE (1u << 2)

/**
 * @brief Per-cell graphic-rendition state.
 *
 * Phase 2 supports basic 16-color SGR plus bold / underline / reverse.
 * 256-color and truecolor are deferred (see docs/ice-monitor-vt100.md
 * "Non-goals").
 */
struct vt100_sgr {
	uint8_t fg;    /**< 0-15 = ANSI color; VT100_DEFAULT_COLOR if unset. */
	uint8_t bg;    /**< 0-15 = ANSI color; VT100_DEFAULT_COLOR if unset. */
	uint8_t attrs; /**< Bitmask of VT100_ATTR_*. */
	uint8_t _pad;  /**< Pads the struct to 4 bytes. */
};

/**
 * @brief A single grid cell.
 *
 * A cell with @c cp == 0 renders as blank; the @c sgr field still
 * applies (so an erased region carries the SGR active at erase time
 * and the bg color paints through).
 */
struct vt100_cell {
	uint32_t cp;	      /**< Unicode codepoint; 0 = blank. */
	struct vt100_sgr sgr; /**< Graphic-rendition state. */
};

/* ------------------------------------------------------------------ */
/*  Counters (test observability)                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Counters for byte classification, exposed for unit tests.
 *
 * Bumped from the dispatch entry points.  Useful for asserting that a
 * captured byte stream classifies into the expected mix of printable /
 * control / escape sequences alongside the new grid-state assertions.
 */
struct vt100_counters {
	unsigned long printable; /**< printable bytes printed in GROUND. */
	unsigned long c0;	 /**< C0 controls executed. */
	unsigned long csi;	 /**< CSI sequences dispatched. */
	unsigned long osc;	 /**< OSC strings dispatched. */
	unsigned long esc;	 /**< non-CSI / non-OSC ESC sequences. */
};

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/** Create a vt100 with a @p rows * @p cols grid.  Dies on OOM. */
struct vt100 *vt100_new(int rows, int cols);

/** Free a vt100 and everything it owns.  NULL-safe. */
void vt100_free(struct vt100 *V);

/**
 * @brief Resize the grid to @p rows * @p cols.
 *
 * Phase-2 policy: blank a fresh grid.  Parser state, reply buffer,
 * counters, SGR, and saved-cursor are preserved across resize; the
 * live cursor is clamped into the new dimensions.
 */
void vt100_resize(struct vt100 *V, int rows, int cols);

/* ------------------------------------------------------------------ */
/*  Byte input                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Feed @p n bytes of chip output into the parser.
 *
 * Pure data-in / data-out: no I/O, no callbacks.  A multi-byte
 * sequence split across two @c vt100_input invocations dispatches
 * exactly once; parser state is preserved between calls.
 */
void vt100_input(struct vt100 *V, const void *buf, size_t n);

/**
 * @brief Borrow a pointer to the accumulated reply bytes.
 *
 * Pointer is stable for the lifetime of @p V and is never NULL.
 * Caller writes @c r->buf[0..r->len] back over the serial connection
 * and then calls @c sbuf_reset(r).  Phase 2 never appends here -- DSR
 * synth lands in phase 4.
 */
struct sbuf *vt100_reply(struct vt100 *V);

/* ------------------------------------------------------------------ */
/*  Grid + cursor accessors                                           */
/* ------------------------------------------------------------------ */

/** Grid rows. */
int vt100_rows(const struct vt100 *V);

/** Grid columns. */
int vt100_cols(const struct vt100 *V);

/**
 * @brief Borrow a pointer to the cell at (@p row, @p col).
 *
 * Returns NULL if (row, col) is out of range.
 */
const struct vt100_cell *vt100_cell(const struct vt100 *V, int row, int col);

/** Cursor row (0-based).  Always within [0, rows). */
int vt100_cursor_row(const struct vt100 *V);

/** Cursor column (0-based).  Always within [0, cols). */
int vt100_cursor_col(const struct vt100 *V);

/** Whether the cursor is currently visible (DECTCEM ?25). */
int vt100_cursor_visible(const struct vt100 *V);

/**
 * @brief Whether the cursor is in deferred-wrap state.
 *
 * Set when a printable byte just landed in the rightmost column; the
 * actual wrap fires on the next printable byte (or is cleared by the
 * next cursor-positioning op).
 */
int vt100_pending_wrap(const struct vt100 *V);

/**
 * @brief Top row of the active scroll region (0-based, inclusive).
 *
 * Default is 0; set by DECSTBM (`ESC [ <t> ; <b> r`).
 */
int vt100_scroll_top(const struct vt100 *V);

/**
 * @brief Bottom row of the active scroll region (0-based, inclusive).
 *
 * Default is rows-1; set by DECSTBM.
 */
int vt100_scroll_bottom(const struct vt100 *V);

/**
 * @brief Alt-screen mode flag (DECSET ?47, ?1047, ?1049).
 *
 * Phase 3 just tracks the bit; no second grid is allocated.  When set,
 * scroll-up does not enqueue rows into the scrolled-off queue
 * (alt-screen content is ephemeral).
 */
int vt100_alt_screen(const struct vt100 *V);

/* ------------------------------------------------------------------ */
/*  Scrolled-off rows (live grid → scrollback bridge)                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Number of rows queued in the scrolled-off list.
 *
 * Rows enter the list when a full-screen scroll-up evicts content from
 * the top of the grid (via implicit scroll on cursor-past-bottom or
 * explicit SU).  Partial scroll regions and alt-screen do not enqueue.
 */
int vt100_scrolled_count(const struct vt100 *V);

/**
 * @brief Borrow a pointer to the @p i-th scrolled-off row's cells.
 *
 * Out-param @p cols (may be NULL) receives the row width at the time
 * of scroll-off (since vt100_resize between scrolls can change it).
 * Returns NULL if @p i is out of range.
 */
const struct vt100_cell *vt100_scrolled_row(const struct vt100 *V, int i,
					    int *cols);

/**
 * @brief Drop every queued scrolled-off row and reset the count.
 *
 * Caller (typically @c tui.c) drains after materialising rows into its
 * scrollback ring on each render tick.
 */
void vt100_drain_scrolled(struct vt100 *V);

/* ------------------------------------------------------------------ */
/*  Cell row → ANSI byte string                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Serialize @p cols cells into @p out as ANSI-decorated bytes.
 *
 * Trailing blank cells (cp == 0) are trimmed.  Embedded blank cells
 * become spaces.  SGR transitions emit @c "\\x1b[0;...m" sequences; if
 * any non-default attribute was emitted, the row ends with @c
 * "\\x1b[0m" so colour does not bleed past the line.
 *
 * Output is suitable for storage in @c tui_log's line ring or for
 * direct emission to a VT-capable terminal.  Phase 5 only: codepoints
 * >= 0x80 currently fall back to '?'; UTF-8 encoding is deferred until
 * a chip program exercises it (see "Non-goals" in
 * docs/ice-monitor-vt100.md).
 */
void vt100_serialize_row(struct sbuf *out, const struct vt100_cell *cells,
			 int cols);

/**
 * @brief True when the parser is at rest (GROUND state).
 *
 * After feeding a complete byte stream, an idle parser indicates no
 * half-consumed sequences remain.
 */
int vt100_idle(const struct vt100 *V);

/** Borrow a pointer to the byte-classification counters. */
const struct vt100_counters *vt100_counters(const struct vt100 *V);

#endif /* VT100_H */
