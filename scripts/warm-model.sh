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
