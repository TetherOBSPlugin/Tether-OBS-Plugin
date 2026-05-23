/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "webrtc.h"

#include <util/threading.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rtc/rtc.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/threading.h>

#include "log.h"

struct track_entry {
	int handle;
	int track_id;
	bool is_video;
	char mid[64];
};

struct tether_webrtc {
	tether_webrtc_config_t cfg;

	int pc; // libdatachannel peer-connection handle
	volatile long state;

	DARRAY(struct track_entry) tracks;
	int next_track_id;

	char local_fingerprint[128];
	pthread_mutex_t lock;
};

static volatile long g_init_count = 0;

bool tether_webrtc_global_init(void)
{
	if (os_atomic_inc_long(&g_init_count) == 1) {
		rtcInitLogger(RTC_LOG_DEBUG, NULL);
		tether_log_info("webrtc: libdatachannel initialised");
	}
	return true;
}

void tether_webrtc_global_shutdown(void)
{
	if (os_atomic_dec_long(&g_init_count) == 0) {
		rtcCleanup();
		tether_log_info("webrtc: libdatachannel cleaned up");
	}
}

static void RTC_API on_state_change(int pc, rtcState state, void *user)
{
	(void)pc;
	tether_webrtc_t *w = user;
	tether_webrtc_state_t mapped;
	switch (state) {
	case RTC_NEW:
		mapped = TETHER_WRTC_STATE_NEW;
		break;
	case RTC_CONNECTING:
		mapped = TETHER_WRTC_STATE_CONNECTING;
		break;
	case RTC_CONNECTED:
		mapped = TETHER_WRTC_STATE_CONNECTED;
		break;
	case RTC_DISCONNECTED:
	case RTC_FAILED:
		mapped = TETHER_WRTC_STATE_FAILED;
		break;
	case RTC_CLOSED:
		mapped = TETHER_WRTC_STATE_CLOSED;
		break;
	default:
		mapped = TETHER_WRTC_STATE_NEW;
		break;
	}
	os_atomic_store_long(&w->state, mapped);
	if (w->cfg.on_state) {
		w->cfg.on_state(w->cfg.user, mapped);
	}
}

static void RTC_API on_local_description(int pc, const char *sdp, const char *type, void *user)
{
	(void)pc;
	tether_webrtc_t *w = user;
	// Extract our DTLS fingerprint from the SDP for fingerprint pinning.
	// SDP carries a line "a=fingerprint:<hash> <colon-separated-hex>".
	const char *fp = strstr(sdp, "a=fingerprint:");
	if (fp) {
		fp += strlen("a=fingerprint:");
		size_t len = 0;
		while (fp[len] && fp[len] != '\r' && fp[len] != '\n' && len < sizeof(w->local_fingerprint) - 1) {
			++len;
		}
		pthread_mutex_lock(&w->lock);
		memcpy(w->local_fingerprint, fp, len);
		w->local_fingerprint[len] = '\0';
		pthread_mutex_unlock(&w->lock);
	}
	if (w->cfg.on_local_sdp) {
		w->cfg.on_local_sdp(w->cfg.user, type, sdp);
	}
}

static void RTC_API on_local_candidate(int pc, const char *candidate, const char *mid, void *user)
{
	(void)pc;
	tether_webrtc_t *w = user;
	tether_log_info("webrtc: local ICE candidate mid=%s: %s", mid, candidate);
	if (w->cfg.on_local_ice) {
		// mid is the m-line identifier; mline_index can be derived from
		// it but libdatachannel does not expose it directly. The
		// signaling side accepts the mid string and reconstructs.
		w->cfg.on_local_ice(w->cfg.user, candidate, mid, 0);
	}
}

static void parse_mid_from_desc(const char *desc, char *out, size_t out_size)
{
	out[0] = '\0';
	const char *p = desc ? strstr(desc, "a=mid:") : NULL;
	if (!p) {
		return;
	}
	p += 6;
	size_t i = 0;
	while (*p && *p != '\r' && *p != '\n' && i < out_size - 1) {
		out[i++] = *p++;
	}
	out[i] = '\0';
}

