# Tilde Linux

Minimal Linux proof of Tilde's quiet inline writing suggestions.

The current milestone is intentionally small: an Fcitx5 input method that asks
a local Ollama model for an inline continuation after printable input. `Tab`
accepts the next word, backtick/tilde accepts the full phrase, and `Esc`
dismisses it.

## Current scope

- Fcitx5 input-method addon
- Client-side preedit where the application supports it
- Debounced, asynchronous local inference through Ollama
- Stale-response rejection by input-context UUID and revision
- Fixed proof suggestion as an immediate fallback
- User-local installation under `~/.local`

The default model is `qwen2.5-coder:1.5b-base`. Requests begin after a 120 ms
typing pause, generate at most 16 tokens, and keep the model loaded for 30
minutes. The proof fallback is ` — Tilde is working`. Each word accepted with
`Tab` includes one trailing space; the final word also adds a trailing space.

The current visual experiment renders the continuation with Fcitx's
`Bold`/active style, directly after the normal caret with no boundary marker.

For this English-layout proof, Tilde owns plain printable keys as a short IME
composition and commits them on acceptance, dismissal, or an editing boundary.
Dead-key and composed-layout handling is intentionally deferred until the input
boundary passes across the initial application matrix.

## Validation status

- Build and deterministic key-policy tests pass.
- Model request serialization, response parsing, output sanitization, and
  stale-response policy tests pass.
- Fcitx addon discovery and manual Qt preedit display have been demonstrated.
- Model-backed inline suggestions have been manually demonstrated in Omawrite
  with the model fully offloaded to an NVIDIA RTX 3090.
- Exact Tab acceptance is not yet proven end to end.
- The GUI runner requires `ydotool`; `wtype` bypasses the Fcitx path and is not
  a valid input-method test.
- The model currently sees only the active Tilde composition, not the full
  document or surrounding application context.

## Build and install

Requirements: Fcitx5 development files, CMake, Ninja, a C++17 compiler,
libcurl, JsonCpp, and a running Ollama service.

On Omarchy, install the CUDA runtime and pull the default model:

```bash
omarchy pkg add ollama-cuda
sudo systemctl enable --now ollama.service
ollama pull qwen2.5-coder:1.5b-base
```

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
before expanding document-context capture.

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

1. Fcitx5 owns key routing, inline preedit, and deterministic acceptance.
2. A single worker debounces typing and calls Ollama's localhost API without
   blocking the Fcitx event loop.
3. Input-context revisions prevent older responses from replacing newer text.
4. A future context service can add document/window context without changing
   the acceptance path.
5. An optional Omarchy shell plugin can expose model status and controls.

## License

MIT
