/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Sender side. Registered as an obs_source_info of OBS_SOURCE_TYPE_FILTER
 * that attaches to any video source; once enabled it asks the signaling
 * backend for a token, shows the waiting room, and on accept it starts
 * pushing the source's video plus selected audio tracks over WebRTC.
 *
 * Registering as a filter (rather than a separate source) lets users put
 * Tether on any existing source in the scene without re-architecting the
 * scene graph. The filter is a pass-through; it does not modify the visible
 * output.
 */

#pragma once

void tether_sender_register(void);
void tether_sender_shutdown(void);
