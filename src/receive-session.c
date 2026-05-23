/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "receive-session.h"

#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "properties.h"
#include "signaling.h"
#include "token.h"
#include "webrtc.h"

struct tether_rx_subscription {
	tether_receive_session_t *session;
	tether_rx_video_cb_t on_video;
	tether_rx_audio_cb_t on_audio;
	tether_rx_state_cb_t on_state;
	void *user;
};

struct tether_receive_session {
	char *token;
	int refcount;

	tether_signaling_t *sig;
	tether_webrtc_t *wrtc;

	volatile long state;

	pthread_mutex_t lock;
	DARRAY(struct tether_rx_subscription *) subs;

	// Stats: updated under lock when packets arrive. Reads also take the
	// lock to get a coherent snapshot.
	tether_receive_stats_t stats;
};

// Global registry of live sessions, keyed by token (one session per token).
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(tether_receive_session_t *) g_sessions;
static bool g_initialised = false;

const char *tether_receive_state_name(tether_receive_state_t s)
{
	switch (s) {
	case TETHER_RX_STATE_CONNECTING:
		return "connecting";
	case TETHER_RX_STATE_AWAITING_ACCEPT:
		return "awaiting accept";
	case TETHER_RX_STATE_ACCEPTED:
		return "accepted";
	case TETHER_RX_STATE_NEGOTIATING:
		return "negotiating";
	case TETHER_RX_STATE_CONNECTED:
		return "connected";
	case TETHER_RX_STATE_FAILED:
		return "failed";
	case TETHER_RX_STATE_CLOSED:
		return "closed";
	}
	return "?";
}

static inline char *dup_span(const char *p, size_t n)
{
	char *out = bmalloc(n + 1);
	memcpy(out, p, n);
	out[n] = '\0';
	return out;
}

static void set_state(tether_receive_session_t *s, tether_receive_state_t new_state)
{
	long old = os_atomic_load_long(&s->state);
	if (old == new_state) {
		return;
	}
	os_atomic_store_long(&s->state, new_state);
	// Snapshot subscribers under lock, fire callbacks outside.
	pthread_mutex_lock(&s->lock);
	size_t n = s->subs.num;
	struct tether_rx_subscription **copy = n > 0 ? bmalloc(n * sizeof(*copy)) : NULL;
	for (size_t i = 0; i < n; ++i) {
		copy[i] = s->subs.array[i];
	}
	pthread_mutex_unlock(&s->lock);
	for (size_t i = 0; i < n; ++i) {
		if (copy[i]->on_state) {
			copy[i]->on_state(copy[i]->user, new_state);
		}
	}
	bfree(copy);
}

static void wrtc_video_cb(void *user, const uint8_t *data, size_t size, int64_t pts, int tid)
{
	tether_receive_session_t *s = user;
	pthread_mutex_lock(&s->lock);
	s->stats.video_bytes += size;
	s->stats.video_packets += 1;
	s->stats.last_update_ns = os_gettime_ns();
	size_t n = s->subs.num;
	struct tether_rx_subscription **copy = n > 0 ? bmalloc(n * sizeof(*copy)) : NULL;
	for (size_t i = 0; i < n; ++i) {
		copy[i] = s->subs.array[i];
	}
	pthread_mutex_unlock(&s->lock);
	// width/height/pts are not known here — the subscriber decodes the H.264
	// NAL units and pulls the dimensions out of the SPS itself.
	(void)tid;
	for (size_t i = 0; i < n; ++i) {
		if (copy[i]->on_video) {
			copy[i]->on_video(copy[i]->user, data, size, 0, 0, pts);
		}
	}
	bfree(copy);
}

static void wrtc_audio_cb(void *user, const uint8_t *data, size_t size, int64_t pts, int tid)
{
	tether_receive_session_t *s = user;
	pthread_mutex_lock(&s->lock);
	s->stats.audio_bytes += size;
	s->stats.audio_packets += 1;
	s->stats.last_update_ns = os_gettime_ns();
	size_t n = s->subs.num;
	struct tether_rx_subscription **copy = n > 0 ? bmalloc(n * sizeof(*copy)) : NULL;
	for (size_t i = 0; i < n; ++i) {
		copy[i] = s->subs.array[i];
	}
	pthread_mutex_unlock(&s->lock);
	for (size_t i = 0; i < n; ++i) {
		if (copy[i]->on_audio) {
			// Reuse track id as channel-count hint for now; subscribers
			// pass it to their Opus decoder which carries the canonical
			// channel layout in the SDP-negotiated track.
			copy[i]->on_audio(copy[i]->user, data, size, 48000, 2, pts);
		}
	}
	bfree(copy);
	(void)tid;
}

