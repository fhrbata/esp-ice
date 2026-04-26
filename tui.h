/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tui.h
 * @brief Small widgets layered on top of the term_* event / output
 *        primitives: a scrollable list, a modal input prompt, a read-
 *        only info modal, and a scrolling log pane.
 *
 * The widgets are domain-agnostic -- items / lines carry no semantics
 * and the caller decides what Enter / Space mean.  Rendering happens
 * inside the alt screen set up by @ref term_screen_enter; callers
 * drive redraws in response to @ref term_read_event events (or, for
 * @ref tui_log, in response to incoming streaming bytes).
 *
 * Render functions write the frame into a caller-owned @c sbuf rather
 * than flushing to stdout themselves.  This lets the caller compose
 * multiple widgets (log + help modal, list + prompt) into one buffer;
 * @ref tui_flush then emits the buffer wrapped in DEC private mode
 * 2026 (synchronized output) so the terminal applies the whole frame
 * atomically.  Without that wrap a single @c fputs is still split
 * across syscalls and refresh cycles, so help / search overlays would
 * briefly flicker as background log content streams in behind them.
 */
#ifndef TUI_H
#define TUI_H

#include "sbuf.h"

struct term_event;

/**
 * @brief Flush a composed frame to stdout and release the buffer.
 *
 * Caller convenience around the @c fputs + @c fflush + @ref sbuf_release
 * triple that closes every render path.  The render functions write
 * into a caller-owned @c sbuf rather than flushing themselves so
 * multiple widgets (log + help modal, list + prompt) can be composed
 * into one buffer; this helper does the final flush in one place.
 */
void tui_flush(struct sbuf *out);

/* ================================================================== */
/*  Scrollable list                                                   */
/* ================================================================== */

/** Not selectable: cursor skips the item.  Still drawn. */
#define TUI_ITEM_DISABLED 0x01

/**
 * Drawn as a section label (dim, left-aligned, no cursor stop).
 * Useful for grouping related entries under a shared heading.
 */
#define TUI_ITEM_HEADING 0x02

struct tui_list_item {
	const char *text;  /**< Left-aligned label; widget does not own. */
	const char *value; /**< Right-aligned value string; NULL for none. */
	const char *value_sgr; /**< Optional SGR params (e.g. "1;32") wrapped
				*   around the value column on non-cursor rows.
				*   NULL keeps the row-level style.  Ignored
				*   when the row is highlighted so the reverse-
				*   video stripe stays consistent. */
	int flags;	       /**< Bitmask of @c TUI_ITEM_* flags. */
	void *userdata;	       /**< Opaque to the widget; caller interprets. */
};

struct tui_list {
	const char *title;  /**< Bar at the top of the frame (may be NULL). */
	const char *footer; /**< Hint row at the bottom (may be NULL). */
	const struct tui_list_item *items;
	int n_items;
	int cursor; /**< Index of highlighted item. */
	int top;    /**< Index of the first visible item (scroll). */
	int width;
	int height;
};

/** @brief Initialise to an empty list, zeroed cursor / scroll. */
void tui_list_init(struct tui_list *L);

/** @brief Replace the backing items.  Resets cursor to the first
 *         selectable item; preserves @c width / @c height. */
void tui_list_set_items(struct tui_list *L, const struct tui_list_item *items,
			int n);

void tui_list_set_title(struct tui_list *L, const char *title);
void tui_list_set_footer(struct tui_list *L, const char *footer);

/** @brief Update frame dimensions.  Call on startup and on
 *         @c TK_RESIZE events.  Adjusts scroll if the new height
 *         would hide the cursor. */
void tui_list_resize(struct tui_list *L, int width, int height);

/**
 * @brief Feed an input event to the list.
 *
 * Consumes navigation keys (Up, Down, PgUp, PgDn, Home, End) --
 * cursor skips over @c TUI_ITEM_HEADING / @c TUI_ITEM_DISABLED items.
 * Does not react to Enter / Space / letter keys; callers handle
 * those against @ref tui_list_current.
 *
 * @return 1 if the event was consumed (caller should redraw),
 *         0 otherwise.
 */
