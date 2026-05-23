/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "sender.h"

#include <obs-module.h>
#include <obs.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "admission.h"
#include "audio.h"
#include "log.h"
#include "properties.h"
#include "signaling.h"
#include "token.h"
#include "webrtc.h"

static inline char *dup_span(const char *p, size_t n)
{
	char *out = bmalloc(n + 1);
	memcpy(out, p, n);
	out[n] = '\0';
	return out;
}

// Per-session user-data passed to libdatachannel callbacks so the SDP / ICE /
// state callbacks can identify the peer they fired for.
struct session_ctx {
	struct tether_sender *sender;
	char peer_id[TETHER_PEER_ID_MAX];
};

// Per-source encoder state inside a receiver session. With multi-source
// senders, each session holds N of these — one per captured video source —
// pushing into N separate WebRTC video tracks under unique mids.
struct source_encoder {
	int video_track_id;
	char mid[64];
	AVCodecContext *enc;
	AVFrame *enc_in;
	AVPacket *enc_pkt;
	struct SwsContext *sws;
	int enc_width;
	int enc_height;
	int64_t frame_pts;
};

struct receiver_session {
	char peer_id[TETHER_PEER_ID_MAX];
	tether_webrtc_t *wrtc;
	tether_audio_sender_t *audio;
	struct session_ctx *ctx;

	// One entry per captured video source, same order as sender->captures.
	DARRAY(struct source_encoder *) src_encoders;
};

// One capture pipeline per video source the sender is sharing. Lives only on
// the graphics thread (texrender/stagesurf) but exposes its latest captured
// frame to the encoder worker under sender->frame_lock.
struct video_capture {
	char source_name[256];
	char mid[64];
	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurf;
	uint32_t stage_w;
	uint32_t stage_h;

	// Producer/consumer buffer — graphics thread fills, worker thread
	// consumes. No queue: only the latest frame matters.
	uint8_t *latest_frame;
	size_t latest_frame_size;
	uint32_t latest_frame_w;
	uint32_t latest_frame_h;
	uint32_t latest_frame_linesize;
	bool frame_ready;
};

struct tether_sender {
	tether_sender_config_t cfg_owned;
	DARRAY(char *) audio_source_names;
	DARRAY(struct video_capture *) captures;

	tether_sender_callbacks_t cbs;

	tether_signaling_t *sig;
	tether_admission_t *adm;

	pthread_mutex_t lock;
	DARRAY(struct receiver_session *) sessions;

	char current_token[TETHER_TOKEN_BUF];

	// Single frame_lock guards the per-capture latest_frame buffers and
	// frame_ready flags. The worker wakes whenever ANY capture flips
	// frame_ready true; it then encodes every ready capture.
	pthread_mutex_t frame_lock;
	pthread_cond_t frame_cond;
	bool worker_running;
	pthread_t worker;
};

static void session_local_sdp(void *user, const char *type, const char *sdp);
static void session_local_ice(void *user, const char *cand, const char *mid, int mline);
static void session_state(void *user, tether_webrtc_state_t state);

static void on_admission_changed(void *user, const tether_peer_t *peer);
static void on_signaling_event(void *user, tether_signaling_event_t evt, const tether_signaling_msg_t *msg);

static void graphics_render_cb(void *param, uint32_t cx, uint32_t cy);
static void *encoder_worker_main(void *param);

// ---- helpers -----------------------------------------------------------

static struct receiver_session *find_session_locked(struct tether_sender *s, const char *peer_id)
{
	for (size_t i = 0; i < s->sessions.num; ++i) {
		if (strcmp(s->sessions.array[i]->peer_id, peer_id) == 0) {
			return s->sessions.array[i];
		}
	}
	return NULL;
}

