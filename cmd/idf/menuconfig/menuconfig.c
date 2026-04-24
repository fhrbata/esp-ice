/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cmd/idf/menuconfig/menuconfig.c
 * @brief `ice idf menuconfig` -- native TUI on top of kconfgen.
 *
 * Plumbing layer: takes explicit --kconfig / --config / --output
 * paths, runs the kconfgen parse + evaluate pipeline, and drives a
 * tui_list over the resulting menu tree.  Project-aware shortcuts
 * (ice menuconfig) are a future wrapper that resolves these paths
 * from the active profile.
 *
 * v1 scope: bool toggle, submenu navigation, choice groups rendered
 * as radio-button submenus.  int / hex / string editing is deferred
 * to a follow-up -- such entries render as dim, non-selectable rows
 * so users can see the current value without being able to edit yet.
 */
#include "ice.h"
#include "tui.h"

#include "cmd/idf/kconfgen/kc_ast.h"
#include "cmd/idf/kconfgen/kc_eval.h"
#include "cmd/idf/kconfgen/kc_io.h"

void kc_parse_file(struct kc_ctx *ctx, const char *path,
		   const char *const *env);

/* clang-format off */
static const struct cmd_manual idf_menuconfig_manual = {
	.name = "ice idf menuconfig",
	.summary = "interactive Kconfig editor (TUI)",

	.description =
	H_PARA("Launches a native ncurses-free terminal UI for browsing "
	       "and editing Kconfig symbols against a given @b{--kconfig} "
	       "tree.  Input and output paths are separate so build-system "
	       "callers (esp-idf's cmakev2) can load from a source "
	       "sdkconfig and write to a build-directory copy without "
	       "mutating the project tree.")
	H_PARA("@b{--config} loads an existing sdkconfig on entry "
	       "(optional -- omit for pure-defaults start).  @b{--output} "
	       "is where @b{s} saves; if omitted it defaults to "
	       "@b{--config} so the common case of editing a single file "
	       "in place is a single flag.  At least one of the two must "
	       "be given for @b{s} to have somewhere to write.")
	H_PARA("Bool symbols toggle with @b{Space} or @b{Enter}.  Submenus "
	       "and choice groups open on @b{Enter}; @b{Esc} / @b{Left} "
	       "goes up a level.  @b{s} saves without leaving; @b{q} quits "
	       "without saving.  At the root menu, @b{Esc} prompts to save "
	       "any pending changes before quitting.")
	H_PARA("Int / hex / float / string symbols open a modal input "
	       "box on @b{Enter}, pre-filled with the current value.  "
	       "Confirm with @b{Enter}, discard with @b{Esc}.  Invalid "
	       "input (non-numeric text in an int field, for example) "
	       "keeps the modal open so the typo can be fixed without "
	       "losing the in-progress text."),

	.examples =
	H_EXAMPLE("ice idf menuconfig --kconfig Kconfig --config sdkconfig")
	H_EXAMPLE("ice idf menuconfig -k Kconfig -c in.sdkconfig -o out.sdkconfig"),
};
/* clang-format on */

static const char *opt_kconfig;
static const char *opt_config;
static const char *opt_output;

static const struct option cmd_idf_menuconfig_opts[] = {
    OPT_STRING('k', "kconfig", &opt_kconfig, "path",
	       "root Kconfig file (required)", NULL),
    OPT_STRING('c', "config", &opt_config, "path",
	       "existing sdkconfig to load on entry (optional)", NULL),
    OPT_STRING('o', "output", &opt_output, "path",
	       "where 's' writes; defaults to --config if omitted", NULL),
    OPT_END(),
};

int cmd_idf_menuconfig(int argc, const char **argv);

const struct cmd_desc cmd_idf_menuconfig_desc = {
    .name = "menuconfig",
    .fn = cmd_idf_menuconfig,
    .opts = cmd_idf_menuconfig_opts,
    .manual = &idf_menuconfig_manual,
};