int tui_list_on_event(struct tui_list *L, const struct term_event *ev);

/** @brief Append the full frame to @p out (caller flushes). */
void tui_list_render(struct sbuf *out, const struct tui_list *L);

/** @brief Item under the cursor, or NULL when the list is empty. */
const struct tui_list_item *tui_list_current(const struct tui_list *L);

/* ================================================================== */
/*  Modal input prompt                                                */
/* ================================================================== */

/** Maximum length of the editable buffer (bytes, not including NUL). */
#define TUI_PROMPT_CAP 255

struct tui_prompt {
	const char *title;	      /**< Heading above the input. */
	char buf[TUI_PROMPT_CAP + 1]; /**< NUL-terminated. */
	int len;		      /**< @c strlen(buf). */
	int cursor;		      /**< Byte index into buf. */
	int width;
	int height;
};

/** @brief Set up the prompt with @p title and optional @p initial text. */
void tui_prompt_init(struct tui_prompt *P, const char *title,
		     const char *initial);

void tui_prompt_resize(struct tui_prompt *P, int width, int height);

/**
 * @brief Feed an input event to the prompt.
 *
 * Printable ASCII and C0 letters append; Backspace / Delete edit;
 * arrows move the cursor; Home / End jump.  Enter confirms the
 * current buffer; Esc cancels.
 *
 * @return  1 on Enter -- @c P->buf holds the accepted value.
 *         -1 on Esc   -- caller should discard.
 *          0 otherwise -- caller should redraw and keep looping.
 */
int tui_prompt_on_event(struct tui_prompt *P, const struct term_event *ev);

/** @brief Append the prompt frame to @p out (caller flushes). */
void tui_prompt_render(struct sbuf *out, const struct tui_prompt *P);

/* ================================================================== */
/*  Read-only scrollable info box                                     */
/* ================================================================== */

/**
 * @brief Scrollable read-only text modal.
 *
 * Used for help popups and similar "show this blob of text" UX.  The
 * body is split into lines at parse-time; the caller owns both the
 * @p title and @p body strings (the widget stores pointers into the
 * body, so it must outlive the widget).  Navigation keys scroll the
 * visible window; Esc / Enter / q request a close.
 */
struct tui_info {
	const char *title;
	const char *body;   /**< Caller-owned; split into lines by init. */
	const char **lines; /**< Pointers into @c body, one per line. */
	int *line_lens;	    /**< Byte length of each line (newline stripped). */
	int n_lines;
	int top_line; /**< Index of the first visible line. */
	int width;
	int height;
};

/**
 * @brief Initialise from @p title and @p body.
 *
 * Splits @p body on @c \n into an internal line array.  The caller
 * must keep @p body alive for the widget's lifetime (the lines point
 * into it).  @ref tui_info_release frees the line array.
 */
void tui_info_init(struct tui_info *I, const char *title, const char *body);

/** @brief Free the widget's internal line array. */
void tui_info_release(struct tui_info *I);

void tui_info_resize(struct tui_info *I, int width, int height);

/**
 * @brief Feed an input event.
 *
 * Up / Down / PgUp / PgDn / Home / End scroll.  Esc / Enter / q
 * request a close (return 1).  Everything else returns 0 for the
 * caller to keep looping.
 *
 * @return 1 on close, 0 otherwise.
 */
int tui_info_on_event(struct tui_info *I, const struct term_event *ev);

/** @brief Append the info-modal frame to @p out (caller flushes). */
void tui_info_render(struct sbuf *out, const struct tui_info *I);

/* ================================================================== */
/*  Scrolling log pane                                                */
/* ================================================================== */

