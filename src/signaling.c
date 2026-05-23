/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * WebSocket signaling client. Uses libdatachannel's libjuice/rtcWebSocket
 * helper (exposed through <rtc/rtc.h>) so we do not pull in a second
 * WebSocket implementation.
 *
 * Protocol (line-delimited JSON, both directions):
 *
 *   client → server
 *     { "type": "hello",      "role": "sender"|"receiver",
 *       "name": "...",        "token": "TTHR-...",
 *       "ttl_minutes": 30,    "reusable": false }
 *     { "type": "accept",     "peer_id": "..." }
 *     { "type": "reject",     "peer_id": "..." }
 *     { "type": "revoke" }
 *     { "type": "sdp",        "peer_id": "...", "sdp_type": "offer"|"answer", "sdp": "..." }
 *     { "type": "ice",        "peer_id": "...", "candidate": "...", "mid": "0", "mline": 0 }
 *
 *   server → client
 *     { "type": "token",      "token": "TTHR-..." }
 *     { "type": "pending",    "peer_id": "...", "name": "...", "fingerprint": "..." }
 *     { "type": "peer_gone",  "peer_id": "..." }
 *     { "type": "accepted" } | { "type": "rejected" }
 *     { "type": "token_invalid" } | { "type": "token_locked_out", "retry_after": 30 }
 *     { "type": "sdp",        "peer_id": "...", "sdp_type": "...", "sdp": "..." }
 *     { "type": "ice",        ... }
 */

#include "signaling.h"

#include <util/threading.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rtc/rtc.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/threading.h>

#include "log.h"

struct tether_signaling {
	tether_signaling_config_t cfg;
	char *server_url;
	char *display_name;
	char *token;

	int ws; // libdatachannel websocket handle, -1 if unset
	volatile long state;

	pthread_mutex_t lock;
};

static void emit(tether_signaling_t *sig, tether_signaling_event_t evt, const char *peer_id, const char *payload)
{
	if (!sig->cfg.cb) {
		return;
	}
	tether_signaling_msg_t m = {.peer_id = peer_id, .json_payload = payload};
	sig->cfg.cb(sig->cfg.cb_user, evt, &m);
}

static void set_state(tether_signaling_t *sig, tether_signaling_state_t s)
{
	tether_signaling_state_t prev = (tether_signaling_state_t)os_atomic_exchange_long(&sig->state, s);
	if (prev != s) {
		emit(sig, TETHER_SIG_EVT_STATE_CHANGED, NULL, NULL);
	}
}

// Very small JSON helpers — we only need to look up string fields by key
// in a flat object. Avoids pulling in a full JSON library.
static const char *json_find_str(const char *json, const char *key, size_t *out_len)
{
	size_t klen = strlen(key);
	const char *p = json;
	while ((p = strstr(p, key)) != NULL) {
		// Require quote-key-quote
		if (p == json || *(p - 1) != '"') {
			++p;
			continue;
		}
		if (p[klen] != '"') {
			++p;
			continue;
		}
		p += klen + 1;
		while (*p == ':' || *p == ' ') {
			++p;
		}
		if (*p != '"') {
			return NULL;
		}
		++p;
		const char *start = p;
		while (*p && *p != '"') {
			if (*p == '\\' && p[1]) {
				p += 2;
				continue;
			}
			++p;
		}
		if (out_len) {
			*out_len = (size_t)(p - start);
		}
		return start;
	}
	return NULL;
}

static bool json_eq_str(const char *json, const char *key, const char *value)
{
	size_t vlen = 0;
	const char *v = json_find_str(json, key, &vlen);
	return v && strlen(value) == vlen && memcmp(v, value, vlen) == 0;
}

static char *json_dup_str(const char *json, const char *key)
{
	size_t vlen = 0;
	const char *v = json_find_str(json, key, &vlen);
	if (!v) {
		return NULL;
	}
	char *s = bmalloc(vlen + 1);
	memcpy(s, v, vlen);
	s[vlen] = '\0';
	return s;
}

static void send_raw(tether_signaling_t *sig, const char *json)
{
	if (sig->ws <= 0) {
		return;
	}
	int rc = rtcSendMessage(sig->ws, json, (int)strlen(json));
	if (rc < 0) {
		tether_log_warning("signaling: send failed rc=%d", rc);
	}
}

