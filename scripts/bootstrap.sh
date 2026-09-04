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

# Ollama may already be here from a package, from Ollama's own install
# script, or in a container. pacman only knows about its own packages, so
# asking it would say "missing" for an Ollama that is right there and then
# fail on a conflicting install. Ask the machine instead.
ollama_answers() {
  curl -fsS --max-time 2 http://127.0.0.1:11434/api/tags >/dev/null 2>&1
}
ollama_present() {
  command -v ollama >/dev/null || ollama_answers
}

if [[ $skip_packages != true ]]; then
  stage packages "Checking system packages"
  packages=(git cmake ninja gcc pkgconf fcitx5 jsoncpp curl jq
    tesseract tesseract-data-eng grim)
  if ollama_present; then
    echo "Ollama is already installed; leaving it as it is."
    if [[ $ollama_package != ollama ]] && ! pacman -Qq "$ollama_package" >/dev/null 2>&1; then
      echo "If suggestions are slow, $ollama_package adds GPU acceleration."
    fi
  else
    packages+=("$ollama_package")
  fi
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
if ! ollama_answers; then
  # A packaged Ollama runs as ollama.service. Other installs may use a
  # different unit or none at all, so a missing unit is not an error as
  # long as something answers on the port.
  if systemctl cat ollama.service >/dev/null 2>&1; then
    systemctl is-active --quiet ollama.service ||
      sudo systemctl enable --now ollama.service
  fi
  for _ in $(seq 1 30); do
    ollama_answers && break
    sleep 1
  done
fi
if ! ollama_answers; then
  echo "Ollama is not answering on 127.0.0.1:11434." >&2
  echo "Start it the way you normally do, then run this again." >&2
  echo "For a packaged install: sudo systemctl enable --now ollama.service" >&2
  exit 1
fi

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

"$project_dir/scripts/install-omarchy-user.sh"
# install.sh builds in a throwaway directory and moves the verified tree into
# place afterwards, so record where the source will live, not where it is now.
printf '%s\n' "${OMATAB_SOURCE_HOME:-$project_dir}" >"$source_file"

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
# Input contexts created while Fcitx was still loading can come up on the
# plain keyboard even though the profile says Oma Tab; switch them over.
for _ in $(seq 1 20); do
  busctl --user --quiet status org.fcitx.Fcitx5 >/dev/null 2>&1 && break
  sleep 0.5
done
fcitx5-remote -s omatab >/dev/null 2>&1 || true

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
# Check the bytes against the digest pinned in the catalog before anything
# loads them. A moved tag on the registry stops setup here.
stage model "Checking $model_label against its pinned digest"
"$omatab_bin" model verify "$model_id"
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
