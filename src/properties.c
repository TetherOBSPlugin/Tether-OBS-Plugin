/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "properties.h"

#include <obs-module.h>
#include <obs.h>

#include "known-tokens.h"
#include "log.h"

// Compile-time managed default. Replace the subdomain with the one your
// deployed Worker script registers under (configurable in CI).
#ifndef TETHER_DEFAULT_SIGNALING_URL
#define TETHER_DEFAULT_SIGNALING_URL "wss://tether-signaling.tether-plugin.workers.dev/v1"
#endif

const char *tether_default_server_url(void)
{
	return TETHER_DEFAULT_SIGNALING_URL;
}

static void add_advanced(obs_properties_t *p)
{
	obs_properties_t *g = obs_properties_create();
	obs_property_t *codec = obs_properties_add_list(g, "video_codec", obs_module_text("Settings.Advanced.Codec"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(codec, obs_module_text("Settings.Advanced.Codec.H264"), 0);
	obs_property_list_add_int(codec, obs_module_text("Settings.Advanced.Codec.Vp9"), 1);
	obs_property_list_add_int(codec, obs_module_text("Settings.Advanced.Codec.Av1"), 2);

	obs_properties_add_int(g, "max_bitrate", obs_module_text("Settings.Advanced.MaxBitrate"), 500, 20000, 250);
	obs_properties_add_int(g, "max_receivers", obs_module_text("Settings.Advanced.MaxReceivers"), 1, 16, 1);
	obs_properties_add_int(g, "token_ttl_minutes", obs_module_text("Settings.Advanced.TokenTtlMinutes"), 5, 1440,
			       5);
	obs_properties_add_bool(g, "auto_accept_pinned", obs_module_text("Settings.Advanced.AutoAccept.Pinned"));

	obs_properties_add_group(p, "advanced", obs_module_text("Settings.Group.Advanced"), OBS_GROUP_NORMAL, g);
}

static void add_connection_group(obs_properties_t *p)
{
	obs_properties_t *g = obs_properties_create();
	obs_property_t *url =
		obs_properties_add_text(g, "server_url", obs_module_text("Settings.Server.Url"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(url, obs_module_text("Settings.Server.Url.Description"));

	obs_properties_add_text(g, "stun_url", obs_module_text("Settings.Stun.Url"), OBS_TEXT_DEFAULT);

	obs_property_t *turn =
		obs_properties_add_text(g, "turn_url", obs_module_text("Settings.Turn.Url"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(turn, obs_module_text("Settings.Turn.Url.Description"));
	obs_properties_add_text(g, "turn_user", obs_module_text("Settings.Turn.Username"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(g, "turn_pass", obs_module_text("Settings.Turn.Credential"), OBS_TEXT_PASSWORD);

	obs_properties_add_group(p, "connection", obs_module_text("Settings.Group.Connection"), OBS_GROUP_NORMAL, g);
}

// ---- receiver ----

obs_properties_t *tether_properties_for_receiver(obs_source_t *src)
{
	UNUSED_PARAMETER(src);
	obs_properties_t *p = obs_properties_create();

	add_connection_group(p);

	obs_properties_t *g = obs_properties_create();
	// Editable combo: pulls from tokens the user registered in
	// Tools→Tether→Receive, but also allows free-form entry so a fresh token
	// can be pasted without first walking through the dialog.
	obs_property_t *t = obs_properties_add_list(g, "token", obs_module_text("Token.Paste"), OBS_COMBO_TYPE_EDITABLE,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(t, obs_module_text("Token.Paste.Description"));
	size_t n = 0;
	char **known = tether_known_tokens_snapshot(&n);
	for (size_t i = 0; i < n; ++i) {
		obs_property_list_add_string(t, known[i], known[i]);
	}
	tether_known_tokens_free_snapshot(known, n);
	obs_properties_add_group(p, "token_group", obs_module_text("Settings.Group.Source"), OBS_GROUP_NORMAL, g);

	add_advanced(p);
	return p;
}

// ---- sender ----
