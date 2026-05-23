/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Process-wide list of receive tokens the user has registered via the
 * Tools→Tether→Receive dialog. The source-properties combo populates from
 * here so the user can pick a previously-entered token without retyping.
 *
 * Lifetime: registered tokens persist for the OBS session only — they are
 * not written back to disk. Tether-Quelle sources still embed their own
 * token in their saved settings.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void tether_known_tokens_init(void);
void tether_known_tokens_shutdown(void);

// Adds a token if not already present. Returns true if newly added.
bool tether_known_tokens_add(const char *token);
bool tether_known_tokens_remove(const char *token);

// Renames a known token. Returns true if a row was updated.
bool tether_known_tokens_set_name(const char *token, const char *name);

// Looks up the friendly name for a token. Returns NULL if no entry or no
// name set. Caller must NOT free the returned pointer; it stays valid only
// until the next mutation (so copy if you need to hold it).
const char *tether_known_tokens_get_name(const char *token);

// Snapshot of the current list. Caller must call _free_snapshot when done.
// out[*count] is a malloc'd array of bstrdup'd strings; size_t *count is
// populated. Pass NULL to free. The parallel `names` array (also malloc'd,
// size *count) contains the matching friendly names or NULL.
char **tether_known_tokens_snapshot(size_t *count);
char **tether_known_tokens_snapshot_with_names(size_t *count, char ***names_out);
void tether_known_tokens_free_snapshot(char **list, size_t count);
void tether_known_tokens_free_names(char **names, size_t count);

#ifdef __cplusplus
}
#endif
