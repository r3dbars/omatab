#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
override_source="$project_dir/packaging/omarchy-fcitx5.service.d/10-omatab-addon-path.conf"
override_target="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/omarchy-fcitx5.service.d/10-omatab-addon-path.conf"

"$project_dir/scripts/install-user.sh"
install -Dm644 "$override_source" "$override_target"
systemctl --user daemon-reload
systemctl --user enable --now omatab-model-warm.timer
systemctl --user restart omarchy-fcitx5.service

default_model=hf.co/mradermacher/Qwen3.5-4B-Base-GGUF:Q8_0
configured_model=$(systemctl --user show omarchy-fcitx5.service --property=Environment --value 2>/dev/null |
  tr ' ' '\n' | sed -n 's/^OMATAB_MODEL=//p' | tail -n 1)
if [[ -z ${OMATAB_SKIP_MODEL_PULL:-} && -z $configured_model ]] && command -v ollama >/dev/null; then
  if ! ollama show "$default_model" >/dev/null 2>&1; then
    echo "Downloading the default model ($default_model, about 4.3 GB)..."
    ollama pull "$default_model" || echo "Model download failed; run 'omatab model install qwen-balanced' later." >&2
  fi
fi

echo "Restarted Omarchy's Fcitx5 service with the user-local addon path."
echo "Add 'Oma Tab' to your Fcitx input-method group, then run: omatab doctor"
