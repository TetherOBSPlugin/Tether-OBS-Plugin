/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * WebRTC peer-connection wrapper around libdatachannel.
 *
 * One tether_webrtc_t per peer (sender↔receiver pair). The sender owns N
 * such objects, one per accepted receiver. Each carries up to one video
 * track and N audio tracks.
 *
 * Threading: libdatachannel callbacks fire on its internal worker thread.
 * The caller passes callbacks that must be safe from that thread; this
 * module does no marshalling onto the OBS graphics thread — that is the
 * source/sender module's job.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "signaling.h"

bool tether_webrtc_global_init(void);
void tether_webrtc_global_shutdown(void);

typedef struct tether_webrtc tether_webrtc_t;

typedef enum {
	TETHER_WRTC_STATE_NEW,
	TETHER_WRTC_STATE_CONNECTING,
	TETHER_WRTC_STATE_CONNECTED,
	TETHER_WRTC_STATE_FAILED,
	TETHER_WRTC_STATE_CLOSED,
} tether_webrtc_state_t;

typedef enum {
	TETHER_CODEC_H264,
	TETHER_CODEC_VP9,
	TETHER_CODEC_AV1,
} tether_video_codec_t;

typedef void (*tether_wrtc_local_sdp_cb_t)(void *user, const char *sdp_type, const char *sdp);
typedef void (*tether_wrtc_local_ice_cb_t)(void *user, const char *candidate, const char *mid, int mline_index);
typedef void (*tether_wrtc_state_cb_t)(void *user, tether_webrtc_state_t state);
typedef void (*tether_wrtc_video_cb_t)(void *user, const uint8_t *data, size_t size, int64_t pts, int track_id);
typedef void (*tether_wrtc_audio_cb_t)(void *user, const uint8_t *data, size_t size, int64_t pts, int track_id);

typedef struct {
	const char *stun_url;
	const char *turn_url;
	const char *turn_username;
	const char *turn_credential;
	tether_video_codec_t video_codec;
	int max_bitrate_kbps;
	bool is_offerer;

	tether_wrtc_local_sdp_cb_t on_local_sdp;
	tether_wrtc_local_ice_cb_t on_local_ice;
	tether_wrtc_state_cb_t on_state;
	tether_wrtc_video_cb_t on_video;
	tether_wrtc_audio_cb_t on_audio;
	void *user;
} tether_webrtc_config_t;

tether_webrtc_t *tether_webrtc_create(const tether_webrtc_config_t *cfg);
void tether_webrtc_release(tether_webrtc_t *w);

// Inbound from the remote peer.
bool tether_webrtc_apply_remote_sdp(tether_webrtc_t *w, const char *sdp_type, const char *sdp);
bool tether_webrtc_add_remote_ice(tether_webrtc_t *w, const char *candidate, const char *mid);

// Track management. Returns a stable track id, or -1 on failure.
int tether_webrtc_add_video_track(tether_webrtc_t *w);
int tether_webrtc_add_audio_track(tether_webrtc_t *w, const char *label);

// Initiate (or restart) negotiation. For media tracks libdatachannel's auto-
// negotiation does not fire on its own — the offerer must call this after
// adding tracks, and the answerer needs no explicit call (setRemoteDescription
// triggers the answer generation).
bool tether_webrtc_negotiate(tether_webrtc_t *w);

// Outbound media (sender side). The payload format is codec-specific — for
// H.264 we expect Annex-B NAL units; for Opus, an opaque packet.
bool tether_webrtc_push_video(tether_webrtc_t *w, int track_id, const uint8_t *data, size_t size, int64_t pts);
bool tether_webrtc_push_audio(tether_webrtc_t *w, int track_id, const uint8_t *data, size_t size, int64_t pts);

// Returns the local DTLS fingerprint in the form "sha-256 AA:BB:..." after
// the connection has produced its local description. The buffer is owned by
// the caller; pass at least 128 bytes.
bool tether_webrtc_local_fingerprint(tether_webrtc_t *w, char *out, size_t out_size);

tether_webrtc_state_t tether_webrtc_state(const tether_webrtc_t *w);
