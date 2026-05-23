/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Properties builder for the receiver OBS source (paste-the-token side).
 * Sender properties live in the Qt dialog at src/sender-dialog.cpp.
 *
 * Keeps all UI-string lookups (obs_module_text) in one place so the rest of
 * the codebase does not sprinkle them across the source.c file.
 */

#pragma once

#include <obs.h>

obs_properties_t *tether_properties_for_receiver(obs_source_t *src);

// Returns the default managed signaling endpoint. Compile-time constant.
const char *tether_default_server_url(void);
