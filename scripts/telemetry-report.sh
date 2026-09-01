#!/usr/bin/env bash
set -euo pipefail

log_path=${1:-${TILDE_LOG_PATH:-$HOME/.local/state/tilde/events.jsonl}}

if [[ ! -s "$log_path" ]]; then
  echo "No Tilde telemetry events found at $log_path" >&2
  exit 1
fi

jq -s '
  def model_results: map(select(.type == "model_result"));
  def shown: model_results | map(select(.outcome == "shown"));
  def actions: map(select(.type == "accept_word" or .type == "accept_full" or
                          .type == "dismiss" or .type == "typed_over" or
                          .type == "clear_editing" or .type == "reset"));
  def accepted_ids: actions |
    map(select(.type == "accept_word" or .type == "accept_full") | .request_id) |
    unique;
  def percentile($values; $fraction):
    ($values | sort) as $sorted |
    if ($sorted | length) == 0 then null
    else $sorted[((($sorted | length) - 1) * $fraction | floor)] end;
  (model_results | map(.latency_ms | select(type == "number"))) as $latencies |
  shown as $shown |
  accepted_ids as $accepted |
  {
    event_count: length,
    model_request_count: (model_results | length),
    shown_count: ($shown | length),
    empty_count: (model_results | map(select(.outcome == "empty")) | length),
    stale_count: (model_results | map(select(.outcome == "stale")) | length),
    cancelled_count: (model_results | map(select(.outcome == "cancelled")) | length),
    error_count: (model_results | map(select(.outcome == "request_error")) | length),
    accepted_request_count: ($accepted | length),
    acceptance_rate: (if ($shown | length) == 0 then null
                      else (($accepted | length) / ($shown | length)) end),
    word_accept_count: (actions | map(select(.type == "accept_word")) | length),
    full_accept_count: (actions | map(select(.type == "accept_full")) | length),
    dismiss_count: (actions | map(select(.type == "dismiss")) | length),
    typed_over_count: (actions | map(select(.type == "typed_over")) | length),
    latency_ms: {
      average: (if ($latencies | length) == 0 then null
                else ($latencies | add) / ($latencies | length) end),
      p50: percentile($latencies; 0.50),
      p95: percentile($latencies; 0.95),
      maximum: (if ($latencies | length) == 0 then null
                else ($latencies | max) end)
    },
    possible_reply_examples: ($shown |
      map(select(.suggestion | test("^[[:space:]]*(Sure|Certainly|I can|I would|It sounds|As an AI|Yes[,!.]|No[,!.])"; "i"))) |
      map({request_id, textbox_prefix, suggestion, window_class, window_title}) |
      .[0:10]),
    repeated_suggestions: ($shown |
      group_by(.suggestion) |
      map(select(length > 1) | {suggestion: .[0].suggestion, count: length}) |
      sort_by(-.count) | .[0:10])
  }
' "$log_path"
