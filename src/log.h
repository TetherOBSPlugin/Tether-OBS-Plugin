/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Thin wrapper around obs_log()/blog() that prepends a "[tether]" tag.
 * Use these macros everywhere instead of raw blog() so the log prefix
 * stays consistent across modules.
 */

#pragma once

#include <obs-module.h>
#include <util/base.h>

#define TETHER_LOG_PREFIX "[tether] "

#define tether_log_debug(fmt, ...)   blog(LOG_DEBUG,   TETHER_LOG_PREFIX fmt, ##__VA_ARGS__)
#define tether_log_info(fmt, ...)    blog(LOG_INFO,    TETHER_LOG_PREFIX fmt, ##__VA_ARGS__)
#define tether_log_warning(fmt, ...) blog(LOG_WARNING, TETHER_LOG_PREFIX fmt, ##__VA_ARGS__)
#define tether_log_error(fmt, ...)   blog(LOG_ERROR,   TETHER_LOG_PREFIX fmt, ##__VA_ARGS__)