static void RTC_API on_track_message(int tr, const char *msg, int size, void *user)
{
	tether_webrtc_t *w = user;
	if (!msg || size == 0) {
		return;
	}
	// Negative size = text frame; media is always binary.
	if (size < 0) {
		return;
	}
	bool is_video = false;
	const char *mid = NULL;
	pthread_mutex_lock(&w->lock);
	for (size_t i = 0; i < w->tracks.num; ++i) {
		if (w->tracks.array[i].handle == tr) {
			is_video = w->tracks.array[i].is_video;
			mid = w->tracks.array[i].mid;
			break;
		}
	}
	pthread_mutex_unlock(&w->lock);
	if (is_video) {
		if (w->cfg.on_video) {
			w->cfg.on_video(w->cfg.user, (const uint8_t *)msg, (size_t)size, 0, tr, mid);
		}
	} else {
		if (w->cfg.on_audio) {
			w->cfg.on_audio(w->cfg.user, (const uint8_t *)msg, (size_t)size, 0, tr, mid);
		}
	}
}

static void RTC_API on_track(int pc, int tr, void *user)
{
	(void)pc;
	tether_webrtc_t *w = user;
	// Incoming remote track on the receiver side. Capture its m-line so we
	// can address it by mid and hook the message callback so depacketised
	// payloads land in the configured on_video / on_audio.
	char desc[1024];
	int n = rtcGetTrackDescription(tr, desc, sizeof(desc));
	bool is_video = false;
	struct track_entry e = {.handle = tr, .track_id = w->next_track_id++};
	if (n > 0) {
		is_video = strstr(desc, "m=video") != NULL;
		parse_mid_from_desc(desc, e.mid, sizeof(e.mid));
	}
	e.is_video = is_video;
	pthread_mutex_lock(&w->lock);
	da_push_back(w->tracks, &e);
	pthread_mutex_unlock(&w->lock);

	rtcSetUserPointer(tr, w);
	rtcSetMessageCallback(tr, on_track_message);
}

tether_webrtc_t *tether_webrtc_create(const tether_webrtc_config_t *cfg)
{
	if (!cfg) {
		return NULL;
	}

	tether_webrtc_t *w = bzalloc(sizeof(*w));
	w->cfg = *cfg;
	w->next_track_id = 1;
	w->state = TETHER_WRTC_STATE_NEW;
	pthread_mutex_init(&w->lock, NULL);

	const char *ice_servers[2] = {NULL, NULL};
	char turn_buf[512] = {0};
	int n_servers = 0;
	// TETHER_NO_STUN=1 disables STUN entirely → only host candidates are
	// gathered. This is the auto-detect fallback for same-host testing
	// where srflx candidates from STUN can't be hairpinned back through
	// the local NAT. Production deployments (different machines) keep STUN
	// for NAT traversal.
	const char *no_stun = getenv("TETHER_NO_STUN");
	if (cfg->stun_url && *cfg->stun_url && !(no_stun && strcmp(no_stun, "1") == 0)) {
		ice_servers[n_servers++] = cfg->stun_url;
	}
	if (cfg->turn_url && *cfg->turn_url) {
		// libdatachannel expects "turn:user:pass@host:port".
		const char *u = cfg->turn_username ? cfg->turn_username : "";
		const char *p = cfg->turn_credential ? cfg->turn_credential : "";
		snprintf(turn_buf, sizeof(turn_buf), "turn:%s:%s@%s", u, p,
			 cfg->turn_url + (strncmp(cfg->turn_url, "turn:", 5) == 0 ? 5 : 0));
		ice_servers[n_servers++] = turn_buf;
	}

	// Bind-address overrides (host-side, no negotiation involved):
	//   TETHER_BIND_LOOPBACK=1   → 127.0.0.1   (same-host loopback testing)
	//   TETHER_BIND_IP=<addr>    → <addr>      (force a specific NIC, e.g.
	//                                          the LAN IP, so juice does
	//                                          not gather/probe via docker/
	//                                          libvirt/tailscale interfaces)
	const char *bind_loopback = getenv("TETHER_BIND_LOOPBACK");
	const char *bind_ip = getenv("TETHER_BIND_IP");
	const char *bind_addr = NULL;
	if (bind_loopback && strcmp(bind_loopback, "1") == 0) {
		bind_addr = "127.0.0.1";
	} else if (bind_ip && *bind_ip) {
		bind_addr = bind_ip;
	}
	rtcConfiguration pc_cfg = {
		.iceServers = ice_servers,
		.iceServersCount = n_servers,
		.disableAutoNegotiation = false,
		.bindAddress = bind_addr,
	};

	int pc = rtcCreatePeerConnection(&pc_cfg);
	if (pc <= 0) {
		tether_log_error("webrtc: rtcCreatePeerConnection failed rc=%d", pc);
		bfree(w);
		return NULL;
	}
	w->pc = pc;

	rtcSetUserPointer(pc, w);
	rtcSetStateChangeCallback(pc, on_state_change);
	rtcSetLocalDescriptionCallback(pc, on_local_description);
	rtcSetLocalCandidateCallback(pc, on_local_candidate);
	rtcSetTrackCallback(pc, on_track);

	return w;
}

