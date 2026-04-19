/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file help.h
 * @brief Per-command manual pages embedded as C strings.
 *
 * A command defines a @c cmd_manual with prose sections (DESCRIPTION,
 * EXAMPLES, optional extras).  NAME / SYNOPSIS / OPTIONS are synthesized
 * by print_manual() from the option table and usage[] array so that the
 * option docs have a single source of truth.
 *
 * Prose strings use the H_* fragment macros below plus the inline
 * @c \@x{...} color tokens defined in term.h.  All fragments expand to
 * C string literals and concatenate at compile time -- no runtime cost.
 *
 * Convention: each field ends with a trailing newline; paragraph breaks
 * use H_PARA (adds a blank line).  print_manual() adds spacing between
 * sections automatically.
 *
 * Example:
 *   static const struct cmd_manual build_manual = {
 *       .name = "ice-build",
 *       .summary = "build the default target",
 *       .description =
 *           H_PARA("Runs the CMake @b{all} target in the configured "
 *                  "build directory."),
 *       .examples =
 *           H_EXAMPLE("ice build")
 *           H_EXAMPLE("ice -B out build"),
 *   };
 */
#ifndef HELP_H
#define HELP_H

struct option;
struct cmd_desc;

struct cmd_manual {
	const char *name; /**< Display name (e.g. "ice target set"). */
	const char
	    *summary; /**< One-line tagline after the dash on the NAME line. */
	const char *description; /**< Prose built with H_PARA/H_LINE. */
	const char *examples;	 /**< Optional; built with H_EXAMPLE. */
	const char *extras; /**< Optional; extra sections with H_SECTION. */

	/**
	 * When non-zero, print_manual() auto-emits an ALIASES section
	 * listing every @c alias.<name> defined in the active config
	 * (skipped silently if the user has no aliases).
	 */
	int list_aliases;
};

/*
 * Reflow markers: invisible control bytes the renderer uses to know
 * which regions of text should be word-wrapped.
 *
 *   H_R_BEG <indent>  start a reflow region with the given left indent
 *   H_R_END           end it
 *
 * Only paragraph and definition-value macros emit them.  Every other
 * macro (H_LINE, H_RAW, H_EXAMPLE, H_SECTION) stays verbatim -- the
 * author controls the exact layout.
 */
#define H_R_BEG "\x02"
#define H_R_END "\x03"

/** Paragraph: 4-space indent, reflowable, trailing blank line. */
#define H_PARA(t) H_R_BEG "\x04" t H_R_END "\n\n"

/** Single indented line, verbatim (no reflow). */
#define H_LINE(t) "    " t "\n"

/** Raw line -- no indent, verbatim. */
#define H_RAW(t) t "\n"

/** Shell example: indented, colored prompt + bold command, verbatim. */
#define H_EXAMPLE(c) "    @y{$} @b{" c "}\n"

/**
 * Definition-list entry: bold key at indent 4, description reflowed
 * at indent 8 (man-style two-line form).
 */
#define H_ITEM(k, v) "    @b{" k "}\n" H_R_BEG "\x08" v H_R_END "\n\n"

/** Section heading for use inside .extras (NAME/SYNOPSIS/OPTIONS are auto). */
#define H_SECTION(n) "@b{" n "}\n"

/**
 * @brief Render a full manual page to stdout.
 *
 * Emits NAME, SYNOPSIS, DESCRIPTION, OPTIONS (auto-generated from
 * @c desc->opts), CONFIG / ENVIRONMENT, COMMANDS (auto-generated from
 * @c desc->subcommands when non-empty), EXAMPLES, and @c extras.
 * All @c \@x{...} color tokens are expanded by the platform printf
 * override.
 *
 * @param cmd_name  Display name as typed by the user (e.g. "config").
 *                  "ice" is the top-level manual.  Used to synthesize
 *                  "ice-<name>" for NAME.
 * @param desc      Command descriptor.  @c NULL is tolerated but
 *                  yields a nearly-empty manual page.
 */
void print_manual(const char *cmd_name, const struct cmd_desc *desc);

#endif /* HELP_H */
