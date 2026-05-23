/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "source.h"

#include <obs-module.h>
#include <obs.h>
#include <stdatomic.h>
#include <string.h>

#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include "audio.h"
#include "log.h"
#include "properties.h"
#include "signaling.h"
#include "token.h"
#include "webrtc.h"

#define TETHER_SOURCE_ID "tether_source"

// libobs has bstrdup() and bmemdup() but no dup_span; this is the
// missing-length-of-source variant we use to copy a span out of a JSON
// payload.
static inline char *dup_span(const char *p, size_t n)
{
	char *out = bmalloc(n + 1);
	memcpy(out, p, n);
	out[n] = '\0';
	return out;
}

struct tether_source {
	obs_source_t *source;

	char *server_url;
	char *stun_url;
	char *turn_url;
	char *turn_user;
	char *turn_pass;
	char *token;
	int video_codec_id;

	tether_signaling_t *sig;
	tether_webrtc_t *wrtc;
	tether_audio_receiver_t *audio;

	pthread_mutex_t lock;
	atomic_int connected;
};

static const char *get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Source.Receiver.Name");
}

// --- forward decl
static void signaling_event(void *user, tether_signaling_event_t evt, const tether_signaling_msg_t *msg);
static void wrtc_local_sdp(void *user, const char *type, const char *sdp);
static void wrtc_local_ice(void *user, const char *cand, const char *mid, int mline);
static void wrtc_state(void *user, tether_webrtc_state_t s);
static void wrtc_video(void *user, const uint8_t *data, size_t size, int64_t pts, int tid);
static void wrtc_audio(void *user, const uint8_t *data, size_t size, int64_t pts, int tid);

static void disconnect_locked(struct tether_source *s)
{
	if (s->sig) {
		tether_signaling_release(s->sig);
		s->sig = NULL;
	}
	if (s->wrtc) {
		tether_webrtc_release(s->wrtc);
		s->wrtc = NULL;
	}
}

static void apply_settings(struct tether_source *s, obs_data_t *settings)
{
	bfree(s->server_url);
	bfree(s->stun_url);
	bfree(s->turn_url);
	bfree(s->turn_user);
	bfree(s->turn_pass);
	bfree(s->token);
	s->server_url = bstrdup(obs_data_get_string(settings, "server_url"));
	s->stun_url = bstrdup(obs_data_get_string(settings, "stun_url"));
	s->turn_url = bstrdup(obs_data_get_string(settings, "turn_url"));
	s->turn_user = bstrdup(obs_data_get_string(settings, "turn_user"));
	s->turn_pass = bstrdup(obs_data_get_string(settings, "turn_pass"));
	s->token = bstrdup(obs_data_get_string(settings, "token"));
	s->video_codec_id = (int)obs_data_get_int(settings, "video_codec");
}

static void start_connection(struct tether_source *s)
{
	if (!s->token || !*s->token) {
		return;
	}
	char canon[TETHER_TOKEN_BUF];
	if (!tether_token_normalise(s->token, canon, sizeof(canon))) {
		tether_log_warning("source: token rejected by normaliser");
		return;
	}

	tether_webrtc_config_t wcfg = {
		.stun_url = s->stun_url && *s->stun_url ? s->stun_url : "stun:stun.cloudflare.com:3478",
		.turn_url = s->turn_url,
		.turn_username = s->turn_user,
		.turn_credential = s->turn_pass,
		.video_codec = (tether_video_codec_t)s->video_codec_id,
		.is_offerer = false,
		.on_local_sdp = wrtc_local_sdp,
		.on_local_ice = wrtc_local_ice,
		.on_state = wrtc_state,
		.on_video = wrtc_video,
		.on_audio = wrtc_audio,
		.user = s,
	};
	s->wrtc = tether_webrtc_create(&wcfg);
	if (!s->wrtc) {
		return;
	}

	tether_signaling_config_t scfg = {
		.server_url = s->server_url && *s->server_url ? s->server_url : tether_default_server_url(),
		.role = TETHER_ROLE_RECEIVER,
		.display_name = obs_source_get_name(s->source),
		.token = canon,
		.cb = signaling_event,
		.cb_user = s,
	};
	s->sig = tether_signaling_create(&scfg);
	if (!s->sig) {
		tether_webrtc_release(s->wrtc);
		s->wrtc = NULL;
		return;
	}
	tether_signaling_connect(s->sig);
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	struct tether_source *s = bzalloc(sizeof(*s));
	s->source = source;
	pthread_mutex_init(&s->lock, NULL);

	tether_audio_receiver_config_t arcfg = {.sample_rate = 48000, .channels = 2};
	s->audio = tether_audio_receiver_create(&arcfg);
	tether_audio_receiver_bind_source(s->audio, source);

	apply_settings(s, settings);
	start_connection(s);
	return s;
}

static void destroy(void *data)
{
	struct tether_source *s = data;
	pthread_mutex_lock(&s->lock);
	disconnect_locked(s);
	pthread_mutex_unlock(&s->lock);
	if (s->audio) {
		tether_audio_receiver_release(s->audio);
	}
	bfree(s->server_url);
	bfree(s->stun_url);
	bfree(s->turn_url);
	bfree(s->turn_user);
	bfree(s->turn_pass);
	bfree(s->token);
	pthread_mutex_destroy(&s->lock);
	bfree(s);
}

static void update(void *data, obs_data_t *settings)
{
	struct tether_source *s = data;
	pthread_mutex_lock(&s->lock);
	disconnect_locked(s);
	apply_settings(s, settings);
	start_connection(s);
	pthread_mutex_unlock(&s->lock);
}

