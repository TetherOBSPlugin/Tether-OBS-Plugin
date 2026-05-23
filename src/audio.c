/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "audio.h"

#include <stdlib.h>
#include <string.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include "log.h"

// libopus is bundled by libdatachannel; declare the few symbols we need
// rather than pull a full opus.h dependency in the public include list.
typedef struct OpusEncoder OpusEncoder;
typedef struct OpusDecoder OpusDecoder;

extern OpusEncoder *opus_encoder_create(int Fs, int channels, int application, int *error);
extern void opus_encoder_destroy(OpusEncoder *st);
extern int  opus_encode_float(OpusEncoder *st, const float *pcm, int frame_size,
			      unsigned char *data, int max_data_bytes);
extern int  opus_encoder_ctl(OpusEncoder *st, int request, ...);

extern OpusDecoder *opus_decoder_create(int Fs, int channels, int *error);
extern void opus_decoder_destroy(OpusDecoder *st);
extern int  opus_decode_float(OpusDecoder *st, const unsigned char *data, int len,
			      float *pcm, int frame_size, int decode_fec);

#define OPUS_APPLICATION_AUDIO    2049
#define OPUS_SET_BITRATE_REQUEST  4002

#define OPUS_FRAME_SIZE_MS 20  // standard low-latency setting

// ------------------- sender -------------------

struct sender_track {
	int track_id;
	char obs_source_name[256];
	obs_source_t *source;  // strong ref
	OpusEncoder *enc;
	int channels;
	int sample_rate;

	// Ring buffer of float frames waiting to be encoded into a 20 ms block.
	float *buf;
	size_t buf_capacity;
	size_t buf_fill;

	pthread_mutex_t lock;
	tether_audio_sender_t *owner;  // back-ref for callback access
};

struct tether_audio_sender {
	tether_audio_sender_config_t cfg;
	DARRAY(struct sender_track *) tracks;
	pthread_mutex_t lock;
};

static void encode_and_send(struct sender_track *t)
{
	const size_t frame_samples =
		(size_t)t->sample_rate * OPUS_FRAME_SIZE_MS / 1000;
	const size_t needed = frame_samples * (size_t)t->channels;
	if (t->buf_fill < needed) {
		return;
	}

	uint8_t out[1500];
	int n = opus_encode_float(t->enc, t->buf, (int)frame_samples, out, sizeof(out));
	if (n > 0) {
		tether_webrtc_push_audio(t->owner->cfg.webrtc, t->track_id, out,
					 (size_t)n, 0);
	}

	// Drop the consumed samples from the head of the buffer.
	memmove(t->buf, t->buf + needed, (t->buf_fill - needed) * sizeof(float));
	t->buf_fill -= needed;
}

static void on_audio_capture(void *param, obs_source_t *source,
			     const struct audio_data *audio_data, bool muted)
{
	(void)source;
	if (muted || !audio_data) {
		return;
	}
	struct sender_track *t = param;

	// audio_data->data[i] are per-channel planar float32 buffers.
	const size_t frames = audio_data->frames;
	const size_t add = frames * (size_t)t->channels;

	pthread_mutex_lock(&t->lock);
	if (t->buf_fill + add > t->buf_capacity) {
		// Drop oldest by realigning.
		size_t drop = t->buf_fill + add - t->buf_capacity;
		memmove(t->buf, t->buf + drop, (t->buf_fill - drop) * sizeof(float));
		t->buf_fill -= drop;
	}
	// Interleave channels into the encoder buffer.
	float *dst = t->buf + t->buf_fill;
	for (size_t f = 0; f < frames; ++f) {
		for (int c = 0; c < t->channels; ++c) {
			const float *plane = (const float *)audio_data->data[c];
			dst[f * (size_t)t->channels + c] = plane ? plane[f] : 0.0f;
		}
	}
	t->buf_fill += add;

	while (t->buf_fill * sizeof(float) >= 1) {
		size_t before = t->buf_fill;
		encode_and_send(t);
		if (t->buf_fill == before) {
			break;
		}
	}
	pthread_mutex_unlock(&t->lock);
}