/* ================================================================== */
/*  Menu-tree -> tui_list adapter                                     */
/* ================================================================== */

/*
 * Rendered-row state.  @c owned_text and @c owned_value hold strdup'd
 * labels the widget points at; we free them before each rebuild.
 */
struct row {
	char *owned_text;
	char *owned_value;
};

/*
 * One cursor position saved while descending into a submenu.  On
 * ascent we pop the top entry and restore the caret so the user
 * returns to where they were, rather than snapping to the first
 * selectable row.
 */
struct saved_pos {
	int cursor;
	int top;
};

struct view {
	struct kc_ctx *kc;
	struct kmenu *cur; /* Menu currently being displayed. */
	struct tui_list list;
	struct tui_list_item *items;
	struct row *rows;
	size_t n;
	size_t cap;
	struct saved_pos *stack;
	size_t stack_n;
	size_t stack_cap;
	int modified; /* Set by any kc_sym_set_user the UI performs. */
};

static void push_position(struct view *v)
{
	if (v->stack_n == v->stack_cap) {
		size_t next = v->stack_cap ? v->stack_cap * 2 : 8;
		v->stack = realloc(v->stack, next * sizeof(*v->stack));
		if (!v->stack)
			die_errno("realloc");
		v->stack_cap = next;
	}
	v->stack[v->stack_n].cursor = v->list.cursor;
	v->stack[v->stack_n].top = v->list.top;
	v->stack_n++;
}

static void pop_position(struct view *v)
{
	if (v->stack_n == 0)
		return;
	v->stack_n--;
	int c = v->stack[v->stack_n].cursor;
	int t = v->stack[v->stack_n].top;
	if (c < v->list.n_items) {
		v->list.cursor = c;
		v->list.top = t;
		tui_list_resize(&v->list, v->list.width, v->list.height);
	}
}

static void row_free(struct row *r)
{
	free(r->owned_text);
	free(r->owned_value);
	r->owned_text = NULL;
	r->owned_value = NULL;
}

static void view_reset(struct view *v)
{
	for (size_t i = 0; i < v->n; i++)
		row_free(&v->rows[i]);
	v->n = 0;
}

static void view_add(struct view *v, char *text, char *value,
		     const char *value_sgr, int flags, struct kmenu *menu)
{
	if (v->n == v->cap) {
		size_t next = v->cap ? v->cap * 2 : 16;
		v->items = realloc(v->items, next * sizeof(*v->items));
		v->rows = realloc(v->rows, next * sizeof(*v->rows));
		if (!v->items || !v->rows)
			die_errno("realloc");
		v->cap = next;
	}
	v->rows[v->n].owned_text = text;
	v->rows[v->n].owned_value = value;
	v->items[v->n].text = text;
	v->items[v->n].value = value;
	v->items[v->n].value_sgr = value_sgr;
	v->items[v->n].flags = flags;
	v->items[v->n].userdata = menu;
	v->n++;
}

/*
 * Return the first active @c KP_PROMPT text for @p s (or NULL if the
 * symbol has no prompt in the current configuration).  A promptless
 * symbol never appears in the UI -- matches python kconfiglib.
 */
static const char *sym_prompt(const struct ksym *s)
{
	for (struct kprop *p = s->props; p; p = p->next) {
		if (p->kind != KP_PROMPT)
			continue;
		if (!kc_expr_bool(p->cond))
			continue;
		return p->text;
	}
	return NULL;
}

static char *bool_marker(const struct ksym *s)
{
	int on = s->cur_val && strcmp(s->cur_val, "y") == 0;
	if (s->choice_parent)
		return sbuf_strdup(on ? "(*)" : "( )");
	return sbuf_strdup(on ? "[*]" : "[ ]");
}