static bool ensure_source_encoder(struct source_encoder *r, int w, int h, int bitrate_kbps)
{
	if (r->enc && r->enc_width == w && r->enc_height == h) {
		return true;
	}
	if (r->sws) {
		sws_freeContext(r->sws);
		r->sws = NULL;
	}
	if (r->enc_pkt) {
		av_packet_free(&r->enc_pkt);
	}
	if (r->enc_in) {
		av_frame_free(&r->enc_in);
	}
	if (r->enc) {
		avcodec_free_context(&r->enc);
	}
	r->enc_width = w;
	r->enc_height = h;
	r->frame_pts = 0;

	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!codec) {
		return false;
	}
	r->enc = avcodec_alloc_context3(codec);
	if (!r->enc) {
		return false;
	}
	r->enc->width = w;
	r->enc->height = h;
	r->enc->pix_fmt = AV_PIX_FMT_YUV420P;
	r->enc->time_base = (AVRational){1, 1000000};
	r->enc->framerate = (AVRational){30, 1};
	r->enc->gop_size = 60;
	r->enc->max_b_frames = 0;
	r->enc->bit_rate = (int64_t)bitrate_kbps * 1000;
	av_opt_set(r->enc->priv_data, "preset", "veryfast", 0);
	av_opt_set(r->enc->priv_data, "tune", "zerolatency", 0);
	av_opt_set(r->enc->priv_data, "profile", "baseline", 0);
	if (avcodec_open2(r->enc, codec, NULL) < 0) {
		avcodec_free_context(&r->enc);
		return false;
	}
	r->enc_in = av_frame_alloc();
	r->enc_pkt = av_packet_alloc();
	if (!r->enc_in || !r->enc_pkt) {
		return false;
	}
	r->enc_in->format = AV_PIX_FMT_YUV420P;
	r->enc_in->width = w;
	r->enc_in->height = h;
	if (av_frame_get_buffer(r->enc_in, 32) < 0) {
		return false;
	}
	r->sws = sws_getContext(w, h, AV_PIX_FMT_BGRA, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
	return r->sws != NULL;
}

static void release_source_encoder(struct source_encoder *e)
{
	if (!e) {
		return;
	}
	if (e->sws) {
		sws_freeContext(e->sws);
	}
	if (e->enc_pkt) {
		av_packet_free(&e->enc_pkt);
	}
	if (e->enc_in) {
		av_frame_free(&e->enc_in);
	}
	if (e->enc) {
		avcodec_free_context(&e->enc);
	}
	bfree(e);
}

static void release_session(struct receiver_session *r)
{
	if (!r) {
		return;
	}
	for (size_t i = 0; i < r->src_encoders.num; ++i) {
		release_source_encoder(r->src_encoders.array[i]);
	}
	da_free(r->src_encoders);
	if (r->audio) {
		tether_audio_sender_release(r->audio);
	}
	if (r->wrtc) {
		tether_webrtc_release(r->wrtc);
	}
	bfree(r->ctx);
	bfree(r);
}

static const char *fallback_or(const char *value, const char *fallback)
{
	return (value && *value) ? value : fallback;
}

