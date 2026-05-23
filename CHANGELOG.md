# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). Entries are derived from [Conventional Commits](https://www.conventionalcommits.org/).

## [Unreleased]

### Added

- Initial plugin skeleton following the v0.5 concept.
- Token generator (`TTHR-XXXX-XXXX-XXXX`, 32-char alphabet without `0/O/1/I`).
- Signaling client over WebSocket against the Tether signaling service.
- Waiting-room admission model: pending → accepted/rejected, fingerprint pinning, revoke.
- WebRTC transport via `libdatachannel` (DTLS-SRTP, ICE, STUN + managed TURN).
- Multi-track audio routing — host microphone plus N additional OBS audio sources.
- Native OBS source type for the receiver (`tether_source`) with multi-track audio output.
- Sender output that publishes a selected OBS source plus selected audio tracks.
- Properties UI with mode select (Standard / Twitch Stream Together), source pick, audio pick, server URL override.
- Locales `en-US` (source) and `de-DE`.
- Backend: Cloudflare Workers + Durable Objects signaling service (`backend/`).
- Build matrix CI for Windows, macOS, Linux (from `obs-plugintemplate`).
- Community-health files: README, CONTRIBUTING, CODE_OF_CONDUCT, SECURITY, this CHANGELOG, issue and PR templates.

[Unreleased]: https://github.com/TetherOBSPlugin/Tether-OBS-Plugin/commits/main
