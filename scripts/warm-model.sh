#!/usr/bin/env bash
# Loads the configured model into Ollama and keeps it resident. Retries only
# while Ollama itself is unreachable; a model that is not downloaded yet is
# not an error here (the installer or `omatab model install` handles that).
set -euo pipefail

model=${OMATAB_MODEL:-hf.co/mradermacher/Qwen3.5-4B-Base-GGUF:Q8_0}
context=${OMATAB_NUM_CTX:-8192}
endpoint=${OLLAMA_HOST:-http://127.0.0.1:11434}

for _attempt in $(seq 1 30); do
  if curl --fail --silent --max-time 5 "$endpoint/api/tags" >/dev/null 2>&1; then
    break
  fi
  sleep 2
done
if ! curl --fail --silent --max-time 5 "$endpoint/api/tags" >/dev/null 2>&1; then
  echo "Ollama is not reachable at $endpoint" >&2
  exit 1
fi

if ! curl --fail --silent --max-time 10 --request POST "$endpoint/api/show" \
    --header 'Content-Type: application/json' \
    --data "$(jq -nc --arg model "$model" '{model: $model}')" >/dev/null 2>&1; then
  echo "Model $model is not downloaded yet; nothing to warm."
  exit 0
fi

# The warm timer is the other place a model enters Ollama's long-lived
# runtime, so it checks the same pin the installer checked. A model somebody
# configured by hand is not in the catalog and is left alone.
omatab_bin=$HOME/.local/bin/omatab
if [[ -x $omatab_bin ]]; then
  model_id=$("$omatab_bin" models --json 2>/dev/null |
    jq -r --arg model "$model" '
      def normalized: sub(":latest$"; "");
      map(select(((.model // "") | normalized) == ($model | normalized))) |
      first | .id // ""' 2>/dev/null || true)
  if [[ -n $model_id ]]; then
    verify_status=0
    "$omatab_bin" model verify "$model_id" >/dev/null || verify_status=$?
    # 6 is a digest mismatch, the one answer that must stop the load. Any
    # other failure means the check could not run; the model was already
    # verified when it was installed, so warming continues.
    if ((verify_status == 6)); then
      echo "Not warming $model: it no longer matches its pinned digest." >&2
      exit 1
    fi
  fi
fi

payload=$(jq -nc --arg model "$model" --argjson context "$context" '{
  model: $model,
  prompt: "",
  stream: false,
  keep_alive: -1,
  options: {num_ctx: $context, num_predict: 0}
}')
curl --fail --silent --show-error --max-time 180 \
  --request POST "$endpoint/api/generate" \
  --header 'Content-Type: application/json' \
  --data "$payload" >/dev/null