tether_audio_sender_t *tether_audio_sender_create(const tether_audio_sender_config_t *cfg)
{
	tether_audio_sender_t *s = bzalloc(sizeof(*s));
	s->cfg = *cfg;
	if (s->cfg.sample_rate <= 0) {
		s->cfg.sample_rate = 48000;
	}
	if (s->cfg.channels <= 0) {
		s->cfg.channels = 2;
	}
	if (s->cfg.bitrate_kbps <= 0) {
		s->cfg.bitrate_kbps = 96;
	}
	pthread_mutex_init(&s->lock, NULL);
	return s;
}

void tether_audio_sender_release(tether_audio_sender_t *s)
{
	if (!s) {
		return;
	}
	for (size_t i = 0; i < s->tracks.num; ++i) {
		struct sender_track *t = s->tracks.array[i];
		if (t->source) {
			obs_source_remove_audio_capture_callback(t->source,
								 on_audio_capture, t);
			obs_source_release(t->source);
		}
		if (t->enc) {
			opus_encoder_destroy(t->enc);
		}
		bfree(t->buf);
		pthread_mutex_destroy(&t->lock);
		bfree(t);
	}
	da_free(s->tracks);
	pthread_mutex_destroy(&s->lock);
	bfree(s);
}

int tether_audio_sender_attach(tether_audio_sender_t *s, const char *obs_source_name,
			       const char *label)
{
	if (!s || !obs_source_name) {
		return -1;
	}
	obs_source_t *src = obs_get_source_by_name(obs_source_name);
	if (!src) {
		tether_log_warning("audio: source not found: %s", obs_source_name);
		return -1;
	}

	int track_id = tether_webrtc_add_audio_track(s->cfg.webrtc, label);
	if (track_id < 0) {
		obs_source_release(src);
		return -1;
	}

	struct sender_track *t = bzalloc(sizeof(*t));
	t->track_id = track_id;
	t->source = src;
	t->channels = s->cfg.channels;
	t->sample_rate = s->cfg.sample_rate;
	t->owner = s;
	strncpy(t->obs_source_name, obs_source_name, sizeof(t->obs_source_name) - 1);
	pthread_mutex_init(&t->lock, NULL);

	int err = 0;
	t->enc = opus_encoder_create(t->sample_rate, t->channels,
				     OPUS_APPLICATION_AUDIO, &err);
	if (!t->enc || err != 0) {
		tether_log_error("audio: opus encoder create rc=%d", err);
		obs_source_release(src);
		bfree(t);
		return -1;
	}
	opus_encoder_ctl(t->enc, OPUS_SET_BITRATE_REQUEST,
			 s->cfg.bitrate_kbps * 1000);

	// Buffer up to 200 ms.
	t->buf_capacity =
		(size_t)t->sample_rate * 200 / 1000 * (size_t)t->channels;
	t->buf = bzalloc(t->buf_capacity * sizeof(float));

	obs_source_add_audio_capture_callback(src, on_audio_capture, t);

	pthread_mutex_lock(&s->lock);
	da_push_back(s->tracks, &t);
	pthread_mutex_unlock(&s->lock);

	tether_log_info("audio: attached '%s' as track %d (%d Hz, %d ch)",
			obs_source_name, track_id, t->sample_rate, t->channels);
	return track_id;
}

void tether_audio_sender_detach(tether_audio_sender_t *s, int track_id)
{
	if (!s) {
		return;
	}
	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < s->tracks.num; ++i) {
		struct sender_track *t = s->tracks.array[i];
		if (t->track_id != track_id) {
			continue;
		}
		da_erase(s->tracks, i);
		pthread_mutex_unlock(&s->lock);

		if (t->source) {
			obs_source_remove_audio_capture_callback(t->source,
								 on_audio_capture, t);
			obs_source_release(t->source);
		}
		if (t->enc) {
			opus_encoder_destroy(t->enc);
		}
		bfree(t->buf);
		pthread_mutex_destroy(&t->lock);
		bfree(t);
		return;
	}
	pthread_mutex_unlock(&s->lock);
}

