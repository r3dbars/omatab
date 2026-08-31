#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="$project_dir/build"

cmake \
  -S "$project_dir" \
  -B "$build_dir" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build "$build_dir"
cmake --install "$build_dir"

echo "Installed Tilde Linux Proof for the current user."
echo "Fcitx5 must include $HOME/.local/lib/fcitx5 in FCITX_ADDON_DIRS."
echo "On Omarchy, use scripts/install-omarchy-user.sh to configure and restart it."