static void create_session(struct tether_sender *s, const tether_peer_t *peer)
{
	struct session_ctx *ctx = bzalloc(sizeof(*ctx));
	ctx->sender = s;
	strncpy(ctx->peer_id, peer->peer_id, sizeof(ctx->peer_id) - 1);

	tether_webrtc_config_t wcfg = {
		.stun_url = fallback_or(s->cfg_owned.stun_url, "stun:stun.cloudflare.com:3478"),
		.turn_url = s->cfg_owned.turn_url,
		.turn_username = s->cfg_owned.turn_username,
		.turn_credential = s->cfg_owned.turn_credential,
		.video_codec = TETHER_CODEC_H264,
		.max_bitrate_kbps = s->cfg_owned.video_bitrate_kbps,
		.is_offerer = true,
		.on_local_sdp = session_local_sdp,
		.on_local_ice = session_local_ice,
		.on_state = session_state,
		.user = ctx,
	};
	tether_webrtc_t *w = tether_webrtc_create(&wcfg);
	if (!w) {
		bfree(ctx);
		return;
	}

	struct receiver_session *r = bzalloc(sizeof(*r));
	strncpy(r->peer_id, peer->peer_id, sizeof(r->peer_id) - 1);
	r->ctx = ctx;
	r->wrtc = w;
	da_init(r->src_encoders);

	// Add one WebRTC video track per captured source. Each gets a unique
	// mid (videoN) so the receiver can address them individually.
	for (size_t i = 0; i < s->captures.num; ++i) {
		struct video_capture *cap = s->captures.array[i];
		struct source_encoder *se = bzalloc(sizeof(*se));
		strncpy(se->mid, cap->mid, sizeof(se->mid) - 1);
		se->video_track_id = tether_webrtc_add_video_track_ex(w, cap->mid, cap->source_name);
		da_push_back(r->src_encoders, &se);
	}

	tether_audio_sender_config_t arcfg = {
		.webrtc = w,
		.sample_rate = 48000,
		.channels = 2,
		.bitrate_kbps = 96,
	};
	r->audio = tether_audio_sender_create(&arcfg);
	for (size_t i = 0; i < s->audio_source_names.num; ++i) {
		const char *name = s->audio_source_names.array[i];
		if (s->cfg_owned.twitch_st_mode && name && strstr(name, "Mic")) {
			continue;
		}
		tether_audio_sender_attach(r->audio, name, name);
	}

	pthread_mutex_lock(&s->lock);
	da_push_back(s->sessions, &r);
	pthread_mutex_unlock(&s->lock);

	tether_log_info("sender: created session for peer=%s audio_tracks=%d", peer->peer_id,
			(int)s->audio_source_names.num);

	// Tracks are added; kick off the offer. libdatachannel only auto-
	// negotiates the initial Data Channel — media tracks need an explicit
	// setLocalDescription call from the offerer.
	if (!tether_webrtc_negotiate(w)) {
		tether_log_error("sender: failed to start SDP negotiation for peer=%s", peer->peer_id);
	}
}

// ---- callbacks from signaling / admission ------------------------------

static void session_local_sdp(void *user, const char *type, const char *sdp)
{
	struct session_ctx *ctx = user;
	tether_signaling_send_sdp(ctx->sender->sig, ctx->peer_id, type, sdp);
}

static void session_local_ice(void *user, const char *cand, const char *mid, int mline)
{
	struct session_ctx *ctx = user;
	tether_signaling_send_ice(ctx->sender->sig, ctx->peer_id, cand, mid, mline);
}

static void session_state(void *user, tether_webrtc_state_t state)
{
	struct session_ctx *ctx = user;
	tether_sender_state_t dialog_state = TETHER_SENDER_STATE_WAITING;
	if (state == TETHER_WRTC_STATE_CONNECTED) {
		dialog_state = TETHER_SENDER_STATE_PEER_CONNECTED;
	} else if (state == TETHER_WRTC_STATE_FAILED || state == TETHER_WRTC_STATE_CLOSED) {
		dialog_state = TETHER_SENDER_STATE_FAILED;
	}
	if (ctx->sender->cbs.on_peer_state) {
		ctx->sender->cbs.on_peer_state(ctx->sender->cbs.user, ctx->peer_id, dialog_state);
	}
	if (state == TETHER_WRTC_STATE_FAILED || state == TETHER_WRTC_STATE_CLOSED) {
		tether_admission_disconnect_peer(ctx->sender->adm, ctx->peer_id);
	}
}

static void on_admission_changed(void *user, const tether_peer_t *peer)
{
	struct tether_sender *s = user;
	switch (peer->state) {
	case TETHER_PEER_ACCEPTED:
		tether_signaling_accept(s->sig, peer->peer_id);
		create_session(s, peer);
		break;
	case TETHER_PEER_REJECTED:
	case TETHER_PEER_REVOKED:
		tether_signaling_reject(s->sig, peer->peer_id);
		if (s->cbs.on_peer_gone) {
			s->cbs.on_peer_gone(s->cbs.user, peer->peer_id);
		}
		break;
	default:
		break;
	}
}

