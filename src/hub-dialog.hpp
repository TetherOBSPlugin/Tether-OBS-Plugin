/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Tools-menu hub dialog. First UI step: choose Share or Receive. Dispatches
 * to the sender dialog or to the receive dialog. See sender-dialog.hpp and
 * receive-dialog.hpp for the per-role dialogs.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Opens (or raises) the hub picker on the Qt main thread. Safe to call from
// any thread; posts onto the GUI thread internally.
void tether_open_hub_dialog(void);

// Tears down any open hub/sender/receive dialog. Called from
// obs_module_unload before libdatachannel global cleanup.
void tether_close_all_dialogs(void);

#ifdef __cplusplus
}
#endif
