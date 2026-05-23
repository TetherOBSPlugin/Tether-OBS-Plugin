/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Sender-side admission state machine. Holds the list of pending requests
 * coming out of the signaling waiting room, the list of currently accepted
 * peers (with their pinned fingerprints), and the rate-limit ledger.
 *
 * Threading: callers from any thread. All public functions take an
 * internal lock; callbacks fire on the caller's thread.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TETHER_PEER_ID_MAX     64
#define TETHER_FINGERPRINT_MAX 128
#define TETHER_DISPLAY_MAX     128

typedef enum {
	TETHER_PEER_PENDING,
	TETHER_PEER_ACCEPTED,
	TETHER_PEER_REJECTED,
	TETHER_PEER_REVOKED,
	TETHER_PEER_DISCONNECTED,
} tether_peer_state_t;

typedef struct {
	char peer_id[TETHER_PEER_ID_MAX];
	char display_name[TETHER_DISPLAY_MAX];
	char fingerprint[TETHER_FINGERPRINT_MAX];
	tether_peer_state_t state;
	bool pinned;
	uint64_t first_seen_ns;
} tether_peer_t;

typedef struct tether_admission tether_admission_t;

// Notifications fired when the admission list changes.
typedef void (*tether_admission_cb_t)(void *user, const tether_peer_t *peer);

typedef struct {
	int max_receivers;
	int rate_limit_window_seconds;
	int rate_limit_max_attempts;
	int lockout_seconds;
	bool auto_accept_pinned;
	tether_admission_cb_t on_changed;
	void *cb_user;
} tether_admission_config_t;

tether_admission_t *tether_admission_create(const tether_admission_config_t *cfg);
void tether_admission_release(tether_admission_t *adm);

// Returns true if the peer was added as pending. Returns false (and ignores
// the call) if the rate limit is hit, the max-receivers cap is reached, or
// the peer is already present.
bool tether_admission_add_pending(tether_admission_t *adm, const char *peer_id,
				  const char *display_name, const char *fingerprint);

bool tether_admission_accept(tether_admission_t *adm, const char *peer_id, bool pin);
bool tether_admission_reject(tether_admission_t *adm, const char *peer_id);
bool tether_admission_disconnect_peer(tether_admission_t *adm, const char *peer_id);
void tether_admission_revoke_all(tether_admission_t *adm);

// Look up a peer; the returned pointer is valid until the next mutation. For
// safety, copy fields out under the same lock — the implementation does that
// internally via the iter callback below.
typedef void (*tether_admission_iter_cb_t)(void *user, const tether_peer_t *peer);
void tether_admission_iter(tether_admission_t *adm, tether_admission_iter_cb_t cb,
			   void *user);

// Snapshot helpers
int tether_admission_count(tether_admission_t *adm, tether_peer_state_t state);
bool tether_admission_is_locked_out(tether_admission_t *adm);
int tether_admission_lockout_remaining(tether_admission_t *adm);

// Verifies that a peer that is reconnecting silently presents the same DTLS
// fingerprint that was pinned at accept time. Returns true if the connection
// should be re-admitted without user prompt.
bool tether_admission_verify_pinned(tether_admission_t *adm, const char *peer_id,
				    const char *fingerprint);
