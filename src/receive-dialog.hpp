/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Qt tools-menu receive dialog. Opened from the hub. Currently a thin layer
 * over the source-based receive flow — it surfaces a token entry + the list
 * of active Tether-Quelle source instances in the current scene collection,
 * so the user has a single place to see what's connected.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void tether_open_receive_dialog(void);
void tether_close_receive_dialog(void);

#ifdef __cplusplus
}
#endif
