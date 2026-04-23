/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pager.h
 * @brief Paging support for long interactive output (e.g. manual pages).
 *
 * pager_start() spawns a pager (honoring $PAGER, defaulting to
 * @c less) when stdout is a tty and redirects stdout fd 1 through
 * its pipe.  Subsequent printf/fputs naturally flow to the pager.
 * On program exit, pager_end() flushes, restores the original
 * stdout, and reaps the pager process.
 *
 * Color tokens must still reach the pager so @c less -R can render
 * them -- see term.c's use_color_for() which treats stdout as
 * colorable while the pager is active.
 */
#ifndef PAGER_H
#define PAGER_H

/**
 * @brief Start the pager if stdout is an interactive terminal.
 *
 * No-op if stdout is already redirected, if the pager is already
 * running, or if the child process fails to spawn.  An atexit()
 * handler ensures the pager is closed cleanly when the process exits.
 */
void pager_start(void);

/** @brief Stop the pager (flush, restore stdout, reap). */
void pager_end(void);

/** @brief Non-zero when the pager pipe is currently bound to stdout. */
int pager_active(void);

#endif /* PAGER_H */
