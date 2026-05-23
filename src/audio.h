/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Multi-track audio routing.
 *
 * Sender side: takes a set of selected OBS audio source names, attaches an
 * audio_capture callback to each, feeds the PCM into an Opus encoder and
 * pushes the encoded packets onto a webrtc audio track.
 *
 * Receiver side: takes incoming Opus packets per track, decodes, and pushes
 * the PCM out as an OBS source's audio output.
 *
 * The Opus encoder/decoder is provided by libdatachannel's bundled libopus
 * (linked transitively); we use the symbols directly.
 */

#pragma once

#include <obs.h>
#include <stdbool.h>
#include <stdint.h>

#include "webrtc.h"

#define TETHER_AUDIO_MAX_TRACKS 6

typedef struct tether_audio_sender   tether_audio_sender_t;
typedef struct tether_audio_receiver tether_audio_receiver_t;

// ------------------- sender -------------------

typedef struct {
	tether_webrtc_t *webrtc;  // borrowed
	int sample_rate;          // 48000 recommended for Opus
	int channels;             // 1 (mono) or 2 (stereo) per track
	int bitrate_kbps;         // per track
} tether_audio_sender_config_t;

tether_audio_sender_t *tether_audio_sender_create(const tether_audio_sender_config_t *cfg);
void tether_audio_sender_release(tether_audio_sender_t *s);

// Attach an OBS audio source by name. Returns the WebRTC track id on success.
int tether_audio_sender_attach(tether_audio_sender_t *s, const char *obs_source_name,
			       const char *label);

void tether_audio_sender_detach(tether_audio_sender_t *s, int track_id);

// ------------------- receiver -------------------

typedef struct {
	int sample_rate;
	int channels;
} tether_audio_receiver_config_t;

tether_audio_receiver_t *tether_audio_receiver_create(
	const tether_audio_receiver_config_t *cfg);
void tether_audio_receiver_release(tether_audio_receiver_t *r);

// Called by webrtc when a track delivers a packet. The receiver decodes and
// hands PCM frames to the OBS source's audio output via obs_source_output_audio.
void tether_audio_receiver_push_packet(tether_audio_receiver_t *r, int track_id,
				       const uint8_t *data, size_t size, int64_t pts);

// Bind the receiver to an obs_source — its decoded PCM will be sent out via
// obs_source_output_audio on this source.
void tether_audio_receiver_bind_source(tether_audio_receiver_t *r, obs_source_t *src);
