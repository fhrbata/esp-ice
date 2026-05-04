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
struct vt100;	   /* defined in vt100.h */
struct vt100_cell; /* defined in vt100.h */

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
/*  Rectangles + layout helpers                                       */
/* ================================================================== */

/**
 * @brief A 1-based axis-aligned rectangle on the terminal grid.
 *
 * Used to express "this widget occupies rows @c y..y+h-1, columns
 * @c x..x+w-1."  Each widget has its own @c origin_x / @c origin_y
 * and @c width / @c height; @ref tui_rect groups them so layout code
 * stays compact when computing splits.
 */
struct tui_rect {
	int x; /**< 1-based leftmost column. */
	int y; /**< 1-based topmost row. */
	int w; /**< Width in cells (>= 0). */
	int h; /**< Height in rows (>= 0). */
};

/**
 * @brief Split @p parent into a left and a right rectangle.
 *
 * @p left_w columns go to @p left; the remaining columns go to @p right.
 * Negative or out-of-range @p left_w is clamped to @c [0, parent->w].
 * The two output rectangles together cover @p parent exactly with no
 * gap; if a divider column is desired, the caller subtracts it from
 * @p left_w (or from one of the outputs) and draws it in the freed
 * cell.  Both outputs are filled even when one ends up zero-width;
 * callers can detect that and skip rendering that pane.
 */
void tui_rect_split_v(const struct tui_rect *parent, struct tui_rect *left,
		      struct tui_rect *right, int left_w);

/**
 * @brief Split @p parent into a top and a bottom rectangle.
 *
 * Mirror of @ref tui_rect_split_v: @p top_h rows go to @p top, the rest
 * to @p bottom.  Same clamping and "covers exactly" rules apply.
 */
void tui_rect_split_h(const struct tui_rect *parent, struct tui_rect *top,
		      struct tui_rect *bottom, int top_h);

/**
 * @brief Carve margins off all sides of @p r.
 *
 * Returns @p r shrunk by @p top rows from the top, @p bottom rows from
 * the bottom, @p left columns from the left, and @p right columns from
 * the right.  Useful for reserving a 1-cell border or a divider row.
 * If the margins exceed the rectangle's dimensions, the result has
 * @c w or @c h clamped to 0.
 */
struct tui_rect tui_rect_inset(struct tui_rect r, int top, int right,
			       int bottom, int left);

/**
 * @brief Paint a one-row status bar across @p r with @p text.
 *
 * Emits a cursor jump to @p r->y / @p r->x, switches to @p sgr (e.g.
 * @c "1;37;44" for bold white-on-blue), prints a leading space, then
 * @p text truncated or right-padded to fit @p r->w.  SGR is reset
 * before returning.  No-op when @p r is empty.
 *
 * Used by @c{ice monitor}, @c{ice qemu}, and @c{ice qemu --debug}
 * to draw their cmd-level top status row outside the @ref tui_log
 * widget's own self-rendered status row -- the cmd row carries
 * cmd-prefix shortcuts (h=help, x=exit, ...) while the widget row
 * carries widget-prefix shortcuts (i=inspect, ?=help, ...).
 */
void tui_status_bar(struct sbuf *out, const struct tui_rect *r,
		    const char *text, const char *sgr);

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
	int cursor;   /**< Index of highlighted item. */
	int top;      /**< Index of the first visible item (scroll). */
	int origin_x; /**< 1-based leftmost column on the screen. */
	int origin_y; /**< 1-based topmost row on the screen. */
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

/** @brief Place the widget's top-left cell at terminal column @p x,
 *         row @p y (both 1-based).  Default after @ref tui_list_init
 *         is (1, 1) -- the entire screen.  Call before rendering when
 *         composing two or more widgets side by side. */
void tui_list_set_origin(struct tui_list *L, int x, int y);

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
	int origin_x; /**< 1-based leftmost column on the screen. */
	int origin_y; /**< 1-based topmost row on the screen. */
	int width;
	int height;
};