static char *paren_value(const struct ksym *s)
{
	const char *v = s->cur_val ? s->cur_val : "";
	struct sbuf sb = SBUF_INIT;
	sbuf_addch(&sb, '(');
	sbuf_addstr(&sb, v);
	sbuf_addch(&sb, ')');
	return sbuf_detach(&sb);
}

static void flatten_node(struct view *v, struct kmenu *c);

static void flatten_children(struct view *v, struct kmenu *m)
{
	for (struct kmenu *c = m->children; c; c = c->next)
		flatten_node(v, c);
}

static void flatten_node(struct view *v, struct kmenu *c)
{
	switch (c->kind) {
	case KM_IF:
		/* Transparent.  Python kconfiglib folds the @c if guard into
		 * each child's effective_dep so visibility is already decided
		 * per symbol -- we just walk through. */
		flatten_children(v, c);
		return;
	case KM_SYM: {
		if (!c->sym)
			return;
		const char *prompt = sym_prompt(c->sym);
		if (!c->sym->visible || !prompt)
			return;
		char *text = sbuf_strdup(prompt);
		char *value;
		const char *value_sgr = NULL;
		if (c->sym->type == KS_BOOL) {
			value = bool_marker(c->sym);
			/* Bold green for "on" markers so a glance across
			 * the list shows what's active.  "off" markers stay
			 * in the default colour; colouring them too makes
			 * the list louder than it needs to be. */
			int on = c->sym->cur_val &&
				 strcmp(c->sym->cur_val, "y") == 0;
			if (on)
				value_sgr = "1;32";
		} else {
			/* int / hex / string / float: render the current
			 * value in cyan so the row reads as "editable,
			 * non-bool".  Enter opens a modal prompt (see
			 * edit_value). */
			value = paren_value(c->sym);
			value_sgr = "36";
		}
		view_add(v, text, value, value_sgr, 0, c);
		return;
	}
	case KM_CHOICE: {
		/* Choice groups are entered on Enter; render the current
		 * winner's prompt as the value so the collapsed row is
		 * informative.  A hidden choice (visible==0) drops out. */
		if (!c->sym || !c->sym->visible)
			return;
		const char *prompt = sym_prompt(c->sym);
		if (!prompt)
			return;
		struct sbuf lbl = SBUF_INIT;
		sbuf_addstr(&lbl, prompt);
		sbuf_addstr(&lbl, "  --->");
		/* Walk the choice's members to find the active one. */
		const char *winner_prompt = "";
		for (struct kmenu *m = c->children; m; m = m->next) {
			if (m->kind != KM_SYM || !m->sym)
				continue;
			if (m->sym->cur_val &&
			    strcmp(m->sym->cur_val, "y") == 0) {
				const char *p = sym_prompt(m->sym);
				if (p) {
					winner_prompt = p;
					break;
				}
			}
		}
		struct sbuf val = SBUF_INIT;
		sbuf_addch(&val, '(');
		sbuf_addstr(&val, winner_prompt);
		sbuf_addch(&val, ')');
		/* Cyan value matches the non-bool paren style. */
		view_add(v, sbuf_detach(&lbl), sbuf_detach(&val), "36", 0, c);
		return;
	}
	case KM_MENU: {
		if (!c->prompt)
			return;
		struct sbuf sb = SBUF_INIT;
		sbuf_addstr(&sb, c->prompt);
		sbuf_addstr(&sb, "  --->");
		view_add(v, sbuf_detach(&sb), NULL, NULL, 0, c);
		return;
	}
	case KM_COMMENT: {
		if (!c->prompt)
			return;
		char *text = sbuf_strdup(c->prompt);
		view_add(v, text, NULL, NULL, TUI_ITEM_HEADING, c);
		return;
	}
	case KM_ROOT:
		return;
	}
}

/* ================================================================== */
/*  Navigation + event dispatch                                       */
/* ================================================================== */

