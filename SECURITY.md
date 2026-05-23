# Security Policy

## Reporting a vulnerability

Please report security vulnerabilities **privately**. Do not open a public GitHub issue, do not discuss the issue in a pull request, and do not post about it on social media until a fix is available.

Two ways to report:

1. **GitHub Security Advisories** (preferred) — open a draft advisory at <https://github.com/TetherOBSPlugin/Tether-OBS-Plugin/security/advisories/new>.
2. **Email** — `tether@example.invalid`. PGP not required. Encrypt if you must; we will reply within five working days regardless.

Include in the report:

- A description of the issue, ideally with proof-of-concept code or steps to reproduce.
- The version of the plugin, OBS Studio, and the OS you tested on.
- An assessment of the impact (information disclosure, RCE, denial of service, admission bypass, etc.).

We will acknowledge receipt within **five working days** and aim to ship a fix or a public mitigation within **90 days**. If we cannot meet that window we will tell you why and agree on an extended embargo with you.

## Supported versions

Tether is pre-1.0. Only the latest tagged release on the `main` branch is supported with security fixes. Once 1.0 is reached this policy will be updated to cover at least the latest minor release line.

## Scope

In scope:

- The OBS plugin source under `src/`.
- The signaling backend under `backend/`.
- Build, packaging, and CI configuration that could ship a compromised plugin to users.

Out of scope:

- Third-party services we depend on (Cloudflare Workers, Cloudflare Realtime TURN, public STUN servers). Report those to the respective vendors.
- `libdatachannel` and other upstream libraries. Report upstream; we will pick up the fix once released.
- Issues that require physical access to the user's machine or a pre-compromised OBS install.

## Threat model — what Tether actually defends against

A short, honest summary so reports can target real attack surface:

- **Token leak / brute force.** Tokens are doorbells, not keys. Even a guessed or leaked token only lands the attacker in the waiting room. Media flow requires explicit accept by the sender. Rate-limit and lockout in the signaling layer make online brute force impractical; offline guessing has no oracle. Report findings that bypass this gate.
- **Session hijack after accept.** Accept is bound to the receiver's DTLS fingerprint. A different peer presenting the same token after accept must not get media. Report deviations.
- **Signaling integrity.** Signaling runs over TLS. The signaling server sees room IDs, presence, and SDP/ICE metadata, but not media. Media is end-to-end DTLS-SRTP between sender and receiver. Report any path where the signaling operator could read or inject media.
- **TURN relay.** The TURN relay sees only encrypted bytes. Report any configuration in which Tether would negotiate an unencrypted transport.
- **OBS process safety.** The plugin runs in the OBS process. Memory-safety bugs in our code (UAF, OOB, double-free) are in scope. So is any path where untrusted network input drives unbounded allocation or blocks a graphics thread.

## What we will not treat as vulnerabilities

- Self-DoS (a sender who deliberately accepts a malicious receiver).
- The fact that signaling metadata (IPs in ICE candidates, connection times) transits the managed signaling provider — this is documented in the README and CONTRIBUTING.
- Issues that require the user to disable DTLS or to use a custom signaling endpoint that does not implement the documented protocol.

## Coordinated disclosure

Once a fix lands, we will:

1. Publish a GitHub Security Advisory with a CVE if applicable.
2. Credit the reporter in the advisory and the `CHANGELOG.md` unless they ask to remain anonymous.
3. Tag a patch release immediately after the embargo ends.
