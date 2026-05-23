// SPDX-FileCopyrightText: 2026 Tether contributors
// SPDX-License-Identifier: GPL-2.0-or-later

import { Room, generateToken } from './room';
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
const TOKEN_REGEX =
  /^TTHR-[A-HJKMNPQRSTUVWXYZ2-9]{4}-[A-HJKMNPQRSTUVWXYZ2-9]{4}-[A-HJKMNPQRSTUVWXYZ2-9]{4}$/;

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    // Health check — useful for monitoring.
    if (url.pathname === '/health') {
      return new Response('ok', { status: 200 });
    }

    if (url.pathname !== '/v1') {
      return new Response('not found', { status: 404 });
    }

    const upgrade = request.headers.get('Upgrade');
    if (upgrade !== 'websocket') {
      return new Response('expected websocket', { status: 426 });
    }

    // Routing: the DO id IS the token. A receiver knows the token and supplies
    // it via ?token=. A fresh sender connects without one, so we mint the
    // token here at the edge and route to the matching DO. We pass the minted
    // token through to the DO via the X-Tether-Room header so the DO doesn't
    // have to mint its own (and so sender + receiver always end up in the
    // same DO).
    const tokenParam = url.searchParams.get('token');
    let roomName: string;
    if (tokenParam) {
      const normalised = tokenParam.toUpperCase();
      if (!TOKEN_REGEX.test(normalised)) {
        return new Response('invalid token', { status: 400 });
      }
      roomName = normalised;
    } else {
      roomName = generateToken();
    }

    const id = env.ROOMS.idFromName(roomName);
    const stub = env.ROOMS.get(id);
    const forwarded = new Request(request.url, request);
    forwarded.headers.set('X-Tether-Room', roomName);
    return stub.fetch(forwarded);
  },
};
