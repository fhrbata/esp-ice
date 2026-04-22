/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file kc_ast.h
 * @brief Kconfig AST types shared by parser, evaluator, and I/O.
 *
 * The parser (kc_parse.c) produces a @p kc_ctx rooted at a top-level
 * menu tree.  Every @c config / @c menuconfig / @c choice introduces a
 * @p ksym (interned by name in the context's @p symtab); repeated
 * declarations of the same symbol append to its property list.
 *
 * Expression and property trees are heap-allocated and freed by
 * kc_ctx_release().  All strings inside the AST are owned; originals
 * from the input buffer are copied before being stored.
 */
#ifndef KC_AST_H
#define KC_AST_H

#include <stddef.h>

#include "smap.h"
#include "svec.h"

/* ------------------------------------------------------------------ */
/*  Expressions                                                       */
/* ------------------------------------------------------------------ */

enum kexpr_op {
	KE_LITERAL, /**< Leaf: literal string (symbol value or numeric). */
	KE_SYMREF,  /**< Leaf: reference to an interned ksym. */
	KE_NOT,	    /**< Unary !expr -- uses @p l. */
	KE_AND,	    /**< expr && expr. */
	KE_OR,	    /**< expr || expr. */
	KE_EQ,	    /**< l == r (comparison). */
	KE_NE,	    /**< l != r. */
	KE_LT,	    /**< l < r. */
	KE_LE,	    /**< l <= r. */
	KE_GT,	    /**< l > r. */
	KE_GE,	    /**< l >= r. */
};

struct ksym;

struct kexpr {
	enum kexpr_op op;
	struct kexpr *l;  /**< Left (unary: operand; binary: lhs). */
	struct kexpr *r;  /**< Right (binary: rhs; NULL for unary/leaf). */
	struct ksym *sym; /**< KE_SYMREF: referenced symbol (interned). */
	char *str;	  /**< KE_LITERAL: owned literal string. */
};

/* ------------------------------------------------------------------ */
/*  Properties                                                        */
/* ------------------------------------------------------------------ */

enum kprop_kind {
	KP_PROMPT,  /**< prompt "TEXT" [if EXPR]  (or inline on a type). */
	KP_DEFAULT, /**< default EXPR [if EXPR]. */
	KP_SELECT,  /**< select NAME [if EXPR]. */
	KP_IMPLY,   /**< imply NAME [if EXPR]. */
	KP_RANGE,   /**< range EXPR EXPR [if EXPR]. */
	KP_HELP,    /**< help text block. */
	KP_ENV,	    /**< option env="NAME". */
	KP_DEPENDS, /**< depends on EXPR (symbol/menu/choice/comment). */
	KP_VISIBLE, /**< visible if EXPR (menu only). */
};

struct kprop {
	enum kprop_kind kind;
	struct kexpr *cond;   /**< Optional `if EXPR` guard (NULL = always). */
	char *text;	      /**< KP_PROMPT / KP_HELP / KP_ENV: owned text. */
	struct kexpr *expr;   /**< KP_DEFAULT / KP_SELECT / KP_IMPLY /
			       *   KP_DEPENDS / KP_VISIBLE: expression /
			       *   target name; KP_RANGE: low bound. */
	struct kexpr *expr2;  /**< KP_RANGE: high bound. */
	const char *src_file; /**< Input file for diagnostics. */
	int src_line;	      /**< Input line for diagnostics. */
	struct kprop *next;   /**< Linked-list link on a symbol. */
};

/* ------------------------------------------------------------------ */
/*  Symbols                                                           */
/* ------------------------------------------------------------------ */

enum ksym_type {
	KS_UNKNOWN,
	KS_BOOL,
	KS_INT,
	KS_HEX,
	KS_STRING,
	KS_FLOAT, /**< ESP-IDF extension -- not in vanilla Kconfig. */
};

struct ksym {
	char *name; /**< Owned; without CONFIG_ prefix. */
	enum ksym_type type;
	struct kprop *props;	    /**< Declaration-order linked list. */
	struct kprop *props_tail;   /**< O(1) append helper. */
	int is_choice;		    /**< This symbol is a choice group. */
	struct ksym *choice_parent; /**< For choice members; NULL otherwise. */
};

/* ------------------------------------------------------------------ */
/*  Menus                                                             */
/* ------------------------------------------------------------------ */

enum kmenu_kind {
	KM_ROOT,    /**< Top-level container (mainmenu). */
	KM_MENU,    /**< menu ... endmenu. */
	KM_CHOICE,  /**< choice ... endchoice. */
	KM_SYM,	    /**< config / menuconfig. */
	KM_COMMENT, /**< comment "..." node. */
	KM_IF,	    /**< if EXPR ... endif (transparent grouping). */
};

struct kmenu {
	enum kmenu_kind kind;
	char *prompt;		  /**< KM_ROOT: mainmenu title; KM_MENU /
				   *   KM_COMMENT: prompt text; else NULL. */
	struct ksym *sym;	  /**< KM_SYM / KM_CHOICE / menuconfig. */
	struct kexpr *dep;	  /**< Own `depends on` / `if`-block
				   *   condition (not yet propagated). */
	struct kexpr *visible_if; /**< KM_MENU only. */
	struct kmenu *parent;
	struct kmenu *children; /**< First child. */
	struct kmenu *tail;	/**< O(1) append helper (last child). */
	struct kmenu *next;	/**< Next sibling. */
	const char *src_file;
	int src_line;
};

/* ------------------------------------------------------------------ */
/*  Context                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Global parse context -- shared by lexer, parser, evaluator.
 *
 * The context owns the AST (via @p root) and the symbol table.  Input
 * file paths (for error messages) are interned in @p file_names; their
 * storage outlives individual parse frames so property source-location
 * pointers remain valid for the lifetime of the context.
 */
struct kc_ctx {
	struct kmenu *root;	/**< Top-level KM_ROOT menu. */
	struct smap symtab;	/**< name -> struct ksym * (interned). */
	struct svec symlist;	/**< First-sight order for stable iteration. */
	struct svec file_names; /**< Owned source-file path strings. */
};

/** Initialize an empty context (equivalent to zero-init + root creation). */
void kc_ctx_init(struct kc_ctx *ctx);

/** Release the context: menus, symbols, props, exprs, and the symtab. */
void kc_ctx_release(struct kc_ctx *ctx);

/**
 * @brief Intern a file-path string in the context.
 *
 * Returned pointer is owned by the context and stays valid until
 * kc_ctx_release().  Safe to store in @p src_file fields.
 */
const char *kc_ctx_intern_file(struct kc_ctx *ctx, const char *path);

/**
 * @brief Intern a symbol by name.
 *
 * Returns the existing symbol if one with @p name already exists;
 * otherwise creates a fresh empty ksym and records it in @p symtab and
 * @p symlist.  The returned pointer is stable for the lifetime of the
 * context.
 */
struct ksym *kc_sym_intern(struct kc_ctx *ctx, const char *name);

/* ------------------------------------------------------------------ */
/*  Debug                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Dump the parsed AST to stdout in a human-readable form.
 *
 * Format is not stable -- intended for debugging only.
 */
void kc_ast_dump(const struct kc_ctx *ctx);

#endif /* KC_AST_H */