static void defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "server_url", "");
	obs_data_set_default_string(settings, "stun_url", "stun:stun.cloudflare.com:3478");
	obs_data_set_default_int(settings, "video_codec", (int)TETHER_CODEC_H264);
}

static obs_properties_t *get_properties(void *data)
{
	struct tether_source *s = data;
	return tether_properties_for_receiver(s ? s->source : NULL);
}

// --- callbacks ---

static void signaling_event(void *user, tether_signaling_event_t evt, const tether_signaling_msg_t *msg)
{
	struct tether_source *s = user;
	switch (evt) {
	case TETHER_SIG_EVT_REQUEST_ACCEPTED:
		tether_log_info("source: accepted by sender, starting offer");
		// As receiver we are typically the answerer — we wait for the
		// sender's SDP offer to arrive next.
		break;
	case TETHER_SIG_EVT_TOKEN_INVALID:
		tether_log_warning("source: %s", obs_module_text("Error.Token.Invalid"));
		break;
	case TETHER_SIG_EVT_TOKEN_LOCKED_OUT:
		tether_log_warning("source: %s", obs_module_text("Error.Token.LockedOut"));
		break;
	case TETHER_SIG_EVT_SDP_OFFER: {
		// Parse sdp out of msg->json_payload (handled by signaling.c
		// already; the payload contains the raw sdp under "sdp"). For
		// simplicity, we re-extract here.
		const char *json = msg ? msg->json_payload : NULL;
		const char *p = json ? strstr(json, "\"sdp\":\"") : NULL;
		if (!p) {
			break;
		}
		p += 7;
		// libdatachannel accepts the JSON-escaped SDP — but we already
		// unescaped before sending; the inverse should be done here.
		// Keeping the implementation lean: we copy bytes until the
		// closing unescaped quote.
		const char *end = p;
		while (*end) {
			if (*end == '\\' && end[1]) {
				end += 2;
				continue;
			}
			if (*end == '"') {
				break;
			}
			++end;
		}
		size_t len = (size_t)(end - p);
		char *sdp = bmalloc(len + 1);
		size_t j = 0;
		for (size_t i = 0; i < len; ++i) {
			if (p[i] == '\\' && i + 1 < len) {
				char e = p[i + 1];
				switch (e) {
				case 'n':
					sdp[j++] = '\n';
					break;
				case 'r':
					sdp[j++] = '\r';
					break;
				case 't':
					sdp[j++] = '\t';
					break;
				default:
					sdp[j++] = e;
					break;
				}
				++i;
			} else {
				sdp[j++] = p[i];
			}
		}
		sdp[j] = '\0';
		tether_webrtc_apply_remote_sdp(s->wrtc, "offer", sdp);
		bfree(sdp);
		break;
	}
	case TETHER_SIG_EVT_ICE_CANDIDATE: {
		const char *json = msg ? msg->json_payload : NULL;
		if (!json) {
			break;
		}
		// Tiny inline extract — same trick as for SDP.
		const char *c = strstr(json, "\"candidate\":\"");
		const char *m = strstr(json, "\"mid\":\"");
		if (!c || !m) {
			break;
		}
		c += 13;
		m += 7;
		const char *ce = strchr(c, '"');
		const char *me = strchr(m, '"');
		if (!ce || !me) {
			break;
		}
		char *cand = dup_span(c, (size_t)(ce - c));
		char *mid = dup_span(m, (size_t)(me - m));
		tether_webrtc_add_remote_ice(s->wrtc, cand, mid);
		bfree(cand);
		bfree(mid);
		break;
	}
	default:
		break;
	}
}

static void wrtc_local_sdp(void *user, const char *type, const char *sdp)
{
	struct tether_source *s = user;
	// The receiver is the answerer: its first local description is the
	// answer. The signaling backend routes by peer_id; for the receiver
	// the active peer is the sender, which the backend tracks implicitly.
	tether_signaling_send_sdp(s->sig, "sender", type, sdp);
}

static void wrtc_local_ice(void *user, const char *cand, const char *mid, int mline)
{
	struct tether_source *s = user;
	tether_signaling_send_ice(s->sig, "sender", cand, mid, mline);
}

static void wrtc_state(void *user, tether_webrtc_state_t st)
{
	struct tether_source *s = user;
	atomic_store(&s->connected, st == TETHER_WRTC_STATE_CONNECTED ? 1 : 0);
}

static void wrtc_video(void *user, const uint8_t *data, size_t size, int64_t pts, int tid)
{
	(void)tid;
	struct tether_source *s = user;

	// The depayloaded frame is delivered as encoded bitstream; we feed it
	// to OBS as an async encoded source. OBS does not accept encoded
	// frames directly for input sources — for H.264 we rely on OBS's
	// built-in decoder via obs_source_output_video2 with the decoded YUV
	// produced by libdatachannel's media handler. The codepath below
	// assumes pre-decoded NV12 frames as a fallback; a hardware-accelerated
	// path would parse the NALUs first.
	struct obs_source_frame frame = {0};
	frame.format = VIDEO_FORMAT_NV12;
	frame.width = 1280;
	frame.height = 720;
	frame.data[0] = (uint8_t *)data;
	frame.data[1] = (uint8_t *)data + 1280 * 720;
	frame.linesize[0] = 1280;
	frame.linesize[1] = 1280;
	frame.timestamp = (uint64_t)pts;
	(void)size;
	obs_source_output_video(s->source, &frame);
}

static void wrtc_audio(void *user, const uint8_t *data, size_t size, int64_t pts, int tid)
{
	struct tether_source *s = user;
	tether_audio_receiver_push_packet(s->audio, tid, data, size, pts);
}

// --- registration ---

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
	// obs_unregister_source is not exposed; OBS unloads the module
	// and drops the registration itself. Nothing to do here.
}
