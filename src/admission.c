/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "admission.h"

#include <string.h>

#include <util/bmem.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include "log.h"

struct tether_admission {
	tether_admission_config_t cfg;
	DARRAY(tether_peer_t) peers;

	// Sliding window of attempt timestamps for rate limiting.
	DARRAY(uint64_t) attempts;
	uint64_t lockout_until_ns;

	pthread_mutex_t lock;
};

static uint64_t now_ns(void)
{
	return os_gettime_ns();
}

static tether_peer_t *find_peer_locked(tether_admission_t *adm, const char *peer_id)
{
	for (size_t i = 0; i < adm->peers.num; ++i) {
		if (strcmp(adm->peers.array[i].peer_id, peer_id) == 0) {
			return &adm->peers.array[i];
		}
	}
	return NULL;
}

static int count_locked(const tether_admission_t *adm, tether_peer_state_t s)
{
	int n = 0;
	for (size_t i = 0; i < adm->peers.num; ++i) {
		if (adm->peers.array[i].state == s) {
			++n;
		}
	}
	return n;
}

static void prune_attempts_locked(tether_admission_t *adm)
{
	if (adm->cfg.rate_limit_window_seconds <= 0) {
		return;
	}
	uint64_t cutoff = now_ns() -
			  (uint64_t)adm->cfg.rate_limit_window_seconds * 1000000000ULL;
	size_t keep_from = 0;
	for (; keep_from < adm->attempts.num; ++keep_from) {
		if (adm->attempts.array[keep_from] >= cutoff) {
			break;
		}
	}
	if (keep_from > 0) {
		memmove(adm->attempts.array, adm->attempts.array + keep_from,
			(adm->attempts.num - keep_from) * sizeof(uint64_t));
		adm->attempts.num -= keep_from;
	}
}

static bool rate_limit_blocks_locked(tether_admission_t *adm)
{
	uint64_t now = now_ns();
	if (adm->lockout_until_ns > now) {
		return true;
	}
	prune_attempts_locked(adm);
	if (adm->cfg.rate_limit_max_attempts > 0 &&
	    (int)adm->attempts.num >= adm->cfg.rate_limit_max_attempts) {
		adm->lockout_until_ns =
			now + (uint64_t)adm->cfg.lockout_seconds * 1000000000ULL;
		tether_log_warning("admission: rate limit hit, locking out for %ds",
				   adm->cfg.lockout_seconds);
		return true;
	}
	return false;
}

tether_admission_t *tether_admission_create(const tether_admission_config_t *cfg)
{
	tether_admission_t *adm = bzalloc(sizeof(*adm));
	adm->cfg = *cfg;
	if (adm->cfg.max_receivers <= 0) {
		adm->cfg.max_receivers = 4;
	}
	if (adm->cfg.rate_limit_window_seconds <= 0) {
		adm->cfg.rate_limit_window_seconds = 60;
	}
	if (adm->cfg.rate_limit_max_attempts <= 0) {
		adm->cfg.rate_limit_max_attempts = 10;
	}
	if (adm->cfg.lockout_seconds <= 0) {
		adm->cfg.lockout_seconds = 300;
	}
	pthread_mutex_init(&adm->lock, NULL);
	return adm;
}

void tether_admission_release(tether_admission_t *adm)
{
	if (!adm) {
		return;
	}
	pthread_mutex_destroy(&adm->lock);
	da_free(adm->peers);
	da_free(adm->attempts);
	bfree(adm);
}

bool tether_admission_add_pending(tether_admission_t *adm, const char *peer_id,
				  const char *display_name, const char *fingerprint)
{
	if (!adm || !peer_id) {
		return false;
	}
	pthread_mutex_lock(&adm->lock);

	if (rate_limit_blocks_locked(adm)) {
		pthread_mutex_unlock(&adm->lock);
		return false;
	}
	da_push_back(adm->attempts, &(uint64_t){now_ns()});

	if (find_peer_locked(adm, peer_id)) {
		pthread_mutex_unlock(&adm->lock);
		return false;
	}
	if (count_locked(adm, TETHER_PEER_ACCEPTED) >= adm->cfg.max_receivers) {
		pthread_mutex_unlock(&adm->lock);
		tether_log_info("admission: cap reached (%d), rejecting %s",
				adm->cfg.max_receivers, peer_id);
		return false;
	}

	tether_peer_t p = {0};
	strncpy(p.peer_id, peer_id, sizeof(p.peer_id) - 1);
	if (display_name) {
		strncpy(p.display_name, display_name, sizeof(p.display_name) - 1);
	}
	if (fingerprint) {
		strncpy(p.fingerprint, fingerprint, sizeof(p.fingerprint) - 1);
	}
	p.first_seen_ns = now_ns();
	p.state = TETHER_PEER_PENDING;

	// Auto-accept pinned reconnects if configured and fingerprint matches.
	if (adm->cfg.auto_accept_pinned && fingerprint) {
		for (size_t i = 0; i < adm->peers.num; ++i) {
			tether_peer_t *q = &adm->peers.array[i];
			if (q->pinned && strcmp(q->fingerprint, fingerprint) == 0) {
				p.pinned = true;
				p.state = TETHER_PEER_ACCEPTED;
				break;
			}
		}
	}

	da_push_back(adm->peers, &p);
	tether_peer_t copy = p;
	pthread_mutex_unlock(&adm->lock);

	if (adm->cfg.on_changed) {
		adm->cfg.on_changed(adm->cfg.cb_user, &copy);
	}
	return true;
}

