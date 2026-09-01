#!/usr/bin/env bash
set -euo pipefail

# One-time migration from the early "tilde" proof install to Oma Tab.
# Carries over the selected model and telemetry choice, moves the state
# directory, renames the Fcitx input-method entry, removes the old files,
# and installs Oma Tab. A tilde-control shim keeps older shell plugins working.

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
config_home=${XDG_CONFIG_HOME:-$HOME/.config}
state_home=${XDG_STATE_HOME:-$HOME/.local/state}
fcitx_dropins=$config_home/systemd/user/omarchy-fcitx5.service.d
warm_dropins=$config_home/systemd/user/omatab-model-warm.service.d
settings=$config_home/fcitx5/conf/omatab.conf
profile=$config_home/fcitx5/profile

old_env=$(cat "$fcitx_dropins"/*tilde*.conf "$config_home"/systemd/user/tilde-model-warm.service.d/*.conf 2>/dev/null |
  sed -n 's/^Environment=//p' || true)
value() { sed -n "s/^$1=//p" <<<"$old_env" | tail -n 1; }
old_model=$(value TILDE_MODEL)
old_context_model=$(value TILDE_CONTEXT_MODEL)
old_fim=$(value TILDE_FIM)
old_num_ctx=$(value TILDE_NUM_CTX)
had_telemetry=false
[[ -n $(value TILDE_LOG_PATH) ]] && had_telemetry=true

echo "Stopping the old Tilde addon..."
systemctl --user disable --now tilde-model-warm.timer >/dev/null 2>&1 || true
systemctl --user stop omarchy-fcitx5.service

rm -f "$HOME/.local/lib/fcitx5/libtilde.so" \
      "$HOME/.local/share/fcitx5/addon/tilde.conf" \
      "$HOME/.local/bin/tilde-telemetry-report" \
      "$HOME/.local/libexec/tilde-warm-model" \
      "$HOME/.local/share/systemd/user/tilde-model-warm.service" \
      "$HOME/.local/share/systemd/user/tilde-model-warm.timer" \
      "$fcitx_dropins"/10-tilde-addon-path.conf \
      "$fcitx_dropins"/20-tilde-model.conf \
      "$fcitx_dropins"/30-tilde-selected-model.conf
rm -rf "$config_home/systemd/user/tilde-model-warm.service.d"

if [[ -n $old_model ]]; then
  echo "Keeping model $old_model"
  install -Dm644 /dev/stdin "$fcitx_dropins/30-omatab-selected-model.conf" <<EOF
[Service]
Environment=OMATAB_MODEL=$old_model
Environment=OMATAB_CONTEXT_MODEL=${old_context_model:-$old_model}
Environment=OMATAB_FIM=${old_fim:-1}
EOF
  install -Dm644 /dev/stdin "$warm_dropins/30-omatab-selected-model.conf" <<EOF
[Service]
Environment=OMATAB_MODEL=$old_model
Environment=OMATAB_NUM_CTX=${old_num_ctx:-8192}
EOF
fi

# The proof always captured screen text and, on this machine, always logged.
# Preserve both choices explicitly; fresh installs default to off.
mkdir -p "$(dirname "$settings")"
install -m 600 /dev/stdin "$settings" <<EOF
ScreenContext=True
Telemetry=$([[ $had_telemetry == true ]] && echo True || echo False)

[FullAcceptKey]
0=Shift+Tab
EOF

if [[ -d $state_home/tilde && ! -L $state_home/tilde && ! -e $state_home/omatab ]]; then
  mv "$state_home/tilde" "$state_home/omatab"
  ln -s omatab "$state_home/tilde"
fi

if [[ -f $profile ]]; then
  sed -i 's/^DefaultIM=tilde$/DefaultIM=omatab/; s/^Name=tilde$/Name=omatab/' "$profile"
fi

install -Dm755 /dev/stdin "$HOME/.local/bin/tilde-control" <<'EOF'
#!/usr/bin/env bash
# Compatibility shim for plugins written against the old name.
exec omatab "$@"
EOF

"$project_dir/scripts/install-omarchy-user.sh"
echo "Migration complete. Settings: $settings"
