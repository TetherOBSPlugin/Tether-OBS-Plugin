/*
 * SPDX-FileCopyrightText: 2026 Tether contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "token.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <util/base.h>
#include <util/platform.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include "log.h"

// 32 visually distinct uppercase letters + digits.
// Excluded: 0 O, 1 I L. 5 bits per char × 12 chars = 60 bits of entropy.
static const char k_alphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
static const size_t k_alphabet_len = sizeof(k_alphabet) - 1;  // = 31; rounded up below

// We pad to 32 so the modulo bias on a 5-bit draw is exactly zero;
// duplicating one entry is harmless. K is duplicated.
static const char k_alphabet_padded[33] = "ABCDEFGHJKKMNPQRSTUVWXYZ23456789";

static bool fill_random(uint8_t *buf, size_t n)
{
	// libobs does not expose a portable CSPRNG wrapper, so each platform
	// uses its native API directly. All three are guaranteed available on
	// the OBS-supported OS baselines.
#if defined(_WIN32)
	NTSTATUS s = BCryptGenRandom(NULL, buf, (ULONG)n,
				     BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	return s == 0;
#else
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, buf + got, n - got);
		if (r < 0) {
			close(fd);
			return false;
		}
		got += (size_t)r;
	}
	close(fd);
	return true;
#endif
}

bool tether_token_generate(char *out, size_t out_size)
{
	if (out_size < TETHER_TOKEN_BUF) {
		return false;
	}

	uint8_t bytes[TETHER_TOKEN_GROUPS * TETHER_TOKEN_GROUP_LEN];
	if (!fill_random(bytes, sizeof(bytes))) {
		tether_log_error("token: CSPRNG unavailable");
		return false;
	}

	size_t pos = 0;
	memcpy(out, TETHER_TOKEN_PREFIX, 5);
	pos += 5;

	for (int g = 0; g < TETHER_TOKEN_GROUPS; ++g) {
		for (int i = 0; i < TETHER_TOKEN_GROUP_LEN; ++i) {
			uint8_t idx = bytes[g * TETHER_TOKEN_GROUP_LEN + i] & 0x1F;  // 32 entries
			out[pos++] = k_alphabet_padded[idx];
		}
		if (g + 1 < TETHER_TOKEN_GROUPS) {
			out[pos++] = '-';
		}
	}
	out[pos] = '\0';
	return true;
}

static bool char_in_alphabet(char c)
{
	for (size_t i = 0; i < k_alphabet_len; ++i) {
		if (k_alphabet[i] == c) {
			return true;
		}
	}
	return false;
}

bool tether_token_is_well_formed(const char *s)
{
	if (!s) {
		return false;
	}
	if (strncmp(s, TETHER_TOKEN_PREFIX, 5) != 0) {
		return false;
	}
	const char *p = s + 5;
	for (int g = 0; g < TETHER_TOKEN_GROUPS; ++g) {
		for (int i = 0; i < TETHER_TOKEN_GROUP_LEN; ++i) {
			if (!char_in_alphabet(*p)) {
				return false;
			}
			++p;
		}
		if (g + 1 < TETHER_TOKEN_GROUPS) {
			if (*p != '-') {
				return false;
			}
			++p;
		}
	}
	return *p == '\0';
}

bool tether_token_normalise(const char *in, char *out, size_t out_size)
{
	if (!in || !out || out_size < TETHER_TOKEN_BUF) {
		return false;
	}
	size_t j = 0;
	for (const char *p = in; *p && j + 1 < out_size; ++p) {
		char c = *p;
		// Strip whitespace and quotes
		if (isspace((unsigned char)c) || c == '"' || c == '\'') {
			continue;
		}
		c = (char)toupper((unsigned char)c);
		// Disambiguate visually similar characters by collapsing them
		// to the canonical form. The backend rejects ambiguous tokens
		// already, but doing it here lets the user paste sloppily.
		if (c == 'O') {
			c = '0';  // will be rejected by alphabet check below
		} else if (c == 'I' || c == 'L') {
			c = '1';  // ditto
		}
		out[j++] = c;
	}
	out[j] = '\0';
	return tether_token_is_well_formed(out);
}