/** @brief Set up the prompt with @p title and optional @p initial text. */
void tui_prompt_init(struct tui_prompt *P, const char *title,
		     const char *initial);

void tui_prompt_resize(struct tui_prompt *P, int width, int height);

/** @brief Place the modal box's reference rectangle at terminal column
 *         @p x, row @p y (both 1-based).  The widget centres its box
 *         within the @c width x @c height rectangle anchored at the
 *         origin.  Default after @ref tui_prompt_init is (1, 1). */
void tui_prompt_set_origin(struct tui_prompt *P, int x, int y);

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
	int origin_x; /**< 1-based leftmost column on the screen. */
	int origin_y; /**< 1-based topmost row on the screen. */
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

/** @brief Place the modal box's reference rectangle at terminal column
 *         @p x, row @p y (both 1-based).  The widget centres its box
 *         within the @c width x @c height rectangle anchored at the
 *         origin.  Default after @ref tui_info_init is (1, 1). */
void tui_info_set_origin(struct tui_info *I, int x, int y);

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
/*
 * Inspect-mode sub-state, used by the embedded state machine inside
 * @ref tui_log.  @c TUI_INSPECT_LIVE means the log is dormant and
 * keys flow through unmodified; the other values gate the freeze
 * snapshot, the regex search prompt, and the keymap-help modal.
 * Most callers don't touch the enum directly -- they read
 * @c L->inspect_active to drive their status bar.
 */
enum tui_inspect_mode {
	TUI_INSPECT_LIVE = 0, /* dormant; log is not frozen */
	TUI_INSPECT_NAV,      /* frozen; nav / vim shortcuts active */
	TUI_INSPECT_SEARCH,   /* search prompt is up */
	TUI_INSPECT_HELP,     /* help modal is up */
};

/*
 * Where the widget paints its self-rendered status row inside its
 * own rect.  Pick on init via @ref tui_log_init.
 *
 *   @c TUI_LOG_STATUS_NONE     no status row -- body fills the rect.
 *   @c TUI_LOG_STATUS_TOP      status row at @c origin_y, body
 *                              starts one row down.
 *   @c TUI_LOG_STATUS_BOTTOM   status row at @c origin_y+height-1,
 *                              body ends one row above.
 *
 * Single-pane commands typically pick @c TOP (the row reads as the
 * window title); multi-pane layouts typically pick @c BOTTOM on each
 * pane so the host's own top status bar stays visually distinct from
 * the per-pane chrome.
 */
enum tui_log_status_pos {
	TUI_LOG_STATUS_NONE = 0,
	TUI_LOG_STATUS_TOP,
	TUI_LOG_STATUS_BOTTOM,
};

/*
 * Init-time flags controlling status-bar chrome the widget paints.
 * Currently a single bit: when @c TUI_LOG_SHOW_EXIT is set the live-
 * mode hint includes @c x=exit, telling the user the cmd quits on
 * @c Ctrl-T+x.  Single-pane hosts (monitor, qemu) typically set it;
 * multi-pane hosts (qemu --debug) leave it clear because their own
 * top-level status row already advertises the quit binding.
 */
