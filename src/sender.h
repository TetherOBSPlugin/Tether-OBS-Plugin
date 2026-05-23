/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Sender side. Driven from the Qt tools-menu dialog in sender-dialog.cpp.
 *
 * A tether_sender_t binds to one OBS source (by name), opens a signaling
 * session, mints a token, runs the receiver waiting room and pushes
 * per-session H.264 + Opus tracks once a receiver is accepted. It captures
 * the bound source on the graphics thread via obs_add_main_render_callback +
 * gs_texrender + stagesurface map, hands the BGRA bytes to a worker thread,
 * and encodes from there.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tether_sender tether_sender_t;

typedef enum {
	TETHER_SENDER_STATE_IDLE,
	TETHER_SENDER_STATE_WAITING,
	TETHER_SENDER_STATE_PEER_CONNECTED,
	TETHER_SENDER_STATE_FAILED,
} tether_sender_state_t;

// Asynchronous notifications the dialog subscribes to so it can update the
// UI on the Qt thread. Implementations marshal to the Qt thread themselves —
// we just hand them the data; assume they capture by value.
typedef struct {
	void *user;
	void (*on_token)(void *user, const char *token);
	void (*on_pending)(void *user, const char *peer_id, const char *display_name, const char *fingerprint);
	void (*on_peer_state)(void *user, const char *peer_id, tether_sender_state_t state);
	void (*on_peer_gone)(void *user, const char *peer_id);
} tether_sender_callbacks_t;

typedef struct {
	const char *server_url;                // wss://... ; NULL = use compile-time default
	const char *stun_url;                  // optional
	const char *turn_url;                  // optional
	const char *turn_username;             // optional
	const char *turn_credential;           // optional
	const char *source_name;               // OBS source name to share, must be non-NULL
	const char *const *audio_source_names; // null-terminated; may be NULL
	int video_bitrate_kbps;                // 0 → 6000
	int max_receivers;                     // 0 → 4
	int token_ttl_minutes;                 // 0 → 30
	bool reusable_token;
	bool twitch_st_mode; // strip host mic
} tether_sender_config_t;

// Module-load hook. Currently no-op aside from announcing readiness; the
// tools-menu item is registered in plugin-main.c so it can pull in the
// obs-frontend-api dependency cleanly.
void tether_sender_register(void);
void tether_sender_shutdown(void);

// Lifecycle. The caller owns the returned handle and must release it on dialog
// close. Returns NULL on invalid config (e.g. NULL source_name).
tether_sender_t *tether_sender_create(const tether_sender_config_t *cfg, const tether_sender_callbacks_t *cbs);
void tether_sender_release(tether_sender_t *s);

// Waiting-room operations called from the dialog when the user clicks
// Accept/Reject on a pending row.
void tether_sender_accept(tether_sender_t *s, const char *peer_id, bool pin);
void tether_sender_reject(tether_sender_t *s, const char *peer_id);
void tether_sender_revoke_token(tether_sender_t *s);

// Latest issued token (NUL-terminated). Returns NULL until the signaling
// server has replied with one. Caller must not free.
const char *tether_sender_current_token(tether_sender_t *s);

#ifdef __cplusplus
}
#endif
