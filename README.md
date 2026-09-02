# Oma Tab

Quiet inline writing suggestions for Linux, powered by a local model. Type,
pause, and a short continuation appears after your caret. `Tab` accepts one
word, `Shift+Tab` accepts the whole suggestion, `Esc` dismisses it. Nothing
leaves your machine.

Oma Tab is the Linux port of [Tilde](https://github.com/r3dbars/tilde), built
as an Fcitx5 input method and tuned for Omarchy.

## How it works

- An Fcitx5 input-method addon owns key routing, inline preedit, and
  deterministic acceptance.
- A worker thread debounces typing and asks a local Ollama model for a
  continuation after a 120 ms pause. A request still running when the next
  keystroke arrives is aborted immediately; Ollama stops generating when the
  connection closes, so the GPU moves straight to the newest text.
- Suggestions are trimmed to the rest of the current clause: they stop after
  sentence-ending punctuation, or after a comma, semicolon, colon, or em dash
  once at least one word has been offered. A shorter suggestion is a smaller
  commitment to accept and easier to judge at a glance.
- Cursor-aware surrounding text gives the model up to 4 KiB before the caret
  and 1 KiB after it in apps that expose it. Other apps use recently typed
  text as a fallback.
- Optional screen-text context reads the active window with OCR on a
  background thread and never delays a suggestion. See Settings.
- Password and sensitive input contexts, and known password-manager windows,
  never trigger predictions or capture.

The default model is the balanced profile, `Qwen3.5-4B-Base` at Q8_0 through
Ollama (about 4.3 GB). It uses native fill-in-the-middle tokens, so Oma Tab
can complete a partial word and condition on text after the caret. Faster and
smarter Qwen profiles and Gemma profiles are available through the model
picker.

## Install on Omarchy

The easy way is the bar widget. Add the
[Oma Tab plugin](https://github.com/r3dbars/omarchy-omatab), click the `⇥`
in the bar, and press **Install Oma Tab**. It opens a terminal you can watch.

The same thing from a shell:

```bash
git clone https://github.com/r3dbars/omatab ~/.local/src/omatab
~/.local/src/omatab/scripts/bootstrap.sh
```

The bootstrap installs the packages it needs (Fcitx5, CMake, Ninja, a C++
compiler, libcurl, JsonCpp, jq, Tesseract, grim, and the Ollama build that
matches your GPU), starts Ollama, builds and installs under `~/.local`, adds
that directory to the user Fcitx5 service's addon search path, selects Oma
Tab as the input method, and downloads a model sized to your GPU: the
balanced 4B model with 7 GB or more of video memory, the fast 2B model
otherwise. Pass `--model ID` to choose yourself. It writes its progress to
`~/.local/state/omatab/setup.json` so the bar widget can show it, and it is
safe to rerun; `omatab update` pulls the latest source and reruns it.

It also installs a user-level D-Bus service file that points Fcitx
activation at Omarchy's Fcitx unit. Without it, any application asking for
Fcitx during a restart makes D-Bus launch a bare fcitx5 that lacks the addon
path, holds the bus name, and saves a profile without Oma Tab.

`omatab doctor` confirms everything is working.

For a system-wide setup, copy `packaging/fcitx5-global.conf` to
`~/.config/fcitx5/config` and restart Fcitx. `Ctrl+Space` then toggles between
Oma Tab and the plain keyboard in every compatible application.

Upgrading from the early `tilde` proof install: run
`./scripts/migrate-from-tilde.sh` once. It keeps your model choice and
telemetry setting, moves the state directory, and leaves a `tilde-control`
shim for older shell plugins.

## Command line

Everything is controlled through one command. Every reporting command accepts
`--json`, every command exits non-zero on failure, and `omatab commands
--json` lists them all, so agents and shell plugins can drive it safely.

```bash
omatab status            # installation, model, settings, quality summary
omatab enable            # suggestions on, instantly and globally
omatab disable           # suggestions off; keys pass straight through
omatab toggle
omatab telemetry on|off  # private local suggestion log (default off)
omatab ocr on|off        # screen-text context capture (default off)
omatab key full-accept Control+Return   # Tab always accepts one word
omatab config
omatab models            # curated local-model catalog
omatab model install qwen-smart
omatab model use qwen-fast
omatab warm              # preload the model
omatab doctor            # health check, exit 0 only when fully working
omatab demo              # local playground page for trying it out
omatab update            # pull the latest source and rebuild in place
omatab uninstall         # remove Oma Tab; --purge also deletes settings and logs
```

## Try it

```bash
omatab demo
```

serves a playground page on loopback and opens it in your browser. It has a
prose field, a rich-text editor, a single-line field, and a password field
that must stay silent. Because Oma Tab is an input method, suggestions appear
in those fields exactly as they do everywhere else. The page mirrors the
suggestion the browser sees through composition events, and when telemetry is
on it shows Oma Tab's own view of each keystroke: what was shown, accepted,
typed past, or cancelled, with latency. A checklist ticks itself as each
behavior is observed, and the page has buttons for the on/off, OCR, and
telemetry switches.

## Settings

Settings live in `~/.config/fcitx5/conf/omatab.conf`, apply live without a
restart, and can also be edited in `fcitx5-configtool`.

| Setting | Default | Meaning |
|---|---|---|
| `FullAcceptKey` | `Shift+Tab` | Key that accepts the whole suggestion. `Tab` always accepts one word. |
| `ScreenContext` | `False` | Read the active window with OCR and give the model that text as background. |
| `Telemetry` | `False` | Record a private local log of every request and outcome for tuning. |

**Screen context.** When on, Oma Tab screenshots the active window and runs
OCR about every two seconds while you type, on a background thread. Each
request uses the latest capture for the current window; a capture older than
`OMATAB_OCR_MAX_AGE_MS` (default `5000`) is withheld, and switching windows
discards the previous window's capture. It sees only rendered content, and
anything overlapping the window, such as a notification, is read too. The
text is capped at 4 KiB.

**Telemetry.** When on, a JSONL log is written with mode `0600` to
`~/.local/state/omatab/events.jsonl` (override with `OMATAB_LOG_PATH`). It
contains full textbox and OCR context, exact model requests and responses,
latency, and every outcome: shown, word or full acceptance, dismissal,
typed-over with whether the suggestion had predicted the next character,
stale, cancelled, filtered (with the reason), cleared, and reset. Because it can contain private writing,
it stays local and must never be committed or uploaded. It rotates to `.1` at
50 MiB (`OMATAB_LOG_MAX_BYTES`, 1 MiB to 1 GiB).

```bash
omatab-telemetry-report
```

produces a quality summary: acceptance rate, acceptance by window class and
by OCR age, suggestion length, latency percentiles, cancellation counts, and
suggestions that look like chat replies rather than continuations.

## Model tuning

Set these in the Fcitx5 service environment (the model picker manages the
model variables through systemd drop-ins).

| Variable | Default | Range |
|---|---|---|
| `OMATAB_MODEL` | balanced Qwen 4B | any Ollama model |
| `OMATAB_CONTEXT_MODEL` | same as model | used when screen context is present |
| `OMATAB_FIM` | `1` | `0` for plain-continuation models such as Gemma |
| `OMATAB_DEBOUNCE_MS` | `120` | 0 to 1000 |
| `OMATAB_NUM_PREDICT` | `16` | 1 to 64 |
| `OMATAB_TEMPERATURE` | `0.2` | 0 to 2 |
| `OMATAB_TOP_P` | `0.9` | 0 to 1 |
| `OMATAB_NUM_CTX` | `8192` | 1024 to 32768 |
| `OMATAB_TIMEOUT_MS` | `2500` | 250 to 10000 |
| `OMATAB_KEEP_ALIVE` | `30m` | `-1` keeps the model resident |
| `OMATAB_OCR_MAX_AGE_MS` | `5000` | 500 to 60000 |

Invalid values fall back to the defaults. The optional `omatab-model-warm.timer`
preloads the configured model and refreshes its lease every minute; the
installer enables it.

## Behavior details

For this English-layout release, Oma Tab commits plain printable keys
immediately and keeps only its unaccepted continuation in IME preedit, so
toolkits without formatted-preedit support never style your real text as an
unfinished composition. Dead-key and composed-layout handling is deferred.

Each word accepted with `Tab` includes one trailing space; the final word also
adds a trailing space. Pending, failed, timed-out, cancelled, and empty model
requests show no suggestion, leaving native `Tab` behavior intact.

A final filter drops suggestions that are not worth a keypress: under three
characters, punctuation only, a character repeated six or more times, or a
verbatim copy of a whole line of the screen such as a status bar. Reusing a
phrase from the screen is allowed on purpose, so a reply can echo the
question it answers.

Suggestions are shown in every application, including terminals. Use
`omatab disable` or the Omarchy bar plugin to pause them.

## Safety gate

Oma Tab must not duplicate or lose committed text, consume `Tab`, the
full-accept key, or `Esc` without a visible suggestion, break shortcuts, or
leave stale preedit in a new field.

## Development

```bash
./scripts/test.sh        # build plus deterministic policy and model tests
./scripts/test-gui.sh    # human-style GUI lane; requires ydotool
```

The model tests cover request serialization, response parsing and trimming,
stale-response policy, in-flight cancellation against a silent local socket,
and the background OCR provider with an injected slow capture. See
[`docs/testing.md`](docs/testing.md) for the full test design.

## License

MIT