// ------------------- receiver -------------------

struct receiver_track {
	int track_id;
	OpusDecoder *dec;
};

struct tether_audio_receiver {
	tether_audio_receiver_config_t cfg;
	DARRAY(struct receiver_track *) tracks;
	obs_source_t *bound_source;  // weak (the source owns us)
	pthread_mutex_t lock;
};

tether_audio_receiver_t *tether_audio_receiver_create(
	const tether_audio_receiver_config_t *cfg)
{
	tether_audio_receiver_t *r = bzalloc(sizeof(*r));
	r->cfg = *cfg;
	if (r->cfg.sample_rate <= 0) {
		r->cfg.sample_rate = 48000;
	}
	if (r->cfg.channels <= 0) {
		r->cfg.channels = 2;
	}
	pthread_mutex_init(&r->lock, NULL);
	return r;
}

void tether_audio_receiver_release(tether_audio_receiver_t *r)
{
	if (!r) {
		return;
	}
	for (size_t i = 0; i < r->tracks.num; ++i) {
		struct receiver_track *t = r->tracks.array[i];
		if (t->dec) {
			opus_decoder_destroy(t->dec);
		}
		bfree(t);
	}
	da_free(r->tracks);
	pthread_mutex_destroy(&r->lock);
	bfree(r);
}

void tether_audio_receiver_bind_source(tether_audio_receiver_t *r, obs_source_t *src)
{
	if (!r) {
		return;
	}
	r->bound_source = src;
}

static struct receiver_track *get_or_create_track(tether_audio_receiver_t *r, int id)
{
	for (size_t i = 0; i < r->tracks.num; ++i) {
		if (r->tracks.array[i]->track_id == id) {
			return r->tracks.array[i];
		}
	}
	int err = 0;
	OpusDecoder *dec = opus_decoder_create(r->cfg.sample_rate, r->cfg.channels, &err);
	if (!dec || err != 0) {
		tether_log_error("audio: opus decoder create rc=%d", err);
		return NULL;
	}
	struct receiver_track *t = bzalloc(sizeof(*t));
	t->track_id = id;
	t->dec = dec;
	da_push_back(r->tracks, &t);
	return t;
}

void tether_audio_receiver_push_packet(tether_audio_receiver_t *r, int track_id,
				       const uint8_t *data, size_t size, int64_t pts)
{
	(void)pts;
	if (!r || !data || size == 0 || !r->bound_source) {
		return;
	}
	pthread_mutex_lock(&r->lock);
	struct receiver_track *t = get_or_create_track(r, track_id);
	if (!t) {
		pthread_mutex_unlock(&r->lock);
		return;
	}

	// Decode up to 120 ms at 48 kHz stereo — Opus' max frame.
	const int max_frames = r->cfg.sample_rate * 120 / 1000;
	float *pcm = bmalloc((size_t)max_frames * (size_t)r->cfg.channels * sizeof(float));
	int frames = opus_decode_float(t->dec, data, (int)size, pcm, max_frames, 0);
	pthread_mutex_unlock(&r->lock);

	if (frames <= 0) {
		bfree(pcm);
		return;
	}

	struct obs_source_audio out = {0};
	out.frames = (uint32_t)frames;
	out.samples_per_sec = (uint32_t)r->cfg.sample_rate;
	out.speakers = r->cfg.channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
	out.format = AUDIO_FORMAT_FLOAT;

	// OBS expects planar audio. Deinterleave.
	float *plane[MAX_AV_PLANES] = {0};
	float *deinterleaved = bmalloc((size_t)frames * (size_t)r->cfg.channels * sizeof(float));
	for (int c = 0; c < r->cfg.channels; ++c) {
		plane[c] = deinterleaved + (size_t)c * (size_t)frames;
		for (int f = 0; f < frames; ++f) {
			plane[c][f] = pcm[f * r->cfg.channels + c];
		}
		out.data[c] = (uint8_t *)plane[c];
	}
	out.timestamp = os_gettime_ns();

	obs_source_output_audio(r->bound_source, &out);

	bfree(deinterleaved);
	bfree(pcm);
}
