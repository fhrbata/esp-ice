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

#include <strings.h> /* strncasecmp for the search matcher */

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
static struct svec opt_defaults = SVEC_INIT;
static struct svec opt_renames = SVEC_INIT;
static struct svec opt_env = SVEC_INIT;
static const char *opt_env_file;

static const struct option cmd_idf_menuconfig_opts[] = {
    OPT_STRING('k', "kconfig", &opt_kconfig, "path",
	       "root Kconfig file (required)", NULL),
    OPT_STRING('c', "config", &opt_config, "path",
	       "existing sdkconfig to load on entry (optional)", NULL),
    OPT_STRING('o', "output", &opt_output, "path",
	       "where 's' writes; defaults to --config if omitted", NULL),
    OPT_STRING_LIST(0, "defaults", &opt_defaults, "path",
		    "sdkconfig.defaults file (repeatable; later wins)", NULL),
    OPT_STRING_LIST(0, "sdkconfig-rename", &opt_renames, "path",
		    "deprecated->current symbol rename map (repeatable)", NULL),
    OPT_STRING_LIST('E', "env", &opt_env, "NAME=VAL",
		    "env variable for Kconfig $(VAR) expansion (repeatable)",
		    NULL),
    OPT_STRING(0, "env-file", &opt_env_file, "path",
	       "file with NAME=VAL lines or JSON, layered under --env", NULL),
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
	/* Heap buffer backing tui_list->title so refresh_list can weave
	 * in the dirty-state prefix without making the widget copy. */
	char title_buf[256];
};

