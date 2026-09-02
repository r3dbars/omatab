#!/usr/bin/env bash
# One-shot Oma Tab setup for Omarchy: packages, Ollama, build, Fcitx wiring,
# and a model sized to the GPU. Safe to rerun; `omatab update` runs it again.
#
# Progress is written to $XDG_STATE_HOME/omatab/setup.json so the bar widget
# can show what is happening while this runs in a terminal.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
state_dir=${XDG_STATE_HOME:-$HOME/.local/state}/omatab
config_home=${XDG_CONFIG_HOME:-$HOME/.config}
setup_file=$state_dir/setup.json
source_file=$state_dir/source_dir
omatab_bin=$HOME/.local/bin/omatab
fcitx_service=omarchy-fcitx5.service
requested_model=${OMATAB_MODEL_ID:-}
skip_packages=false

usage() {
  cat <<'EOF'
Usage: scripts/bootstrap.sh [--model ID] [--skip-packages]

  --model ID        Use this catalog model instead of sizing one to the GPU
                    (see `omatab models`).
  --skip-packages   Do not install or check system packages.
EOF
}

while (($# > 0)); do
  case "$1" in
    --model) requested_model=${2:-}; shift 2 ;;
    --skip-packages) skip_packages=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

mkdir -p -m 700 "$state_dir"
finished=false

stage() {
  jq -nc --arg stage "$1" --arg detail "$2" --argjson pid "$$" \
    '{stage: $stage, detail: $detail, pid: $pid, updated: (now | todate)}' \
    >"$setup_file"
  printf '\n==> %s\n' "$2"
}

on_exit() {
  local code=$?
  if [[ $finished != true ]]; then
    local detail="Setup stopped before it finished (exit $code)."
    jq -nc --arg detail "$detail" --argjson pid "$$" \
      '{stage: "failed", detail: $detail, pid: $pid, updated: (now | todate)}' \
      >"$setup_file"
    echo "$detail Rerun this script to try again." >&2
  fi
}
trap on_exit EXIT

if ! command -v jq >/dev/null; then
  echo "jq is required first: sudo pacman -S --needed jq" >&2
  exit 1
fi

# ---- hardware ----

gpu_vendor() {
  local pci
  pci=$(lspci 2>/dev/null | grep -iE 'vga|3d controller|display' || true)
  if grep -qi nvidia <<<"$pci"; then
    echo nvidia
  elif grep -qiE 'amd|ati|radeon' <<<"$pci"; then
    echo amd
  else
    echo none
  fi
}

# Video memory in MiB, or 0 when unknown.
gpu_memory_mib() {
  local vendor=$1 best=0 path bytes mib
  case "$vendor" in
    nvidia)
      if command -v nvidia-smi >/dev/null; then
        mib=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null |
          sort -n | tail -1 | tr -dc '0-9')
        [[ -n $mib ]] && best=$mib
      fi
      ;;
    amd)
      for path in /sys/class/drm/card*/device/mem_info_vram_total; do
        [[ -r $path ]] || continue
        bytes=$(<"$path")
        mib=$((bytes / 1024 / 1024))
        ((mib > best)) && best=$mib
      done
      ;;
  esac
  echo "$best"
}

vendor=$(gpu_vendor)
memory_mib=$(gpu_memory_mib "$vendor")
case "$vendor" in
  nvidia) ollama_package=ollama-cuda ;;
  amd) ollama_package=ollama-rocm ;;
  *) ollama_package=ollama ;;
esac

# ---- 1. packages ----

