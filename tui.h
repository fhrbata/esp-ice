/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tui.h
 * @brief Two small widgets layered on top of the term_* event /
 *        output primitives: a scrollable list and a modal prompt.
 *
 * The widgets are domain-agnostic -- items carry opaque @c userdata
 * pointers and the caller decides what Enter / Space mean.  Rendering
 * happens inside the alt screen set up by @ref term_screen_enter;
 * callers drive redraws in response to @ref term_read_event events.
 *
 * Frames are built into an @c sbuf and flushed with a single @c fputs
 * so a full redraw costs one write, independent of how many cells
 * change.
 */
#ifndef TUI_H
#define TUI_H

struct term_event;

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
	int flags;	   /**< Bitmask of @c TUI_ITEM_* flags. */
	void *userdata;	   /**< Opaque to the widget; caller interprets. */
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

/** @brief Render the full frame to stdout (alt-screen). */
void tui_list_render(const struct tui_list *L);

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

/** @brief Render the prompt as a centred modal box. */
void tui_prompt_render(const struct tui_prompt *P);

#endif /* TUI_H */