/**
 * @brief Per-byte-range SGR overlay applied to a log line at render time.
 *
 * Each overlay wraps @c [start, end) of a line with @c sgr_open /
 * @c sgr_close (semicolon-separated SGR parameter strings, e.g. @c "31"
 * or @c "0;1;33" -- the renderer wraps them in @c \x1b[...m).  Decorators
 * (level coloring, search highlight, rule-based keyword coloring)
 * populate an array of these structs for the renderer to consume; the
 * renderer never inspects the meaning of the SGR codes.
 *
 * Ordering: overlays must be sorted by @c start ascending; on ties,
 * larger @c end first (so the outer overlay opens before any nested
 * overlay sharing its start).  The renderer maintains a stack of
 * currently-active overlays and closes them in LIFO order, so
 * @b{nested} ranges (e.g. a search highlight inside a whole-line level
 * color) and @b{disjoint} ranges (e.g. several search matches on the
 * same line) both compose correctly.  @b{Non-nested overlapping} -- two
 * ranges that interleave without containment -- breaks the LIFO close
 * model and is not supported; producers must split such overlaps into
 * disjoint pieces if they really need them.
 *
 * SGR composition: after every overlay close the renderer re-issues
 * each still-active overlay's @c sgr_open in stack order, and after
 * every hard wrap mid-overlay it re-issues every active overlay's
 * @c sgr_open at the new row's start.  Both cases mean callers can
 * use any @c sgr_close they want -- including a full reset @c "0" --
 * without erasing outer overlays' styles.  The cost is a few extra
 * SGR sequences per close, which is negligible compared to the
 * single-fputs-per-frame envelope the widget already lives in.
 *
 * The @c sgr_open / @c sgr_close pointers must outlive the render call;
 * the typical pattern is to point at static string literals.
 */
struct tui_overlay {
	size_t start;	       /**< Byte offset, inclusive. */
	size_t end;	       /**< Byte offset, exclusive. */
	const char *sgr_open;  /**< SGR params asserted at @c start. */
	const char *sgr_close; /**< SGR params asserted at @c end. */
};

struct tui_log;
struct tui_search; /* opaque, defined in tui.c */

/**
 * @brief Per-line decorator callback consulted at render time.
 *
 * Called by @ref tui_log_render for each visible line.  Fills @p out
 * with at most @p max overlays sorted by @c start and pairwise non-
 * overlapping; returns the count produced (0 means "no decoration",
 * the line renders verbatim).  Decorators are stateless from the
 * widget's point of view -- any state they need lives behind the
 * @c ctx pointer registered alongside.
 *
 * Common patterns:
 *   - Inspect the line's leading bytes for a domain-specific prefix
 *     (e.g. ESP-IDF's E/W/I/D/V level letter) and emit a single
 *     whole-line overlay carrying the level colour.
 *   - Run a regex against the line and emit one overlay per match
 *     (search highlight).
 *
 * Decorators must not allocate heap memory per call -- they can rely
 * on the renderer providing @p out as a stack buffer of fixed size.
 */
typedef int (*tui_log_decorate_fn)(const char *line, size_t len, void *ctx,
				   struct tui_overlay *out, int max);

/**
 * @brief Append-only scrollback log pane with vim/less-style scrolling.
 *
 * Backs a fixed-size ring buffer of completed lines plus an in-progress
 * line being built up byte by byte.  By default the body auto-tails:
 * each frame shows the most recent visual rows that fit.  When the
 * caller-driven event handler accepts @c TK_PGUP / @c TK_HOME the
 * widget switches into "frozen" mode -- the visible content stays
 * anchored on a specific log line as new bytes arrive in the
 * background, exactly the way @c less and @c vim's terminal handle
 * scroll-back.  @c TK_END returns to live tailing.
 *
 * The anchor is a monotonic global line index, not an offset from the
 * tail, so eviction (when the ring fills up) shifts the user-visible
 * indices correctly without smearing the user's view.  If the
 * anchored line itself is evicted the view clamps to the oldest
 * surviving line.
 *
 * Embedded ANSI SGR sequences are passed through to the terminal but
 * do not count toward visible column width, so colored output renders
 * with correct wrapping.  Lines longer than @c width hard-wrap onto
 * the next visual row; SGR is reset at every wrap boundary so colour
 * does not bleed across rows.
 *
 * Layout (1-based rows):
 *   row 1                   : @c status bar (when set; bold white-on-blue)
 *   row 2..@c height-1      : log body, last column reserved for the
 *                             scrollbar track / thumb
 *   row @c height           : @c footer hint (when set; white-on-blue)
 *
 * If @c status is NULL the body extends up to row 1.  If @c footer is
 * NULL the body extends down to row @c height.
 */