static void on_signaling_event(void *user, tether_signaling_event_t evt, const tether_signaling_msg_t *msg)
{
	struct tether_sender *s = user;
	switch (evt) {
	case TETHER_SIG_EVT_TOKEN_ISSUED: {
		const char *tok = msg ? msg->json_payload : NULL;
		if (tok) {
			strncpy(s->current_token, tok, sizeof(s->current_token) - 1);
			tether_log_info("sender: token issued %s", tok);
			if (s->cbs.on_token) {
				s->cbs.on_token(s->cbs.user, s->current_token);
			}
		}
		break;
	}
	case TETHER_SIG_EVT_REQUEST_PENDING: {
		const char *json = msg ? msg->json_payload : NULL;
		if (!json || !msg->peer_id) {
			break;
		}
		const char *n = strstr(json, "\"name\":\"");
		const char *f = strstr(json, "\"fingerprint\":\"");
		char name[128] = {0};
		char fp[128] = {0};
		if (n) {
			const char *e = strchr(n + 8, '"');
			if (e) {
				size_t l = (size_t)(e - (n + 8));
				if (l >= sizeof(name)) {
					l = sizeof(name) - 1;
				}
				memcpy(name, n + 8, l);
			}
		}
		if (f) {
			const char *e = strchr(f + 15, '"');
			if (e) {
				size_t l = (size_t)(e - (f + 15));
				if (l >= sizeof(fp)) {
					l = sizeof(fp) - 1;
				}
				memcpy(fp, f + 15, l);
			}
		}
		tether_admission_add_pending(s->adm, msg->peer_id, name, fp);
		if (s->cbs.on_pending) {
			s->cbs.on_pending(s->cbs.user, msg->peer_id, name, fp);
		}
		break;
	}
	case TETHER_SIG_EVT_SDP_ANSWER: {
		if (!msg || !msg->peer_id) {
			break;
		}
		pthread_mutex_lock(&s->lock);
		struct receiver_session *r = find_session_locked(s, msg->peer_id);
		if (r && msg->json_payload) {
			const char *p = strstr(msg->json_payload, "\"sdp\":\"");
			if (p) {
				p += 7;
				const char *end = strchr(p, '"');
				if (end) {
					char *sdp = dup_span(p, (size_t)(end - p));
					tether_webrtc_apply_remote_sdp(r->wrtc, "answer", sdp);
					bfree(sdp);
				}
			}
		}
		pthread_mutex_unlock(&s->lock);
		break;
	}
	case TETHER_SIG_EVT_ICE_CANDIDATE: {
		if (!msg || !msg->peer_id || !msg->json_payload) {
			break;
		}
		pthread_mutex_lock(&s->lock);
		struct receiver_session *r = find_session_locked(s, msg->peer_id);
		if (r) {
			const char *c = strstr(msg->json_payload, "\"candidate\":\"");
			const char *m = strstr(msg->json_payload, "\"mid\":\"");
			if (c && m) {
				c += 13;
				m += 7;
				const char *ce = strchr(c, '"');
				const char *me = strchr(m, '"');
				if (ce && me) {
					char *cand = dup_span(c, (size_t)(ce - c));
					char *mid = dup_span(m, (size_t)(me - m));
					tether_webrtc_add_remote_ice(r->wrtc, cand, mid);
					bfree(cand);
					bfree(mid);
				}
			}
		}
		pthread_mutex_unlock(&s->lock);
		break;
	}
	case TETHER_SIG_EVT_PEER_GONE:
		if (msg && msg->peer_id) {
			tether_admission_disconnect_peer(s->adm, msg->peer_id);
			if (s->cbs.on_peer_gone) {
				s->cbs.on_peer_gone(s->cbs.user, msg->peer_id);
			}
		}
		break;
	default:
		break;
	}
}

// ---- capture pipeline --------------------------------------------------

