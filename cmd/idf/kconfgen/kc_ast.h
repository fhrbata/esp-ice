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

	/* ---- Evaluation state (populated by kc_eval) ---- */
	struct kexpr *effective_dep; /**< Ancestor menu dep AND
				      *   every KP_DEPENDS on this sym. */
	struct kexpr *rev_dep;	     /**< OR of (sel && if) for every
				      *   `select SYM if ...' targeting
				      *   this sym. */
	struct kexpr *weak_rev_dep;  /**< Same but for `imply`. */
	char *cur_val;		     /**< Owned resolved value: "y"/"n"/
				      *   int/hex/float/string body. */
	int visible;		     /**< Computed visibility (bool). */
	int user_set;		     /**< Value came from --config or
				      *   --defaults (not a built-in
				      *   default). */
	int default_applied;	     /**< Current cur_val came from a
				      *   matched @c default property (used
				      *   by emit filters to distinguish an
				      *   applied-default 0 from the
				      *   type's zero fallback -- python
				      *   kconfgen only emits no-prompt
				      *   ints when a default fired). */
	int emit_seen;		     /**< Scratch flag used by the output
				      *   walkers to dedup symbols that are
				      *   reachable via multiple menu nodes
				      *   (e.g. a Kconfig with repeated
				      *   @c config X ... blocks tagged
				      *   @c # ignore: multiple-definition).
				      *   Writers reset this across all
				      *   symbols at the start of the walk. */
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

	/* ---- Evaluation state (populated by kc_eval) ---- */
	struct kexpr *ctx_dep; /**< Parent ctx_dep AND own dep. */
};

/* ------------------------------------------------------------------ */
/*  Context                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief One deprecated->current symbol rename entry.
 *
 * Populated by kc_load_rename().  @c old_name and @c new_name are
 * stored WITHOUT the @c CONFIG_ prefix to match what ksym->name
 * carries.  When @c invert is set, a bool value is flipped on
 * translation: @c old=y -> @c new=n and vice versa.
 */
struct kc_rename {
	char *old_name;
	char *new_name;
	int invert;
};

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

	/* ---- sdkconfig.rename state (populated by kc_load_rename) ---- */
	struct kc_rename *renames;
	size_t n_renames;
	size_t alloc_renames;
	int no_deprecated; /**< Set by --dont-write-deprecated. */

	/*
	 * ESP-IDF-specific banner substitution.  Python kconfgen reads
	 * @c IDF_VERSION from the env and interpolates it into the
	 * preamble of sdkconfig / sdkconfig.h so downstream diffs can
	 * tell which IDF release generated the file.  Owned string;
	 * empty / NULL leaves the preamble with a double-space gap like
	 * python does when the var is unset.
	 */
	char *idf_version;

	/*
	 * Source-tree root used to relativize file paths in the menu id
	 * slugs of kconfig_menus.json.  Matches python kconfiglib's
	 * `srctree` semantics: path names of @c source-d Kconfig files
	 * that live inside this prefix are shortened to their path
	 * relative to it, so ids stay portable across machines.  Owned
	 * string, ends with a '/' (or empty if unset).  kc_parse_file
	 * seeds it from @c $srctree env var, falling back to the current
	 * working directory at parse time.
	 *
	 * @p root_file records the raw @c --kconfig path for the top
	 * Kconfig file.  python kconfiglib special-cases this: the root
	 * file's @c MenuNode.filename is the user-supplied string as-is,
	 * while sourced files go through the srctree-relativization
	 * step.  Ice mirrors that so the root Kconfig's slug keeps the
	 * absolute path even when srctree would otherwise cover it.
	 */
	char *srctree;
	char *root_file;
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
