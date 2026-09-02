#!/usr/bin/env bash
# Re-derives the digest pinned for every catalog model, straight from the
# registry and without downloading any weights.
#
# Ollama identifies a model by the sha256 of its manifest, which is the digest
# it reports in /api/tags. Fetching that manifest and hashing it therefore
# reproduces the value pinned in `model_catalog`, so the pins in this
# repository can be checked by anyone in a few seconds.
#
#   scripts/pin-models.sh          print each model and its registry digest
#   scripts/pin-models.sh --check  exit non-zero when a pin no longer matches
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
check=false
[[ ${1:-} == --check ]] && check=true

command -v jq >/dev/null || { echo "jq is required" >&2; exit 1; }

drift=0
while IFS=$'\t' read -r id model pinned; do
  ref=${model#hf.co/}
  repo=${ref%%:*}
  tag=${ref##*:}
  [[ $tag == "$ref" ]] && tag=latest

  manifest=$(mktemp)
  code=$(curl -sS -o "$manifest" -w '%{http_code}' \
    "https://huggingface.co/v2/$repo/manifests/$tag" || echo 000)
  if [[ $code != 200 ]]; then
    printf '%-16s %s\n' "$id" "registry returned HTTP $code for $repo:$tag"
    rm -f "$manifest"
    drift=1
    continue
  fi
  actual=$(sha256sum "$manifest" | cut -d' ' -f1)
  rm -f "$manifest"

  if [[ $check == true ]]; then
    if [[ $actual == "$pinned" ]]; then
      printf '%-16s ok    %s\n' "$id" "$actual"
    else
      printf '%-16s DRIFT pinned %s, registry %s\n' "$id" "$pinned" "$actual"
      drift=1
    fi
  else
    printf '%-16s %s\n  %s\n' "$id" "$model" "$actual"
  fi
done < <(bash "$project_dir/scripts/omatab.sh" models --json |
  jq -r '.[] | [.id, .model, (.digest // "")] | @tsv')

if [[ $check == true && $drift -ne 0 ]]; then
  echo "A pinned model digest no longer matches the registry." >&2
  echo "Review the change before updating model_catalog in scripts/omatab.sh." >&2
  exit 1
fi