static void capture_one(struct tether_sender *s, struct video_capture *cap)
{
	obs_source_t *src = obs_get_source_by_name(cap->source_name);
	if (!src) {
		return;
	}
	uint32_t w = obs_source_get_width(src);
	uint32_t h = obs_source_get_height(src);
	if (w == 0 || h == 0) {
		obs_source_release(src);
		return;
	}

	if (!cap->texrender) {
		cap->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	}
	if (cap->stage_w != w || cap->stage_h != h) {
		if (cap->stagesurf) {
			gs_stagesurface_destroy(cap->stagesurf);
		}
		cap->stagesurf = gs_stagesurface_create(w, h, GS_BGRA);
		cap->stage_w = w;
		cap->stage_h = h;
	}

	gs_texrender_reset(cap->texrender);
	if (!gs_texrender_begin(cap->texrender, w, h)) {
		obs_source_release(src);
		return;
	}
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	obs_source_video_render(src);
	gs_texrender_end(cap->texrender);

	gs_stage_texture(cap->stagesurf, gs_texrender_get_texture(cap->texrender));
	uint8_t *data = NULL;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(cap->stagesurf, &data, &linesize)) {
		size_t need = (size_t)linesize * h;
		pthread_mutex_lock(&s->frame_lock);
		if (cap->latest_frame_size < need) {
			bfree(cap->latest_frame);
			cap->latest_frame = bmalloc(need);
			cap->latest_frame_size = need;
		}
		memcpy(cap->latest_frame, data, need);
		cap->latest_frame_w = w;
		cap->latest_frame_h = h;
		cap->latest_frame_linesize = linesize;
		cap->frame_ready = true;
		pthread_cond_signal(&s->frame_cond);
		pthread_mutex_unlock(&s->frame_lock);
		gs_stagesurface_unmap(cap->stagesurf);
	}
	obs_source_release(src);
}

static void graphics_render_cb(void *param, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	struct tether_sender *s = param;
	for (size_t i = 0; i < s->captures.num; ++i) {
		capture_one(s, s->captures.array[i]);
	}
}

static void encode_capture_to_sessions(struct tether_sender *s, size_t cap_idx, const uint8_t *bgra, uint32_t w,
				       uint32_t h, uint32_t linesize)
{
	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < s->sessions.num; ++i) {
		struct receiver_session *r = s->sessions.array[i];
		if (cap_idx >= r->src_encoders.num) {
			continue;
		}
		struct source_encoder *se = r->src_encoders.array[cap_idx];
		int br = s->cfg_owned.video_bitrate_kbps > 0 ? s->cfg_owned.video_bitrate_kbps : 6000;
		if (!ensure_source_encoder(se, (int)w, (int)h, br)) {
			continue;
		}
		const uint8_t *src_planes[4] = {bgra, NULL, NULL, NULL};
		int src_strides[4] = {(int)linesize, 0, 0, 0};
		sws_scale(se->sws, src_planes, src_strides, 0, (int)h, se->enc_in->data, se->enc_in->linesize);
		se->enc_in->pts = se->frame_pts++;
		if (avcodec_send_frame(se->enc, se->enc_in) < 0) {
			continue;
		}
		while (avcodec_receive_packet(se->enc, se->enc_pkt) == 0) {
			tether_webrtc_push_video(r->wrtc, se->video_track_id, se->enc_pkt->data,
						 (size_t)se->enc_pkt->size, se->enc_pkt->pts);
			av_packet_unref(se->enc_pkt);
		}
	}
	pthread_mutex_unlock(&s->lock);
}

static void *encoder_worker_main(void *param)
{
	struct tether_sender *s = param;
	uint8_t *local = NULL;
	size_t local_cap = 0;
	while (true) {
		pthread_mutex_lock(&s->frame_lock);
		// Wait until at least one capture has a ready frame or we shut down.
		bool any_ready = false;
		do {
			any_ready = false;
			for (size_t i = 0; i < s->captures.num; ++i) {
				if (s->captures.array[i]->frame_ready) {
					any_ready = true;
					break;
				}
			}
			if (!s->worker_running || any_ready) {
				break;
			}
			pthread_cond_wait(&s->frame_cond, &s->frame_lock);
		} while (true);
		if (!s->worker_running) {
			pthread_mutex_unlock(&s->frame_lock);
			break;
		}

		// Take a snapshot of all currently-ready captures, clear their
		// flags, and release the frame_lock before kicking off encodes.
		size_t ncap = s->captures.num;
		for (size_t i = 0; i < ncap; ++i) {
			struct video_capture *cap = s->captures.array[i];
			if (!cap->frame_ready) {
				continue;
			}
			uint32_t w = cap->latest_frame_w;
			uint32_t h = cap->latest_frame_h;
			uint32_t ls = cap->latest_frame_linesize;
			size_t need = (size_t)ls * h;
			if (local_cap < need) {
				bfree(local);
				local = bmalloc(need);
				local_cap = need;
			}
			memcpy(local, cap->latest_frame, need);
			cap->frame_ready = false;
			pthread_mutex_unlock(&s->frame_lock);
			encode_capture_to_sessions(s, i, local, w, h, ls);
			pthread_mutex_lock(&s->frame_lock);
		}
		pthread_mutex_unlock(&s->frame_lock);
	}
	bfree(local);
	return NULL;
}

