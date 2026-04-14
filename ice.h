/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ice.h
 * @brief Project-wide header -- include first in every .c file.
 *
 * Pulls in standard headers, the platform abstraction layer, and
 * all common project modules so that every translation unit starts
 * from a common baseline.
 */
#ifndef ICE_H
#define ICE_H

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "ar.h"
#include "cmakecache.h"
#include "config.h"
#include "term.h"
#include "elf.h"
#include "error.h"
#include "http.h"
#include "json.h"
#include "map.h"
#include "options.h"
#include "platform.h"
#include "process.h"
#include "sbuf.h"
#include "svec.h"

/* Subcommands -- cmake targets (cmd/cmake/) */
int cmd_build(int argc, const char **argv);
int cmd_clean(int argc, const char **argv);
int cmd_flash(int argc, const char **argv);
int cmd_menuconfig(int argc, const char **argv);
int cmd_reconfigure(int argc, const char **argv);

/* Subcommands -- standalone */
int cmd_config(int argc, const char **argv);
int cmd_configdep(int argc, const char **argv);
int cmd_ldgen(int argc, const char **argv);
int cmd_size(int argc, const char **argv);

#endif /* ICE_H */
