#!/usr/bin/env bash
set -euo pipefail

log_path=${1:-${OMATAB_LOG_PATH:-$HOME/.local/state/omatab/events.jsonl}}

if [[ ! -s "$log_path" ]]; then
  echo "No Oma Tab telemetry events found at $log_path" >&2
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
    filtered_count: (model_results | map(select(.outcome == "filtered")) | length),
    filtered_by_reason: (model_results | map(select(.outcome == "filtered")) |
      group_by(.filter_reason) | map({reason: .[0].filter_reason, count: length}) |
      sort_by(-.count)),
    accepted_request_count: ($accepted | length),
    acceptance_rate: (if ($shown | length) == 0 then null
                      else (($accepted | length) / ($shown | length)) end),
    word_accept_count: (actions | map(select(.type == "accept_word")) | length),
    full_accept_count: (actions | map(select(.type == "accept_full")) | length),
    dismiss_count: (actions | map(select(.type == "dismiss")) | length),
    typed_over_count: (actions | map(select(.type == "typed_over")) | length),
    typed_over_matching_count: (actions | map(select(.type == "typed_over" and .matched == true)) | length),
    quality: {
      by_window: ($shown | group_by(.window_class) | map(
        .[0].window_class as $window |
        (map(.request_id) | unique) as $ids |
        {
          window_class: $window,
          shown: length,
          accepted: ($accepted | map(select(. as $id | $ids | index($id))) | length),
          acceptance_rate: (($accepted | map(select(. as $id | $ids | index($id))) | length) / length)
        }) | sort_by(-.shown)),
      by_ocr_age: ($shown | group_by(
          if (.ocr_age_ms // -1) < 0 then "none"
          elif .ocr_age_ms < 2000 then "fresh_under_2s"
          else "aged_2s_plus" end) | map(
        .[0] as $first |
        (if ($first.ocr_age_ms // -1) < 0 then "none"
         elif $first.ocr_age_ms < 2000 then "fresh_under_2s"
         else "aged_2s_plus" end) as $bucket |
        (map(.request_id) | unique) as $ids |
        {
          ocr_age: $bucket,
          shown: length,
          accepted: ($accepted | map(select(. as $id | $ids | index($id))) | length),
          acceptance_rate: (($accepted | map(select(. as $id | $ids | index($id))) | length) / length)
        })),
      suggestion_length: (($shown | map(.suggestion | length)) as $chars |
        ($shown | map(.suggestion | [scan("\\S+")] | length)) as $words |
        {
          median_chars: percentile($chars; 0.5),
          median_words: percentile($words; 0.5),
          max_chars: (if ($chars | length) == 0 then null else ($chars | max) end)
        })
    },
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