// ---- lifecycle ---------------------------------------------------------

void tether_sender_register(void)
{
	tether_log_debug("sender: ready (tools-menu entry registered by plugin-main)");
}

void tether_sender_shutdown(void)
{
	// Nothing to do — sender_t instances are owned by the dialog.
}

tether_sender_t *tether_sender_create(const tether_sender_config_t *cfg, const tether_sender_callbacks_t *cbs)
{
	if (!cfg) {
		return NULL;
	}
	// Resolve the video sources: the explicit array beats the legacy single
	// source_name. Need at least one valid entry.
	const char *first = NULL;
	if (cfg->video_source_names) {
		for (const char *const *p = cfg->video_source_names; *p; ++p) {
			if (*p && **p) {
				first = *p;
				break;
			}
		}
	}
	if (!first) {
		first = (cfg->source_name && *cfg->source_name) ? cfg->source_name : NULL;
	}
	if (!first) {
		return NULL;
	}

	struct tether_sender *s = bzalloc(sizeof(*s));
	s->cfg_owned = *cfg;
	if (cfg->video_bitrate_kbps <= 0) {
		s->cfg_owned.video_bitrate_kbps = 6000;
	}
	if (cfg->max_receivers <= 0) {
		s->cfg_owned.max_receivers = 4;
	}
	if (cfg->token_ttl_minutes <= 0) {
		s->cfg_owned.token_ttl_minutes = 30;
	}
	if (cbs) {
		s->cbs = *cbs;
	}

	pthread_mutex_init(&s->lock, NULL);
	pthread_mutex_init(&s->frame_lock, NULL);
	pthread_cond_init(&s->frame_cond, NULL);
	da_init(s->captures);

	// Build the capture list. Each capture gets mid "video0", "video1", …
	// so receivers can route per-source. We populate from the array if
	// present, otherwise fall back to the single source_name.
	int idx = 0;
	if (cfg->video_source_names) {
		for (const char *const *p = cfg->video_source_names; *p; ++p) {
			if (!*p || !**p) {
				continue;
			}
			struct video_capture *cap = bzalloc(sizeof(*cap));
			strncpy(cap->source_name, *p, sizeof(cap->source_name) - 1);
			snprintf(cap->mid, sizeof(cap->mid), "video%d", idx++);
			da_push_back(s->captures, &cap);
		}
	}
	if (s->captures.num == 0) {
		struct video_capture *cap = bzalloc(sizeof(*cap));
		strncpy(cap->source_name, first, sizeof(cap->source_name) - 1);
		snprintf(cap->mid, sizeof(cap->mid), "video0");
		da_push_back(s->captures, &cap);
	}

	if (cfg->audio_source_names) {
		for (const char *const *p = cfg->audio_source_names; *p; ++p) {
			char *dup = bstrdup(*p);
			da_push_back(s->audio_source_names, &dup);
		}
	}

	tether_admission_config_t acfg = {
		.max_receivers = s->cfg_owned.max_receivers,
		.rate_limit_window_seconds = 60,
		.rate_limit_max_attempts = 10,
		.lockout_seconds = 300,
		.auto_accept_pinned = true,
		.on_changed = on_admission_changed,
		.cb_user = s,
	};
	s->adm = tether_admission_create(&acfg);

	tether_signaling_config_t scfg = {
		.server_url = fallback_or(s->cfg_owned.server_url, tether_default_server_url()),
		.role = TETHER_ROLE_SENDER,
		.display_name = s->captures.num > 0 ? s->captures.array[0]->source_name : "tether",
		.token_ttl_minutes = s->cfg_owned.token_ttl_minutes,
		.reusable_token = s->cfg_owned.reusable_token,
		.cb = on_signaling_event,
		.cb_user = s,
	};
	s->sig = tether_signaling_create(&scfg);
	if (!tether_signaling_connect(s->sig)) {
		tether_log_warning("sender: signaling connect failed for %s", scfg.server_url);
	}

	s->worker_running = true;
	pthread_create(&s->worker, NULL, encoder_worker_main, s);

	obs_add_main_render_callback(graphics_render_cb, s);

	return s;
}

