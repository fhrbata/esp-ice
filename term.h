/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file term.h
 * @brief Terminal presentation support (colors, box-drawing).
 *
 * Colored output uses inline tokens in printf format strings:
 *
 *   fprintf(stderr, "@r{fatal}: %s\n", msg);
 *   printf("@g{done} in %d seconds\n", elapsed);
 *
 * Tokens:
 *   @r{...}  red          @R{...}  bold red
 *   @g{...}  green        @G{...}  bold green
 *   @y{...}  yellow       @Y{...}  bold yellow
 *   @b{...}  bold         @c{...}  cyan
 *   @[sgr]{...}     numeric SGR, e.g. @[38;5;208]{orange}
 *   @[name]{...}    named color, e.g. @[COLOR_RED]{red}
 *   @@       literal @
 *   @}       literal } inside a color block
 *
 * The @x{...} tokens are expanded by the platform fprintf/vfprintf
 * overrides (platform/posix/io.c, platform/win/io.c). When color is
 * disabled (piped output or --no-color), tokens are stripped.
 *
 * On Windows, if ENABLE_VIRTUAL_TERMINAL_PROCESSING is available
 * (Windows 10 1511+), ANSI codes are emitted directly. On older
 * Windows, colors are rendered via the Console API
 * (SetConsoleTextAttribute) as a fallback.
 */
#ifndef TERM_H
#define TERM_H

#include <stddef.h>
#include <stdio.h>

/* platform.h is included for the ssize_t typedef on Windows; on POSIX
 * it pulls in <sys/types.h> which provides the native declaration. */
#include "platform.h"

struct sbuf;

/** Global color flag. Set by color_init(), cleared by --no-color. */
extern int use_color;

/**
 * Global VT processing flag. Set when the terminal handles ANSI
 * escape codes natively (POSIX always, Windows 10 1511+).
 * When false and use_color is true, the Console API fallback is used.
 */
extern int use_vt;

/**
 * @brief Initialize color support.
 *
 * Enables color if @p fd refers to a terminal (isatty).
 * On POSIX, also sets use_vt (always true).
 * On Windows, use_vt is set by wmain's setup_io() based on
 * whether ENABLE_VIRTUAL_TERMINAL_PROCESSING succeeds.
 * Call once at startup before any colored output.
 */
void color_init(int fd);

/**
 * @brief Expand @x{...} color tokens in a format string.
 *
 * When @p colorize is non-zero, tokens are replaced with ANSI escape
 * codes.  When zero, tokens are stripped and only the text content
 * remains.  The expanded result is a valid printf format string.
 *
 * Called by the platform fprintf/vfprintf overrides with the
 * per-stream decision (e.g. stdout may be redirected even when
 * stderr is a tty).
 *
 * @param out       Destination sbuf for the expanded format string.
 * @param fmt       Format string with @x{...} tokens.
 * @param colorize  1 -> emit ANSI codes; 0 -> strip tokens.
 */
void expand_colors(struct sbuf *out, const char *fmt, int colorize);

/**
 * @brief Per-stream colorization decision.
 *
 * Returns non-zero when @p stream should receive ANSI codes --
 * i.e. @c use_color is set AND the underlying fd is a tty.
 */
int use_color_for(FILE *stream);

/**
 * @brief Rule for keyword-based colorization in color_text().
 *
 * When a keyword is found in the text, it is wrapped with the
 * corresponding @x{...} color token.
 */
struct color_rule {
	const char *keyword;
	int len;
	const char *color; /* named color, e.g. "COLOR_BOLD_RED" */
};

#define COLOR_RULE(kw, color) {(kw), sizeof(kw) - 1, (color)}

/**
 * @brief Colorize plain text using @x{...} tokens.
 *
 * Wraps recognized patterns with color tokens suitable for
 * expand_colors():
 *   - Keywords from the caller-supplied rule table.
 *   - Quoted strings ('...', `...`, "...") → bold.
 *   - GCC caret ranges (^~~~) → red.
 *   - Standalone numbers (decimal, hex) → cyan.
 *   - Escapes @ and } so the result is safe for expand_colors().
 *
 * @param out    Destination sbuf for tokenized text.
 * @param text   Input buffer (not necessarily NUL-terminated).
 * @param len    Length of @p text in bytes.
 * @param rules  NULL-terminated keyword→color rule table (may be NULL).
 */
void color_text(struct sbuf *out, const char *text, size_t len,
		const struct color_rule *rules);

/* ------------------------------------------------------------------ */
/*  Raw terminal input                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Enter raw terminal mode on stdin.
 *
 * Character-at-a-time, no-echo, no signal generation.  Control
 * characters (Ctrl-C, Ctrl-], ...) arrive as regular bytes / key
 * events.  Output processing is left enabled so '\n' still produces
 * a carriage return.  Registers an atexit handler on first call to
 * restore the original mode.
 *
 * @return 0 on success, -errno on failure (-ENOTTY when stdin is
 *         not a terminal).
 */
int term_raw_enter(void);

/**
 * @brief Leave raw terminal mode.  Safe when not in raw mode (no-op).
 */
void term_raw_leave(void);

/**
 * @brief Read bytes from stdin with a timeout.
 *
 * Returns immediately if @p timeout_ms is 0 and no input is
 * available.  Only meaningful after term_raw_enter().
 *
 * @return bytes read (> 0), 0 on timeout, -1 on error.
 */
ssize_t term_read(void *buf, size_t n, unsigned timeout_ms);

/* ------------------------------------------------------------------ */
/*  Terminal size + resize notification                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Query the current terminal size.
 *
 * @param cols  Out: column count (width).
 * @param rows  Out: row count (height).
 * @return 0 on success, -errno on failure.
 */
