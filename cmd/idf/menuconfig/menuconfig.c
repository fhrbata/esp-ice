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
	       "tree.  An existing sdkconfig can be layered via @b{--config}; "
	       "changes are written to @b{--output} (or @b{--config} if "
	       "@b{--output} is omitted).")
	H_PARA("Bool symbols toggle with @b{Space} or @b{Enter}.  Submenus "
	       "and choice groups open on @b{Enter}; @b{Esc} / @b{Left} "
	       "goes up a level.  @b{s} saves without leaving; @b{q} quits "
	       "without saving.  At the root menu, @b{Esc} prompts to save "
	       "any pending changes before quitting.")
	H_PARA("Int / hex / string editing is not yet implemented -- those "
	       "entries render with their current value but cannot be "
	       "edited from the UI.  Edit them directly in the sdkconfig "
	       "file as a workaround until support lands."),

	.examples =
	H_EXAMPLE("ice idf menuconfig --kconfig Kconfig --config sdkconfig")
	H_EXAMPLE("ice idf menuconfig -k Kconfig -c sdkconfig -o sdkconfig.out"),
};
/* clang-format on */

static const char *opt_kconfig;
static const char *opt_config;
static const char *opt_output;

static const struct option cmd_idf_menuconfig_opts[] = {
    OPT_STRING('k', "kconfig", &opt_kconfig, "path",
	       "root Kconfig file (required)", NULL),
    OPT_STRING('c', "config", &opt_config, "path",
	       "existing sdkconfig to load (optional)", NULL),
    OPT_STRING('o', "output", &opt_output, "path",
	       "where to save changes (default: same as --config)", NULL),
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

struct view {
	struct kc_ctx *kc;
	struct kmenu *cur; /* Menu currently being displayed. */
	struct tui_list list;
	struct tui_list_item *items;
	struct row *rows;
	size_t n;
	size_t cap;
	int modified; /* Set by any kc_sym_set_user the UI performs. */
};

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

static void view_add(struct view *v, char *text, char *value, int flags,
		     struct kmenu *menu)
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
		int flags = 0;
		if (c->sym->type == KS_BOOL) {
			value = bool_marker(c->sym);
		} else {
			/* Non-bool display-only in v1.  Dim the row so
			 * the caret skips it and users see why pressing
			 * Space / Enter does nothing. */
			value = paren_value(c->sym);
			flags |= TUI_ITEM_DISABLED;
		}
		view_add(v, text, value, flags, c);
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
		view_add(v, sbuf_detach(&lbl), sbuf_detach(&val), 0, c);
		return;
	}
	case KM_MENU: {
		if (!c->prompt)
			return;
		struct sbuf sb = SBUF_INIT;
		sbuf_addstr(&sb, c->prompt);
		sbuf_addstr(&sb, "  --->");
		view_add(v, sbuf_detach(&sb), NULL, 0, c);
		return;
	}
	case KM_COMMENT: {
		if (!c->prompt)
			return;
		char *text = sbuf_strdup(c->prompt);
		view_add(v, text, NULL, TUI_ITEM_HEADING, c);
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
 * Toggle @p sym between "y" and "n".  For choice members, any toggle
 * is treated as a "set this one" -- kc_resolve's enforce_choice
 * forces the other members to "n".
 */
static void toggle_bool(struct view *v, struct ksym *s)
{
	const char *next = "y";
	if (!s->choice_parent) {
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
	if (m->kind == KM_SYM && m->sym && m->sym->type == KS_BOOL) {
		toggle_bool(v, m->sym);
		return;
	}
	if (m->kind == KM_MENU || m->kind == KM_CHOICE) {
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
	return 1;
}

/* ================================================================== */
/*  Save / quit prompt                                                */
/* ================================================================== */

static const char *save_path(void)
{
	return opt_output ? opt_output : opt_config;
}

static int save_config(struct view *v)
{
	const char *path = save_path();
	if (!path) {
		/* Nothing to save to -- tell the user via the footer
		 * rather than dying in the middle of a TUI session. */
		tui_list_set_footer(
		    &v->list, "  no --output / --config path; press q to quit");
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
		int rc = term_read_event(&ev, 0);
		if (rc < 0)
			return -1;
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
	if (opt_config)
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
	kc_ctx_release(&kc);
	return status;
}
