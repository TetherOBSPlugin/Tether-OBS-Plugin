/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "control.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include "known-tokens.h"
#include "log.h"
#include "receive-session.h"
#include "sender.h"

struct ctrl_peer {
	char peer_id[64];
	char name[64];
	char fingerprint[160];
	int state; // -1 unknown, 0 waiting, 1 connected, 2 failed
};

struct tether_control {
	int listen_fd;
	pthread_t thread;
	volatile long stop;
	char socket_path[128];

	tether_sender_t *sender;
	char current_token[64];

	pthread_mutex_t lock;
	DARRAY(struct ctrl_peer) peers;
};

static struct tether_control *g_ctrl = NULL;

static void on_sender_token(void *user, const char *token)
{
	struct tether_control *c = user;
	pthread_mutex_lock(&c->lock);
	if (token) {
		strncpy(c->current_token, token, sizeof(c->current_token) - 1);
		c->current_token[sizeof(c->current_token) - 1] = '\0';
	}
	pthread_mutex_unlock(&c->lock);
}

static void on_sender_pending(void *user, const char *peer_id, const char *name, const char *fp)
{
	struct tether_control *c = user;
	struct ctrl_peer p = {0};
	if (peer_id) {
		strncpy(p.peer_id, peer_id, sizeof(p.peer_id) - 1);
	}
	if (name) {
		strncpy(p.name, name, sizeof(p.name) - 1);
	}
	if (fp) {
		strncpy(p.fingerprint, fp, sizeof(p.fingerprint) - 1);
	}
	p.state = 0;
	pthread_mutex_lock(&c->lock);
	da_push_back(c->peers, &p);
	pthread_mutex_unlock(&c->lock);
}

static void on_sender_peer_state(void *user, const char *peer_id, tether_sender_state_t state)
{
	struct tether_control *c = user;
	pthread_mutex_lock(&c->lock);
	for (size_t i = 0; i < c->peers.num; ++i) {
		if (strcmp(c->peers.array[i].peer_id, peer_id) == 0) {
			c->peers.array[i].state = (state == TETHER_SENDER_STATE_PEER_CONNECTED) ? 1
						  : (state == TETHER_SENDER_STATE_FAILED)       ? 2
												: 0;
			break;
		}
	}
	pthread_mutex_unlock(&c->lock);
}

static void on_sender_peer_gone(void *user, const char *peer_id)
{
	struct tether_control *c = user;
	pthread_mutex_lock(&c->lock);
	for (size_t i = 0; i < c->peers.num; ++i) {
		if (strcmp(c->peers.array[i].peer_id, peer_id) == 0) {
			da_erase(c->peers, i);
			break;
		}
	}
	pthread_mutex_unlock(&c->lock);
}

static const char *state_word(int state)
{
	switch (state) {
	case 0:
		return "waiting";
	case 1:
		return "connected";
	case 2:
		return "failed";
	default:
		return "unknown";
	}
}

static int write_all(int fd, const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, data, len);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

static void reply(int fd, const char *s)
{
	write_all(fd, s, strlen(s));
	if (s[strlen(s) - 1] != '\n') {
		write_all(fd, "\n", 1);
	}
}

static void replyf(int fd, const char *fmt, ...)
{
	struct dstr buf;
	dstr_init(&buf);
	va_list ap;
	va_start(ap, fmt);
	dstr_vprintf(&buf, fmt, ap);
	va_end(ap);
	reply(fd, buf.array);
	dstr_free(&buf);
}

