/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "known-tokens.h"

#include <util/bmem.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static DARRAY(char *) g_tokens;
static bool g_initialised = false;

void tether_known_tokens_init(void)
{
	pthread_mutex_lock(&g_lock);
	if (!g_initialised) {
		da_init(g_tokens);
		g_initialised = true;
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
