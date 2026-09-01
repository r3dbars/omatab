#!/usr/bin/env bash
set -euo pipefail

if ! command -v ydotool >/dev/null 2>&1; then
  echo 'FAIL ydotool is required: wtype bypasses the Fcitx input-method path' >&2
  exit 2
fi

run_dir=$(mktemp -d -t omatab-e2e.XXXXXX)
result_path="$run_dir/agent-proof.md"
expected=$'Agent proof: \nAgentHello — Oma Tab is working'
current_workspace=$(hyprctl -j activeworkspace | jq -r '.id')
test_workspace=9
test_pid=

cleanup() {
  fcitx5-remote -s keyboard-us >/dev/null 2>&1 || true
  if [[ -n "$test_pid" ]]; then
    kill "$test_pid" >/dev/null 2>&1 || true
    wait "$test_pid" >/dev/null 2>&1 || true
  fi
  hyprctl dispatch \
    "hl.dsp.focus({ workspace = $current_workspace })" >/dev/null 2>&1 || true
  rm -rf -- "$run_dir"
}
trap cleanup EXIT

occupied=$(hyprctl -j workspaces | jq --argjson id "$test_workspace" \
  '[.[] | select(.id == $id) | .windows] | add // 0')
if (( occupied > 0 )); then
  printf 'FAIL test workspace %s is not empty\n' "$test_workspace" >&2
  exit 1
fi

printf 'Agent proof: \n' >"$result_path"
hyprctl dispatch \
  "hl.dsp.focus({ workspace = $test_workspace })" >/dev/null
omawrite "$result_path" &
test_pid=$!

for _ in {1..50}; do
  active_pid=$(hyprctl -j activewindow | jq -r '.pid // 0')
  if [[ "$active_pid" == "$test_pid" ]]; then
    break
  fi
  sleep 0.1
done

active_pid=$(hyprctl -j activewindow | jq -r '.pid // 0')
active_class=$(hyprctl -j activewindow | jq -r '.class // ""')
if [[ "$active_pid" != "$test_pid" || "$active_class" != omawrite ]]; then
  echo 'FAIL refused to type: disposable Omawrite is not active' >&2
  exit 1
fi

active=
for _ in {1..50}; do
  fcitx5-remote -o
  fcitx5-remote -s omatab
  active=$(fcitx5-remote -n)
  if [[ "$active" == omatab ]]; then
    break
  fi
  sleep 0.1
done
if [[ "$active" != omatab ]]; then
  printf 'FAIL could not activate Oma Tab; active input method is <%s>\n' "$active" >&2
  exit 1
fi

ydotool key 29:1 107:1 107:0 29:0
ydotool type --key-delay 70 -- 'AgentHello'
ydotool key 15:1 15:0
ydotool key 29:1 31:1 31:0 29:0
sleep 0.5

actual=$(<"$result_path")
if [[ "$actual" != "$expected" ]]; then
  printf 'FAIL expected <%s>, got <%s>\n' "$expected" "$actual" >&2
  exit 1
fi

printf 'PASS Omawrite agent typed and accepted <%s>\n' "$actual"
