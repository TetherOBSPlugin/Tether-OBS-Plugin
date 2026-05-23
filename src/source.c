/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Tether-Quelle source: viewport into a receive-session keyed by token. The
 * session (in receive-session.c) owns the network connection, so it stays
 * up between source create/destroy and across hub-dialog token registration.
 * This file just decodes the H.264 packets forwarded by the session and
 * pushes the result into the OBS source pipeline.
 */

#include "source.h"

#include <obs-module.h>
#include <obs.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include "audio.h"
#include "known-tokens.h"
#include "log.h"
#include "properties.h"
#include "receive-session.h"

#define TETHER_SOURCE_ID "tether_source"

struct tether_source {
	obs_source_t *source;

	char *token;     // currently-bound token; NULL means no session
	char *video_mid; // empty/NULL = render first / any video stream
	char *audio_mid; // empty/NULL = consume first / any audio stream
	tether_receive_session_t *session;
	tether_rx_subscription_t *sub;

	tether_audio_receiver_t *audio;

	AVCodecContext *vctx;
	AVPacket *vpkt;
	AVFrame *vframe;
	pthread_mutex_t decoder_lock;
};

static const char *get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Source.Receiver.Name");
}

static bool init_video_decoder(struct tether_source *s)
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		tether_log_error("source: H.264 decoder not available");
		return false;
	}
	s->vctx = avcodec_alloc_context3(codec);
	if (!s->vctx) {
		return false;
	}
	s->vctx->thread_count = 0;
	if (avcodec_open2(s->vctx, codec, NULL) < 0) {
		avcodec_free_context(&s->vctx);
		return false;
	}
	s->vpkt = av_packet_alloc();
	s->vframe = av_frame_alloc();
	if (!s->vpkt || !s->vframe) {
		if (s->vpkt) {
			av_packet_free(&s->vpkt);
		}
		if (s->vframe) {
			av_frame_free(&s->vframe);
		}
		avcodec_free_context(&s->vctx);
		return false;
	}
	return true;
}

static void release_video_decoder(struct tether_source *s)
{
	if (s->vframe) {
		av_frame_free(&s->vframe);
	}
	if (s->vpkt) {
		av_packet_free(&s->vpkt);
	}
	if (s->vctx) {
		avcodec_free_context(&s->vctx);
	}
}

