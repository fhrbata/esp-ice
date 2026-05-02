/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file monitor.h
 * @brief Reusable chip-output monitor session.
 *
 * Layered like @ref vt100 and @ref tui_log: a passive widget that
 * holds state and exposes a small set of "feed bytes / feed event /
 * render / drain TX" primitives.  Callers own all I/O (which serial
 * port / pipe / pty produces the chip bytes, where keys come from,
 * how often to render) and pick when to give the session control.
 *
 * Used by @c{ice target monitor} (full screen, owns the terminal),
 * @c{ice qemu} (single pane against qemu's UART pipe), and
 * @c{ice qemu --debug} / @c{ice debug} (dual-pane embedded into a
 * parent layout).  In every shape the session is unchanged; only the
 * caller's loop differs.
 *
 * Lifecycle:
 *   1. @ref monitor_new with config (rect, scrollback, optional
 *      target-side action callbacks).
 *   2. Per tick: @ref monitor_feed_chip with bytes from the chip,
 *      @ref monitor_feed_event with terminal events, drain
 *      @ref monitor_chip_tx into the chip transport.
 *   3. When @ref monitor_dirty: build a frame with @ref monitor_render
 *      and flush to the user terminal; clear the dirty flag.
 *   4. Loop while @ref monitor_should_quit is 0.
 *   5. @ref monitor_release.
 */
#ifndef MONITOR_H
#define MONITOR_H

#include <stddef.h>
#include <stdint.h>

struct sbuf;
struct term_event;

/**
 * @brief Per-session configuration.  Everything is by-value or owned
 * by the caller; the struct itself does not need to outlive @ref
 * monitor_new.
 */
struct monitor_config {
	/** Scrollback ring size in lines.  Must be >= 1. */
	int scrollback;

	/** 1-based screen rect for the session's drawing area.  Modal
	 *  overlays (search, help) clip to this rect, so embedded panes
	 *  get scaled-down modals instead of full-screen ones. */
	int origin_x, origin_y;
	int width, height;

	/** Free-form label shown in the status bar after "ice monitor: ".
	 *  Standalone passes "/dev/ttyUSB0 @ 115200 baud"; embedded
	 *  callers pass whatever's meaningful for their transport (chip
	 *  name, "qemu UART", etc.).  Copied; caller owns the storage. */
	const char *source_label;

	/** Optional target-side action callbacks.  NULL means the action
	 *  isn't available on this transport (qemu has no DTR/RTS, the
	 *  in-app gdb stub controls reset itself, ...): the session omits
	 *  the corresponding Ctrl-T+{r,p} dispatch from the help modal
	 *  and treats the keystroke as a silent prefix-cancel. */
	void (*on_reset)(void *ctx);
	void (*on_bootloader)(void *ctx);
	void *cb_ctx;
};

struct monitor_session;

/**
 * @brief Create a new monitor session.  Returns NULL on allocation
 * failure.  The caller owns the result and must call @ref
 * monitor_release.
 */
struct monitor_session *monitor_new(const struct monitor_config *cfg);

/**
 * @brief Release all resources held by @p m.  Safe on NULL.
 */
void monitor_release(struct monitor_session *m);

/**
 * @brief Re-place / re-size the session within the parent's screen.
 *
 * Cheap to call -- forwards to @c tui_log_resize and the inner
 * @c vt100.  Standalone callers invoke this on every TK_RESIZE;
 * embedded callers invoke this whenever the parent's layout changes
 * (terminal resize, focus toggle that changes pane heights, ...).
 */
void monitor_set_rect(struct monitor_session *m, int origin_x, int origin_y,
		      int width, int height);

/**
 * @brief Feed bytes received from the chip side.
 *
 * The caller is responsible for the actual read (serial port, pipe,
 * pty -- anything that produces a byte stream).  The session feeds
 * the bytes through its inner vt100, drains the device-bound reply
 * into the chip-TX buffer (DSR responses and the like), and pulls
 * scrolled-off rows into the scrollback ring.
 *
 * Sets the dirty flag if the visible frame may have changed.
 */
void monitor_feed_chip(struct monitor_session *m, const uint8_t *buf, size_t n);

/**
 * @brief Feed a parsed terminal event from the user.
 *
 * The session interprets the event according to its current mode --
 * Ctrl-T prefix, normal-mode passthrough, inspect-mode navigation,
 * search-prompt input, help-modal dismissal -- and updates state
 * accordingly.  Bytes destined for the chip (printable input,
 * literal Ctrl-T pass-through after Ctrl-T+Ctrl-T) accumulate in the
 * chip-TX buffer; quit gestures (Ctrl-T+x, Ctrl-]) flip
 * @ref monitor_should_quit.
 *
 * Sets the dirty flag if anything that affects the rendered frame
 * changed.
 */
void monitor_feed_event(struct monitor_session *m, const struct term_event *ev);

/**
 * @brief Bytes the session wants to send back to the chip.
 *
 * The caller drains the returned sbuf in one or more writes to the
 * chip transport, then calls @ref sbuf_reset on it.  Mirrors the
 * @c vt100_reply pattern.  Never NULL; an empty sbuf means nothing
 * to send.
 */
struct sbuf *monitor_chip_tx(struct monitor_session *m);

/**
 * @brief Append the current frame (status bar + body + active modal,
 * if any) to @p out.
 *
 * Cursor positioning is included for the focused (i.e. only) pane;
 * embedded callers that draw chrome after the session render must
 * re-position the cursor themselves using @ref monitor_cursor_row /
 * @ref monitor_cursor_col.
 */
void monitor_render(struct sbuf *out, struct monitor_session *m);

/**
 * @brief Has the user requested termination?
 *
 * Set by Ctrl-T+x and Ctrl-]; never auto-cleared.
 */
int monitor_should_quit(const struct monitor_session *m);

/**
 * @brief Has anything changed since the last @ref monitor_clear_dirty?
 *
 * Cheap predicate for rate-limiting redraws.  Both byte feeds and
 * key events set this; callers reading from a select-based loop can
 * use it to coalesce a burst of bytes into one render.
 */
int monitor_dirty(const struct monitor_session *m);

/**
 * @brief Reset the dirty flag.  Call after rendering.
 */
void monitor_clear_dirty(struct monitor_session *m);

/**
 * @brief Cursor position (in screen coordinates) the session would
 * place the terminal cursor at on the next render.
 *
 * Returns 1 if the cursor is visible (chip didn't issue
 * @c{\e[?25l}, no modal is up), 0 otherwise.  When 1, @p row and
 * @p col are written with 1-based screen rows/columns; when 0 they
 * are untouched and the caller should leave the cursor hidden.
 *
 * Embedded callers use this after their post-pane chrome (divider,
 * footer status bar) has been drawn, since each character emit
 * advances the terminal cursor and would otherwise leave it parked
 * on the footer instead of inside the focused pane.
 */
int monitor_cursor(const struct monitor_session *m, int *row, int *col);

#endif /* MONITOR_H */
