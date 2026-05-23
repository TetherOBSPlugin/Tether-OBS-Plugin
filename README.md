# Tether

[![License: GPL v2+](https://img.shields.io/badge/License-GPLv2%2B-blue.svg)](LICENSE)
[![OBS Studio](https://img.shields.io/badge/OBS%20Studio-%E2%89%A5%2031-purple)](https://obsproject.com)
[![Status](https://img.shields.io/badge/status-pre--release-orange)](#roadmap)

Share a single OBS source — plus your microphone — with another streamer over the public internet, with admission you control. No login, no public link, no own server.

```
You generate a short token  ──▶  Receiver pastes it
                                 ──▶  Lands in your waiting room
                                 ──▶  You accept (or reject) them
                                 ──▶  Source appears natively in their OBS
```

A WebRTC pipe between two OBS instances, gated by a token plus your explicit accept. Audio and video share one WebRTC media clock — lip-synced from the first frame, no manual offset.

## Why?

NDI works on the LAN but doesn't traverse NAT. NDI Bridge does — if you have a public IP and an open port. VDO.Ninja gives a link, but anyone with the link is in. Stream Together is tied to one platform.

Tether sits one level earlier: it joins two OBS instances on the *production* layer. The shared source becomes part of the receiver's scene. Where the scene is then streamed — Twitch, YouTube, Kick, Restream, self-hosted RTMP/SRT/WHIP — is none of Tether's business.

## Features

- **Token-gated admission.** Receiver gets a short opaque token, lands in a waiting room, no media flows until the sender accepts. A leaked token only rings the doorbell.
- **Native OBS source.** Not a browser source — a real source type, scalable, filterable, with routable multi-track audio.
- **A/V sync for free.** Single WebRTC session, single media clock — RTP/RTCP keeps audio and video locked.
- **Platform-neutral.** Tether ends at the OBS scene. Output destination is whatever OBS streams to.
- **No own server required.** Default signaling runs on Cloudflare Workers + Durable Objects with a free `*.workers.dev` subdomain. TURN via managed provider, only used as a CGNAT fallback.
- **Self-host option.** Same protocol, replace the default endpoints with your own coturn + tiny signaling service.

## How does it work?

1. **Sender** picks a video source and audio tracks, generates a token.
2. **Receiver** pastes the token into their Tether source.
3. **Signaling** (Cloudflare Workers + Durable Objects) validates the token and puts the receiver in a waiting room.
4. **Sender** sees the request with its DTLS fingerprint, accepts or rejects.
5. On accept, SDP/ICE flow, then DTLS-SRTP encrypted media. Direct P2P where possible, TURN fallback otherwise.

The token is a doorbell, not a key. Even brute-forced or leaked tokens land at the waiting room — rate-limited, locked out, never automatically connected.

## Build

Requires CMake ≥ 3.28, an OBS Studio source tree, and `libdatachannel`. The project follows the standard [`obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate) layout — see [CONTRIBUTING.md](CONTRIBUTING.md) for full setup.

```sh
cmake --preset linux
cmake --build --preset linux
```

Other presets: `macos`, `windows-x64`. CI builds installers for all three on tagged releases.

## Backend (signaling)

The `backend/` directory contains the Cloudflare Workers signaling service (TypeScript, Durable Objects). It is deployable to any Workers account; the free tier covers the load. See [backend/README.md](backend/README.md).

For self-hosters, the backend protocol is documented enough to reimplement on top of any small WebSocket service. coturn handles TURN.

## Roadmap

- **MVP** — Sender + receiver, token + accept flow, managed backend.
- **v1** — Multi-track audio, fingerprint pinning, revoke, rate-limit/lockout, OBS plugin directory listing.
- **v2** — Self-host package (coturn + signaling), optional Twitch Stream Together mode, local co-stream recording.

## License

[GPL-2.0-or-later](LICENSE). Tether links against `libobs`, which is GPLv2+, so Tether inherits the license. Source code remains available.

## Security

Token rate limits, lockout, accept-per-receiver, DTLS-SRTP for media, fingerprint pinning for re-connects. To report a vulnerability, see [SECURITY.md](SECURITY.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Issues and PRs welcome. Be kind — [Code of Conduct](CODE_OF_CONDUCT.md).