/*
 * Rebuild the flattened item list for @c v->cur.  Pass @c preserve
 * when the menu contents have changed but the user's focus should
 * stay (e.g. after toggling a bool in place); pass 0 when switching
 * menus so the cursor lands on the first selectable row of the new
 * list.  If the saved index is out of range after the rebuild (the
 * list shrank), the widget's own "first selectable" fallback wins.
 */
static void refresh_list(struct view *v, int preserve)
{
	int saved_cursor = v->list.cursor;
	int saved_top = v->list.top;
	view_reset(v);
	flatten_children(v, v->cur);
	tui_list_set_items(&v->list, v->items, (int)v->n);
	tui_list_set_title(&v->list, v->cur->prompt ? v->cur->prompt : "(top)");
	if (preserve && saved_cursor < v->list.n_items) {
		v->list.cursor = saved_cursor;
		v->list.top = saved_top;
		tui_list_resize(&v->list, v->list.width, v->list.height);
	}
}

static void resync_size(struct view *v)
{
	int cols, rows;
	if (term_size(&cols, &rows) < 0) {
		cols = 80;
		rows = 24;
	}
	tui_list_resize(&v->list, cols, rows);
}

static void redraw(struct view *v) { tui_list_render(&v->list); }

/*
 * Clear user_set on every member of the choice @p s belongs to,
 * except @p s itself.  Called before picking a new choice winner so
 * enforce_choice's "last user-set y" rule resolves unambiguously --
 * otherwise, a sibling from a previous interaction keeps winning by
 * declaration-order tie-break even when the user just picked another
 * entry.
 */
static void clear_choice_siblings(struct view *v, struct ksym *s)
{
	struct ksym *choice = s->choice_parent;
	if (!choice || !choice->choice_menu)
		return;
	/* Walk the choice_menu's subtree -- members may be nested under
	 * KM_IF blocks, so a linear scan of direct children isn't enough. */
	struct kmenu *stack[32];
	int sp = 0;
	stack[sp++] = choice->choice_menu;
	while (sp > 0 && sp < (int)(sizeof(stack) / sizeof(stack[0]))) {
		struct kmenu *m = stack[--sp];
		for (struct kmenu *c = m->children; c; c = c->next) {
			if (c->kind == KM_SYM && c->sym &&
			    c->sym->choice_parent == choice && c->sym != s) {
				c->sym->user_set = 0;
				c->sym->user_default_seeded = 0;
			}
			if (c->kind == KM_IF &&
			    sp < (int)(sizeof(stack) / sizeof(stack[0])))
				stack[sp++] = c;
		}
	}
	(void)v;
}

/*
 * Type-aware input validation for the edit modal.  Returns 1 if @p s
 * is a legal value for @p t, 0 otherwise.  kc_resolve clamps numeric
 * values against any @c range property during the fixpoint, so we
 * only need to gate on the lexical shape of the input here.
 */
static int looks_like_int(const char *s)
{
	if (!s || !*s)
		return 0;
	const char *p = s;
	if (*p == '-' || *p == '+')
		p++;
	if (!*p)
		return 0;
	for (; *p; p++)
		if (*p < '0' || *p > '9')
			return 0;
	return 1;
}

static int looks_like_hex(const char *s)
{
	if (!s || !*s)
		return 0;
	const char *p = s;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;
	if (!*p)
		return 0;
	for (; *p; p++) {
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
		      (*p >= 'A' && *p <= 'F')))
			return 0;
	}
	return 1;
}

static int looks_like_float(const char *s)
{
	if (!s || !*s)
		return 0;
	char *end;
	strtod(s, &end);
	return end != s && *end == '\0';
}

static int value_is_valid(enum ksym_type t, const char *s)
{
	switch (t) {
	case KS_INT:
		return looks_like_int(s);
	case KS_HEX:
		return looks_like_hex(s);
	case KS_FLOAT:
		return looks_like_float(s);
	case KS_STRING:
		return 1; /* any text accepted */
	default:
		return 0;
	}
}

