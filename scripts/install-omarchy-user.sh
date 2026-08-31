#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
override_source="$project_dir/packaging/omarchy-fcitx5.service.d/10-tilde-addon-path.conf"
override_target="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/omarchy-fcitx5.service.d/10-tilde-addon-path.conf"

"$project_dir/scripts/install-user.sh"
install -Dm644 "$override_source" "$override_target"
systemctl --user daemon-reload
systemctl --user restart omarchy-fcitx5.service

echo "Restarted Omarchy's Fcitx5 service with the user-local addon path."