static void handle_message(tether_signaling_t *sig, const char *json)
{
	if (json_eq_str(json, "type", "token")) {
		char *tok = json_dup_str(json, "token");
		pthread_mutex_lock(&sig->lock);
		bfree(sig->token);
		sig->token = tok ? bstrdup(tok) : NULL;
		pthread_mutex_unlock(&sig->lock);
		emit(sig, TETHER_SIG_EVT_TOKEN_ISSUED, NULL, tok);
		bfree(tok);
		return;
	}
	if (json_eq_str(json, "type", "pending")) {
		char *peer = json_dup_str(json, "peer_id");
		emit(sig, TETHER_SIG_EVT_REQUEST_PENDING, peer, json);
		bfree(peer);
		return;
	}
	if (json_eq_str(json, "type", "peer_gone")) {
		char *peer = json_dup_str(json, "peer_id");
		emit(sig, TETHER_SIG_EVT_PEER_GONE, peer, NULL);
		bfree(peer);
		return;
	}
	if (json_eq_str(json, "type", "accepted")) {
		emit(sig, TETHER_SIG_EVT_REQUEST_ACCEPTED, NULL, NULL);
		return;
	}
	if (json_eq_str(json, "type", "rejected")) {
		emit(sig, TETHER_SIG_EVT_REQUEST_REJECTED, NULL, NULL);
		return;
	}
	if (json_eq_str(json, "type", "token_invalid")) {
		emit(sig, TETHER_SIG_EVT_TOKEN_INVALID, NULL, NULL);
		return;
	}
	if (json_eq_str(json, "type", "token_locked_out")) {
		emit(sig, TETHER_SIG_EVT_TOKEN_LOCKED_OUT, NULL, json);
		return;
	}
	if (json_eq_str(json, "type", "sdp")) {
		bool offer = json_eq_str(json, "sdp_type", "offer");
		char *peer = json_dup_str(json, "peer_id");
		emit(sig, offer ? TETHER_SIG_EVT_SDP_OFFER : TETHER_SIG_EVT_SDP_ANSWER, peer, json);
		bfree(peer);
		return;
	}
	if (json_eq_str(json, "type", "ice")) {
		char *peer = json_dup_str(json, "peer_id");
		emit(sig, TETHER_SIG_EVT_ICE_CANDIDATE, peer, json);
		bfree(peer);
		return;
	}
	tether_log_debug("signaling: unhandled message");
}

static void RTC_API on_open(int id, void *user)
{
	(void)id;
	tether_signaling_t *sig = user;
	set_state(sig, TETHER_SIG_STATE_CONNECTED);
	tether_log_info("signaling: connected to %s", sig->server_url);

	// Send hello.
	struct dstr buf;
	dstr_init(&buf);
	dstr_printf(&buf,
		    "{\"type\":\"hello\",\"role\":\"%s\",\"name\":\"%s\""
		    "%s%s%s,\"ttl_minutes\":%d,\"reusable\":%s}",
		    sig->cfg.role == TETHER_ROLE_SENDER ? "sender" : "receiver",
		    sig->display_name ? sig->display_name : "",
		    sig->cfg.role == TETHER_ROLE_RECEIVER ? ",\"token\":\"" : "",
		    sig->cfg.role == TETHER_ROLE_RECEIVER && sig->cfg.token ? sig->cfg.token : "",
		    sig->cfg.role == TETHER_ROLE_RECEIVER ? "\"" : "", sig->cfg.token_ttl_minutes,
		    sig->cfg.reusable_token ? "true" : "false");
	send_raw(sig, buf.array);
	dstr_free(&buf);
}

static void RTC_API on_closed(int id, void *user)
{
	(void)id;
	tether_signaling_t *sig = user;
	set_state(sig, TETHER_SIG_STATE_DISCONNECTED);
}

static void RTC_API on_error(int id, const char *error, void *user)
{
	(void)id;
	tether_signaling_t *sig = user;
	tether_log_warning("signaling: error %s", error ? error : "(null)");
	set_state(sig, TETHER_SIG_STATE_FAILED);
}

static void RTC_API on_message(int id, const char *msg, int size, void *user)
{
	UNUSED_PARAMETER(id);
	tether_signaling_t *sig = user;
	if (!msg) {
		return;
	}
	// libdatachannel's WebSocket callback uses size < 0 for text frames
	// (|size| includes the trailing NUL) and size > 0 for binary frames.
	// Our wire protocol is JSON over text, so we treat negative-size as a
	// NUL-terminated string and ignore positive-size binary frames.
	if (size < 0) {
		handle_message(sig, msg);
	}
}

tether_signaling_t *tether_signaling_create(const tether_signaling_config_t *cfg)
{
	if (!cfg || !cfg->server_url) {
		return NULL;
	}
	tether_signaling_t *sig = bzalloc(sizeof(*sig));
	sig->cfg = *cfg;
	sig->server_url = bstrdup(cfg->server_url);
	sig->display_name = bstrdup(cfg->display_name ? cfg->display_name : "");
	sig->ws = -1;
	sig->state = TETHER_SIG_STATE_DISCONNECTED;
	pthread_mutex_init(&sig->lock, NULL);
	return sig;
}

