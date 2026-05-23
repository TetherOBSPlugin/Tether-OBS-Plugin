// SPDX-FileCopyrightText: 2026 Tether contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A Room is one active sharing session: one sender and zero-or-more
// receivers connected over the same token.
//
// Lifecycle:
//   1. Sender connects (no token query). Room generates a token, stores it
//      under DO storage, sends `{ type: "token", token: "TTHR-..." }`.
//   2. Sender's connection is the authoritative one for this Room.
//      Subsequent receiver connections use the same token in the query.
//   3. Receivers connect → server adds them to the pending list and forwards
//      `{ type: "pending", ... }` to the sender.
//   4. Sender sends `{ type: "accept", peer_id }`. Server replies to receiver
//      `{ type: "accepted" }` and starts relaying SDP/ICE between the two.
//   5. Rate-limit: per-token attempts within a sliding window. Lockout if
//      exceeded.

interface Env {
  MAX_TOKEN_TTL_MINUTES: string;
  RATE_LIMIT_WINDOW_SECONDS: string;
  RATE_LIMIT_MAX_ATTEMPTS: string;
  LOCKOUT_SECONDS: string;
}

type Role = 'sender' | 'receiver';

interface PeerConn {
  id: string;
  role: Role;
  name: string;
  fingerprint?: string;
  ws: WebSocket;
  accepted: boolean;
  pinned: boolean;
}

interface HelloMsg {
  type: 'hello';
  role: Role;
  name?: string;
  token?: string;
  ttl_minutes?: number;
  reusable?: boolean;
  fingerprint?: string;
}

interface SignalMsg {
  type: string;
  peer_id?: string;
  sdp_type?: string;
  sdp?: string;
  candidate?: string;
  mid?: string;
  mline?: number;
}

const ALPHABET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';

export function generateToken(): string {
  // 12 chars × 5 bits ≈ 60 bits entropy.
  const groups: string[] = [];
  for (let g = 0; g < 3; g++) {
    let s = '';
    const buf = new Uint8Array(4);
    crypto.getRandomValues(buf);
    for (let i = 0; i < 4; i++) {
      s += ALPHABET[buf[i] % ALPHABET.length];
    }
    groups.push(s);
  }
  return 'TTHR-' + groups.join('-');
}

export class Room {
  private state: DurableObjectState;
  private env: Env;
  private token: string | null = null;
  private expiresAt: number = 0;
  private reusable: boolean = false;
  private peers: Map<string, PeerConn> = new Map();
  private attempts: number[] = [];
  private lockedOutUntil: number = 0;

  constructor(state: DurableObjectState, env: Env) {
    this.state = state;
    this.env = env;
    this.state.blockConcurrencyWhile(async () => {
      this.token = (await state.storage.get<string>('token')) ?? null;
      this.expiresAt = (await state.storage.get<number>('expiresAt')) ?? 0;
      this.reusable = (await state.storage.get<boolean>('reusable')) ?? false;
    });
  }

  async fetch(request: Request): Promise<Response> {
    const pair = new WebSocketPair();
    const [client, server] = [pair[0], pair[1]];
    server.accept();

    const url = new URL(request.url);
    const tokenParam = url.searchParams.get('token');
    // The edge in index.ts has already minted the token for fresh senders and
    // routed both sender + receiver to this DO under the same name. Adopt that
    // name as our token whenever we see it — the DO name IS the token, so the
    // header is authoritative. Persist immediately so a later cold-load can
    // skip the "first sender hello" path entirely.
    const roomHint = request.headers.get('X-Tether-Room');
    if (roomHint && this.token !== roomHint) {
      this.token = roomHint;
      await this.state.storage.put('token', roomHint);
    }

    server.addEventListener('message', (event) => {
      this.onMessage(server, event, tokenParam);
    });
    server.addEventListener('close', () => {
      this.onClose(server);
    });
    server.addEventListener('error', () => {
      this.onClose(server);
    });

    return new Response(null, { status: 101, webSocket: client });
  }

  private onMessage(ws: WebSocket, event: MessageEvent, tokenParam: string | null): void {
    let msg: SignalMsg | HelloMsg;
    try {
      msg = JSON.parse(typeof event.data === 'string' ? event.data : '');
    } catch {
      return;
    }

    if (msg.type === 'hello') {
      this.handleHello(ws, msg as HelloMsg, tokenParam);
      return;
    }

    const peer = this.findPeerByWs(ws);
    if (!peer) {
      return;
    }

    switch (msg.type) {
      case 'accept':
        this.handleAccept(peer, msg as SignalMsg);
        break;
      case 'reject':
        this.handleReject(peer, msg as SignalMsg);
        break;
      case 'revoke':
        this.handleRevoke(peer);
        break;
      case 'sdp':
      case 'ice':
        this.relayToPeer(peer, msg as SignalMsg);
        break;
    }
  }