static void push_position(struct view *v)
{
	ALLOC_GROW(v->stack, v->stack_n + 1, v->stack_cap);
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
	ALLOC_GROW(v->items, v->n + 1, v->cap);
	/* items and rows grow in lockstep; track capacity on cap once. */
	REALLOC_ARRAY(v->rows, v->cap);
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
		/*
		 * Honour both the menu's propagated `depends on` chain
		 * (ctx_dep) and its optional `visible if` clause -- the
		 * latter is how esp-idf's Kconfig gates target-specific
		 * menus such as `Boot ROM Behavior` (visible if
		 * !IDF_TARGET_ESP32).  kc_expr_bool returns 1 for a NULL
		 * expression, so menus without either clause always show.
		 */
		if (!kc_expr_bool(c->ctx_dep) || !kc_expr_bool(c->visible_if))
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
	/*
	 * Weave in a leading `[*] ` when the config has unsaved changes so
	 * users see the dirty state at a glance.  Title buffer lives on
	 * struct view so the pointer stays stable across redraws.
	 *
	 * flatten_children's view_add stores heap-allocated row text on
	 * @c v->rows and @c v->items; those arrays (and their strings) are
	 * freed by @ref view_reset on the next rebuild and by the explicit
	 * view_reset + free pair in cmd_idf_menuconfig's cleanup.  The
	 * analyzer can't trace ownership through struct fields across
	 * function boundaries, so it flags the next line as a potential
	 * leak -- suppress the specific check.  A real leak would surface
	 * as a missed view_reset, not at this random site.
	 */
	/* NOLINTNEXTLINE(clang-analyzer-unix.Malloc) */
	const char *base = v->cur->prompt ? v->cur->prompt : "(top)";
	snprintf(v->title_buf, sizeof(v->title_buf), "%s%s",
		 v->modified ? "[*] " : "", base);
	tui_list_set_title(&v->list, v->title_buf);
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
 * Concatenate the help text from every @c KP_HELP property on @p s
 * into a single block, joined by blank lines.  Symbols with more
 * than one help prop are rare but legal; we preserve them in source
 * order so the reader sees the same sequencing as kconfiglib.
 */
static char *build_help_body(const struct ksym *s)
{
	struct sbuf sb = SBUF_INIT;

	const char *prompt = sym_prompt(s);
	sbuf_addf(&sb, "Symbol:  CONFIG_%s\n", s->name);
	if (prompt)
		sbuf_addf(&sb, "Prompt:  %s\n", prompt);
	sbuf_addf(&sb, "Type:    %s\n", type_label(s->type));
	if (s->cur_val && *s->cur_val)
		sbuf_addf(&sb, "Value:   %s\n", s->cur_val);
	const char *lo = NULL, *hi = NULL;
	if (s->type == KS_INT || s->type == KS_HEX || s->type == KS_FLOAT)
		get_active_range(s, &lo, &hi);
	if (lo && hi)
		sbuf_addf(&sb, "Range:   %s..%s\n", lo, hi);
	if (s->decl_file && s->decl_line)
		sbuf_addf(&sb, "Defined: %s:%d\n", s->decl_file, s->decl_line);

	int had_help = 0;
	for (struct kprop *p = s->props; p; p = p->next) {
		if (p->kind != KP_HELP)
			continue;
		if (!had_help)
			sbuf_addstr(&sb, "\nDescription:\n");
		else
			sbuf_addch(&sb, '\n');
		sbuf_addstr(&sb, p->text ? p->text : "");
		had_help = 1;
	}
	if (!had_help)
		sbuf_addstr(&sb, "\n(no help text available)");

	return sbuf_detach(&sb);
}

/*
 * Drive a tui_info modal until the user dismisses it.  Shared by
 * @ref show_help (per-symbol help) and @ref show_keys_help (static
 * key-binding reference) so both reuse the same event loop and
 * resize handling.
 */
static void run_info_modal(struct view *v, const char *title, const char *body)
{
	int cols, rows;
	term_size(&cols, &rows);

	struct tui_info info;
	tui_info_init(&info, title, body);
	tui_info_resize(&info, cols, rows);
	tui_info_render(&info);

	for (;;) {
		struct term_event ev;
		int rc = term_read_event(&ev, 1000);
		if (rc < 0)
			break;
		if (rc == 0)
			continue;
		if (ev.key == TK_RESIZE) {
			tui_info_resize(&info, ev.cols, ev.rows);
			redraw(v);
			tui_info_render(&info);
			continue;
		}
		if (tui_info_on_event(&info, &ev))
			break;
		tui_info_render(&info);
	}

	tui_info_release(&info);
	redraw(v);
}

/*
 * Show the static key-binding reference.  Bound to F1 -- `?` stays
 * reserved for contextual help on the current symbol.
 */
static void show_keys_help(struct view *v)
{
	static const char *body =
	    "Navigation\n"
	    "\n"
	    "  Up / Down             Move the cursor one row\n"
	    "  PageUp / PageDown     Move by a full page\n"
	    "  Home / End            Jump to first / last row\n"
	    "  Enter                 Open a submenu / choice, toggle a bool,\n"
	    "                        or edit a numeric / string value\n"
	    "  Space                 Toggle a bool; select a choice member\n"
	    "  Esc / Left            Go up one menu level (save prompt at "
	    "root)\n"
	    "\n"
	    "Actions\n"
	    "\n"
	    "  s / S                 Save to the --output path\n"
	    "  q / Q                 Quit (prompts to save when modified)\n"
	    "  /                     Global symbol search + jump\n"
	    "  ? / h / H             Help for the symbol under the cursor\n"
	    "  F1                    This key reference\n"
	    "  Ctrl-C                Abort without saving\n"
	    "\n"
	    "Symbol indicators\n"
	    "\n"
	    "  [*] / [ ]             Bool on / off\n"
	    "  (*) / ( )             Choice member: selected / not\n"
	    "  (value)               Int / hex / string -- Enter to edit\n"
	    "  --->                  Submenu or choice group -- Enter to open\n"
	    "\n"
	    "Search modal\n"
	    "\n"
	    "  Type anything         Case-insensitive match on name + prompt\n"
	    "  Up / Down             Move through matches\n"
	    "  Enter                 Jump to the highlighted symbol\n"
	    "  Esc                   Cancel and return to the list\n";
	run_info_modal(v, "Key bindings", body);
}

/*
 * Open a read-only help modal for @p s and block until the user
 * dismisses it (Esc / q / Enter).  Background list is redrawn on
 * close so the modal's box is painted over.
 */
static void show_help(struct view *v, struct ksym *s)
{
	char *body = build_help_body(s);
	char title[160];
	const char *prompt = sym_prompt(s);
	snprintf(title, sizeof(title), "Help -- %s", prompt ? prompt : s->name);
	run_info_modal(v, title, body);
	free(body);
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
	/* Rebuild so the `[*]` prefix in the title goes away. */
	refresh_list(v, 1);
	tui_list_set_footer(
	    &v->list, "  saved.  Enter=open  /=search  ?=help  s=save  q=quit");
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
/*  Global symbol search (`/`)                                         */
/* ================================================================== */

/*
 * Case-insensitive substring match.  Returns 1 when @p needle occurs
 * anywhere in @p haystack under ASCII case folding.  Empty needles
 * match everything so the initial render with an empty query shows
 * the full symbol list.
 */
static int ci_contains(const char *haystack, const char *needle)
{
	if (!needle || !*needle)
		return 1;
	if (!haystack)
		return 0;
	size_t nlen = strlen(needle);
	for (const char *h = haystack; *h; h++) {
		if (strncasecmp(h, needle, nlen) == 0)
			return 1;
	}
	return 0;
}

/*
 * One search match.  @c menu is the @c KM_SYM kmenu node that declares
 * the symbol, i.e. the node a jump needs to land on in the parent's
 * flattened item list.
 */
struct hit {
	struct kmenu *menu;
	struct ksym *sym;
};

/*
 * Walk the menu tree collecting every visible symbol whose name or
 * prompt contains @p query.  Uses the same visibility / prompt rules
 * as the list flattener so search only returns symbols the user
 * could actually navigate to.
 */
static void collect_hits_visit(struct kmenu *m, const char *query,
			       struct hit **hits, size_t *n, size_t *cap)
{
	for (struct kmenu *c = m->children; c; c = c->next) {
		if (c->kind == KM_IF || c->kind == KM_MENU ||
		    c->kind == KM_CHOICE || c->kind == KM_ROOT)
			collect_hits_visit(c, query, hits, n, cap);
		if ((c->kind == KM_SYM || c->kind == KM_CHOICE) && c->sym &&
		    c->sym->visible) {
			const char *prompt = sym_prompt(c->sym);
			if (!prompt)
				continue;
			if (!ci_contains(c->sym->name, query) &&
			    !ci_contains(prompt, query))
				continue;
			ALLOC_GROW(*hits, *n + 1, *cap);
			(*hits)[*n].menu = c;
			(*hits)[*n].sym = c->sym;
			(*n)++;
		}
	}
}

/*
 * Walk up from a @c KM_SYM node through any transparent @c KM_IF
 * wrappers until we hit the real enclosing container the list
 * flattener would render it under (menu / choice / root).
 */
static struct kmenu *find_container(struct kmenu *sym_menu)
{
	struct kmenu *p = sym_menu ? sym_menu->parent : NULL;
	while (p && p->kind == KM_IF)
		p = p->parent;
	return p;
}

/*
 * Make @p target_sym_menu the current selection: switch to its
 * enclosing container, rebuild the item list, and position the
 * caret on its row.  Clears the submenu-position stack -- a jump
 * is a teleport, not a descent, so there's no meaningful parent to
 * pop back to with Esc.
 */
static void jump_to(struct view *v, struct kmenu *target_sym_menu)
{
	struct kmenu *container = find_container(target_sym_menu);
	if (!container)
		return;
	v->stack_n = 0;
	v->cur = container;
	refresh_list(v, 0);
	for (size_t i = 0; i < v->n; i++) {
		if (v->items[i].userdata == target_sym_menu) {
			v->list.cursor = (int)i;
			tui_list_resize(&v->list, v->list.width,
					v->list.height);
			return;
		}
	}
}

/*
 * Render the search modal into @p sb.  Layout (centred, ~80% width,
 * ~70% height):
 *
 *   ┌─ Search ──────────────────────────┐
 *   │ query_text                        │   <- input row; cursor visible
 *   ├───────────────────────────────────┤
 *   │ > CONFIG_FOO          Foo prompt  │   <- selected = reverse video
 *   │   CONFIG_BAR          Bar prompt  │
 *   │   ...                             │
 *   └───────────────────────────────────┘
 *    ↑↓ navigate  •  Enter jump  •  Esc cancel
 */
static void render_search(int width, int height, const char *query,
			  const struct hit *hits, size_t n_hits, int selected,
			  int top)
{
	struct sbuf sb = SBUF_INIT;

	int cols = width * 8 / 10;
	if (cols < 40)
		cols = width > 40 ? 40 : width - 2;
	if (cols > width - 2)
		cols = width - 2;
	int box_rows = height * 7 / 10;
	if (box_rows < 10)
		box_rows = height > 10 ? 10 : height - 2;
	if (box_rows > height - 1)
		box_rows = height - 1;
	int left = (width - cols) / 2 + 1;
	int boxtop = (height - box_rows) / 2 + 1;
	int inner = cols - 2;
	int result_rows = box_rows - 4; /* top, input, sep, bottom */

#define S_BORDER "\x1b[36m"
#define S_INPUT "\x1b[0m"
#define S_SEL "\x1b[7m"
#define S_DIM "\x1b[0;2m"
#define S_HINT "\x1b[0;2m"
#define S_RESET "\x1b[0m"

	sbuf_addstr(&sb, "\x1b[?25l");

	/* Top border with inline " Search " label. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", boxtop, left, S_BORDER);
	sbuf_addstr(&sb, "\xe2\x94\x8c");
	const char *label = "\xe2\x94\x80 Search ";
	sbuf_addstr(&sb, label);
	for (int i = 9; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\x90");

	/* Input row. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", boxtop + 1, left, S_BORDER);
	sbuf_addstr(&sb, VL);
	sbuf_addstr(&sb, S_INPUT);
	sbuf_addch(&sb, ' ');
	int qlen = (int)strlen(query);
	int view_w = inner - 1;
	int vstart = qlen > view_w ? qlen - view_w : 0;
	int vlen = qlen - vstart;
	if (vlen > view_w)
		vlen = view_w;
	if (vlen > 0)
		sbuf_add(&sb, query + vstart, (size_t)vlen);
	for (int i = vlen; i < view_w; i++)
		sbuf_addch(&sb, ' ');
	sbuf_addstr(&sb, S_BORDER);
	sbuf_addstr(&sb, VL);

	/* Separator. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", boxtop + 2, left, S_BORDER);
	sbuf_addstr(&sb, "\xe2\x94\x9c");
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\xa4");

	/*
	 * Result rows.  Name left-aligned, prompt right-aligned with a
	 * gap -- truncate either side if the terminal is narrow.  The
	 * currently selected hit is drawn in reverse video so the
	 * cursor reads as the same "stripe" as the list widget.
	 */
	int name_col = 20; /* budget for CONFIG_NAME column */
	if (name_col > inner / 2)
		name_col = inner / 2;
	for (int r = 0; r < result_rows; r++) {
		int idx = top + r;
		sbuf_addf(&sb, "\x1b[%d;%dH%s", boxtop + 3 + r, left, S_BORDER);
		sbuf_addstr(&sb, VL);
		if (idx >= (int)n_hits) {
			sbuf_addstr(&sb, S_INPUT);
			for (int i = 0; i < inner; i++)
				sbuf_addch(&sb, ' ');
			sbuf_addstr(&sb, S_BORDER);
			sbuf_addstr(&sb, VL);
			continue;
		}
		int is_sel = idx == selected;
		sbuf_addstr(&sb, is_sel ? S_SEL : S_INPUT);
		sbuf_addch(&sb, is_sel ? '>' : ' ');
		sbuf_addch(&sb, ' ');

		const struct hit *h = &hits[idx];
		char name_buf[64];
		snprintf(name_buf, sizeof(name_buf), "CONFIG_%s", h->sym->name);
		int nlen = (int)strlen(name_buf);
		if (nlen > name_col)
			nlen = name_col;
		sbuf_add(&sb, name_buf, (size_t)nlen);
		for (int i = nlen; i < name_col; i++)
			sbuf_addch(&sb, ' ');
		sbuf_addch(&sb, ' ');

		const char *pstr = sym_prompt(h->sym);
		int prompt_w =
		    inner - 2 - name_col - 1 - 1; /* prefix + gap + trail */
		if (prompt_w < 0)
			prompt_w = 0;
		int plen = pstr ? (int)strlen(pstr) : 0;
		if (plen > prompt_w)
			plen = prompt_w;
		if (pstr)
			sbuf_add(&sb, pstr, (size_t)plen);
		for (int i = plen; i < prompt_w; i++)
			sbuf_addch(&sb, ' ');
		sbuf_addch(&sb, ' ');
		sbuf_addstr(&sb, S_RESET);
		sbuf_addstr(&sb, S_BORDER);
		sbuf_addstr(&sb, VL);
	}

	/* Bottom border. */
	sbuf_addf(&sb, "\x1b[%d;%dH%s", boxtop + box_rows - 1, left, S_BORDER);
	sbuf_addstr(&sb, "\xe2\x94\x94");
	for (int i = 0; i < inner; i++)
		sbuf_addstr(&sb, HL);
	sbuf_addstr(&sb, "\xe2\x94\x98");
	sbuf_addstr(&sb, S_RESET);

	/* Hint line. */
	if (boxtop + box_rows <= height) {
		struct sbuf hint = SBUF_INIT;
		sbuf_addf(&hint,
			  "\xe2\x86\x91\xe2\x86\x93 navigate  \xe2\x80\xa2  "
			  "Enter jump  \xe2\x80\xa2  Esc cancel  "
			  "\xe2\x80\xa2  %zu match%s",
			  n_hits, n_hits == 1 ? "" : "es");
		sbuf_addf(&sb, "\x1b[%d;%dH%s ", boxtop + box_rows, left,
			  S_HINT);
		int hint_cap = cols - 1;
		int hlen = (int)hint.len;
		if (hlen > hint_cap)
			hlen = hint_cap;
		sbuf_add(&sb, hint.buf, (size_t)hlen);
		for (int i = hlen; i < hint_cap; i++)
			sbuf_addch(&sb, ' ');
		sbuf_release(&hint);
		sbuf_addstr(&sb, S_RESET);
	}

	/* Put the caret in the input field so the user can see where
	 * they're typing.  Column: left-border + 1 gap + visible-offset. */
	int cursor_col = left + 2 + (qlen - vstart);
	sbuf_addf(&sb, "\x1b[%d;%dH\x1b[?25h", boxtop + 1, cursor_col);

	(void)S_DIM;

#undef S_BORDER
#undef S_INPUT
#undef S_SEL
#undef S_DIM
#undef S_HINT
#undef S_RESET

	fputs(sb.buf, stdout);
	fflush(stdout);
	sbuf_release(&sb);
}

/*
 * Entry point: open the search modal, rebuild matches on every
 * keystroke, jump to the selected symbol on Enter, or restore the
 * original view on Esc.
 */
static void search_prompt(struct view *v)
{
	int cols, rows;
	term_size(&cols, &rows);

	char query[128] = "";
	int qlen = 0;

	struct hit *hits = NULL;
	size_t n_hits = 0, cap = 0;
	collect_hits_visit(v->kc->root, query, &hits, &n_hits, &cap);

	int selected = 0;
	int top = 0;
	int result_rows = rows * 7 / 10 - 4;
	if (result_rows < 1)
		result_rows = 1;

	render_search(cols, rows, query, hits, n_hits, selected, top);

	for (;;) {
		struct term_event ev;
		int rc = term_read_event(&ev, 1000);
		if (rc < 0)
			goto out;
		if (rc == 0)
			continue;

		if (ev.key == TK_RESIZE) {
			cols = ev.cols;
			rows = ev.rows;
			result_rows = rows * 7 / 10 - 4;
			if (result_rows < 1)
				result_rows = 1;
			redraw(v);
			render_search(cols, rows, query, hits, n_hits, selected,
				      top);
			continue;
		}
		if (ev.key == TK_ESC)
			goto out;
		if (ev.key == TK_ENTER) {
			if (selected >= 0 && selected < (int)n_hits)
				jump_to(v, hits[selected].menu);
			goto out;
		}
		if (ev.key == TK_UP) {
			if (selected > 0)
				selected--;
			if (selected < top)
				top = selected;
			render_search(cols, rows, query, hits, n_hits, selected,
				      top);
			continue;
		}
		if (ev.key == TK_DOWN) {
			if (selected + 1 < (int)n_hits)
				selected++;
			if (selected >= top + result_rows)
				top = selected - result_rows + 1;
			render_search(cols, rows, query, hits, n_hits, selected,
				      top);
			continue;
		}
		if (ev.key == TK_PGUP) {
			selected -= result_rows;
			if (selected < 0)
				selected = 0;
			top = selected;
			render_search(cols, rows, query, hits, n_hits, selected,
				      top);
			continue;
		}
		if (ev.key == TK_PGDN) {
			selected += result_rows;
			if (selected >= (int)n_hits)
				selected = (int)n_hits - 1;
			if (selected < 0)
				selected = 0;
			if (selected >= top + result_rows)
				top = selected - result_rows + 1;
			render_search(cols, rows, query, hits, n_hits, selected,
				      top);
			continue;
		}
		if (ev.key == TK_BACKSPACE || ev.key == 0x08) {
			if (qlen > 0) {
				qlen--;
				query[qlen] = '\0';
				n_hits = 0;
				collect_hits_visit(v->kc->root, query, &hits,
						   &n_hits, &cap);
				selected = 0;
				top = 0;
			}
			render_search(cols, rows, query, hits, n_hits, selected,
				      top);
			continue;
		}
		if (ev.key >= 0x20 && ev.key <= 0x7e &&
		    qlen < (int)sizeof(query) - 1) {
			query[qlen++] = (char)ev.key;
			query[qlen] = '\0';
			n_hits = 0;
			collect_hits_visit(v->kc->root, query, &hits, &n_hits,
					   &cap);
			selected = 0;
			top = 0;
			render_search(cols, rows, query, hits, n_hits, selected,
				      top);
			continue;
		}
	}

out:
	free(hits);
	redraw(v);
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
		case '?':
		case 'h':
		case 'H': {
			const struct tui_list_item *it =
			    tui_list_current(&v->list);
			if (!it)
				break;
			struct kmenu *m = it->userdata;
			if (m && m->sym)
				show_help(v, m->sym);
			break;
		}
		case '/':
			search_prompt(v);
			break;
		case TK_F1:
			show_keys_help(v);
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

/*
 * Iterate a `;`-separated path list in @p value and call
 * @ref kc_load_rename on each non-empty segment.  Used to apply the
 * per-component rename files ESP-IDF's cmake exports via the
 * @c COMPONENT_SDKCONFIG_RENAMES env var -- python kconfgen reads
 * that env var directly; we do the same so interactive menuconfig
 * sees the full rename coverage without the caller having to
 * repeat `--sdkconfig-rename` for every component.
 */
static void load_rename_list(struct kc_ctx *ctx, const char *value)
{
	const char *p = value;
	while (*p) {
		const char *sep = p;
		while (*sep && *sep != ';')
			sep++;
		if (sep > p) {
			char *path = sbuf_strndup(p, (size_t)(sep - p));
			kc_load_rename(ctx, path);
			free(path);
		}
		p = *sep ? sep + 1 : sep;
	}
}

int cmd_idf_menuconfig(int argc, const char **argv)
{
	argc = parse_options(argc, argv, &cmd_idf_menuconfig_desc);
	if (argc > 0)
		die("too many arguments");
	if (!opt_kconfig)
		die("--kconfig is required");

	/* Layer --env-file under --env, so flags from the CLI override
	 * file entries (matches kconfgen's ordering). */
	if (opt_env_file)
		kc_load_env_file(&opt_env, opt_env_file);

	struct kc_ctx kc;
	kc_ctx_init(&kc);

	/* Rename tables must be loaded before any sdkconfig load so the
	 * reader can translate legacy CONFIG_* keys on the fly. */
	for (size_t i = 0; i < opt_renames.nr; i++)
		kc_load_rename(&kc, opt_renames.v[i]);

	/* COMPONENT_SDKCONFIG_RENAMES gives the per-component rename
	 * files as a `;`-separated list.  Look in the --env table first
	 * (explicit beats implicit), fall back to the process env so
	 * setup_project's populate_kconfig_env path works transparently. */
	const char *comp_renames = NULL;
	for (size_t i = 0; i < opt_env.nr; i++) {
		const char pfx[] = "COMPONENT_SDKCONFIG_RENAMES=";
		if (!strncmp(opt_env.v[i], pfx, sizeof(pfx) - 1)) {
			comp_renames = opt_env.v[i] + sizeof(pfx) - 1;
			break;
		}
	}
	if (!comp_renames)
		comp_renames = getenv("COMPONENT_SDKCONFIG_RENAMES");
	if (comp_renames)
		load_rename_list(&kc, comp_renames);

	kc_parse_file(&kc, opt_kconfig,
		      opt_env.nr ? (const char *const *)opt_env.v : NULL);

	/* Layered sdkconfig load: --defaults in CLI order (later wins),
	 * then --config on top.  Skip entries whose file doesn't exist
	 * yet -- first-run use writes them on save. */
	for (size_t i = 0; i < opt_defaults.nr; i++) {
		if (access(opt_defaults.v[i], F_OK) == 0)
			kc_load_config(&kc, opt_defaults.v[i]);
	}
	if (opt_config && access(opt_config, F_OK) == 0)
		kc_load_config(&kc, opt_config);
	kc_eval(&kc);

	struct view v;
	memset(&v, 0, sizeof(v));
	v.kc = &kc;
	v.cur = kc.root;
	tui_list_init(&v.list);
	tui_list_set_footer(&v.list, "  Enter=open  /=search  ?=help  "
				     "F1=keys  s=save  q=quit");

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