void tether_signaling_release(tether_signaling_t *sig)
{
	if (!sig) {
		return;
	}
	tether_signaling_disconnect(sig);
	pthread_mutex_destroy(&sig->lock);
	bfree(sig->server_url);
	bfree(sig->display_name);
	bfree(sig->token);
	bfree(sig);
}

bool tether_signaling_connect(tether_signaling_t *sig)
{
	if (!sig || sig->ws > 0) {
		return false;
	}
	set_state(sig, TETHER_SIG_STATE_CONNECTING);

	rtcWsConfiguration cfg = {
		.disableTlsVerification = false,
	};
	int ws = rtcCreateWebSocketEx(sig->server_url, &cfg);
	if (ws <= 0) {
		tether_log_error("signaling: create failed url=%s rc=%d", sig->server_url, ws);
		set_state(sig, TETHER_SIG_STATE_FAILED);
		return false;
	}
	rtcSetUserPointer(ws, sig);
	rtcSetOpenCallback(ws, on_open);
	rtcSetClosedCallback(ws, on_closed);
	rtcSetErrorCallback(ws, on_error);
	rtcSetMessageCallback(ws, on_message);
	sig->ws = ws;
	return true;
}

void tether_signaling_disconnect(tether_signaling_t *sig)
{
	if (!sig || sig->ws <= 0) {
		return;
	}
	rtcClose(sig->ws);
	rtcDelete(sig->ws);
	sig->ws = -1;
	set_state(sig, TETHER_SIG_STATE_DISCONNECTED);
}

tether_signaling_state_t tether_signaling_state(const tether_signaling_t *sig)
{
	return sig ? (tether_signaling_state_t)os_atomic_load_long(&((tether_signaling_t *)sig)->state)
		   : TETHER_SIG_STATE_DISCONNECTED;
}

bool tether_signaling_accept(tether_signaling_t *sig, const char *peer_id)
{
	if (!sig || !peer_id) {
		return false;
	}
	struct dstr buf;
	dstr_init(&buf);
	dstr_printf(&buf, "{\"type\":\"accept\",\"peer_id\":\"%s\"}", peer_id);
	send_raw(sig, buf.array);
	dstr_free(&buf);
	return true;
}

bool tether_signaling_reject(tether_signaling_t *sig, const char *peer_id)
{
	if (!sig || !peer_id) {
		return false;
	}
	struct dstr buf;
	dstr_init(&buf);
	dstr_printf(&buf, "{\"type\":\"reject\",\"peer_id\":\"%s\"}", peer_id);
	send_raw(sig, buf.array);
	dstr_free(&buf);
	return true;
}

bool tether_signaling_revoke_token(tether_signaling_t *sig)
{
	if (!sig) {
		return false;
	}
	send_raw(sig, "{\"type\":\"revoke\"}");
	return true;
}

bool tether_signaling_send_sdp(tether_signaling_t *sig, const char *peer_id, const char *sdp_type, const char *sdp)
{
	if (!sig || !peer_id || !sdp_type || !sdp) {
		return false;
	}
	// SDP can contain newlines and quotes — encode minimally.
	struct dstr esc;
	dstr_init(&esc);
	for (const char *p = sdp; *p; ++p) {
		switch (*p) {
		case '\\':
			dstr_cat(&esc, "\\\\");
			break;
		case '"':
			dstr_cat(&esc, "\\\"");
			break;
		case '\n':
			dstr_cat(&esc, "\\n");
			break;
		case '\r':
			dstr_cat(&esc, "\\r");
			break;
		case '\t':
			dstr_cat(&esc, "\\t");
			break;
		default:
			dstr_ncat(&esc, p, 1);
			break;
		}
	}
	struct dstr buf;
	dstr_init(&buf);
	dstr_printf(&buf, "{\"type\":\"sdp\",\"peer_id\":\"%s\",\"sdp_type\":\"%s\",\"sdp\":\"%s\"}", peer_id, sdp_type,
		    esc.array ? esc.array : "");
	send_raw(sig, buf.array);
	dstr_free(&buf);
	dstr_free(&esc);
	return true;
}

bool tether_signaling_send_ice(tether_signaling_t *sig, const char *peer_id, const char *candidate, const char *mid,
			       int mline_index)
{
	if (!sig || !peer_id || !candidate || !mid) {
		return false;
	}
	struct dstr buf;
	dstr_init(&buf);
	dstr_printf(&buf,
		    "{\"type\":\"ice\",\"peer_id\":\"%s\",\"candidate\":\"%s\","
		    "\"mid\":\"%s\",\"mline\":%d}",
		    peer_id, candidate, mid, mline_index);
	send_raw(sig, buf.array);
	dstr_free(&buf);
	return true;
}