struct tui_log {
	char **lines;  /**< Ring of completed lines (heap strings). */
	int cap_lines; /**< Ring capacity (oldest evicted on overflow). */
	int n_lines;   /**< Lines currently in the ring (0..cap_lines). */
	int start;     /**< Ring index of the oldest line. */

	struct sbuf pending; /**< In-progress line, no @c '\\n' yet. */

	/* Monotonic counter of completed lines ever pushed -- never
	 * reset, increases past @c cap_lines once eviction kicks in.
	 * Pairs with @c anchor to translate scroll position to a ring
	 * slot in a way that survives eviction. */
	long long total_pushed;

	/* Bottom-of-body anchor as a global line index.  -1 means
	 * "follow tail": the widget auto-scrolls so the newest line
	 * (or @c pending) sits at the bottom of the body.  When >= 0,
	 * the widget freezes the view at that global line. */
	long long anchor;

	/* Snapshot ceiling as a global line index (exclusive).  -1
	 * means no ceiling and the whole ring (plus pending) is in
	 * play.  When set, render / search / counters all clip to
	 * lines with @c global_idx < @c ceiling -- callers use this to
	 * present a frozen view of "the buffer at the moment I entered
	 * this state" while @ref tui_log_append keeps stuffing new
	 * bytes into the ring in the background.  Lines that fall
	 * within the ceiling but have since been evicted from the ring
	 * simply stop being visible (the ceiling doesn't pin them in
	 * memory); long inspect sessions on a high-throughput stream
	 * can therefore see the snapshot's oldest lines disappear. */
	long long ceiling;

	int width;
	int height;

	const char *status; /**< Top status bar; widget does not own. */
	const char *footer; /**< Bottom hint; widget does not own. */

	tui_log_decorate_fn decorate_fn; /**< Per-line overlay producer;
					  *   NULL renders verbatim. */
	void *decorate_ctx;		 /**< Opaque pointer passed to
					  *   @c decorate_fn. */

	struct tui_search *search; /**< Active search state, or NULL. */
};

/**
 * @brief Initialise an empty log with @p max_lines of scrollback.
 *
 * Allocates the ring; pair with @ref tui_log_release.  @p max_lines
 * must be > 0.  Callers typically pick a few thousand -- once on the
 * alt screen the terminal's native scrollback is gone, so the ring is
 * the only history users can navigate.
 */
void tui_log_init(struct tui_log *L, int max_lines);

/** @brief Free the line ring and pending buffer.  Safe to call twice. */
void tui_log_release(struct tui_log *L);

/** @brief Update frame dimensions.  Call on startup and on
 *         @c TK_RESIZE events. */
void tui_log_resize(struct tui_log *L, int width, int height);

/**
 * @brief Freeze the visible / searchable range to @c [oldest, total_pushed).
 *
 * Records the current @c L->total_pushed as the ceiling, so subsequent
 * renders, searches, and counters behave as if no lines exist past
 * that point even though @ref tui_log_append keeps stuffing new bytes
 * into the ring.  The pending line is excluded from the snapshot --
 * its bytes are still being assembled and would shift under the user.
 * Sets @c anchor to the line at @c ceiling-1 so the bottom of the
 * body lines up with what was visible at the moment of the call.
 */
void tui_log_freeze(struct tui_log *L);

/** @brief Drop any active ceiling.  The full ring (plus pending) is
 *         visible again; @c anchor is reset to live-tail (-1). */
void tui_log_unfreeze(struct tui_log *L);

/** @brief Non-zero when a snapshot ceiling is active. */
int tui_log_is_frozen(const struct tui_log *L);

void tui_log_set_status(struct tui_log *L, const char *status);
void tui_log_set_footer(struct tui_log *L, const char *footer);

