# Contributing to Tether

Thanks for considering a contribution. This document covers project setup, the coding style we follow, and how to get a change reviewed and merged.

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). By participating, you agree to uphold it.

## Project layout

```
tether/
├── src/                 C source — one responsibility per file
├── data/locale/         i18n .ini files (PascalCase keys)
├── backend/             Cloudflare Workers signaling service (TypeScript)
├── cmake/               Build helpers from obs-plugintemplate
├── build-aux/           Format helpers from obs-plugintemplate
├── .github/             CI workflows, issue and PR templates
├── CMakeLists.txt       Top-level CMake
├── CMakePresets.json    Presets per platform
└── buildspec.json       Plugin metadata + dependency hashes
```

## Building the plugin

You need:

- CMake ≥ 3.28
- A C compiler (`clang`/`gcc` on Linux/macOS, MSVC on Windows)
- An OBS Studio source tree (CMake will fetch a pinned version via `buildspec.json` on first configure)
- `libdatachannel` ≥ 0.21 — system package or vendored submodule
- Qt 6 (only required if `ENABLE_QT=ON`; the default UI uses OBS properties API only)

Linux:

```sh
cmake --preset linux
cmake --build --preset linux
```

macOS:

```sh
cmake --preset macos
cmake --build --preset macos
```

Windows:

```sh
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Building the backend

```sh
cd backend
npm install
npm run dev       # local Wrangler dev server
npm run deploy    # deploy to your Workers account
```

## Coding style

We follow the OBS project style — applied automatically via `.clang-format` (do not modify it). CI enforces it.

Format your changes before pushing:

```sh
./build-aux/.run-format.zsh
```

Conventions in `src/`:

- **One responsibility per file**, soft limit ~300–400 lines.
- `snake_case` for everything; public symbols prefixed with `tether_`.
- Allocations via `bzalloc` / `bfree`. Every `*_create` paired with `*_release`.
- Never block the OBS graphics/render thread. Network and WebRTC run on their own threads; shared state is locked.
- Logging exclusively through `obs_log()` / `blog()`. Each line prefixed `[tether]` (the prefix is added by the logging wrapper, you don't write it manually).
- No hard-coded user-facing strings. Use `obs_module_text("Key")`. Keys are `PascalCase` with optional dot hierarchy: `Source.Name`, `Settings.Token`, `Admission.Request.Accept`.
- Comments explain *why*, not *what*. Don't restate code.
- Error paths are explicit. No silent `return;` on failure.

Conventions in `backend/`:

- TypeScript strict mode. No `any` unless wrapping an untyped Web API.
- ESLint + Prettier (config in repo). CI enforces.

## Localization

Default locale is `en-US`. `de-DE` is required at every release. Other locales welcome — copy `data/locale/en-US.ini` to `data/locale/<your-locale>.ini` and translate values. Don't change the keys.

## Commits and PRs

- [Conventional Commits](https://www.conventionalcommits.org/): `feat:`, `fix:`, `refactor:`, `docs:`, `chore:`, `test:`, `build:`, `ci:`.
- Subject ≤ 72 chars, imperative mood, English.
- One PR = one logical change. Splitting a refactor across two PRs is fine; bundling two unrelated changes in one is not.
- Reference issues with `Fixes #N` in the PR body.
- Be patient — every PR is reviewed by a maintainer, the bar is "ships safely on three platforms".

## Versioning and releases

[Semantic Versioning](https://semver.org). A tagged release `vX.Y.Z` triggers CI installer builds for all three platforms. Conventional Commits feed the [CHANGELOG](CHANGELOG.md) — `feat:` bumps minor, `fix:` bumps patch, anything with `BREAKING CHANGE:` in the footer bumps major.

## Security

If you find a vulnerability — please **do not** open a public issue. See [SECURITY.md](SECURITY.md) for the reporting channel.