static const char *type_label(enum ksym_type t)
{
	switch (t) {
	case KS_INT:
		return "int";
	case KS_HEX:
		return "hex";
	case KS_FLOAT:
		return "float";
	case KS_STRING:
		return "string";
	default:
		return "?";
	}
}

/*
 * Resolve a range-bound expression to a displayable string.
 *
 * Range bounds come in three shapes:
 *   - @c KE_LITERAL  : e->str is the text (rare for numeric ranges).
 *   - @c KE_SYMREF to a numeric bareword : the parser interned the
 *     number itself as a KS_UNKNOWN symbol whose @c name is the digit
 *     string; @c cur_val is never populated for these.  This is the
 *     common @c "range 1024 16384" case.
 *   - @c KE_SYMREF to a real config symbol : its @c cur_val is the
 *     evaluated value (e.g. @c `range MIN MAX` where MIN / MAX are
 *     other int symbols).
 */
static const char *bound_str(const struct kexpr *e)
{
	if (!e)
		return NULL;
	if (e->op == KE_LITERAL)
		return e->str;
	if (e->op == KE_SYMREF && e->sym) {
		if (e->sym->cur_val && *e->sym->cur_val)
			return e->sym->cur_val;
		if (e->sym->name && *e->sym->name)
			return e->sym->name;
	}
	return NULL;
}

/*
 * Return the @c (lo, hi) strings for the first active @c range
 * property on @p s, or @c (NULL, NULL) when the symbol has none.
 * The active one is the first whose @c if guard evaluates true --
 * matches @c kc_eval's range_clamp semantics.
 */
static void get_active_range(const struct ksym *s, const char **lo,
			     const char **hi)
{
	*lo = *hi = NULL;
	for (struct kprop *p = s->props; p; p = p->next) {
		if (p->kind != KP_RANGE)
			continue;
		if (!kc_expr_bool(p->cond))
			continue;
		*lo = bound_str(p->expr);
		*hi = bound_str(p->expr2);
		return;
	}
}

/*
 * Modal editor for int / hex / float / string symbols.  Opens a
 * prompt pre-filled with the current value; confirms on Enter when
 * the input passes value_is_valid, discards on Esc, and rejects
 * silently (keeps the prompt open) on Enter with invalid input so
 * the user can fix the typo without losing their text.
 */
static void edit_value(struct view *v, struct ksym *s)
{
	int cols, rows;
	term_size(&cols, &rows);

	char title[160];
	const char *prompt = sym_prompt(s);
	const char *lo = NULL, *hi = NULL;
	if (s->type == KS_INT || s->type == KS_HEX || s->type == KS_FLOAT)
		get_active_range(s, &lo, &hi);
	if (lo && hi) {
		snprintf(title, sizeof(title), "%s (%s, range %s..%s)",
			 prompt ? prompt : s->name, type_label(s->type), lo,
			 hi);
	} else {
		snprintf(title, sizeof(title), "%s (%s)",
			 prompt ? prompt : s->name, type_label(s->type));
	}

	struct tui_prompt p;
	tui_prompt_init(&p, title, s->cur_val ? s->cur_val : "");
	tui_prompt_resize(&p, cols, rows);
	tui_prompt_render(&p);

	for (;;) {
		struct term_event ev;
		int rc = term_read_event(&ev, 1000);
		if (rc < 0)
			return;
		if (rc == 0)
			continue;

		if (ev.key == TK_RESIZE) {
			tui_prompt_resize(&p, ev.cols, ev.rows);
			redraw(v);
			tui_prompt_render(&p);
			continue;
		}

		int r = tui_prompt_on_event(&p, &ev);
		if (r < 0) {
			/* Esc: discard.  Redraw the list so the modal's
			 * box is painted over. */
			redraw(v);
			return;
		}
		if (r > 0) {
			if (!value_is_valid(s->type, p.buf)) {
				/* Reject silently -- keep the modal open so
				 * the user can fix their input.  No visible
				 * error indicator in v1; the fact that Enter
				 * didn't close is the signal. */
				tui_prompt_render(&p);
				continue;
			}
			kc_sym_set_user(v->kc, s->name, p.buf);
			kc_resolve(v->kc);
			v->modified = 1;
			refresh_list(v, 1);
			redraw(v);
			return;
		}
		tui_prompt_render(&p);
	}
}

