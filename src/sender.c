/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "sender.h"

#include <obs-module.h>
#include <obs.h>
#include <string.h>

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

#define TETHER_SENDER_FILTER_ID "tether_sender"

static inline char *dup_span(const char *p, size_t n)
{
	char *out = bmalloc(n + 1);
	memcpy(out, p, n);
	out[n] = '\0';
	return out;
}

// Per-session user-data passed to libdatachannel callbacks. We allocate one
// per receiver so the SDP/ICE/state callbacks can route by peer_id — without
// this, every callback fires with the same generic struct tether_sender* and
// has no way to identify which receiver produced the event.
struct session_ctx {
	struct tether_sender *sender;
	char peer_id[TETHER_PEER_ID_MAX];
};

// One outgoing receiver session.
struct receiver_session {
	char peer_id[TETHER_PEER_ID_MAX];
	tether_webrtc_t *wrtc;
	tether_audio_sender_t *audio; // per-session opus pipeline
	struct session_ctx *ctx;      // owned; used as webrtc 'user' pointer
	int video_track_id;
};

struct tether_sender {
	obs_source_t *parent;
	obs_source_t *filter;

	char *server_url;
	char *stun_url;
	char *turn_url;
	char *turn_user;
	char *turn_pass;
	int mode; // 0 = standard, 1 = twitch ST
	int video_codec_id;
	int max_bitrate;
	int max_receivers;
	int token_ttl_minutes;
	bool reusable_token;
	bool auto_accept_pinned;
	DARRAY(char *) audio_source_names;
	char current_token[TETHER_TOKEN_BUF];

	tether_signaling_t *sig;
	tether_admission_t *adm;
	DARRAY(struct receiver_session *) sessions;

	pthread_mutex_t lock;
};

static const char *get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Source.Sender.Name");
}

static void teardown_locked(struct tether_sender *s);
static void start_signaling_locked(struct tether_sender *s);

// --- callbacks ---

static void session_local_sdp(void *user, const char *type, const char *sdp);
static void session_local_ice(void *user, const char *cand, const char *mid, int mline);
static void session_state(void *user, tether_webrtc_state_t state);

static void on_admission_changed(void *user, const tether_peer_t *peer);
static void on_signaling_event(void *user, tether_signaling_event_t evt, const tether_signaling_msg_t *msg);

// --- helpers ---

static struct receiver_session *find_session_locked(struct tether_sender *s, const char *peer_id)
{
	for (size_t i = 0; i < s->sessions.num; ++i) {
		if (strcmp(s->sessions.array[i]->peer_id, peer_id) == 0) {
			return s->sessions.array[i];
		}
	}
	return NULL;
}

static void apply_settings(struct tether_sender *s, obs_data_t *settings)
{
	bfree(s->server_url);
	bfree(s->stun_url);
	bfree(s->turn_url);
	bfree(s->turn_user);
	bfree(s->turn_pass);
	s->server_url = bstrdup(obs_data_get_string(settings, "server_url"));
	s->stun_url = bstrdup(obs_data_get_string(settings, "stun_url"));
	s->turn_url = bstrdup(obs_data_get_string(settings, "turn_url"));
	s->turn_user = bstrdup(obs_data_get_string(settings, "turn_user"));
	s->turn_pass = bstrdup(obs_data_get_string(settings, "turn_pass"));
	s->mode = (int)obs_data_get_int(settings, "mode");
	s->video_codec_id = (int)obs_data_get_int(settings, "video_codec");
	s->max_bitrate = (int)obs_data_get_int(settings, "max_bitrate");
	s->max_receivers = (int)obs_data_get_int(settings, "max_receivers");
	s->token_ttl_minutes = (int)obs_data_get_int(settings, "token_ttl_minutes");
	s->reusable_token = obs_data_get_bool(settings, "reusable_token");
	s->auto_accept_pinned = obs_data_get_bool(settings, "auto_accept_pinned");

	for (size_t i = 0; i < s->audio_source_names.num; ++i) {
		bfree(s->audio_source_names.array[i]);
	}
	da_clear(s->audio_source_names);

	obs_data_array_t *arr = obs_data_get_array(settings, "audio_sources");
	size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; ++i) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *name = obs_data_get_string(item, "value");
		if (name && *name) {
			char *copy = bstrdup(name);
			da_push_back(s->audio_source_names, &copy);
		}
		obs_data_release(item);
	}
	obs_data_array_release(arr);
}