  private async handleHello(
    ws: WebSocket,
    msg: HelloMsg,
    tokenParam: string | null,
  ): Promise<void> {
    if (msg.role === 'sender') {
      // The token is the DO's own name, set in fetch() from X-Tether-Room.
      // Persist TTL / reusable on first sender hello so receivers landing on
      // the same DO later see the token as alive.
      if (!this.expiresAt) {
        const ttl = Math.min(msg.ttl_minutes ?? 30, Number(this.env.MAX_TOKEN_TTL_MINUTES));
        this.expiresAt = Date.now() + ttl * 60_000;
        this.reusable = !!msg.reusable;
        if (this.token) {
          await this.state.storage.put('token', this.token);
        }
        await this.state.storage.put('expiresAt', this.expiresAt);
        await this.state.storage.put('reusable', this.reusable);
      }

      const senderId = crypto.randomUUID();
      this.peers.set(senderId, {
        id: senderId,
        role: 'sender',
        name: msg.name ?? '',
        ws,
        accepted: true,
        pinned: false,
      });

      ws.send(JSON.stringify({ type: 'token', token: this.token }));
      return;
    }

    // Receiver hello — validate token.
    const claimed = (msg.token ?? tokenParam ?? '').toUpperCase();
    if (!this.token || claimed !== this.token) {
      this.recordAttempt();
      ws.send(JSON.stringify({ type: 'token_invalid' }));
      ws.close(1008, 'invalid token');
      return;
    }
    if (Date.now() > this.expiresAt) {
      ws.send(JSON.stringify({ type: 'token_invalid' }));
      ws.close(1008, 'expired');
      return;
    }
    if (Date.now() < this.lockedOutUntil) {
      const retry = Math.ceil((this.lockedOutUntil - Date.now()) / 1000);
      ws.send(JSON.stringify({ type: 'token_locked_out', retry_after: retry }));
      ws.close(1013, 'locked out');
      return;
    }

    const recvId = crypto.randomUUID();
    this.peers.set(recvId, {
      id: recvId,
      role: 'receiver',
      name: msg.name ?? '',
      fingerprint: msg.fingerprint,
      ws,
      accepted: false,
      pinned: false,
    });

    // Notify the sender.
    const sender = this.findSender();
    if (sender) {
      sender.ws.send(
        JSON.stringify({
          type: 'pending',
          peer_id: recvId,
          name: msg.name ?? '',
          fingerprint: msg.fingerprint ?? '',
        }),
      );
    }
  }

  private handleAccept(sender: PeerConn, msg: SignalMsg): void {
    if (sender.role !== 'sender' || !msg.peer_id) return;
    const r = this.peers.get(msg.peer_id);
    if (!r || r.role !== 'receiver') return;
    r.accepted = true;
    r.ws.send(JSON.stringify({ type: 'accepted' }));
  }

  private handleReject(sender: PeerConn, msg: SignalMsg): void {
    if (sender.role !== 'sender' || !msg.peer_id) return;
    const r = this.peers.get(msg.peer_id);
    if (!r) return;
    r.ws.send(JSON.stringify({ type: 'rejected' }));
    r.ws.close(1000, 'rejected');
    this.peers.delete(r.id);
  }

  private async handleRevoke(sender: PeerConn): Promise<void> {
    if (sender.role !== 'sender') return;
    this.token = null;
    this.expiresAt = 0;
    await this.state.storage.delete('token');
    await this.state.storage.delete('expiresAt');
    // Disconnect all receivers.
    for (const p of this.peers.values()) {
      if (p.role === 'receiver') {
        p.ws.send(JSON.stringify({ type: 'rejected' }));
        p.ws.close(1000, 'revoked');
        this.peers.delete(p.id);
      }
    }
  }

  private relayToPeer(from: PeerConn, msg: SignalMsg): void {
    if (!msg.peer_id) {
      // Sender→receiver relay uses peer_id; receiver→sender uses the
      // sender's implicit id (we look it up).
      if (from.role === 'receiver') {
        const sender = this.findSender();
        if (sender) sender.ws.send(JSON.stringify({ ...msg, peer_id: from.id }));
      }
      return;
    }
    const target = this.peers.get(msg.peer_id);
    if (!target) return;
    // Receivers must be accepted before any SDP/ICE goes through them.
    if (from.role === 'sender' && !target.accepted) return;
    if (from.role === 'receiver' && !from.accepted) return;

    // Rewrite peer_id to the from-peer's id when going to the sender so the
    // sender knows who the message came from.
    const out = { ...msg };
    if (target.role === 'sender') {
      out.peer_id = from.id;
    }
    target.ws.send(JSON.stringify(out));
  }

  private onClose(ws: WebSocket): void {
    const peer = this.findPeerByWs(ws);
    if (!peer) return;
    this.peers.delete(peer.id);

    if (peer.role === 'receiver') {
      const sender = this.findSender();
      if (sender) {
        sender.ws.send(JSON.stringify({ type: 'peer_gone', peer_id: peer.id }));
      }
    } else if (peer.role === 'sender') {
      // Sender went away. Close all receivers if the token is not reusable.
      if (!this.reusable) {
        for (const p of this.peers.values()) {
          p.ws.close(1001, 'sender gone');
        }
        this.peers.clear();
      }
    }
  }

  private recordAttempt(): void {
    const now = Date.now();
    const window = Number(this.env.RATE_LIMIT_WINDOW_SECONDS) * 1000;
    this.attempts = this.attempts.filter((t) => now - t < window);
    this.attempts.push(now);
    if (this.attempts.length >= Number(this.env.RATE_LIMIT_MAX_ATTEMPTS)) {
      this.lockedOutUntil = now + Number(this.env.LOCKOUT_SECONDS) * 1000;
      this.attempts = [];
    }
  }

  private findSender(): PeerConn | null {
    for (const p of this.peers.values()) {
      if (p.role === 'sender') return p;
    }
    return null;
  }

  private findPeerByWs(ws: WebSocket): PeerConn | null {
    for (const p of this.peers.values()) {
      if (p.ws === ws) return p;
    }
    return null;
  }
}