/*
 * Toggle @p sym between "y" and "n".  For choice members, any toggle
 * is treated as a "set this one" -- kc_resolve's enforce_choice
 * forces the other members to "n".
 */
static void toggle_bool(struct view *v, struct ksym *s)
{
	const char *next = "y";
	if (s->choice_parent) {
		/* Exclusive pick: scrub stale user_set flags on siblings
		 * first so the new winner is unambiguous. */
		clear_choice_siblings(v, s);
	} else {
		/* Regular bool: flip current value. */
		int on = s->cur_val && strcmp(s->cur_val, "y") == 0;
		next = on ? "n" : "y";
	}
	kc_sym_set_user(v->kc, s->name, next);
	kc_resolve(v->kc);
	v->modified = 1;
	refresh_list(v, 1);
}

/*
 * Activate the item under the cursor.  Bool -> toggle; menu / choice
 * -> descend.  No-op on headings / disabled rows.
 */
static void activate(struct view *v)
{
	const struct tui_list_item *it = tui_list_current(&v->list);
	if (!it || (it->flags & (TUI_ITEM_DISABLED | TUI_ITEM_HEADING)))
		return;
	struct kmenu *m = it->userdata;
	if (!m)
		return;
	if (m->kind == KM_SYM && m->sym) {
		if (m->sym->type == KS_BOOL) {
			toggle_bool(v, m->sym);
		} else {
			edit_value(v, m->sym);
		}
		return;
	}
	if (m->kind == KM_MENU || m->kind == KM_CHOICE) {
		/* Remember where we were before descending so Esc lands
		 * the caret back on this row rather than at the top. */
		push_position(v);
		v->cur = m;
		refresh_list(v, 0);
	}
}

/* Navigate up one level.  Returns 1 if a level was popped, 0 when
 * already at root. */
static int ascend(struct view *v)
{
	if (!v->cur->parent)
		return 0;
	v->cur = v->cur->parent;
	refresh_list(v, 0);
	pop_position(v);
	return 1;
}

/* ================================================================== */
/*  Save / quit prompt                                                */
/* ================================================================== */

static int save_config(struct view *v)
{
	const char *path = opt_output ? opt_output : opt_config;
	if (!path) {
		tui_list_set_footer(
		    &v->list,
		    "  no --output / --config set; nothing to save to");
		redraw(v);
		return -1;
	}
	kc_write_config(v->kc, path);
	v->modified = 0;
	tui_list_set_footer(
	    &v->list, "  saved.  Space=toggle  Enter=open  s=save  q=quit");
	redraw(v);
	return 0;
}

/*
 * Modal y/n prompt rendered as a single-line tui_prompt whose only
 * accepted characters are y/Y/n/N/Enter/Esc.  Returns 1 on yes,
 * 0 on no, -1 on cancel.
 */
