#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

cmake -S "$repo_root" -B "$repo_root/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON
cmake --build "$repo_root/build"
ctest --test-dir "$repo_root/build" --output-on-failure

test -f "$repo_root/build/libtilde.so"
test -f "$repo_root/build/tilde.conf"
echo "PASS addon artifacts exist"
