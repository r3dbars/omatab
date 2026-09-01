# Testing strategy

Oma Tab treats exact text behavior as a machine-verifiable invariant. A language
model may review suggestion quality, but it never decides whether text was
duplicated, lost, or whether a key was consumed incorrectly.

## Test lanes

1. **Policy tests** run without Fcitx, a display, or a model. They verify that
   Tab and Escape are consumed only when a suggestion is visible and that edit
   operations clear stale preedit.
2. **Fcitx integration tests** verify addon discovery, activation, preedit,
   commit, reset, and focus changes through Fcitx's test frontend.
3. **GUI-agent tests** launch a disposable native editor under Hyprland, select
   Oma Tab, type like a person, press Tab/Escape, capture a screenshot, and compare
   the final text with `tests/e2e/scenarios.json`.
4. **Local-model tests** verify Ollama request JSON, response parsing,
   sanitization, stale-response rejection, structured context bounds, OCR
   window parsing/privacy exclusions, and record live endpoint latency.
5. **GPT review** scores usefulness, style fit, and repetition from saved model
   outputs. GPT reviews only artifacts from a completed deterministic run.

Every GUI run writes evidence matching `tests/e2e/evidence.schema.json`. Failed
runs retain the application name, exact resulting text, logs, and screenshot so
an agent can diagnose the failure instead of merely retrying it.

## Current commands

```bash
./scripts/test.sh
./scripts/test-gui.sh
```

The first command implements lanes 1 and 4 plus build-artifact validation. The GUI
runner isolates a disposable Omawrite document and restores `keyboard-us` on
exit, but currently stops before injection. Wayland `wtype` events bypass the
Fcitx input-method path, so lane 3 requires an evdev-level `ydotool` sequence.
Native editor/browser coverage and the Fcitx test frontend remain required for
fully automated end-to-end evidence. The live model-backed path has been
manually demonstrated in Omawrite.

## Release gate

A build cannot advance when any deterministic invariant fails. Model quality is
reported separately so a subjectively weak suggestion cannot hide an input
method regression.
