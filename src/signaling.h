/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Signaling client. Talks to the Tether signaling backend over a single
 * WebSocket. The framing is line-delimited JSON, documented in
 * backend/README.md. This module is intentionally transport-agnostic
 * w.r.t. WebRTC: it does not parse SDP, it only forwards opaque payloads
 * between the local webrtc.c and the peer.
 *
 * Threading: the signaling client owns its own thread. All callbacks fire
 * on that thread; the caller must do its own locking if it touches OBS
 * state.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "token.h"

typedef struct tether_signaling tether_signaling_t;

typedef enum {
	TETHER_ROLE_SENDER,
	TETHER_ROLE_RECEIVER,
} tether_role_t;

typedef enum {
	TETHER_SIG_STATE_DISCONNECTED,
	TETHER_SIG_STATE_CONNECTING,
	TETHER_SIG_STATE_CONNECTED,
	TETHER_SIG_STATE_FAILED,
} tether_signaling_state_t;

typedef enum {
	// Sender-side
	TETHER_SIG_EVT_TOKEN_ISSUED,    // payload: token string
	TETHER_SIG_EVT_REQUEST_PENDING, // payload: { "peer_id", "name", "fingerprint" }
	TETHER_SIG_EVT_PEER_GONE,       // payload: peer_id

	// Receiver-side
	TETHER_SIG_EVT_REQUEST_ACCEPTED,
	TETHER_SIG_EVT_REQUEST_REJECTED,
	TETHER_SIG_EVT_TOKEN_INVALID,
	TETHER_SIG_EVT_TOKEN_LOCKED_OUT,

	// Both sides
	TETHER_SIG_EVT_SDP_OFFER,
	TETHER_SIG_EVT_SDP_ANSWER,
	TETHER_SIG_EVT_ICE_CANDIDATE,
	TETHER_SIG_EVT_STATE_CHANGED,
} tether_signaling_event_t;

typedef struct {
	const char *peer_id;
	const char *json_payload; // event-specific
} tether_signaling_msg_t;

typedef void (*tether_signaling_cb_t)(void *user, tether_signaling_event_t evt, const tether_signaling_msg_t *msg);

typedef struct {
	const char *server_url; // wss://...
	tether_role_t role;
	const char *display_name;
	const char *token;     // receiver-only; ignored for sender
	int token_ttl_minutes; // sender-only
	bool reusable_token;   // sender-only
	tether_signaling_cb_t cb;
	void *cb_user;
} tether_signaling_config_t;

tether_signaling_t *tether_signaling_create(const tether_signaling_config_t *cfg);
void tether_signaling_release(tether_signaling_t *sig);

bool tether_signaling_connect(tether_signaling_t *sig);
void tether_signaling_disconnect(tether_signaling_t *sig);
tether_signaling_state_t tether_signaling_state(const tether_signaling_t *sig);

// Sender controls
bool tether_signaling_accept(tether_signaling_t *sig, const char *peer_id);
bool tether_signaling_reject(tether_signaling_t *sig, const char *peer_id);
bool tether_signaling_revoke_token(tether_signaling_t *sig);

// Outbound SDP/ICE — these are only forwarded once the peer has been accepted.
bool tether_signaling_send_sdp(tether_signaling_t *sig, const char *peer_id, const char *sdp_type, const char *sdp);
bool tether_signaling_send_ice(tether_signaling_t *sig, const char *peer_id, const char *candidate, const char *mid,
			       int mline_index);
