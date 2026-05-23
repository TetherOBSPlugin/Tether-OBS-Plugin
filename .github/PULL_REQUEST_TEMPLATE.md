<!--
Thanks for contributing!

Title format: Conventional Commits — feat:, fix:, refactor:, docs:, chore:, test:, build:, ci:
Subject ≤ 72 chars, imperative, English. Bundle one logical change per PR.
-->

## Summary

<!-- What does this PR change and why? -->

## Linked issues

Fixes #

## Test plan

- [ ] Builds on Linux (`cmake --preset linux && cmake --build --preset linux`)
- [ ] Builds on macOS (`cmake --preset macos && cmake --build --preset macos`)
- [ ] Builds on Windows (`cmake --preset windows-x64 && cmake --build --preset windows-x64`)
- [ ] `./build-aux/.run-format.zsh` passes
- [ ] Manual: source connects sender → receiver, token + accept flow works end-to-end
- [ ] If touching i18n: `data/locale/en-US.ini` and `data/locale/de-DE.ini` both updated

## Checklist

- [ ] Code is C, English-only identifiers and comments
- [ ] No hard-coded user-facing strings (use `obs_module_text("Key")`)
- [ ] `*_create` / `*_release` paired; `bzalloc` / `bfree` used
- [ ] Network and WebRTC work off the graphics thread; shared state is locked
- [ ] Errors are handled explicitly, no silent failures
- [ ] No secrets, tokens or keys in the diff
