/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "known-tokens.h"

#include <obs-module.h>
#include <obs.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(char *) g_tokens;
static bool g_initialised = false;

static char *config_path(void)
{
	// Stored alongside other OBS module data; cross-platform via
	// obs_module_config_path which returns a writable, per-user path.
	char *p = obs_module_config_path("known_tokens.json");
	if (!p) {
		return NULL;
	}
	// Ensure the parent directory exists.
	char *slash = strrchr(p, '/');
#ifdef _WIN32
	char *bs = strrchr(p, '\\');
	if (bs && (!slash || bs > slash)) {
		slash = bs;
	}
#endif
	if (slash) {
		*slash = '\0';
		os_mkdirs(p);
		*slash = '/';
	}
	return p;
}

static void load_from_disk_locked(void)
{
	char *path = config_path();
	if (!path) {
		return;
	}
	obs_data_t *root = obs_data_create_from_json_file(path);
	if (!root) {
		bfree(path);
		return;
	}
	obs_data_array_t *arr = obs_data_get_array(root, "tokens");
	if (arr) {
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; ++i) {
			obs_data_t *item = obs_data_array_item(arr, i);
			const char *tok = obs_data_get_string(item, "token");
			if (tok && *tok) {
				char *copy = bstrdup(tok);
				da_push_back(g_tokens, &copy);
			}
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}
	obs_data_release(root);
	bfree(path);
}

static void save_to_disk_locked(void)
{
	char *path = config_path();
	if (!path) {
		return;
	}
	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (size_t i = 0; i < g_tokens.num; ++i) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "token", g_tokens.array[i]);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "tokens", arr);
	obs_data_array_release(arr);
	obs_data_save_json(root, path);
	obs_data_release(root);
	bfree(path);
}

void tether_known_tokens_init(void)
{
	pthread_mutex_lock(&g_lock);
	if (!g_initialised) {
		da_init(g_tokens);
		g_initialised = true;
		load_from_disk_locked();
	}
	pthread_mutex_unlock(&g_lock);
}

void tether_known_tokens_shutdown(void)
{
	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < g_tokens.num; ++i) {
		bfree(g_tokens.array[i]);
	}
	da_free(g_tokens);
	g_initialised = false;
	pthread_mutex_unlock(&g_lock);
}

bool tether_known_tokens_add(const char *token)
{
	if (!token || !*token) {
		return false;
	}
	bool added = false;
	pthread_mutex_lock(&g_lock);
	if (g_initialised) {
		bool exists = false;
		for (size_t i = 0; i < g_tokens.num; ++i) {
			if (strcmp(g_tokens.array[i], token) == 0) {
				exists = true;
				break;
			}
		}
		if (!exists) {
			char *copy = bstrdup(token);
			da_push_back(g_tokens, &copy);
			added = true;
			save_to_disk_locked();
		}
	}
	pthread_mutex_unlock(&g_lock);
	return added;
}

bool tether_known_tokens_remove(const char *token)
{
	if (!token) {
		return false;
	}
	bool removed = false;
	pthread_mutex_lock(&g_lock);
	if (g_initialised) {
		for (size_t i = 0; i < g_tokens.num; ++i) {
			if (strcmp(g_tokens.array[i], token) == 0) {
				bfree(g_tokens.array[i]);
				da_erase(g_tokens, i);
				removed = true;
				save_to_disk_locked();
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_lock);
	return removed;
}

char **tether_known_tokens_snapshot(size_t *count)
{
	pthread_mutex_lock(&g_lock);
	size_t n = g_initialised ? g_tokens.num : 0;
	char **out = n > 0 ? bmalloc(n * sizeof(char *)) : NULL;
	for (size_t i = 0; i < n; ++i) {
		out[i] = bstrdup(g_tokens.array[i]);
	}
	pthread_mutex_unlock(&g_lock);
	if (count) {
		*count = n;
	}
	return out;
}

void tether_known_tokens_free_snapshot(char **list, size_t count)
{
	if (!list) {
		return;
	}
	for (size_t i = 0; i < count; ++i) {
		bfree(list[i]);
	}
	bfree(list);
}
