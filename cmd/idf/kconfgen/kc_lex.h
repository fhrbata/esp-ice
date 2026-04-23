/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_lex.h
 * @brief Kconfig lexer -- token stream for the recursive-descent parser.
 *
 * Hand-rolled tokenizer, modelled after cmd/idf/ldgen/lf.c.  Supplies
 * one token per kc_lex_next() call.
 *
 * Kconfig is terminator-delimited (@c endmenu, @c endchoice, @c endif),
 * not indent-structured, so the lexer does NOT track indentation for
 * ordinary statements.  The one exception is the @c help block: when
 * the parser accepts a @c help keyword it calls kc_lex_read_help() to
 * pull the dedented help body as a single synthetic KT_HELPTEXT token.
 *
 * Quoted strings (@c "..." / @c '...') are fully tokenized here,
 * including escape sequences (@c \\n @c \\t @c \\" @c \\\\) and
 * environment-variable interpolation: @c $(VAR), @c ${VAR}, and
 * @c $VAR.  Lookup order on interpolation is the context-supplied env
 * table, then getenv(), then empty with a one-shot warning.
 *
 * Phase 1 scope: single file, no @c source include stack.  Phase 2 will
 * extend this module with a frame stack so @c source / @c rsource /
 * @c osource / @c orsource are transparent to the parser.
 */
#ifndef KC_LEX_H
#define KC_LEX_H

#include <stddef.h>

struct smap;

/* ------------------------------------------------------------------ */
/*  Tokens                                                            */
/* ------------------------------------------------------------------ */

enum kc_tok {
	KT_EOF = 0,
	KT_NL, /**< Logical end-of-line (statement separator). */

	/* Structural keywords. */
	KT_MAINMENU,
	KT_MENU,
	KT_ENDMENU,
	KT_CHOICE,
	KT_ENDCHOICE,
	KT_CONFIG,
	KT_MENUCONFIG,
	KT_IF,
	KT_ENDIF,
	KT_COMMENT,
	KT_SOURCE,
	KT_RSOURCE,
	KT_OSOURCE,
	KT_ORSOURCE,

	/* Property keywords. */
	KT_DEPENDS,
	KT_ON,
	KT_SELECT,
	KT_IMPLY,
	KT_RANGE,
	KT_DEFAULT,
	KT_DEF_BOOL,
	KT_DEF_INT,
	KT_DEF_HEX,
	KT_DEF_STRING,
	KT_DEF_TRISTATE,
	KT_PROMPT,
	KT_HELP,
	KT_OPTION,
	KT_VISIBLE,
	KT_OPTIONAL,
	KT_MODULES, /**< Accepted for parity; ESP-IDF never has modules. */
	KT_WARNING, /**< ESP-IDF extension: `warning "TEXT"` on a symbol. */
	KT_SET,	    /**< ESP-IDF extension: `set NAME=VAL` /
		     *   `set default NAME=VAL` indirect value setter. */

	/* Types. */
	KT_BOOL,
	KT_INT,
	KT_HEX,
	KT_STRING,
	KT_FLOAT,    /**< ESP-IDF extension. */
	KT_TRISTATE, /**< Accepted but unused in ESP-IDF. */

	/* Identifiers and literals. */
	KT_NAME,     /**< Bareword: identifier that didn't match a keyword. */
	KT_STR,	     /**< Quoted string (env interpolation expanded). */
	KT_HELPTEXT, /**< Help body (synthetic; only after KT_HELP). */

	/* Operators. */
	KT_LPAREN,   /**< '(' */
	KT_RPAREN,   /**< ')' */
	KT_AND,	     /**< '&&' */
	KT_OR,	     /**< '||' */
	KT_NOT,	     /**< '!' */
	KT_EQ,	     /**< '=' (comparison / option value binding) */
	KT_COLON_EQ, /**< ':=' (preset-variable immediate-assignment form). */
	KT_NE,	     /**< '!=' */
	KT_LT,	     /**< '<' */
	KT_LE,	     /**< '<=' */
	KT_GT,	     /**< '>' */
	KT_GE,	     /**< '>=' */
};

/* ------------------------------------------------------------------ */
/*  Lexer state                                                       */
/* ------------------------------------------------------------------ */

/**
 * Saved lexer frame -- parent state preserved while a @c source'd child
 * file is being scanned.  One entry per active nesting level.
 */
struct kc_lex_frame {
	const char *src;  /**< Root buffer for this frame. */
	const char *pos;  /**< Scan position to restore. */
	const char *path; /**< Interned file path for diagnostics. */
	int line;	  /**< Line number to restore. */
	char *owned_buf;  /**< Heap buffer to free when this frame
			   *   is popped (NULL for the root frame,
			   *   whose buffer is owned by the caller). */
};

/**
 * Lexer state.
 *
 * Fields @c tok, @c val, @c line, @c path are read by the parser after
 * each call to kc_lex_next().  The string payload (@c val) is owned by
 * the lexer and freed/replaced on the next call; if the parser wants
 * to retain it, it must strdup before advancing.
 *
 * When the parser accepts a @c source / @c rsource / @c osource /
 * @c orsource statement, it calls kc_lex_push_file() (possibly
 * multiple times for glob expansion) to transparently redirect the
 * token stream through the included file.  The lexer auto-pops frames
 * on end-of-buffer, so the parser sees one uninterrupted stream.
 */
struct kc_lexer {
	const char *src;  /**< Active buffer. */
	const char *pos;  /**< Current scan position. */
	const char *path; /**< Active source file path. */
	int line;	  /**< Current line number (1-based). */

	int tok;   /**< Current token type (enum kc_tok). */
	char *val; /**< Owned: KT_NAME / KT_STR / KT_HELPTEXT payload. */

	/* Environment lookup table (NAME=VAL, NULL-terminated).  May be
	 * NULL.  Consulted before getenv() during $VAR interpolation. */
	const char *const *env;

	/* Preset-variable table (owned by the parser context).  Preset
	 * variables declared via `NAME = VALUE` / `NAME := VALUE` at the
	 * top of a Kconfig file take precedence over @p env and the real
	 * environment during $(VAR) / ${VAR} / bare-$VAR interpolation.
	 * May be NULL during early parsing or when the caller hasn't
	 * wired one in. */
	const struct smap *vars;

	/* Include-stack saved-frames.  The "active" frame is the lexer's
	 * top-level fields; @c frames[] stores the parents to restore on
	 * pop.  @c active_buf is the heap buffer backing the active @c
	 * src (NULL for the root frame whose buffer is caller-owned). */
	struct kc_lex_frame frames[32];
	int n_frames;
	char *active_buf;

	int eof;	      /**< Set after first KT_EOF. */
	int eol_pending;      /**< Next EOF must emit a virtual KT_NL. */
	int warned_undef_env; /**< One-shot warning throttle. */
};

/* ------------------------------------------------------------------ */
/*  API                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Prepare a lexer over @p src.
 *
 * @p src must be a NUL-terminated buffer; the caller keeps ownership.
 * @p path is used only for diagnostic messages.  @p env, when non-NULL,
 * points to a NULL-terminated list of "NAME=VAL" strings consulted
 * during @c $(VAR) interpolation.
 *
 * After this call, the lexer is positioned before the first token;
 * the parser calls kc_lex_next() to advance.
 */
void kc_lex_open(struct kc_lexer *l, const char *src, const char *path,
		 const char *const *env);

/** Release any lexer-owned storage (currently just @p val). */
void kc_lex_close(struct kc_lexer *l);

/**
 * @brief Advance to the next token.
 *
 * Returns (and stores in @p l->tok) the new token type.  At end of
 * input, returns KT_EOF repeatedly.
 */
int kc_lex_next(struct kc_lexer *l);

/**
 * @brief Read and return a dedented @c help body as KT_HELPTEXT.
 *
 * Called by the parser right after accepting KT_HELP and its trailing
 * KT_NL.  Consumes the help block (indent-relative) and leaves the
 * lexer positioned at the first token of the following non-help line.
 *
 * @p l->val holds the (heap-allocated) help text after this call.
 */
void kc_lex_read_help(struct kc_lexer *l);

/**
 * @brief Human-readable name for a token type (for error messages).
 */
const char *kc_tok_name(int tok);

/**
 * @brief Abort with "path:line: expected X, got Y" and exit.
 *
 * Convenience wrapper used by both lexer and parser helpers.
 */
void kc_lex_die_unexpected(struct kc_lexer *l, int want);

/**
 * @brief Push an included file onto the lexer frame stack.
 *
 * After this call, subsequent kc_lex_next() reads from @p path.  When
 * @p path is exhausted, the lexer automatically pops back to the
 * parent frame and continues scanning there.  Intended to be called
 * by the parser's @c source-statement handler.
 *
 * @p path is opened as an ordinary file; callers resolve any @c
 * rsource relative path or env-var interpolation before calling.
 * Returns 0 on success.  Returns -1 when @p optional is non-zero and
 * the file does not exist (silent no-op).  Dies on any other I/O
 * failure, and dies on file-not-found when @p optional is zero.
 */
int kc_lex_push_file(struct kc_lexer *l, const char *path, int optional);

/**
 * @brief Expand bare @c $VAR references in a source path.
 *
 * ESP-IDF writes `source "$COMPONENT_KCONFIGS_SOURCE_FILE"` and
 * `orsource "./components/soc/$IDF_TARGET/include/..."` -- bare
 * @c $VAR that has to resolve via the current --env / --env-file /
 * real-environment tables.  String defaults don't get this expansion
 * (the lexer keeps `$FOO` literal inside quoted values), so we do it
 * explicitly for source paths only.  Returns a newly-allocated string;
 * caller frees.  Unknown vars expand to empty, matching the silent-
 * fallback semantics python kconfgen uses when a source path env var
 * happens to be unset.
 */
char *kc_lex_expand_bare_vars(struct kc_lexer *l, const char *raw);

#endif /* KC_LEX_H */
