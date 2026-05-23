/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Optional CLI control plane. Opens a Unix-domain socket at
 *     /tmp/tether-<pid>.sock
 * (when TETHER_CONTROL=1 is set in the environment of the OBS process) and
 * accepts simple line commands so the sender/receive APIs can be driven
 * from scripts. Intended for automated end-to-end tests where clicking the
 * GUI is impractical.
 *
 * Commands (one per connection, newline-terminated):
 *
 *   sender-create <source_name>[,<audio1>,<audio2>...]
 *       Spin up a sender bound to the named video source plus optional
 *       audio sources (comma-separated). Replies with the freshly minted
 *       token once signaling acks the hello, or ERR <reason>.
 *
 *   sender-status
 *       Replies "OK token=<token> peers=<id>:<state>:<name>,<id>:..."
 *
 *   sender-accept <peer_id>
 *       Accept a pending peer.
 *
 *   sender-reject <peer_id>
 *       Reject a pending peer.
 *
 *   sender-disconnect <peer_id>
 *       Tear down an already-accepted peer.
 *
 *   sender-revoke
 *       Revoke the current token + tear down.
 *
 *   receive-register <token>
 *       Add token to known list and bring up its receive session.
 *
 *   receive-status
 *       Lists known tokens with their session state.
 *
 *   receive-forget <token>
 *       Remove a token; tears down its session if active.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void tether_control_init(void);
void tether_control_shutdown(void);

#ifdef __cplusplus
}
#endif
