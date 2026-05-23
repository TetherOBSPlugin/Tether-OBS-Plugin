/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Plugin lifecycle only. Module load registers source types, module unload
 * tears them down. Everything else lives in its own translation unit.
 */

#include <obs-module.h>
#include <plugin-support.h>

#include "log.h"
#include "sender.h"
#include "source.h"
#include "webrtc.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("tether", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Source.Receiver.Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Tether";
}

bool obs_module_load(void)
{
	tether_log_info("loaded version %s", PLUGIN_VERSION);

	if (!tether_webrtc_global_init()) {
		tether_log_error("webrtc global init failed; plugin not loaded");
		return false;
	}

	tether_source_register();
	tether_sender_register();
	return true;
}

void obs_module_unload(void)
{
	tether_sender_shutdown();
	tether_source_shutdown();
	tether_webrtc_global_shutdown();
	tether_log_info("unloaded");
}