static int yn_prompt(struct view *v, const char *title)
{
	struct tui_prompt p;
	int cols, rows;
	term_size(&cols, &rows);
	tui_prompt_init(&p, title, "y");
	tui_prompt_resize(&p, cols, rows);
	tui_prompt_render(&p);
	for (;;) {
		struct term_event ev;
		/*
		 * Block with a generous timeout rather than polling.  A
		 * non-blocking poll here spins the modal's render hundreds
		 * of times a second, which on some terminals manifests as
		 * a flickering cursor and drives the fan -- neither of
		 * which anyone wants while they're answering a y/n prompt.
		 */
		int rc = term_read_event(&ev, 1000);
		if (rc < 0)
			return -1;
		if (rc == 0)
			continue;
		if (ev.key == TK_RESIZE) {
			tui_prompt_resize(&p, ev.cols, ev.rows);
			redraw(v); /* repaint list behind the modal */
			tui_prompt_render(&p);
			continue;
		}
		if (ev.key == TK_ENTER) {
			/* Accept whatever the buffer says.  "y"/"Y" -> yes,
			 * anything else (including empty) -> no. */
			if (p.len > 0 && (p.buf[0] == 'y' || p.buf[0] == 'Y'))
				return 1;
			return 0;
		}
		if (ev.key == TK_ESC)
			return -1;
		tui_prompt_on_event(&p, &ev);
		tui_prompt_render(&p);
	}
}

/* Returns 1 when the UI should exit, 0 when it should keep running. */
static int handle_quit(struct view *v)
{
	if (!v->modified)
		return 1;
	int yes = yn_prompt(v, "Save your changes before quitting? (y/n)");
	if (yes < 0) {
		/* Cancelled -- resume. */
		redraw(v);
		return 0;
	}
	if (yes)
		save_config(v);
	return 1;
}

/* ================================================================== */
/*  Run loop                                                          */
/* ================================================================== */

static int run_loop(struct view *v)
{
	for (;;) {
		struct term_event ev;
		int rc = term_read_event(&ev, 1000);
		if (rc < 0)
			return -1;
		if (rc == 0)
			continue;

		if (ev.key == TK_RESIZE) {
			tui_list_resize(&v->list, ev.cols, ev.rows);
			redraw(v);
			continue;
		}

		if (tui_list_on_event(&v->list, &ev)) {
			redraw(v);
			continue;
		}

		switch (ev.key) {
		case ' ':
		case TK_ENTER:
			activate(v);
			redraw(v);
			break;
		case TK_LEFT:
		case TK_ESC:
			if (!ascend(v)) {
				if (handle_quit(v))
					return 0;
			} else {
				redraw(v);
			}
			break;
		case 'q':
		case 'Q':
			if (handle_quit(v))
				return 0;
			break;
		case 's':
		case 'S':
			save_config(v);
			break;
		case TK_CTRL('c'):
			/* Raw mode suppressed SIGINT; emulate the classic
			 * "Ctrl-C means abort without saving" here. */
			return 0;
		default:
			break;
		}
	}
}

int cmd_idf_menuconfig(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_idf_menuconfig_desc);
	if (argc > 0)
		die("too many arguments");
	if (!opt_kconfig)
		die("--kconfig is required");

	struct kc_ctx kc;
	kc_ctx_init(&kc);
	kc_parse_file(&kc, opt_kconfig, NULL);
	/*
	 * --config loads a starting sdkconfig; optional so the user can
	 * start from pure Kconfig defaults.  Skip the load when --config
	 * points at a file that does not exist yet (first-ever run of
	 * `-c foo -o foo` pattern).
	 */
	if (opt_config && access(opt_config, F_OK) == 0)
		kc_load_config(&kc, opt_config);
	kc_eval(&kc);

	struct view v;
	memset(&v, 0, sizeof(v));
	v.kc = &kc;
	v.cur = kc.root;
	tui_list_init(&v.list);
	tui_list_set_footer(
	    &v.list, "  Space=toggle  Enter=open  Esc=back  s=save  q=quit");

	int rc = term_raw_enter();
	if (rc)
		die("cannot enter raw mode: %s", strerror(-rc));
	term_screen_enter();

	resync_size(&v);
	refresh_list(&v, 0);
	redraw(&v);

	int status = run_loop(&v);

	term_screen_leave();
	term_raw_leave();

	view_reset(&v);
	free(v.items);
	free(v.rows);
	free(v.stack);
	kc_ctx_release(&kc);
	return status;
}
