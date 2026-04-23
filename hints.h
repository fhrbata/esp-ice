/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hints.h
 * @brief Match YAML-defined regex rules against a log file.
 *
 * Reproduces the matching semantics of ESP-IDF's
 * @c tools/idf_py_actions/tools.py::generate_hints_buffer as a library
 * function so both the @c ice @c idf @c hints CLI and the
 * @c process_run_progress failure path can invoke it.
 *
 * The rules file is a YAML sequence of @c {re, hint, match_to_output,
 * variables} mappings; see the ESP-IDF hints.yml for the schema.  The
 * log is normalized by stripping each line's leading and trailing
 * whitespace, dropping empty lines, and joining the remainder with
 * single spaces before any regex runs.  Matching uses PCRE2, so Perl
 * extensions (@c \w, @c \d, negative lookahead) are honored.
 *
 * The helper only produces the matched hint strings; decisions about
 * where (stdout/stderr), how (colored, prefixed, one per line), and
 * whether to present them belong to the caller.  Broken individual
 * rules @c warn() and are skipped so one typo in hints.yml can't
 * suppress every other match.
 */
#ifndef HINTS_H
#define HINTS_H

struct svec;

/**
 * @brief Scan a log file against rules from a hints YAML file.
 *
 * Every rule whose regex matches the normalized log contributes one
 * expanded hint message (copied) to @p out.  @p out is not cleared;
 * callers preserve any pre-existing entries.
 *
 * @param hints_yml_path  Path to a YAML rules file.
 * @param log_path        Path to the log to scan.
 * @param out             Destination string vector; matched hints are
 *                        appended via @c svec_push().
 * @return Number of hints appended, or -1 if either file can't be
 *         read or @p hints_yml_path is not a valid rules document.
 */
int hints_scan(const char *hints_yml_path, const char *log_path,
	       struct svec *out);

#endif /* HINTS_H */