static void start_signaling_locked(struct tether_sender *s)
{
	tether_admission_config_t acfg = {
		.max_receivers = s->max_receivers,
		.rate_limit_window_seconds = 60,
		.rate_limit_max_attempts = 10,
		.lockout_seconds = 300,
		.auto_accept_pinned = s->auto_accept_pinned,
		.on_changed = on_admission_changed,
		.cb_user = s,
	};
	s->adm = tether_admission_create(&acfg);

	tether_signaling_config_t scfg = {
		.server_url = s->server_url && *s->server_url ? s->server_url : tether_default_server_url(),
		.role = TETHER_ROLE_SENDER,
		.display_name = obs_source_get_name(s->parent),
		.token_ttl_minutes = s->token_ttl_minutes,
		.reusable_token = s->reusable_token,
		.cb = on_signaling_event,
		.cb_user = s,
	};
	s->sig = tether_signaling_create(&scfg);
	tether_signaling_connect(s->sig);
}

static void release_session(struct receiver_session *r)
{
	if (!r) {
		return;
	}
	if (r->audio) {
		tether_audio_sender_release(r->audio);
	}
	if (r->wrtc) {
		tether_webrtc_release(r->wrtc);
	}
	bfree(r->ctx);
	bfree(r);
}

static void teardown_locked(struct tether_sender *s)
{
	for (size_t i = 0; i < s->sessions.num; ++i) {
		release_session(s->sessions.array[i]);
	}
	da_clear(s->sessions);

	if (s->sig) {
		tether_signaling_release(s->sig);
		s->sig = NULL;
	}
	if (s->adm) {
		tether_admission_release(s->adm);
		s->adm = NULL;
	}
}

// --- filter shape ---

static void *create(obs_data_t *settings, obs_source_t *filter)
{
	struct tether_sender *s = bzalloc(sizeof(*s));
	s->filter = filter;
	pthread_mutex_init(&s->lock, NULL);
	apply_settings(s, settings);

	pthread_mutex_lock(&s->lock);
	start_signaling_locked(s);
	pthread_mutex_unlock(&s->lock);

	return s;
}

static void destroy(void *data)
{
	struct tether_sender *s = data;
	pthread_mutex_lock(&s->lock);
	teardown_locked(s);
	pthread_mutex_unlock(&s->lock);

	for (size_t i = 0; i < s->audio_source_names.num; ++i) {
		bfree(s->audio_source_names.array[i]);
	}
	da_free(s->audio_source_names);
	da_free(s->sessions);
	bfree(s->server_url);
	bfree(s->stun_url);
	bfree(s->turn_url);
	bfree(s->turn_user);
	bfree(s->turn_pass);
	pthread_mutex_destroy(&s->lock);
	bfree(s);
}

static void update(void *data, obs_data_t *settings)
{
	struct tether_sender *s = data;
	pthread_mutex_lock(&s->lock);
	teardown_locked(s);
	apply_settings(s, settings);
	start_signaling_locked(s);
	pthread_mutex_unlock(&s->lock);
}

static void defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "server_url", "");
	obs_data_set_default_string(settings, "stun_url", "stun:stun.cloudflare.com:3478");
	obs_data_set_default_int(settings, "mode", 0);
	obs_data_set_default_int(settings, "video_codec", (int)TETHER_CODEC_H264);
	obs_data_set_default_int(settings, "max_bitrate", 6000);
	obs_data_set_default_int(settings, "max_receivers", 1);
	obs_data_set_default_int(settings, "token_ttl_minutes", 30);
	obs_data_set_default_bool(settings, "reusable_token", false);
	obs_data_set_default_bool(settings, "auto_accept_pinned", true);
}

static obs_properties_t *get_properties(void *data)
{
	struct tether_sender *s = data;
	s->parent = obs_filter_get_parent(s->filter);
	return tether_properties_for_sender(s ? s->parent : NULL);
}

// Filter passes the source through unchanged — we attach to capture, not
// modify. Encoding happens on the dedicated encoder thread bound to each
// session's video track in webrtc.c; the filter just keeps the OBS source
// alive and present in the graph so the encoder receives frames via the
// normal source-output path.
static struct obs_source_frame *filter_video(void *data, struct obs_source_frame *frame)
{
	UNUSED_PARAMETER(data);
	return frame;
}

// --- callbacks impl ---

