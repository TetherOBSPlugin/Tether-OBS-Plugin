/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Properties builder. Returns an obs_properties_t configured with the
 * fields the receiver source and the sender filter expose to the user.
 *
 * Keeps all UI-string lookups (obs_module_text) in one place so the rest
 * of the codebase does not sprinkle them across the source/sender files.
 */

#pragma once

#include <obs.h>

obs_properties_t *tether_properties_for_receiver(obs_source_t *src);
obs_properties_t *tether_properties_for_sender(obs_source_t *parent);

// Returns the default managed signaling endpoint. Compile-time constant.
const char *tether_default_server_url(void);