void tether_sender_release(tether_sender_t *s)
{
	if (!s) {
		return;
	}
	obs_remove_main_render_callback(graphics_render_cb, s);

	pthread_mutex_lock(&s->frame_lock);
	s->worker_running = false;
	pthread_cond_broadcast(&s->frame_cond);
	pthread_mutex_unlock(&s->frame_lock);
	pthread_join(s->worker, NULL);

	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < s->sessions.num; ++i) {
		release_session(s->sessions.array[i]);
	}
	da_clear(s->sessions);
	pthread_mutex_unlock(&s->lock);

	if (s->sig) {
		tether_signaling_release(s->sig);
	}
	if (s->adm) {
		tether_admission_release(s->adm);
	}

	obs_enter_graphics();
	for (size_t i = 0; i < s->captures.num; ++i) {
		struct video_capture *cap = s->captures.array[i];
		if (cap->stagesurf) {
			gs_stagesurface_destroy(cap->stagesurf);
		}
		if (cap->texrender) {
			gs_texrender_destroy(cap->texrender);
		}
		bfree(cap->latest_frame);
		bfree(cap);
	}
	obs_leave_graphics();
	da_free(s->captures);

	for (size_t i = 0; i < s->audio_source_names.num; ++i) {
		bfree(s->audio_source_names.array[i]);
	}
	da_free(s->audio_source_names);
	da_free(s->sessions);
	pthread_cond_destroy(&s->frame_cond);
	pthread_mutex_destroy(&s->frame_lock);
	pthread_mutex_destroy(&s->lock);
	bfree(s);
}

void tether_sender_accept(tether_sender_t *s, const char *peer_id, bool pin)
{
	if (s) {
		tether_admission_accept(s->adm, peer_id, pin);
	}
}

void tether_sender_reject(tether_sender_t *s, const char *peer_id)
{
	if (s) {
		tether_admission_reject(s->adm, peer_id);
	}
}

void tether_sender_disconnect_peer(tether_sender_t *s, const char *peer_id)
{
	if (!s || !peer_id) {
		return;
	}
	// Drop the live media session for this peer; admission_disconnect_peer
	// also clears the pin so a reconnect lands in the pending list again.
	pthread_mutex_lock(&s->lock);
	struct receiver_session *r = find_session_locked(s, peer_id);
	if (r) {
		tether_webrtc_release(r->wrtc);
		r->wrtc = NULL;
		if (r->audio) {
			tether_audio_sender_release(r->audio);
			r->audio = NULL;
		}
	}
	pthread_mutex_unlock(&s->lock);
	tether_admission_disconnect_peer(s->adm, peer_id);
	tether_signaling_reject(s->sig, peer_id);
	if (s->cbs.on_peer_gone) {
		s->cbs.on_peer_gone(s->cbs.user, peer_id);
	}
}

void tether_sender_revoke_token(tether_sender_t *s)
{
	if (s) {
		tether_admission_revoke_all(s->adm);
		tether_signaling_revoke_token(s->sig);
	}
}

const char *tether_sender_current_token(tether_sender_t *s)
{
	return (s && s->current_token[0]) ? s->current_token : NULL;
}