void tether_webrtc_release(tether_webrtc_t *w)
{
	if (!w) {
		return;
	}
	for (size_t i = 0; i < w->tracks.num; ++i) {
		rtcDeleteTrack(w->tracks.array[i].handle);
	}
	da_free(w->tracks);
	rtcDeletePeerConnection(w->pc);
	pthread_mutex_destroy(&w->lock);
	bfree(w);
}

bool tether_webrtc_negotiate(tether_webrtc_t *w)
{
	if (!w) {
		return false;
	}
	// type=NULL lets libdatachannel pick the right operation (offer when no
	// remote description is set yet, answer when one is). For senders that
	// added tracks before any signaling, this generates the SDP offer and
	// fires on_local_description.
	int rc = rtcSetLocalDescription(w->pc, NULL);
	if (rc < 0) {
		tether_log_warning("webrtc: setLocalDescription rc=%d", rc);
		return false;
	}
	return true;
}

bool tether_webrtc_apply_remote_sdp(tether_webrtc_t *w, const char *sdp_type, const char *sdp)
{
	if (!w || !sdp_type || !sdp) {
		return false;
	}
	int rc = rtcSetRemoteDescription(w->pc, sdp, sdp_type);
	if (rc < 0) {
		tether_log_warning("webrtc: setRemoteDescription rc=%d", rc);
		return false;
	}
	// When the remote sends an offer, we are the answerer. libdatachannel
	// does not auto-generate the answer for media tracks — kick it off.
	if (strcmp(sdp_type, "offer") == 0) {
		int rc2 = rtcSetLocalDescription(w->pc, NULL);
		if (rc2 < 0) {
			tether_log_warning("webrtc: answerer setLocalDescription rc=%d", rc2);
			return false;
		}
	}
	return true;
}

bool tether_webrtc_add_remote_ice(tether_webrtc_t *w, const char *candidate, const char *mid)
{
	if (!w || !candidate) {
		return false;
	}
	tether_log_info("webrtc: remote ICE candidate mid=%s: %s", mid, candidate);
	int rc = rtcAddRemoteCandidate(w->pc, candidate, mid);
	if (rc < 0) {
		tether_log_warning("webrtc: addRemoteCandidate rc=%d", rc);
		return false;
	}
	return true;
}

static int payload_type_for(tether_video_codec_t c)
{
	switch (c) {
	case TETHER_CODEC_H264:
		return 96;
	case TETHER_CODEC_VP9:
		return 98;
	case TETHER_CODEC_AV1:
		return 100;
	}
	return 96;
}

static const char *codec_name(tether_video_codec_t c)
{
	switch (c) {
	case TETHER_CODEC_H264:
		return "h264";
	case TETHER_CODEC_VP9:
		return "vp9";
	case TETHER_CODEC_AV1:
		return "av1";
	}
	return "h264";
}

int tether_webrtc_add_video_track_ex(tether_webrtc_t *w, const char *mid, const char *label)
{
	if (!w) {
		return -1;
	}
	char mid_buf[64];
	if (!mid || !*mid) {
		snprintf(mid_buf, sizeof(mid_buf), "video%d", (int)w->tracks.num);
		mid = mid_buf;
	}
	const char *track_label = (label && *label) ? label : "tether-video";
	rtcTrackInit init = {
		.direction = RTC_DIRECTION_SENDONLY,
		.codec = w->cfg.video_codec == TETHER_CODEC_H264  ? RTC_CODEC_H264
			 : w->cfg.video_codec == TETHER_CODEC_VP9 ? RTC_CODEC_VP9
								  : RTC_CODEC_AV1,
		.payloadType = payload_type_for(w->cfg.video_codec),
		.ssrc = (uint32_t)(rand() & 0x7fffffff),
		.mid = (char *)mid,
		.name = (char *)track_label,
		.msid = "tether",
		.trackId = (char *)track_label,
	};
	int tr = rtcAddTrackEx(w->pc, &init);
	if (tr <= 0) {
		tether_log_warning("webrtc: addTrack(video) rc=%d", tr);
		return -1;
	}
	struct track_entry e = {.handle = tr, .track_id = w->next_track_id++, .is_video = true};
	strncpy(e.mid, mid, sizeof(e.mid) - 1);
	pthread_mutex_lock(&w->lock);
	da_push_back(w->tracks, &e);
	pthread_mutex_unlock(&w->lock);
	tether_log_debug("webrtc: added video track id=%d mid=%s codec=%s", e.track_id, e.mid,
			 codec_name(w->cfg.video_codec));
	return e.track_id;
}