if [[ $skip_packages != true ]]; then
  stage packages "Checking system packages"
  packages=(git cmake ninja gcc pkgconf fcitx5 jsoncpp curl jq
    tesseract tesseract-data-eng grim "$ollama_package")
  mapfile -t missing < <(pacman -T "${packages[@]}" || true)
  if ((${#missing[@]} > 0)); then
    stage packages "Installing ${missing[*]}"
    if command -v omarchy >/dev/null; then
      omarchy pkg add "${missing[@]}"
    else
      sudo pacman -S --needed "${missing[@]}"
    fi
  else
    echo "All packages present."
  fi
fi

# ---- 2. Ollama ----

stage ollama "Starting the Ollama service"
if ! systemctl is-active --quiet ollama.service; then
  sudo systemctl enable --now ollama.service
fi
for _ in $(seq 1 30); do
  curl -fsS --max-time 1 http://127.0.0.1:11434/api/tags >/dev/null 2>&1 && break
  sleep 1
done
curl -fsS --max-time 2 http://127.0.0.1:11434/api/tags >/dev/null 2>&1 ||
  { echo "Ollama did not answer on 127.0.0.1:11434." >&2; exit 1; }

# ---- 3. build and install ----

stage build "Building Oma Tab"
# Applications ask D-Bus for Fcitx whenever it is away, and D-Bus would
# launch a bare fcitx5 without the addon path that then holds the bus name
# and rewrites the profile without Oma Tab. Route activation to the unit
# before the first restart below opens that window.
install -Dm644 "$project_dir/packaging/dbus/org.fcitx.Fcitx5.service" \
  "${XDG_DATA_HOME:-$HOME/.local/share}/dbus-1/services/org.fcitx.Fcitx5.service"
busctl --user call org.freedesktop.DBus /org/freedesktop/DBus \
  org.freedesktop.DBus ReloadConfig >/dev/null 2>&1 || true

OMATAB_SKIP_MODEL_PULL=1 "$project_dir/scripts/install-omarchy-user.sh"
printf '%s\n' "$project_dir" >"$source_file"

# ---- 4. Fcitx wiring ----
# Fcitx rewrites its profile on exit, so stop it, edit, then start.

stage fcitx "Selecting Oma Tab as the input method"
profile=$config_home/fcitx5/profile
global_config=$config_home/fcitx5/config
mkdir -p "$config_home/fcitx5"

systemctl --user stop "$fcitx_service" >/dev/null 2>&1 || true
# Any fcitx5 left outside the unit is one of those strays; stop it too.
for pid in $(pgrep -x fcitx5 || true); do
  unit=$(tail -1 "/proc/$pid/cgroup" 2>/dev/null | sed 's|.*/||')
  if [[ $unit != "$fcitx_service" ]]; then
    systemctl --user stop "$unit" >/dev/null 2>&1 || kill "$pid" 2>/dev/null || true
  fi
done
sleep 1
if [[ ! -f $profile ]]; then
  cat >"$profile" <<'EOF'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=omatab

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=omatab
Layout=us

[GroupOrder]
0=Default
EOF
else
  if ! grep -qx 'Name=omatab' "$profile"; then
    next=$(grep -c '^\[Groups/0/Items/' "$profile" || true)
    printf '\n[Groups/0/Items/%s]\nName=omatab\nLayout=us\n' "$next" >>"$profile"
  fi
  if grep -q '^DefaultIM=' "$profile"; then
    sed -i 's/^DefaultIM=.*/DefaultIM=omatab/' "$profile"
  else
    sed -i '0,/^\[Groups\/0\]/s//[Groups\/0]\nDefaultIM=omatab/' "$profile"
  fi
fi
if [[ ! -f $global_config ]]; then
  install -m 644 "$project_dir/packaging/fcitx5-global.conf" "$global_config"
fi
systemctl --user start "$fcitx_service"

# ---- 5. model ----

catalog=$("$omatab_bin" models --json)
current_id=$("$omatab_bin" status --json | jq -r '.model_id')
current_installed=$(jq -r --arg id "$current_id" \
  'map(select(.id == $id)) | first | .installed // false' <<<"$catalog")

if [[ -n $requested_model ]]; then
  model_id=$requested_model
elif [[ $current_id != custom && $current_installed == true ]]; then
  model_id=$current_id
  echo "Keeping the current model ($model_id)."
elif ((memory_mib >= 7000)); then
  model_id=qwen-balanced
elif ((memory_mib >= 3000)); then
  model_id=qwen-fast
  echo "GPU has ${memory_mib} MiB; using the fast 2B model."
else
  model_id=qwen-fast
  echo "No usable GPU detected; suggestions will be slow on the CPU. Using the fast 2B model." >&2
fi

record=$(jq -c --arg id "$model_id" 'map(select(.id == $id)) | first' <<<"$catalog")
[[ $record != null ]] || { echo "Unknown model id: $model_id" >&2; exit 2; }
model_name=$(jq -r '.model' <<<"$record")
model_label=$(jq -r '.label' <<<"$record")
model_gb=$(jq -r '.download_gb' <<<"$record")

if ! ollama show "$model_name" >/dev/null 2>&1; then
  stage model "Downloading $model_label ($model_gb GB)"
  ollama pull "$model_name"
fi
stage model "Warming up $model_label"
"$omatab_bin" model install "$model_id"

# ---- 6. check ----

stage check "Checking that everything works"
if "$omatab_bin" doctor; then
  finished=true
  stage done "Oma Tab is ready. Start typing anywhere."
else
  echo "Something is not right yet; see 'omatab doctor' above." >&2
  exit 1
fi