int term_size(int *cols, int *rows);

/**
 * @brief Return 1 if a resize has occurred since the last call,
 *        clearing the flag; 0 otherwise.
 *
 * POSIX: the flag is set by a SIGWINCH handler installed on the first
 * call to term_raw_enter().  Windows: the flag is set when
 * term_read() observes a WINDOW_BUFFER_SIZE_EVENT on the console
 * input handle.
 */
int term_resize_pending(void);

/* ------------------------------------------------------------------ */
/*  Input events                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Decoded input event delivered by @ref term_read_event.
 *
 * Printable keys and C0 controls ride in @p key as their byte value
 * (so @c 'y' is @c 'y', Ctrl-C is @c 0x03, bare ESC is @c 0x1b).
 * Named keys use sentinel values in @ref term_key above the byte
 * range.  For @c TK_RESIZE, @p cols / @p rows carry the new size.
 */
struct term_event {
	int key;
	int cols;
	int rows;
};

/**
 * @brief Key codes for non-literal keys.
 *
 * Literal ASCII (printable or C0 control) is delivered as its byte
 * value and does not appear in this enum.  @c TK_CTRL(c) maps a
 * printable letter to its C0 control code so callers can write
 * @c ev.key @c == @c TK_CTRL('c') for Ctrl-C.
 */
enum term_key {
	TK_NONE = 0,
	TK_ENTER = '\r',
	TK_TAB = '\t',
	TK_ESC = 0x1b,
	TK_BACKSPACE = 0x7f,

	TK_UP = 0x100,
	TK_DOWN,
	TK_LEFT,
	TK_RIGHT,
	TK_HOME,
	TK_END,
	TK_PGUP,
	TK_PGDN,
	TK_INS,
	TK_DEL,
	TK_F1,
	TK_F2,
	TK_F3,
	TK_F4,
	TK_F5,
	TK_F6,
	TK_F7,
	TK_F8,
	TK_F9,
	TK_F10,
	TK_F11,
	TK_F12,

	TK_RESIZE,
};

#define TK_CTRL(c) ((c) & 0x1f)

/**
 * @brief Read one input event.
 *
 * Must be called after @ref term_raw_enter.  Resize notifications
 * take priority: if @ref term_resize_pending is set, returns a
 * @c TK_RESIZE event immediately with the new size.  Otherwise reads
 * one byte with @p timeout_ms; on @c ESC drains the rest of the
 * sequence with a short internal timeout and exact-matches against an
 * xterm/VT220 key table.  Unknown escape sequences deliver a bare
 * @c TK_ESC (trailing bytes are dropped).
 *
 * @return 1 when @p ev is populated, 0 on timeout (@c key set to
 *         @c TK_NONE), or -1 on error.
 */
int term_read_event(struct term_event *ev, unsigned timeout_ms);

/* ------------------------------------------------------------------ */
/*  Alternate screen                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Switch to the alternate screen buffer and hide the cursor.
 *
 * For full-screen TUIs.  Pairs with term_screen_leave().  Registers
 * an atexit handler on first call to restore state on exit.  Safe to
 * call while already on the alt screen (no-op).
 */
void term_screen_enter(void);

/**
 * @brief Restore the primary screen and show the cursor.  Safe when
 *        not on the alt screen (no-op).
 */
void term_screen_leave(void);

/* ------------------------------------------------------------------ */
/*  Raw-mode output helpers                                           */
/* ------------------------------------------------------------------ */

/*
 * All helpers below emit ANSI escape sequences directly to stdout
 * with an immediate fflush.  On POSIX the terminal interprets them
 * natively.  On Windows 10+ with VT enabled the path is the same;
 * on legacy conhost, platform/win/io.c's console_write_legacy parses
 * the same sequences and dispatches to Console API calls -- callers
 * stay OS-agnostic.
 *
 * Row / column arguments are 1-based to match the VT100 convention
 * (ESC[1;1H is the top-left cell).
 */

/** @brief Move the cursor to (row, col).  1-based. */
void term_move(int row, int col);

/** @brief Erase from the cursor to the end of the current line. */
void term_clear_to_eol(void);

/** @brief Erase the entire current line.  Cursor position unchanged. */
void term_clear_line(void);

/** @brief Erase the whole screen and home the cursor. */
void term_clear_screen(void);

/**
 * @brief Apply SGR attributes.
 *
 * @p codes is the semicolon-separated parameter string that goes
 * between @c ESC[ and @c m -- e.g. @c "1;37;44" for bold white on
 * blue, @c "0" (or the empty string) to reset all attributes.
 * Passing @c NULL is equivalent to @c "0".
 */
void term_sgr(const char *codes);

/** @brief Reset all SGR attributes.  Shortcut for @c term_sgr("0"). */
void term_sgr_reset(void);

/* Box-drawing characters (UTF-8) matching Rich's heavy-head style. */
#define TL "\xe2\x94\x8f" /* ┏ */
#define TM "\xe2\x94\xb3" /* ┳ */
#define TR "\xe2\x94\x93" /* ┓ */
#define ML "\xe2\x94\xa1" /* ┡ */
#define MM "\xe2\x95\x87" /* ╇ */
#define MR "\xe2\x94\xa9" /* ┩ */
#define BL "\xe2\x94\x94" /* └ */
#define BM "\xe2\x94\xb4" /* ┴ */
#define BR "\xe2\x94\x98" /* ┘ */
#define HH "\xe2\x94\x81" /* ━ heavy horizontal */
#define HL "\xe2\x94\x80" /* ─ light horizontal */
#define VH "\xe2\x94\x83" /* ┃ heavy vertical */
#define VL "\xe2\x94\x82" /* │ light vertical */

#endif /* TERM_H */
