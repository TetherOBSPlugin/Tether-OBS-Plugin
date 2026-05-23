# Tether Signaling Backend

Cloudflare Workers + Durable Objects. One Durable Object per active sharing session, keyed by token. Free-tier covers signaling for many concurrent sessions.

## Local development

```sh
npm install
npm run dev
```

Wrangler starts a local dev server, normally on `http://127.0.0.1:8787`. The WebSocket endpoint is `ws://127.0.0.1:8787/v1`.

## Deploy

```sh
wrangler login        # one-time, opens browser
npm run deploy
```

Deployed URL: `https://tether-signaling.<account>.workers.dev/v1`. Drop the same URL into the plugin's *Signaling server* setting (or rely on the compile-time default if you fork the plugin).

## Protocol

Line-delimited JSON over a single WebSocket. See `src/room.ts` for the message reference. Short version:

### Client → server

| `type`     | Notes                                                                  |
| ---------- | ---------------------------------------------------------------------- |
| `hello`    | Identifies the connection. `role: "sender"` or `"receiver"`, plus name, token (receiver), ttl_minutes/reusable (sender). |
| `accept`   | Sender accepts a pending receiver. `peer_id` from the `pending` event. |
| `reject`   | Sender rejects a pending receiver.                                     |
| `revoke`   | Sender revokes the active token; all receivers are kicked.             |
| `sdp`      | SDP offer/answer to forward to the peer. Includes `peer_id`.           |
| `ice`      | ICE candidate to forward. Includes `peer_id`.                          |

### Server → client

| `type`             | Notes                                                          |
| ------------------ | -------------------------------------------------------------- |
| `token`            | Sender-only: the newly minted token to share out-of-band.      |
| `pending`          | Sender-only: a receiver knocked. `peer_id`, name, fingerprint. |
| `accepted`         | Receiver-only: sender accepted you, SDP/ICE may flow now.      |
| `rejected`         | Receiver-only: sender rejected you.                            |
| `token_invalid`    | Receiver-only: bad or expired token.                           |
| `token_locked_out` | Receiver-only: rate limit hit. Includes `retry_after`.         |
| `peer_gone`        | Either side: counterpart disconnected.                         |
| `sdp` / `ice`      | Relayed payloads from the other side.                          |

## Configuration

`wrangler.toml` exposes:

- `MAX_TOKEN_TTL_MINUTES` — upper bound on the sender-requested TTL.
- `RATE_LIMIT_WINDOW_SECONDS` / `RATE_LIMIT_MAX_ATTEMPTS` — sliding window for failed receiver hellos.
- `LOCKOUT_SECONDS` — how long a token is locked out after exceeding the rate limit.

## Self-hosting

Any small WebSocket service that implements the protocol above works. The Cloudflare deployment is the convenience default — for full data sovereignty, run the protocol on your own infrastructure (e.g. a small VM with `ws` or `actix-web`). The plugin treats the signaling URL as opaque; replace it.