static void create_session(struct tether_sender *s, const tether_peer_t *peer)
{
	struct session_ctx *ctx = bzalloc(sizeof(*ctx));
	ctx->sender = s;
	strncpy(ctx->peer_id, peer->peer_id, sizeof(ctx->peer_id) - 1);

	tether_webrtc_config_t wcfg = {
		.stun_url = s->stun_url && *s->stun_url ? s->stun_url : "stun:stun.cloudflare.com:3478",
		.turn_url = s->turn_url,
		.turn_username = s->turn_user,
		.turn_credential = s->turn_pass,
		.video_codec = (tether_video_codec_t)s->video_codec_id,
		.max_bitrate_kbps = s->max_bitrate,
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
	r->video_track_id = tether_webrtc_add_video_track(w);

	tether_audio_sender_config_t arcfg = {
		.webrtc = w,
		.sample_rate = 48000,
		.channels = 2,
		.bitrate_kbps = 96,
	};
	r->audio = tether_audio_sender_create(&arcfg);
	bool include_mic = (s->mode != 1); // Twitch Stream Together strips the mic
	for (size_t i = 0; i < s->audio_source_names.num; ++i) {
		const char *name = s->audio_source_names.array[i];
		if (!include_mic && name && strstr(name, "Mic")) {
			continue;
		}
		tether_audio_sender_attach(r->audio, name, name);
	}

	pthread_mutex_lock(&s->lock);
	da_push_back(s->sessions, &r);
	pthread_mutex_unlock(&s->lock);

	tether_log_info("sender: created session for peer=%s tracks=video+%d audio", peer->peer_id,
			(int)s->audio_source_names.num);
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
			tether_log_info("sender: token issued");
		}
		break;
	}
	case TETHER_SIG_EVT_REQUEST_PENDING: {
		const char *json = msg ? msg->json_payload : NULL;
		if (!json || !msg->peer_id) {
			break;
		}
		// Extract name and fingerprint from the payload.
		const char *n = strstr(json, "\"name\":\"");
		const char *f = strstr(json, "\"fingerprint\":\"");
		const char *name = "", *fp = "";
		if (n) {
			name = n + 8;
		}
		if (f) {
			fp = f + 15;
		}
		// Truncated lookups; admission's strncpy caps the result anyway.
		tether_admission_add_pending(s->adm, msg->peer_id, name, fp);
		break;
	}
	case TETHER_SIG_EVT_SDP_ANSWER: {
		// Per-session — find by peer_id and apply.
		if (!msg || !msg->peer_id) {
			break;
		}
		pthread_mutex_lock(&s->lock);
		struct receiver_session *r = find_session_locked(s, msg->peer_id);
		if (r) {
			const char *json = msg->json_payload;
			const char *p = strstr(json, "\"sdp\":\"");
			if (p) {
				p += 7;
				const char *end = strchr(p, '"');
				if (end) {
					size_t len = (size_t)(end - p);
					char *sdp = dup_span(p, len);
					tether_webrtc_apply_remote_sdp(r->wrtc, "answer", sdp);
					bfree(sdp);
				}
			}
		}
		pthread_mutex_unlock(&s->lock);
		break;
	}
	case TETHER_SIG_EVT_ICE_CANDIDATE: {
		if (!msg || !msg->peer_id) {
			break;
		}
		pthread_mutex_lock(&s->lock);
		struct receiver_session *r = find_session_locked(s, msg->peer_id);
		if (r && msg->json_payload) {
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
		}
		break;
	default:
		break;
	}
}

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
	const char *label = state == TETHER_WRTC_STATE_CONNECTED    ? "connected"
			    : state == TETHER_WRTC_STATE_CONNECTING ? "connecting"
			    : state == TETHER_WRTC_STATE_FAILED     ? "failed"
			    : state == TETHER_WRTC_STATE_CLOSED     ? "closed"
								    : "new";
	tether_log_info("sender: peer=%s state=%s", ctx->peer_id, label);
	if (state == TETHER_WRTC_STATE_FAILED || state == TETHER_WRTC_STATE_CLOSED) {
		tether_admission_disconnect_peer(ctx->sender->adm, ctx->peer_id);
	}
}

// --- registration ---

static struct obs_source_info filter_info = {
	.id = TETHER_SENDER_FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC_VIDEO,
	.get_name = get_name,
	.create = create,
	.destroy = destroy,
	.get_defaults = defaults,
	.get_properties = get_properties,
	.update = update,
	.filter_video = filter_video,
};

void tether_sender_register(void)
{
	obs_register_source(&filter_info);
	tether_log_debug("sender: registered %s", TETHER_SENDER_FILTER_ID);
}

void tether_sender_shutdown(void)
{
	// Nothing to do — OBS unregisters at module unload.
}
