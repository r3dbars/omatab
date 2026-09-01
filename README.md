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
- Cursor-aware surrounding-text context with bounded UTF-8 prefix and suffix
- Throttled, in-memory OCR of the active window for visible app context
- Stale-response rejection by input-context UUID and revision
- No placeholder suggestion while inference is pending or unavailable
- User-local installation under `~/.local`

The fast completion model is `qwen2.5-coder:1.5b-base`. When visible OCR
context is available, Tilde routes through `qwen2.5:1.5b` to fuse that context
with the exact text before and after the caret. Both paths use raw continuation
prompts rather than chat framing so the model completes the user's writing
instead of replying to it. Requests begin after a 120 ms
typing pause, generate at most 16 tokens, and keep models loaded for 30 minutes.
Each word accepted with `Tab` includes one trailing space; the final word also
adds a trailing space. Pending, failed, timed-out, and empty model requests show
no suggestion, leaving native `Tab` behavior intact.

Set `TILDE_MODEL` and `TILDE_CONTEXT_MODEL` in the Fcitx5 service environment
to test alternate completion and OCR-context models without rebuilding. Both
paths use an 8K context window. The main tuning controls are
`TILDE_DEBOUNCE_MS` (default `120`), `TILDE_NUM_PREDICT` (`16`),
`TILDE_TEMPERATURE` (`0.2`), and `TILDE_TOP_P` (`0.9`). Operational controls
are `TILDE_NUM_CTX` (`8192`) and `TILDE_TIMEOUT_MS` (`2500`). Invalid or
out-of-range values safely fall back to these defaults.

Set `TILDE_KEEP_ALIVE=-1` to keep a model resident indefinitely. The optional
`tilde-model-warm.timer` preloads the configured model and refreshes that lease
every minute, including after Ollama restarts. Enable it with:

```bash
systemctl --user enable --now tilde-model-warm.timer
```

Set `TILDE_FIM=1` for models with native `<|fim_prefix|>`, `<|fim_suffix|>`,
and `<|fim_middle|>` tokens. This lets Tilde complete a partial current word
and fill text at the exact caret while conditioning on text after it. The live
Qwen3.5 9B Base setup supports this mode.

## Private telemetry

Set `TILDE_LOG_PATH` to enable a local JSONL flight recorder. Each record is
written with mode `0600`; the parent directory should be mode `0700`. Logs
include full textbox/OCR context, exact model requests and responses, latency,
errors, and suggestion outcomes including word/full acceptance, dismissal,
typed-over, stale, cleared, and reset states. Sensitive input contexts and
blocked password-manager windows remain excluded by Tilde's existing safety
gate. The log rotates to `.1` at 50 MiB by default; override the byte limit with
`TILDE_LOG_MAX_BYTES` (1 MiB to 1 GiB).

Because these records can contain private writing, they stay local and must not
be committed or uploaded. Generate an agent-readable quality summary with:

```bash
./scripts/telemetry-report.sh
```

The current visual experiment renders the continuation with Fcitx's
`Bold`/active style, directly after the normal caret with no boundary marker.

## Local controls

The user installation includes a small JSON-capable control command for shell
plugins and diagnostics:

```bash
tilde-control status --json
tilde-control enable
tilde-control disable
tilde-control warm
tilde-control restart
tilde-control doctor
```

The companion Omarchy plugin uses this interface instead of editing Tilde's
files directly. Enable and disable are instantaneous and global: a private
marker under `~/.local/state/tilde/` tells the running addon to pass keys
through without requesting suggestions. Selecting Tilde in Fcitx remains a
separate input-method choice.

For this English-layout proof, Tilde commits plain printable keys immediately
and keeps only its unaccepted continuation in IME preedit. This prevents
toolkits without formatted-preedit support from styling the user's real text as
an unfinished composition. Dead-key and composed-layout handling is
intentionally deferred until the input boundary passes across the initial
application matrix.

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
- Apps exposing Fcitx surrounding text provide up to 4 KiB before the caret and
  1 KiB after it. Other apps use recent Tilde-tracked text as a fallback.
- Active-window OCR is cached for two seconds and capped at 4 KiB. It sees only
  rendered content, not hidden/scrolled-off document content or browser DOM.
- Password/sensitive input contexts and known password-manager windows never
  trigger predictions or OCR.

## Build and install

Requirements: Fcitx5 development files, CMake, Ninja, a C++17 compiler,
libcurl, JsonCpp, and a running Ollama service.

On Omarchy, install the CUDA runtime and pull the default model:

```bash
omarchy pkg add ollama-cuda
sudo systemctl enable --now ollama.service
ollama pull qwen2.5-coder:1.5b-base
ollama pull qwen2.5:1.5b
```

```bash
./scripts/install-omarchy-user.sh
```

The Omarchy installer keeps the addon rootless under `~/.local` and adds that
directory to the user Fcitx5 service's addon search path. Add **Tilde Linux
Proof** to the active input-method group and select it. Type in a disposable
document. Press `Tab` only while the suggestion is visible; press `Esc` to
dismiss it.

For a system-wide test, copy `packaging/fcitx5-global.conf` to
`~/.config/fcitx5/config` and restart Fcitx. This shares one input-method state
across compatible applications: `Ctrl+Space` toggles between Tilde and the
normal keyboard. Password fields remain excluded.

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
2. A single worker debounces typing, captures throttled active-window OCR in
   memory, and calls Ollama's localhost API without blocking the Fcitx event
   loop.
3. Input-context revisions prevent older responses from replacing newer text.
4. A future context service can add document/window context without changing
   the acceptance path.
5. An optional Omarchy shell plugin can expose model status and controls.

## License

MIT