/**
 * @brief Register a per-line overlay producer.
 *
 * @p fn is called once per visible line during @ref tui_log_render
 * and may emit SGR overlays for that line.  Pass @c fn = @c NULL to
 * disable decoration; the widget then renders bytes verbatim.  @p ctx
 * is forwarded to @c fn unchanged.
 */
void tui_log_set_decorator(struct tui_log *L, tui_log_decorate_fn fn,
			   void *ctx);

/**
 * @brief Append @p n bytes of streaming output.
 *
 * Splits on @c '\\n' into completed lines (a trailing @c '\\r' is
 * stripped so CRLF input renders as plain lines); bytes between
 * newlines accumulate in @c pending until the next @c '\\n' arrives.
 * When the ring is full the oldest line is evicted.  Embedded ANSI
 * sequences are stored verbatim and re-emitted at render time.
 */
void tui_log_append(struct tui_log *L, const void *data, size_t n);

/**
 * @brief Feed a navigation event.
 *
 * Recognises @c TK_PGUP / @c TK_PGDN (one body of scrolling),
 * @c TK_HOME (jump to oldest line), @c TK_END (resume tailing).
 * Other events are ignored so callers can layer their own bindings on
 * top.  @c TK_RESIZE is intentionally not consumed -- the caller
 * already owns terminal sizing via @ref tui_log_resize.
 *
 * @return 1 if the event was consumed (caller should redraw),
 *         0 otherwise.
 */
int tui_log_on_event(struct tui_log *L, const struct term_event *ev);

/** @brief Non-zero when the widget is in live-tail mode. */
int tui_log_is_tailing(const struct tui_log *L);

/**
 * @brief Append the full pane frame to @p out (caller flushes).
 *
 * Body fills with the most recent visual rows that fit (when tailing)
 * or with the rows around the current anchor (when scrolled).  The
 * status, footer, and scrollbar are repainted on every call.  When a
 * search is active every visible match is highlighted with a reverse-
 * video overlay layered on top of any decorator-supplied colours.
 */
void tui_log_render(struct sbuf *out, const struct tui_log *L);

/* ------------------------------------------------------------------ */
/*  Search                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Set or replace the active search pattern.
 *
 * Compiles @p pattern as a PCRE2 regex (multiline + dotall) and stores
 * it on the widget; subsequent renders highlight every match in each
 * visible line.  Pass @p pattern == NULL or "" to clear (equivalent to
 * @ref tui_log_search_clear).  On compile failure the previous search
 * (if any) is left untouched and -1 is returned -- callers can show a
 * brief error and let the user fix the pattern.
 *
 * @return 0 on success, -1 on PCRE2 compile error.
 */
int tui_log_search_set(struct tui_log *L, const char *pattern);

/** @brief Clear any active search. */
void tui_log_search_clear(struct tui_log *L);

/** @brief Non-zero when a search is currently active. */
int tui_log_search_active(const struct tui_log *L);

/** @brief The active pattern (raw, as the user typed it), or NULL. */
const char *tui_log_search_pattern(const struct tui_log *L);

/**
 * @brief Total matches currently in the buffer for the active pattern.
 *
 * Maintained incrementally as lines are appended and evicted, so the
 * accessor is O(1).  Returns 0 when no search is active.
 */
int tui_log_search_total(const struct tui_log *L);

/**
 * @brief 1-based ordinal of the current match, or 0 if no current.
 *
 * The "current" match is whichever one the most recent
 * @ref tui_log_search_set / @ref tui_log_search_next /
 * @ref tui_log_search_prev call landed on.
 */
int tui_log_search_index(const struct tui_log *L);

/**
 * @brief Move the anchor to the next/previous matching line.
 *
 * "Next" walks toward newer lines starting from the line currently at
 * the bottom of the body; "previous" walks toward older lines.  When a
 * match is found the anchor snaps to that line (callers should redraw
 * after).  Pending content is skipped -- only completed lines are
 * search targets.
 *
 * @return 1 if a match was reached, 0 if no match was found between
 *         the current position and the relevant end of the buffer.
 */
int tui_log_search_next(struct tui_log *L);
int tui_log_search_prev(struct tui_log *L);

#endif /* TUI_H */
