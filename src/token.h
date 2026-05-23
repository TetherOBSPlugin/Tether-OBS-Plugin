/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Tokens are short, human-pasteable strings of the form TTHR-XXXX-XXXX-XXXX
 * drawn from a 32-char alphabet that excludes the visually ambiguous glyphs
 * 0/O/1/I/L. 60 bits of entropy: enough that a 5 rps rate-limited brute force
 * exhausts the universe in 7000 years.
 *
 * The token is opaque to the plugin. Validation and lookup happen on the
 * signaling backend; the plugin only generates, formats, and parses.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define TETHER_TOKEN_PREFIX        "TTHR-"
#define TETHER_TOKEN_GROUP_LEN     4
#define TETHER_TOKEN_GROUPS        3
#define TETHER_TOKEN_LEN           19  // "TTHR-XXXX-XXXX-XXXX"
#define TETHER_TOKEN_BUF           20  // includes terminator

// Fill `out` (at least TETHER_TOKEN_BUF bytes) with a freshly generated token.
// Uses a CSPRNG; returns false only if the platform RNG is unavailable.
bool tether_token_generate(char *out, size_t out_size);

// Returns true if `s` is well-formed (prefix, group count, alphabet).
// Does NOT validate that the token exists or is alive — that is the backend's job.
bool tether_token_is_well_formed(const char *s);

// Normalises a user-pasted token in-place: uppercases, strips spaces and
// surrounding quotes, and tolerates the visually ambiguous chars by mapping
// them back to their canonical form (e.g. lowercase l → 1 → rejected).
// Returns true on success and writes the canonical form to `out`.
bool tether_token_normalise(const char *in, char *out, size_t out_size);