static enum video_format av_to_obs_video_format(enum AVPixelFormat fmt)
{
	switch (fmt) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
		return VIDEO_FORMAT_I444;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

static void deliver_decoded_frame(struct tether_source *s, int64_t pts)
{
	enum video_format obs_fmt = av_to_obs_video_format(s->vframe->format);
	if (obs_fmt == VIDEO_FORMAT_NONE) {
		tether_log_warning("source: decoder produced unhandled pix_fmt=%d", s->vframe->format);
		return;
	}
	struct obs_source_frame frame = {0};
	frame.format = obs_fmt;
	frame.width = (uint32_t)s->vframe->width;
	frame.height = (uint32_t)s->vframe->height;
	frame.timestamp = pts > 0 ? (uint64_t)pts : os_gettime_ns();
	frame.full_range = (s->vframe->color_range == AVCOL_RANGE_JPEG);
	for (int p = 0; p < MAX_AV_PLANES && s->vframe->data[p]; ++p) {
		frame.data[p] = s->vframe->data[p];
		frame.linesize[p] = (uint32_t)s->vframe->linesize[p];
	}
	obs_source_output_video(s->source, &frame);
}

static void on_session_video(void *user, const uint8_t *data, size_t size, uint32_t w, uint32_t h, int64_t pts,
			     const char *mid)
{
	UNUSED_PARAMETER(w);
	UNUSED_PARAMETER(h);
	struct tether_source *s = user;
	if (!data || size == 0) {
		return;
	}
	// Filter: if the user pinned a specific video mid, drop packets from
	// the other tracks. Empty mid → consume whichever stream lands first
	// (single-source backwards compat).
	if (s->video_mid && *s->video_mid && mid && strcmp(s->video_mid, mid) != 0) {
		return;
	}
	pthread_mutex_lock(&s->decoder_lock);
	if (!s->vctx) {
		pthread_mutex_unlock(&s->decoder_lock);
		return;
	}
	av_packet_unref(s->vpkt);
	s->vpkt->data = (uint8_t *)data;
	s->vpkt->size = (int)size;
	s->vpkt->pts = pts;
	s->vpkt->dts = pts;
	int rc = avcodec_send_packet(s->vctx, s->vpkt);
	if (rc < 0 && rc != AVERROR(EAGAIN)) {
		pthread_mutex_unlock(&s->decoder_lock);
		return;
	}
	while ((rc = avcodec_receive_frame(s->vctx, s->vframe)) == 0) {
		deliver_decoded_frame(s, pts);
		av_frame_unref(s->vframe);
	}
	pthread_mutex_unlock(&s->decoder_lock);
}

static void on_session_audio(void *user, const uint8_t *data, size_t size, uint32_t sample_rate, uint32_t channels,
			     int64_t pts, const char *mid)
{
	UNUSED_PARAMETER(sample_rate);
	UNUSED_PARAMETER(channels);
	struct tether_source *s = user;
	if (s->audio_mid && *s->audio_mid && mid && strcmp(s->audio_mid, mid) != 0) {
		return;
	}
	// Track id of 0 is fine — audio.c uses it as a hash key, not a routing
	// label, so a single source attaching a single session collapses to one
	// Opus stream as intended.
	tether_audio_receiver_push_packet(s->audio, 0, data, size, pts);
}

static void on_session_state(void *user, tether_receive_state_t state)
{
	struct tether_source *s = user;
	tether_log_info("source(%s): session state → %s", s->token ? s->token : "(no token)",
			tether_receive_state_name(state));
}

static void attach_session(struct tether_source *s, const char *token)
{
	if (s->sub) {
		tether_receive_session_unsubscribe(s->sub);
		s->sub = NULL;
		s->session = NULL;
	}
	bfree(s->token);
	s->token = token && *token ? bstrdup(token) : NULL;
	if (!s->token) {
		return;
	}
	s->session = tether_receive_session_get(s->token);
	if (!s->session) {
		tether_log_warning("source: failed to create session for token '%s'", s->token);
		return;
	}
	// Track the token in the known list as well so adding a source with a
	// fresh token surfaces it in the hub-dialog Sessions view.
	tether_known_tokens_add(s->token);
	s->sub = tether_receive_session_subscribe(s->session, on_session_video, on_session_audio, on_session_state, s);
	// The subscription bumps refcount; release our initial get_or_create
	// ref so the session is owned exclusively by the subscription.
	tether_receive_session_release(s->session);
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	struct tether_source *s = bzalloc(sizeof(*s));
	s->source = source;
	pthread_mutex_init(&s->decoder_lock, NULL);

	tether_audio_receiver_config_t arcfg = {.sample_rate = 48000, .channels = 2};
	s->audio = tether_audio_receiver_create(&arcfg);
	tether_audio_receiver_bind_source(s->audio, source);

	if (!init_video_decoder(s)) {
		tether_log_warning("source: video decoder init failed");
	}

	const char *tok = obs_data_get_string(settings, "token");
	const char *vmid = obs_data_get_string(settings, "video_mid");
	const char *amid = obs_data_get_string(settings, "audio_mid");
	s->video_mid = (vmid && *vmid) ? bstrdup(vmid) : NULL;
	s->audio_mid = (amid && *amid) ? bstrdup(amid) : NULL;
	attach_session(s, tok);
	return s;
}

static void destroy(void *data)
{
	struct tether_source *s = data;
	if (s->sub) {
		tether_receive_session_unsubscribe(s->sub);
	}
	pthread_mutex_lock(&s->decoder_lock);
	release_video_decoder(s);
	pthread_mutex_unlock(&s->decoder_lock);
	if (s->audio) {
		tether_audio_receiver_release(s->audio);
	}
	bfree(s->token);
	bfree(s->video_mid);
	bfree(s->audio_mid);
	pthread_mutex_destroy(&s->decoder_lock);
	bfree(s);
}

static void update(void *data, obs_data_t *settings)
{
	struct tether_source *s = data;
	const char *tok = obs_data_get_string(settings, "token");
	const char *vmid = obs_data_get_string(settings, "video_mid");
	const char *amid = obs_data_get_string(settings, "audio_mid");
	if (!s->token || !tok || strcmp(s->token ? s->token : "", tok) != 0) {
		attach_session(s, tok);
	}
	bfree(s->video_mid);
	s->video_mid = (vmid && *vmid) ? bstrdup(vmid) : NULL;
	bfree(s->audio_mid);
	s->audio_mid = (amid && *amid) ? bstrdup(amid) : NULL;
}

static void defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "token", "");
}

static obs_properties_t *get_properties(void *data)
{
	struct tether_source *s = data;
	return tether_properties_for_receiver(s ? s->source : NULL);
}

static struct obs_source_info source_info = {
	.id = TETHER_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = get_name,
	.create = create,
	.destroy = destroy,
	.get_defaults = defaults,
	.get_properties = get_properties,
	.update = update,
	.icon_type = OBS_ICON_TYPE_CUSTOM,
};

void tether_source_register(void)
{
	obs_register_source(&source_info);
	tether_log_debug("source: registered %s", TETHER_SOURCE_ID);
}

void tether_source_shutdown(void)
{
	// obs_unregister_source is not exposed; OBS unloads the module and
	// drops the registration itself. Nothing to do here.
}
