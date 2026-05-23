// SPDX-FileCopyrightText: 2026 Tether contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import { Room } from './room';
export { Room };

interface Env {
  ROOMS: DurableObjectNamespace;
  MAX_TOKEN_TTL_MINUTES: string;
  RATE_LIMIT_WINDOW_SECONDS: string;
  RATE_LIMIT_MAX_ATTEMPTS: string;
  LOCKOUT_SECONDS: string;
}

// Token format: TTHR-XXXX-XXXX-XXXX, 32-char ambiguity-free alphabet.
// We validate format here so a malformed token never gets a Durable Object
// instantiated for it (cheap DoS protection).
const TOKEN_REGEX = /^TTHR-[A-HJKMNPQRSTUVWXYZ2-9]{4}-[A-HJKMNPQRSTUVWXYZ2-9]{4}-[A-HJKMNPQRSTUVWXYZ2-9]{4}$/;

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    // Health check — useful for monitoring.
    if (url.pathname === '/health') {
      return new Response('ok', { status: 200 });
    }

    // The main path: /v1 — a fresh sender opens a WebSocket here, gets back
    // a token to share. The DO ID is the token itself (case-folded), which
    // keeps a single instance per active sharing session.
    if (url.pathname !== '/v1') {
      return new Response('not found', { status: 404 });
    }

    const upgrade = request.headers.get('Upgrade');
    if (upgrade !== 'websocket') {
      return new Response('expected websocket', { status: 426 });
    }

    // Senders connect without a token; the Room generates one. Receivers
    // connect with ?token=TTHR-...; the room id is derived from the token.
    const tokenParam = url.searchParams.get('token');
    let roomName: string;
    if (tokenParam) {
      const normalised = tokenParam.toUpperCase();
      if (!TOKEN_REGEX.test(normalised)) {
        return new Response('invalid token', { status: 400 });
      }
      roomName = normalised;
    } else {
      // Fresh sender: mint a one-time-use placeholder room id; the Room
      // will replace it with the generated token on hello.
      roomName = 'pending-' + crypto.randomUUID();
    }

    const id = env.ROOMS.idFromName(roomName);
    const stub = env.ROOMS.get(id);
    return stub.fetch(request);
  },
};
