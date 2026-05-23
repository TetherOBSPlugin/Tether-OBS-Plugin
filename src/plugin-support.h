/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Boilerplate header consumed by the obs-plugintemplate CMake helper.
 * Defines the `PLUGIN_NAME` / `PLUGIN_VERSION` symbols used in plugin-main.c
 * and elsewhere. The companion source file is generated from
 * plugin-support.c.in.
 */

#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

#ifdef __cplusplus
}
#endif
