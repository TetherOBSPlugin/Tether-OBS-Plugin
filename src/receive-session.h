/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Receive session: owns the signaling + WebRTC + video/audio decoder for one
 * receiver token, decoupled from any OBS source. The Tools→Tether→Receive
 * dialog creates a session as soon as the user registers a token, so the
 * connection comes up before any Tether source is added to a scene. A
 * Tether-Quelle source then subscribes to the matching session by token and
 * pulls decoded frames out via callbacks.
 *
 * One session per token, refcounted: the dialog holds one ref while the
 * token is registered; each subscribing source holds one ref while attached.
 * The session goes away once the last ref drops.
 *
 * Threading: signaling + WebRTC callbacks fire on libdatachannel's worker
 * thread. Subscribers must not assume any particular thread; the session
 * marshals where needed but frame callbacks are invoked from the decoder
 * worker thread.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tether_receive_session tether_receive_session_t;

typedef enum {
	TETHER_RX_STATE_CONNECTING,      // WS handshake in flight
	TETHER_RX_STATE_AWAITING_ACCEPT, // hello sent, waiting for sender
	TETHER_RX_STATE_ACCEPTED,        // sender accepted, awaiting SDP
	TETHER_RX_STATE_NEGOTIATING,     // SDP/ICE exchange in progress
	TETHER_RX_STATE_CONNECTED,       // DTLS up, media flowing
	TETHER_RX_STATE_FAILED,          // any terminal error
	TETHER_RX_STATE_CLOSED,
} tether_receive_state_t;

const char *tether_receive_state_name(tether_receive_state_t s);

// Frame callbacks. width/height are valid for video; sample_rate/channels for
// audio. `mid` identifies which SDP track the payload came in on; subscribers
// that only want one stream (e.g. one Tether-Quelle source picking one of N
// video tracks) filter on this. data is owned by the session for the
// duration of the call.
typedef void (*tether_rx_video_cb_t)(void *user, const uint8_t *data, size_t size, uint32_t width, uint32_t height,
				     int64_t pts, const char *mid);
typedef void (*tether_rx_audio_cb_t)(void *user, const uint8_t *data, size_t size, uint32_t sample_rate,
				     uint32_t channels, int64_t pts, const char *mid);
typedef void (*tether_rx_state_cb_t)(void *user, tether_receive_state_t state);

// Lists the mids of currently-attached video/audio tracks on the session.
// out[*count] is a malloc'd array of bstrdup'd strings; pass NULL to free.
char **tether_receive_session_video_mids(tether_receive_session_t *s, size_t *count);
char **tether_receive_session_audio_mids(tether_receive_session_t *s, size_t *count);
void tether_receive_session_free_mids(char **mids, size_t count);

// Registry: get or create a session for a token. Caller gets a ref-bumped
// pointer; release with tether_receive_session_release.
tether_receive_session_t *tether_receive_session_get(const char *token);

// Drop a ref. When refcount hits zero the session tears down its network.
void tether_receive_session_release(tether_receive_session_t *s);

// Current state — snapshot, may have changed by the time you read it.
tether_receive_state_t tether_receive_session_state(tether_receive_session_t *s);

// Token associated with this session (NUL-terminated, owned by session).
const char *tether_receive_session_token(tether_receive_session_t *s);

// Lightweight stats counters updated as media packets land. Snapshot is
// safe to take from any thread.
typedef struct {
	uint64_t video_bytes;
	uint64_t audio_bytes;
	uint64_t video_packets;
	uint64_t audio_packets;
	uint64_t last_update_ns; // os_gettime_ns at most recent packet
} tether_receive_stats_t;

void tether_receive_session_get_stats(tether_receive_session_t *s, tether_receive_stats_t *out);

// Subscribe to frames and state changes. Each subscription holds a ref while
// active; call _unsubscribe with the same handle to release. Returns NULL on
// invalid input.
typedef struct tether_rx_subscription tether_rx_subscription_t;

tether_rx_subscription_t *tether_receive_session_subscribe(tether_receive_session_t *s, tether_rx_video_cb_t on_video,
							   tether_rx_audio_cb_t on_audio, tether_rx_state_cb_t on_state,
							   void *user);
void tether_receive_session_unsubscribe(tether_rx_subscription_t *sub);

// Registry iteration: enumerate currently-live sessions for the dialog UI.
typedef void (*tether_rx_enum_cb_t)(void *user, tether_receive_session_t *session);
void tether_receive_session_enumerate(tether_rx_enum_cb_t cb, void *user);

// Module lifecycle.
void tether_receive_session_init(void);
void tether_receive_session_shutdown(void);

#ifdef __cplusplus
}
#endif