int tether_webrtc_add_video_track(tether_webrtc_t *w)
{
	return tether_webrtc_add_video_track_ex(w, "video", "tether-video");
}

int tether_webrtc_add_audio_track(tether_webrtc_t *w, const char *label)
{
	if (!w) {
		return -1;
	}
	char mid[64];
	snprintf(mid, sizeof(mid), "audio%d", w->next_track_id);
	rtcTrackInit init = {
		.direction = RTC_DIRECTION_SENDONLY,
		.codec = RTC_CODEC_OPUS,
		.payloadType = 111,
		.ssrc = (uint32_t)(rand() & 0x7fffffff),
		.mid = mid,
		.name = label ? label : "tether-audio",
		.msid = "tether",
		.trackId = label ? label : "tether-audio",
	};
	int tr = rtcAddTrackEx(w->pc, &init);
	if (tr <= 0) {
		tether_log_warning("webrtc: addTrack(audio) rc=%d", tr);
		return -1;
	}
	struct track_entry e = {.handle = tr, .track_id = w->next_track_id++, .is_video = false};
	strncpy(e.mid, mid, sizeof(e.mid) - 1);
	pthread_mutex_lock(&w->lock);
	da_push_back(w->tracks, &e);
	pthread_mutex_unlock(&w->lock);
	return e.track_id;
}

static int track_handle_for(tether_webrtc_t *w, int track_id)
{
	for (size_t i = 0; i < w->tracks.num; ++i) {
		if (w->tracks.array[i].track_id == track_id) {
			return w->tracks.array[i].handle;
		}
	}
	return -1;
}

// pts is part of the public API but libdatachannel's RTP packetiser uses its
// own monotonic clock for RTP timestamps. Accepting pts on the API keeps the
// signature compatible with future libdatachannel versions that expose
// per-packet timestamp injection (rtcSendMessageEx variants).
bool tether_webrtc_push_video(tether_webrtc_t *w, int track_id, const uint8_t *data, size_t size, int64_t pts)
{
	UNUSED_PARAMETER(pts);
	if (!w || !data || size == 0) {
		return false;
	}
	int handle = track_handle_for(w, track_id);
	if (handle < 0) {
		return false;
	}
	int rc = rtcSendMessage(handle, (const char *)data, (int)size);
	return rc >= 0;
}

bool tether_webrtc_push_audio(tether_webrtc_t *w, int track_id, const uint8_t *data, size_t size, int64_t pts)
{
	UNUSED_PARAMETER(pts);
	if (!w || !data || size == 0) {
		return false;
	}
	int handle = track_handle_for(w, track_id);
	if (handle < 0) {
		return false;
	}
	int rc = rtcSendMessage(handle, (const char *)data, (int)size);
	return rc >= 0;
}

bool tether_webrtc_local_fingerprint(tether_webrtc_t *w, char *out, size_t out_size)
{
	if (!w || !out || out_size == 0) {
		return false;
	}
	pthread_mutex_lock(&w->lock);
	if (w->local_fingerprint[0] == '\0') {
		pthread_mutex_unlock(&w->lock);
		return false;
	}
	strncpy(out, w->local_fingerprint, out_size - 1);
	out[out_size - 1] = '\0';
	pthread_mutex_unlock(&w->lock);
	return true;
}

tether_webrtc_state_t tether_webrtc_state(const tether_webrtc_t *w)
{
	return w ? (tether_webrtc_state_t)os_atomic_load_long(&((tether_webrtc_t *)w)->state)
		 : TETHER_WRTC_STATE_CLOSED;
}
