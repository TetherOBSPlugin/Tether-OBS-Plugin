/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Receiver-side OBS source. Registers an obs_source_info of kind
 * OBS_SOURCE_TYPE_INPUT with async video and audio outputs.
 *
 * The user pastes a token into the source's properties; the source then
 * dials the signaling server, waits for accept, and starts pushing the
 * incoming video frames + audio packets out to OBS.
 */

#pragma once

void tether_source_register(void);
void tether_source_shutdown(void);