static void wrtc_state_cb(void *user, tether_webrtc_state_t st)
{
	tether_receive_session_t *s = user;
	switch (st) {
	case TETHER_WRTC_STATE_NEW:
	case TETHER_WRTC_STATE_CONNECTING:
		set_state(s, TETHER_RX_STATE_NEGOTIATING);
		break;
	case TETHER_WRTC_STATE_CONNECTED:
		set_state(s, TETHER_RX_STATE_CONNECTED);
		break;
	case TETHER_WRTC_STATE_FAILED:
		set_state(s, TETHER_RX_STATE_FAILED);
		break;
	case TETHER_WRTC_STATE_CLOSED:
		set_state(s, TETHER_RX_STATE_CLOSED);
		break;
	}
}

static void wrtc_local_sdp(void *user, const char *type, const char *sdp)
{
	tether_receive_session_t *s = user;
	tether_signaling_send_sdp(s->sig, "sender", type, sdp);
}

static void wrtc_local_ice(void *user, const char *cand, const char *mid, int mline)
{
	tether_receive_session_t *s = user;
	tether_signaling_send_ice(s->sig, "sender", cand, mid, mline);
}

static void signaling_event(void *user, tether_signaling_event_t evt, const tether_signaling_msg_t *msg)
{
	tether_receive_session_t *s = user;
	switch (evt) {
	case TETHER_SIG_EVT_REQUEST_ACCEPTED:
		tether_log_info("rx-session(%s): accepted by sender", s->token);
		set_state(s, TETHER_RX_STATE_ACCEPTED);
		break;
	case TETHER_SIG_EVT_TOKEN_INVALID:
		tether_log_warning("rx-session(%s): token invalid", s->token);
		set_state(s, TETHER_RX_STATE_FAILED);
		break;
	case TETHER_SIG_EVT_TOKEN_LOCKED_OUT:
		tether_log_warning("rx-session(%s): token locked out", s->token);
		set_state(s, TETHER_RX_STATE_FAILED);
		break;
	case TETHER_SIG_EVT_SDP_OFFER: {
		set_state(s, TETHER_RX_STATE_NEGOTIATING);
		const char *json = msg ? msg->json_payload : NULL;
		const char *p = json ? strstr(json, "\"sdp\":\"") : NULL;
		if (!p) {
			break;
		}
		p += 7;
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

static tether_receive_session_t *session_create_locked(const char *token)
{
	tether_receive_session_t *s = bzalloc(sizeof(*s));
	s->token = bstrdup(token);
	s->refcount = 1;
	s->state = TETHER_RX_STATE_CONNECTING;
	pthread_mutex_init(&s->lock, NULL);
	da_init(s->subs);

	char canon[TETHER_TOKEN_BUF];
	if (!tether_token_normalise(token, canon, sizeof(canon))) {
		tether_log_warning("rx-session: token '%s' rejected by normaliser", token);
		bfree(s->token);
		pthread_mutex_destroy(&s->lock);
		bfree(s);
		return NULL;
	}

	tether_webrtc_config_t wcfg = {
		.stun_url = "stun:stun.cloudflare.com:3478",
		.video_codec = TETHER_CODEC_H264,
		.is_offerer = false,
		.on_local_sdp = wrtc_local_sdp,
		.on_local_ice = wrtc_local_ice,
		.on_state = wrtc_state_cb,
		.on_video = wrtc_video_cb,
		.on_audio = wrtc_audio_cb,
		.user = s,
	};
	s->wrtc = tether_webrtc_create(&wcfg);
	if (!s->wrtc) {
		bfree(s->token);
		da_free(s->subs);
		pthread_mutex_destroy(&s->lock);
		bfree(s);
		return NULL;
	}

	tether_signaling_config_t scfg = {
		.server_url = tether_default_server_url(),
		.role = TETHER_ROLE_RECEIVER,
		.display_name = "Tether",
		.token = canon,
		.cb = signaling_event,
		.cb_user = s,
	};
	s->sig = tether_signaling_create(&scfg);
	if (!s->sig) {
		tether_webrtc_release(s->wrtc);
		bfree(s->token);
		da_free(s->subs);
		pthread_mutex_destroy(&s->lock);
		bfree(s);
		return NULL;
	}
	tether_signaling_connect(s->sig);
	set_state(s, TETHER_RX_STATE_AWAITING_ACCEPT);
	tether_log_info("rx-session(%s): created and connecting", s->token);
	return s;
}

static void session_destroy(tether_receive_session_t *s)
{
	tether_log_info("rx-session(%s): destroyed", s->token);
	if (s->sig) {
		tether_signaling_release(s->sig);
	}
	if (s->wrtc) {
		tether_webrtc_release(s->wrtc);
	}
	for (size_t i = 0; i < s->subs.num; ++i) {
		bfree(s->subs.array[i]);
	}
	da_free(s->subs);
	bfree(s->token);
	pthread_mutex_destroy(&s->lock);
	bfree(s);
}

tether_receive_session_t *tether_receive_session_get(const char *token)
{
	if (!token || !*token) {
		return NULL;
	}
	pthread_mutex_lock(&g_lock);
	if (!g_initialised) {
		pthread_mutex_unlock(&g_lock);
		return NULL;
	}
	for (size_t i = 0; i < g_sessions.num; ++i) {
		if (strcmp(g_sessions.array[i]->token, token) == 0) {
			++g_sessions.array[i]->refcount;
			tether_receive_session_t *s = g_sessions.array[i];
			pthread_mutex_unlock(&g_lock);
			return s;
		}
	}
	tether_receive_session_t *s = session_create_locked(token);
	if (s) {
		da_push_back(g_sessions, &s);
	}
	pthread_mutex_unlock(&g_lock);
	return s;
}

void tether_receive_session_release(tether_receive_session_t *s)
{
	if (!s) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	bool destroy = false;
	if (--s->refcount <= 0) {
		for (size_t i = 0; i < g_sessions.num; ++i) {
			if (g_sessions.array[i] == s) {
				da_erase(g_sessions, i);
				break;
			}
		}
		destroy = true;
	}
	pthread_mutex_unlock(&g_lock);
	if (destroy) {
		session_destroy(s);
	}
}

tether_receive_state_t tether_receive_session_state(tether_receive_session_t *s)
{
	return s ? (tether_receive_state_t)os_atomic_load_long(&s->state) : TETHER_RX_STATE_CLOSED;
}

const char *tether_receive_session_token(tether_receive_session_t *s)
{
	return s ? s->token : NULL;
}

void tether_receive_session_get_stats(tether_receive_session_t *s, tether_receive_stats_t *out)
{
	if (!s || !out) {
		return;
	}
	pthread_mutex_lock(&s->lock);
	*out = s->stats;
	pthread_mutex_unlock(&s->lock);
}

tether_rx_subscription_t *tether_receive_session_subscribe(tether_receive_session_t *s, tether_rx_video_cb_t on_video,
							   tether_rx_audio_cb_t on_audio, tether_rx_state_cb_t on_state,
							   void *user)
{
	if (!s) {
		return NULL;
	}
	struct tether_rx_subscription *sub = bzalloc(sizeof(*sub));
	sub->session = s;
	sub->on_video = on_video;
	sub->on_audio = on_audio;
	sub->on_state = on_state;
	sub->user = user;

	pthread_mutex_lock(&s->lock);
	da_push_back(s->subs, &sub);
	pthread_mutex_unlock(&s->lock);

	// Bump session refcount so the session outlives this subscription.
	pthread_mutex_lock(&g_lock);
	++s->refcount;
	pthread_mutex_unlock(&g_lock);

	// Replay current state immediately so the subscriber is up to date.
	if (on_state) {
		on_state(user, tether_receive_session_state(s));
	}
	return sub;
}

void tether_receive_session_unsubscribe(tether_rx_subscription_t *sub)
{
	if (!sub) {
		return;
	}
	tether_receive_session_t *s = sub->session;
	pthread_mutex_lock(&s->lock);
	for (size_t i = 0; i < s->subs.num; ++i) {
		if (s->subs.array[i] == sub) {
			da_erase(s->subs, i);
			break;
		}
	}
	pthread_mutex_unlock(&s->lock);
	bfree(sub);
	tether_receive_session_release(s);
}

void tether_receive_session_enumerate(tether_rx_enum_cb_t cb, void *user)
{
	if (!cb) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	size_t n = g_sessions.num;
	tether_receive_session_t **copy = n > 0 ? bmalloc(n * sizeof(*copy)) : NULL;
	for (size_t i = 0; i < n; ++i) {
		copy[i] = g_sessions.array[i];
		++copy[i]->refcount; // bump so callback can safely use the pointer
	}
	pthread_mutex_unlock(&g_lock);
	for (size_t i = 0; i < n; ++i) {
		cb(user, copy[i]);
		tether_receive_session_release(copy[i]);
	}
	bfree(copy);
}

void tether_receive_session_init(void)
{
	pthread_mutex_lock(&g_lock);
	if (!g_initialised) {
		da_init(g_sessions);
		g_initialised = true;
	}
	pthread_mutex_unlock(&g_lock);
}

void tether_receive_session_shutdown(void)
{
	pthread_mutex_lock(&g_lock);
	// Force-destroy any remaining sessions.
	size_t n = g_sessions.num;
	tether_receive_session_t **doomed = n > 0 ? bmalloc(n * sizeof(*doomed)) : NULL;
	for (size_t i = 0; i < n; ++i) {
		doomed[i] = g_sessions.array[i];
	}
	da_free(g_sessions);
	g_initialised = false;
	pthread_mutex_unlock(&g_lock);
	for (size_t i = 0; i < n; ++i) {
		session_destroy(doomed[i]);
	}
	bfree(doomed);
}
