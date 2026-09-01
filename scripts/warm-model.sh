#!/usr/bin/env bash
set -euo pipefail

model=${TILDE_MODEL:-qwen2.5-coder:1.5b-base}
context=${TILDE_NUM_CTX:-8192}
endpoint=${OLLAMA_HOST:-http://127.0.0.1:11434}
payload=$(jq -nc --arg model "$model" --argjson context "$context" '{
  model: $model,
  prompt: "",
  stream: false,
  keep_alive: -1,
  options: {num_ctx: $context, num_predict: 0}
}')

for _attempt in $(seq 1 30); do
  if curl --fail --silent --show-error --max-time 180 \
      --request POST "$endpoint/api/generate" \
      --header 'Content-Type: application/json' \
      --data "$payload" >/dev/null; then
    exit 0
  fi
  sleep 2
done

echo "Unable to warm $model through $endpoint after 30 attempts" >&2
exit 1