#define TUI_LOG_SHOW_EXIT (1u << 0)

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

	int origin_x; /**< 1-based leftmost column on the screen. */
	int origin_y; /**< 1-based topmost row on the screen. */
	int width;
	int height;

	/* ---- Self-rendered status row.
	 *
	 * The cmd hands in a static identity portion (@c status_prefix)
	 * at init time and never touches it again.  The widget composes
	 * the visible row at render time as
	 *
	 *     <prefix>  *  <[INSPECT badge]>  *  <key hints>
	 *
	 * so the dynamic part (mode badge, search counter, in-inspect
	 * hints) lives entirely inside the widget.  @c status_pos
	 * controls whether the row sits at the top, bottom, or is
	 * suppressed entirely. */
	const char *status_prefix; /**< Identity portion; not owned. */
	int status_pos;		   /**< @ref tui_log_status_pos. */
	unsigned status_flags;	   /**< Bitmask of @c TUI_LOG_* flags. */

	/* Optional bottom-of-rect hint, separate from the status row.
	 * Currently unused by the in-tree commands; kept for callers
	 * that want a second chrome row. */
	const char *footer;

	tui_log_decorate_fn decorate_fn; /**< Per-line overlay producer;
					  *   NULL renders verbatim. */
	void *decorate_ctx;		 /**< Opaque pointer passed to
					  *   @c decorate_fn. */

	struct tui_search *search; /**< Active search state, or NULL. */

	/* Live grid (chip-side terminal model).  When non-NULL the grid
	 * rows are appended to the addressable line space after the ring
	 * (so render / search / decorator handle them uniformly) and the
	 * cursor position is painted at the end of @ref tui_log_render
	 * when tailing.  Set via @ref tui_log_set_grid; the widget does
	 * not own the grid. */
	struct vt100 *vt100;

	/* Frozen snapshot of the grid, captured by @ref tui_log_freeze.
	 * While set, the snapshot stands in for the live grid in the
	 * addressable line space so inspect-mode scroll and search hit
	 * stable content even as the chip keeps emitting bytes.  Owned
	 * by the widget; freed by @ref tui_log_unfreeze and @ref
	 * tui_log_release. */
	struct vt100_cell *snapshot;
	int snapshot_rows;
	int snapshot_cols;

	/* ---- Inspect-mode state (embedded; see @ref tui_inspect_mode).
	 *
	 * @c inspect_active is the public bool callers read to flip
	 * status-bar chrome ("[INSPECT]" badge, key hints).  The other
	 * fields are widget internals -- search prompt, help modal,
	 * mode, and a "have I scrolled off the tail at any point this
	 * session" latch that gates auto-exit so manual @c Ctrl-T+i
	 * followed by @c Down doesn't immediately kick the user out.
	 *
	 * The widgets are sized to the pane's own rect so modals open
	 * scoped to the pane, not the whole screen -- in dual-pane
	 * layouts each pane's search / help sits inside its own
	 * rectangle. */
	int inspect_mode;	   /**< @ref tui_inspect_mode value. */
	int inspect_active;	   /**< Mirrors @c inspect_mode != LIVE. */
	int inspect_help_active;   /**< 1 while help body is parsed. */
	int inspect_help_was_live; /**< Help opened from live mode --
				    *   on dismiss, return to live and
				    *   unfreeze.  Helps `Ctrl-T+?` from
				    *   live not commit the user to NAV
				    *   when they only wanted to read the
				    *   live-mode help. */
	struct tui_prompt inspect_search; /**< Search prompt widget. */
	struct tui_info inspect_help;	  /**< Help modal widget. */

	/* Widget-level prefix latch.  Set when the host forwards a
	 * @c Ctrl-T (0x14) event after deciding the second key isn't
	 * cmd-reserved; the next event is then treated as an in-widget
	 * command (open help, open search, yank, nav, ...) regardless
	 * of inspect state.  Cleared after dispatch. */
	int prefix_pending;
};

/**
 * @brief Initialise an empty log with @p max_lines of scrollback,
 *        a status-row position, status flags, and a host-supplied
 *        identity prefix.
 *
 * Allocates the ring; pair with @ref tui_log_release.  @p max_lines
 * must be > 0; callers typically pick a few thousand (once on the
 * alt screen the terminal's native scrollback is gone, so the ring
 * is the only history users can navigate).
 *
 * @p status_pos picks where the widget paints its self-rendered
 * status row (top, bottom, or none); see @ref tui_log_status_pos.
 * @p status_flags is a bitmask of @c TUI_LOG_SHOW_EXIT etc. that
 * tweaks the chrome the widget includes in the status row.
 * @p status_prefix is the static identity portion the widget will
 * paint at the head of that row; must outlive the widget (the
 * widget stores the pointer, not a copy).  Pass @c NULL when
 * @c status_pos is @c TUI_LOG_STATUS_NONE.  After init the prefix
 * is read-only -- the widget composes the visible status string
 * each render from prefix + dynamic chrome.
 */