static bool set_peer_state(tether_admission_t *adm, const char *peer_id,
			   tether_peer_state_t new_state, bool pin)
{
	if (!adm || !peer_id) {
		return false;
	}
	pthread_mutex_lock(&adm->lock);
	tether_peer_t *p = find_peer_locked(adm, peer_id);
	if (!p) {
		pthread_mutex_unlock(&adm->lock);
		return false;
	}
	p->state = new_state;
	if (pin) {
		p->pinned = true;
	}
	tether_peer_t copy = *p;
	pthread_mutex_unlock(&adm->lock);

	if (adm->cfg.on_changed) {
		adm->cfg.on_changed(adm->cfg.cb_user, &copy);
	}
	return true;
}

bool tether_admission_accept(tether_admission_t *adm, const char *peer_id, bool pin)
{
	return set_peer_state(adm, peer_id, TETHER_PEER_ACCEPTED, pin);
}

bool tether_admission_reject(tether_admission_t *adm, const char *peer_id)
{
	return set_peer_state(adm, peer_id, TETHER_PEER_REJECTED, false);
}

bool tether_admission_disconnect_peer(tether_admission_t *adm, const char *peer_id)
{
	return set_peer_state(adm, peer_id, TETHER_PEER_DISCONNECTED, false);
}

void tether_admission_revoke_all(tether_admission_t *adm)
{
	if (!adm) {
		return;
	}
	pthread_mutex_lock(&adm->lock);
	for (size_t i = 0; i < adm->peers.num; ++i) {
		adm->peers.array[i].state = TETHER_PEER_REVOKED;
	}
	pthread_mutex_unlock(&adm->lock);
}

void tether_admission_iter(tether_admission_t *adm, tether_admission_iter_cb_t cb,
			   void *user)
{
	if (!adm || !cb) {
		return;
	}
	pthread_mutex_lock(&adm->lock);
	// Snapshot to a local array first so the callback can take its time
	// without holding our lock.
	size_t n = adm->peers.num;
	tether_peer_t *snap = bmalloc(n * sizeof(tether_peer_t));
	memcpy(snap, adm->peers.array, n * sizeof(tether_peer_t));
	pthread_mutex_unlock(&adm->lock);

	for (size_t i = 0; i < n; ++i) {
		cb(user, &snap[i]);
	}
	bfree(snap);
}

int tether_admission_count(tether_admission_t *adm, tether_peer_state_t state)
{
	if (!adm) {
		return 0;
	}
	pthread_mutex_lock(&adm->lock);
	int n = count_locked(adm, state);
	pthread_mutex_unlock(&adm->lock);
	return n;
}

bool tether_admission_is_locked_out(tether_admission_t *adm)
{
	if (!adm) {
		return false;
	}
	pthread_mutex_lock(&adm->lock);
	bool out = adm->lockout_until_ns > now_ns();
	pthread_mutex_unlock(&adm->lock);
	return out;
}

int tether_admission_lockout_remaining(tether_admission_t *adm)
{
	if (!adm) {
		return 0;
	}
	pthread_mutex_lock(&adm->lock);
	uint64_t now = now_ns();
	int remain = adm->lockout_until_ns > now
			     ? (int)((adm->lockout_until_ns - now) / 1000000000ULL)
			     : 0;
	pthread_mutex_unlock(&adm->lock);
	return remain;
}

bool tether_admission_verify_pinned(tether_admission_t *adm, const char *peer_id,
				    const char *fingerprint)
{
	if (!adm || !peer_id || !fingerprint) {
		return false;
	}
	pthread_mutex_lock(&adm->lock);
	tether_peer_t *p = find_peer_locked(adm, peer_id);
	bool ok = p && p->pinned && strcmp(p->fingerprint, fingerprint) == 0;
	pthread_mutex_unlock(&adm->lock);
	return ok;
}
