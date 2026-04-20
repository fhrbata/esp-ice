/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file error.h
 * @brief Error reporting, warnings, and fatal exits.
 *
 * err() / err_errno()     -- log with source location (red); continue.
 * warn() / warn_errno()   -- print "warning: ..." (yellow); continue.
 * hint()                  -- print "hint: ..." (orange); continue.
 * die() / die_errno()     -- print "fatal: ..." (red) and exit.
 *
 * Because die() exits, emit hint() *before* the paired die() so both
 * messages reach stderr; the hint then appears above the fatal line.
 *
 * All output goes through the platform fprintf override, so color
 * tokens are expanded automatically.
 */
#ifndef ERROR_H
#define ERROR_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

/**
 * @brief Print an error with source location (not called directly).
 */
void err_msg(char *file, int line, const char *func, int add_errno, char *fmt,
	     ...);

/** Log an error message with source location (no errno). */
#define err(...) err_msg(__FILE__, __LINE__, __func__, 0, __VA_ARGS__)

/** Log an error message with source location and errno description. */
#define err_errno(...) err_msg(__FILE__, __LINE__, __func__, 1, __VA_ARGS__)

/**
 * @brief Print "warning: ..." to stderr (not called directly).
 */
void warn_msg(int add_errno, const char *fmt, ...);

/** Print "warning: ..." to stderr. */
#define warn(...) warn_msg(0, __VA_ARGS__)

/** Print "warning: ...: strerror" to stderr. */
#define warn_errno(...) warn_msg(1, __VA_ARGS__)

/**
 * @brief Print "hint: ..." to stderr (orange).
 */
void hint(const char *fmt, ...);

/**
 * @brief Print "fatal: ..." to stderr and exit (not called directly).
 */
void NORETURN die_msg(int add_errno, const char *fmt, ...);

/** Print "fatal: ..." and exit. */
#define die(...) die_msg(0, __VA_ARGS__)

/** Print "fatal: ...: strerror" and exit. */
#define die_errno(...) die_msg(1, __VA_ARGS__)

#endif /* ERROR_H */