void tui_log_init(struct tui_log *L, int max_lines, int status_pos,
		  unsigned status_flags, const char *status_prefix);

/** @brief Free the line ring and pending buffer.  Safe to call twice. */
void tui_log_release(struct tui_log *L);

/** @brief Update frame dimensions.  Call on startup and on
 *         @c TK_RESIZE events. */
void tui_log_resize(struct tui_log *L, int width, int height);

/** @brief Place the widget's top-left cell at terminal column @p x,
 *         row @p y (both 1-based).  Default after @ref tui_log_init
 *         is (1, 1) -- the entire screen.  Set this before composing
 *         two or more @c tui_log panes side by side. */
void tui_log_set_origin(struct tui_log *L, int x, int y);

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

/* ------------------------------------------------------------------ */
/*  vt100 → scrollback bridge                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Drain the scrolled-off rows of @p V into @p L's line ring.
 *
 * Each scrolled-off row is serialized via @c vt100_serialize_row and
 * pushed as a complete line, then the queue is drained.  Callers
 * (typically the monitor's render tick) invoke this each frame so the
 * vt100's bounded queue does not grow unboundedly between drains.  No
 * effect when the queue is empty.
 */
void tui_log_pull_from_vt100(struct tui_log *L, struct vt100 *V);

/**
 * @brief Attach a live vt100 grid to @p L.
 *
 * When set and the widget is in tail mode, @ref tui_log_render reserves
 * the bottom @c vt100_rows(V) rows of the body for live grid content
 * (rendered below any visible ring entries) and positions the cursor
 * at the grid's coordinates if @ref vt100_cursor_visible.  Pass
 * @p V == NULL to detach.  The widget does not own the grid.
 */
void tui_log_set_grid(struct tui_log *L, struct vt100 *V);

/**
 * @brief Feed an input event to the log.
 *
 * Inspect-mode aware:
 *
 *   - When @c L->inspect_active is 0 (live): @c TK_PGUP, @c TK_PGDN,
 *     @c TK_WHEEL_UP and @c TK_WHEEL_DOWN auto-enter inspect mode
 *     (freeze + dispatch the event so the first scroll is immediate);
 *     other events return 0 so the host can forward them to its
 *     underlying stream.
 *   - When inspect is active: arrow / Home / End / PgUp / PgDn / vim
 *     shortcuts (@c j/k/g/G) scroll; @c '/' opens the regex search;
 *     @c 'n' / @c 'N' step through matches; @c '?' opens the keymap
 *     help; @c 'y' yanks the current line to the system clipboard
 *     via OSC 52; @c Esc / @c q clears the search and returns to
 *     live; mouse wheel routes to up/down.  Auto-exits to live when
 *     navigation lands back on the tail after at least one scroll.
 *   - @c TK_RESIZE is not consumed -- the host owns terminal sizing
 *     via @ref tui_log_resize, which also re-flows the embedded
 *     search / help modals.
 *
 * The host MUST NOT forward keystrokes to its underlying stream when
 * the call returned 1 -- the inspect machinery owned the event.
 *
 * @return 1 if the event was consumed (caller should redraw),
 *         0 otherwise.
 */
int tui_log_on_event(struct tui_log *L, const struct term_event *ev);

/**
 * @brief Manually enter inspect mode (e.g. from a host hotkey).
 *
 * Freezes the log and switches to @c TUI_INSPECT_NAV at the live
 * tail.  @c Esc / @c q still returns to live, but auto-exit-on-tail
 * is suppressed until the user has scrolled away at least once --
 * so pressing @c Down right after entering doesn't immediately kick
 * the user back out.  No-op if inspect is already active.
 */
void tui_log_inspect_enter(struct tui_log *L);

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
