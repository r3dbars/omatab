# Tilde Linux

Minimal Linux proof of Tilde's quiet inline writing suggestions.

The first milestone is intentionally small: an Fcitx5 input method that shows
a fixed inline suggestion after printable input. `Tab` accepts the next word,
backtick/tilde accepts the full phrase, and `Esc` dismisses it. This proves the
input boundary before any
model runtime, history, screen capture, or settings UI is added.

## Current scope

- Fcitx5 input-method addon
- Client-side preedit where the application supports it
- Fixed, non-model-generated proof suggestion
- User-local installation under `~/.local`

The proof suggestion is ` — Tilde is working`. Each word accepted with `Tab`
includes one trailing space; the final word also adds a trailing space.

The current visual experiment renders the continuation with Fcitx's
`Bold`/active style, directly after the normal caret with no boundary marker.

For this English-layout proof, Tilde owns plain printable keys as a short IME
composition and commits them on acceptance, dismissal, or an editing boundary.
Dead-key and composed-layout handling is intentionally deferred until the input
boundary passes across the initial application matrix.

## Validation status

- Build and deterministic key-policy tests pass.
- Fcitx addon discovery and manual Qt preedit display have been demonstrated.
- Exact Tab acceptance is not yet proven end to end.
- The GUI runner requires `ydotool`; `wtype` bypasses the Fcitx path and is not
  a valid input-method test.
- No model runtime is connected yet.

## Build and install

Requirements: Fcitx5 development files, CMake, Ninja, and a C++17 compiler.

```bash
./scripts/install-omarchy-user.sh
```

The Omarchy installer keeps the addon rootless under `~/.local` and adds that
directory to the user Fcitx5 service's addon search path. Add **Tilde Linux
Proof** to the active input-method group and select it. Type in a disposable
document. Press `Tab` only while the suggestion is visible; press `Esc` to
dismiss it.

## Safety gate

This proof must not duplicate or lose committed text, consume `Tab` or `Esc`
without a visible suggestion, break shortcuts, or leave stale preedit in a new
field. It should be tested in a native GTK/Qt editor, Chromium, and Ghostty
before connecting a local model.

## Test from the start

```bash
./scripts/test.sh
./scripts/test-gui.sh
```

The proof has deterministic keystroke-policy tests. The committed test design
also reserves separate lanes for Fcitx integration, human-style GUI automation,
local-model latency/output checks, and GPT quality review. See
[`docs/testing.md`](docs/testing.md).

## Planned architecture

1. Fcitx5 engine owns key routing and inline preedit.
2. A user-local daemon owns suggestion policy and llama.cpp inference.
3. The two communicate over an owner-only Unix socket.
4. An optional Omarchy shell plugin exposes status and controls.

## License

MIT