static void cmd_sender_create(struct tether_control *c, int fd, char *args)
{
	if (c->sender) {
		reply(fd, "ERR sender already running — call sender-revoke first");
		return;
	}
	if (!args || !*args) {
		reply(fd, "ERR missing source name");
		return;
	}
	// Format: <video1,video2,...>|<audio1,audio2,...>
	// The pipe separates the video list from the audio list. If no pipe is
	// present, fall back to legacy form (first = video, rest = audio).
	char *video_section = args;
	char *audio_section = strchr(args, '|');
	if (audio_section) {
		*audio_section++ = '\0';
	}
	DARRAY(char *) video_ptrs;
	DARRAY(char *) audio_ptrs;
	da_init(video_ptrs);
	da_init(audio_ptrs);

	char *saveptr = NULL;
	for (char *tok = strtok_r(video_section, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
		if (*tok) {
			da_push_back(video_ptrs, &tok);
		}
	}
	if (audio_section) {
		for (char *tok = strtok_r(audio_section, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
			if (*tok) {
				da_push_back(audio_ptrs, &tok);
			}
		}
	} else if (video_ptrs.num > 1) {
		// Legacy: only one video, the rest are audio.
		for (size_t i = 1; i < video_ptrs.num; ++i) {
			da_push_back(audio_ptrs, &video_ptrs.array[i]);
		}
		video_ptrs.num = 1;
	}
	char *sentinel = NULL;
	da_push_back(video_ptrs, &sentinel);
	da_push_back(audio_ptrs, &sentinel);

	if (video_ptrs.num <= 1) {
		reply(fd, "ERR missing video source");
		da_free(video_ptrs);
		da_free(audio_ptrs);
		return;
	}

	tether_sender_config_t cfg = {0};
	cfg.video_source_names = (const char *const *)video_ptrs.array;
	cfg.audio_source_names = (const char *const *)audio_ptrs.array;
	cfg.video_bitrate_kbps = 6000;
	cfg.max_receivers = 4;
	cfg.token_ttl_minutes = 30;
	cfg.reusable_token = true;

	tether_sender_callbacks_t cbs = {0};
	cbs.user = c;
	cbs.on_token = on_sender_token;
	cbs.on_pending = on_sender_pending;
	cbs.on_peer_state = on_sender_peer_state;
	cbs.on_peer_gone = on_sender_peer_gone;

	c->sender = tether_sender_create(&cfg, &cbs);
	da_free(video_ptrs);
	da_free(audio_ptrs);
	if (!c->sender) {
		reply(fd, "ERR sender create failed");
		return;
	}
	// Wait briefly for the token to land. Signaling round-trip is usually
	// <1s on a warm connection; cap at 5s to keep test scripts snappy.
	for (int i = 0; i < 50; ++i) {
		os_sleepto_ns(os_gettime_ns() + 100000000ULL);
		pthread_mutex_lock(&c->lock);
		bool have = c->current_token[0] != '\0';
		pthread_mutex_unlock(&c->lock);
		if (have) {
			break;
		}
	}
	pthread_mutex_lock(&c->lock);
	if (c->current_token[0]) {
		replyf(fd, "OK token=%s", c->current_token);
	} else {
		reply(fd, "ERR token not yet issued (timeout)");
	}
	pthread_mutex_unlock(&c->lock);
}

static void cmd_sender_status(struct tether_control *c, int fd)
{
	struct dstr out;
	dstr_init(&out);
	pthread_mutex_lock(&c->lock);
	dstr_printf(&out, "OK token=%s peers=", c->current_token[0] ? c->current_token : "-");
	for (size_t i = 0; i < c->peers.num; ++i) {
		if (i) {
			dstr_cat(&out, ",");
		}
		dstr_catf(&out, "%s:%s:%s", c->peers.array[i].peer_id, state_word(c->peers.array[i].state),
			  c->peers.array[i].name);
	}
	pthread_mutex_unlock(&c->lock);
	reply(fd, out.array);
	dstr_free(&out);
}

static void cmd_sender_accept(struct tether_control *c, int fd, char *args)
{
	if (!c->sender) {
		reply(fd, "ERR no sender");
		return;
	}
	if (!args || !*args) {
		reply(fd, "ERR missing peer_id");
		return;
	}
	tether_sender_accept(c->sender, args, true);
	replyf(fd, "OK accepted %s", args);
}

static void cmd_sender_reject(struct tether_control *c, int fd, char *args)
{
	if (!c->sender) {
		reply(fd, "ERR no sender");
		return;
	}
	if (!args || !*args) {
		reply(fd, "ERR missing peer_id");
		return;
	}
	tether_sender_reject(c->sender, args);
	replyf(fd, "OK rejected %s", args);
}

static void cmd_sender_disconnect(struct tether_control *c, int fd, char *args)
{
	if (!c->sender) {
		reply(fd, "ERR no sender");
		return;
	}
	if (!args || !*args) {
		reply(fd, "ERR missing peer_id");
		return;
	}
	tether_sender_disconnect_peer(c->sender, args);
	replyf(fd, "OK disconnected %s", args);
}

static void cmd_sender_revoke(struct tether_control *c, int fd)
{
	if (!c->sender) {
		reply(fd, "ERR no sender");
		return;
	}
	tether_sender_revoke_token(c->sender);
	tether_sender_release(c->sender);
	c->sender = NULL;
	pthread_mutex_lock(&c->lock);
	c->current_token[0] = '\0';
	da_resize(c->peers, 0);
	pthread_mutex_unlock(&c->lock);
	reply(fd, "OK revoked");
}

static const char *rx_state_word(tether_receive_state_t s)
{
	switch (s) {
	case TETHER_RX_STATE_CONNECTING:
		return "connecting";
	case TETHER_RX_STATE_AWAITING_ACCEPT:
		return "awaiting-accept";
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

// Each registered token gets one session ref owned by the control plane so
// the session stays up between commands (mirroring the dialog's behaviour).
static DARRAY(tether_receive_session_t *) g_held_sessions;

static void cmd_receive_register(int fd, char *args)
{
	if (!args || !*args) {
		reply(fd, "ERR missing token");
		return;
	}
	tether_known_tokens_add(args);
	tether_receive_session_t *s = tether_receive_session_get(args);
	if (!s) {
		reply(fd, "ERR session create failed");
		return;
	}
	da_push_back(g_held_sessions, &s);
	replyf(fd, "OK registered %s state=%s", args, rx_state_word(tether_receive_session_state(s)));
}

struct enum_ctx {
	struct dstr *out;
	bool first;
};

static void rx_enum_cb(void *user, tether_receive_session_t *session)
{
	struct enum_ctx *ctx = user;
	if (!ctx->first) {
		dstr_cat(ctx->out, ",");
	}
	dstr_catf(ctx->out, "%s:%s", tether_receive_session_token(session),
		  rx_state_word(tether_receive_session_state(session)));
	ctx->first = false;
}

static void cmd_receive_status(int fd)
{
	struct dstr out;
	dstr_init(&out);
	dstr_copy(&out, "OK sessions=");
	struct enum_ctx ctx = {.out = &out, .first = true};
	tether_receive_session_enumerate(rx_enum_cb, &ctx);
	reply(fd, out.array);
	dstr_free(&out);
}

static void cmd_receive_forget(int fd, char *args)
{
	if (!args || !*args) {
		reply(fd, "ERR missing token");
		return;
	}
	tether_known_tokens_remove(args);
	// Drop our held ref(s) for that token.
	for (size_t i = g_held_sessions.num; i > 0; --i) {
		tether_receive_session_t *s = g_held_sessions.array[i - 1];
		if (s && strcmp(tether_receive_session_token(s), args) == 0) {
			tether_receive_session_release(s);
			da_erase(g_held_sessions, i - 1);
		}
	}
	replyf(fd, "OK forgot %s", args);
}

static void dispatch(struct tether_control *c, int fd, char *line)
{
	// Strip trailing whitespace
	size_t L = strlen(line);
	while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r' || line[L - 1] == ' ')) {
		line[--L] = '\0';
	}
	char *space = strchr(line, ' ');
	char *args = NULL;
	if (space) {
		*space = '\0';
		args = space + 1;
	}
	if (strcmp(line, "sender-create") == 0) {
		cmd_sender_create(c, fd, args);
	} else if (strcmp(line, "sender-status") == 0) {
		cmd_sender_status(c, fd);
	} else if (strcmp(line, "sender-accept") == 0) {
		cmd_sender_accept(c, fd, args);
	} else if (strcmp(line, "sender-reject") == 0) {
		cmd_sender_reject(c, fd, args);
	} else if (strcmp(line, "sender-disconnect") == 0) {
		cmd_sender_disconnect(c, fd, args);
	} else if (strcmp(line, "sender-revoke") == 0) {
		cmd_sender_revoke(c, fd);
	} else if (strcmp(line, "receive-register") == 0) {
		cmd_receive_register(fd, args);
	} else if (strcmp(line, "receive-status") == 0) {
		cmd_receive_status(fd);
	} else if (strcmp(line, "receive-forget") == 0) {
		cmd_receive_forget(fd, args);
	} else if (strcmp(line, "ping") == 0) {
		reply(fd, "OK pong");
	} else {
		replyf(fd, "ERR unknown command '%s'", line);
	}
}

static void *worker(void *arg)
{
	struct tether_control *c = arg;
	while (!os_atomic_load_long(&c->stop)) {
		int fd = accept(c->listen_fd, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			}
			break;
		}
		char buf[2048];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			dispatch(c, fd, buf);
		}
		close(fd);
	}
	return NULL;
}

void tether_control_init(void)
{
	const char *enabled = getenv("TETHER_CONTROL");
	if (!enabled || strcmp(enabled, "1") != 0) {
		return;
	}
	g_ctrl = bzalloc(sizeof(*g_ctrl));
	pthread_mutex_init(&g_ctrl->lock, NULL);
	da_init(g_ctrl->peers);
	da_init(g_held_sessions);

	const char *path_override = getenv("TETHER_CONTROL_SOCKET");
	if (path_override && *path_override) {
		strncpy(g_ctrl->socket_path, path_override, sizeof(g_ctrl->socket_path) - 1);
	} else {
		snprintf(g_ctrl->socket_path, sizeof(g_ctrl->socket_path), "/tmp/tether-%d.sock", (int)getpid());
	}
	unlink(g_ctrl->socket_path);

	g_ctrl->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_ctrl->listen_fd < 0) {
		tether_log_error("control: socket() failed: %s", strerror(errno));
		bfree(g_ctrl);
		g_ctrl = NULL;
		return;
	}
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, g_ctrl->socket_path, sizeof(addr.sun_path) - 1);
	if (bind(g_ctrl->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		tether_log_error("control: bind(%s) failed: %s", g_ctrl->socket_path, strerror(errno));
		close(g_ctrl->listen_fd);
		bfree(g_ctrl);
		g_ctrl = NULL;
		return;
	}
	if (listen(g_ctrl->listen_fd, 4) < 0) {
		tether_log_error("control: listen() failed: %s", strerror(errno));
		close(g_ctrl->listen_fd);
		unlink(g_ctrl->socket_path);
		bfree(g_ctrl);
		g_ctrl = NULL;
		return;
	}
	pthread_create(&g_ctrl->thread, NULL, worker, g_ctrl);
	tether_log_info("control: listening on %s", g_ctrl->socket_path);
}

void tether_control_shutdown(void)
{
	if (!g_ctrl) {
		return;
	}
	os_atomic_set_long(&g_ctrl->stop, 1);
	shutdown(g_ctrl->listen_fd, SHUT_RDWR);
	close(g_ctrl->listen_fd);
	pthread_join(g_ctrl->thread, NULL);
	unlink(g_ctrl->socket_path);
	if (g_ctrl->sender) {
		tether_sender_release(g_ctrl->sender);
	}
	for (size_t i = 0; i < g_held_sessions.num; ++i) {
		tether_receive_session_release(g_held_sessions.array[i]);
	}
	da_free(g_held_sessions);
	da_free(g_ctrl->peers);
	pthread_mutex_destroy(&g_ctrl->lock);
	bfree(g_ctrl);
	g_ctrl = NULL;
}
