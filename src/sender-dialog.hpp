/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Qt tools-menu dialog: "Tools → Tether Share". Picks a source + audio
 * tracks, generates a token, displays the waiting room with Accept /
 * Reject buttons.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Opens (or raises) the singleton sender dialog. Called from plugin-main.c
// when the user clicks the tools-menu entry. Thread-safe — must run on the
// Qt main thread but the entry point itself is safe to call from any thread
// (it posts to the GUI thread internally).
void tether_open_sender_dialog(void);

// Tears down the dialog if open. Called from obs_module_unload before
// libdatachannel global cleanup so any active senders are released first.
void tether_close_sender_dialog(void);

#ifdef __cplusplus
}
#endif
