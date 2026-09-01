#!/usr/bin/env bash
set -euo pipefail

state_dir=${XDG_STATE_HOME:-$HOME/.local/state}/tilde
log_path=${TILDE_LOG_PATH:-$state_dir/events.jsonl}
disabled_file=${TILDE_DISABLED_FILE:-$state_dir/disabled}
service=omarchy-fcitx5.service
warm_timer=tilde-model-warm.timer
default_model=qwen2.5-coder:1.5b-base
fcitx_dropin_dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/omarchy-fcitx5.service.d
warm_dropin_dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/tilde-model-warm.service.d
fcitx_model_dropin=$fcitx_dropin_dir/30-tilde-selected-model.conf
warm_model_dropin=$warm_dropin_dir/30-tilde-selected-model.conf

usage() {
  cat <<'EOF'
Usage: tilde-control <command> [options]

Commands:
  status [--json]  Show installation, model, and quality status
  enable           Select Tilde as the active input method
  disable          Turn the active input method off
  toggle           Toggle Tilde on or off
  warm             Warm the configured model without blocking
  restart          Restart Tilde's Fcitx service
  models [--json]  List supported model choices
  model install ID Download, verify, and use a supported model
  model use ID     Use an already-downloaded supported model
  doctor           Run a concise local health check
EOF
}

model_catalog() {
  cat <<'JSON'
[
  {
    "id": "qwen-fast",
    "label": "Qwen Fast · 2B",
    "family": "Qwen 3.5 Base",
    "model": "hf.co/mradermacher/Qwen3.5-2B-Base-GGUF:Q8_0",
    "download_gb": 2.1,
    "fim": true,
    "requires_terms": false,
    "description": "Fast everyday completion for modest GPUs."
  },
  {
    "id": "qwen-balanced",
    "label": "Qwen Balanced · 4B",
    "family": "Qwen 3.5 Base",
    "model": "hf.co/mradermacher/Qwen3.5-4B-Base-GGUF:Q8_0",
    "download_gb": 4.3,
    "fim": true,
    "requires_terms": false,
    "description": "A strong balance of speed and writing quality."
  },
  {
    "id": "qwen-smart",
    "label": "Qwen Smart · 9B",
    "family": "Qwen 3.5 Base",
    "model": "hf.co/mradermacher/Qwen3.5-9B-Base-GGUF:Q8_0",
    "download_gb": 9.2,
    "fim": true,
    "requires_terms": false,
    "description": "Highest-quality recommended Qwen model."
  },
  {
    "id": "gemma-tiny",
    "label": "Gemma Tiny · 1B",
    "family": "Gemma 3 Pretrained",
    "model": "hf.co/google/gemma-3-1b-pt-qat-q4_0-gguf",
    "download_gb": 1.0,
    "fim": false,
    "requires_terms": true,
    "description": "Very light prose completion from Google."
  },
  {
    "id": "gemma-balanced",
    "label": "Gemma Balanced · 4B",
    "family": "Gemma 3 Pretrained",
    "model": "hf.co/google/gemma-3-4b-pt-qat-q4_0-gguf",
    "download_gb": 3.3,
    "fim": false,
    "requires_terms": true,
    "description": "Natural prose completion for everyday writing."
  },
  {
    "id": "gemma-smart",
    "label": "Gemma Smart · 12B",
    "family": "Gemma 3 Pretrained",
    "model": "hf.co/google/gemma-3-12b-pt-qat-q4_0-gguf",
    "download_gb": 8.1,
    "fim": false,
    "requires_terms": true,
    "description": "Larger prose model for higher-end GPUs."
  }
]
JSON
}

model_record() {
  local id=$1
  model_catalog | jq -ce --arg id "$id" '.[] | select(.id == $id)' \
    || { echo "Unknown Tilde model: $id" >&2; return 2; }
}

service_environment() {
  systemctl --user show "$service" --property=Environment --value 2>/dev/null || true
}

environment_value() {
  local key=$1
  local environment=$2
  tr ' ' '\n' <<<"$environment" | sed -n "s/^${key}=//p" | tail -n 1
}

fcitx_bus_available() {
  busctl --user --quiet status org.fcitx.Fcitx5 >/dev/null 2>&1
}

input_method_state() {
  fcitx_bus_available || return 0
  fcitx5-remote 2>/dev/null || true
}

input_method_name() {
  fcitx_bus_available || return 0
  fcitx5-remote -n 2>/dev/null || true
}

status_json() {
  local environment model context input_state input_name service_state timer_state
  local installed=false enabled=true active_input=false telemetry_enabled=false
  local ollama_json='{"models":[]}' telemetry_json='{}' catalog='[]'

  environment=$(service_environment)
  model=$(environment_value TILDE_MODEL "$environment")
  context=$(environment_value TILDE_NUM_CTX "$environment")
  [[ -n $model ]] || model=$default_model
  [[ $context =~ ^[0-9]+$ ]] || context=8192

  if [[ -f $HOME/.local/share/fcitx5/addon/tilde.conf ]] &&
     [[ -f $HOME/.local/lib/fcitx5/libtilde.so ]]; then
    installed=true
  fi

  input_state=$(input_method_state)
  input_name=$(input_method_name)
  if [[ -e $disabled_file ]]; then
    enabled=false
  fi
  if [[ $input_state == 2 && $input_name == tilde ]]; then
    active_input=true
  fi

  service_state=$(systemctl --user is-active "$service" 2>/dev/null || true)
  timer_state=$(systemctl --user is-active "$warm_timer" 2>/dev/null || true)

  ollama_json=$(curl -fsS --max-time 1 http://127.0.0.1:11434/api/ps 2>/dev/null || printf '{"models":[]}')
  catalog=$(models_json "$model")

  if [[ -s $log_path ]]; then
    telemetry_enabled=true
    telemetry_json=$(tilde-telemetry-report "$log_path" 2>/dev/null || printf '{}')
  fi

  jq -nc \
    --argjson installed "$installed" \
    --argjson enabled "$enabled" \
    --argjson activeInput "$active_input" \
    --arg inputMethod "$input_name" \
    --arg service "$service_state" \
    --arg warmTimer "$timer_state" \
    --arg model "$model" \
    --argjson context "$context" \
    --argjson telemetryEnabled "$telemetry_enabled" \
    --arg logPath "$log_path" \
    --argjson ollama "$ollama_json" \
    --argjson models "$catalog" \
    --argjson telemetry "$telemetry_json" '
      ($ollama.models // [] | map(select((.name // .model // "") == $model)) | first // null) as $loaded |
      {
        installed: $installed,
        enabled: $enabled,
        active_input: $activeInput,
        input_method: $inputMethod,
        service: $service,
        warm_timer: $warmTimer,
        model: $model,
        model_loaded: ($loaded != null),
        model_size_bytes: ($loaded.size // null),
        model_vram_bytes: ($loaded.size_vram // null),
        context_length: ($loaded.context_length // $context),
        model_id: (($models | map(select(.current)) | first | .id) // "custom"),
        models: $models,
        telemetry_enabled: $telemetryEnabled,
        telemetry_log: $logPath,
        telemetry: $telemetry
      }'
}

models_json() {
  local current_model=${1:-}
  local tags='{"models":[]}'
  tags=$(curl -fsS --max-time 2 http://127.0.0.1:11434/api/tags 2>/dev/null || printf '{"models":[]}')
  model_catalog | jq -c --arg current "$current_model" --argjson tags "$tags" '
    def normalized: sub(":latest$"; "");
    ($tags.models // [] | map((.name // .model // "") | normalized)) as $installed |
    map(. as $entry | . + {
      installed: ($installed | index($entry.model | normalized) != null),
      current: (($entry.model | normalized) == ($current | normalized))
    })'
}

write_model_dropins() {
  local model=$1
  local fim=$2
  local fcitx_tmp warm_tmp
  fcitx_tmp=$(mktemp)
  warm_tmp=$(mktemp)

  printf '[Service]\nEnvironment=TILDE_MODEL=%s\nEnvironment=TILDE_CONTEXT_MODEL=%s\nEnvironment=TILDE_FIM=%s\n' \
    "$model" "$model" "$fim" >"$fcitx_tmp"
  printf '[Service]\nEnvironment=TILDE_MODEL=%s\nEnvironment=TILDE_NUM_CTX=8192\n' \
    "$model" >"$warm_tmp"

  install -Dm644 "$fcitx_tmp" "$fcitx_model_dropin"
  install -Dm644 "$warm_tmp" "$warm_model_dropin"
  rm -f "$fcitx_tmp" "$warm_tmp"
}

warm_selected_model() {
  local model=$1
  local fim=$2
  local prompt='Tilde local autocomplete validation'
  if [[ $fim == 1 ]]; then
    prompt='<|fim_prefix|>Tilde local autocomplete val<|fim_suffix|><|fim_middle|>'
  fi
  local payload
  payload=$(jq -nc --arg model "$model" --arg prompt "$prompt" '{
    model: $model,
    prompt: $prompt,
    raw: true,
    stream: false,
    keep_alive: -1,
    options: {num_ctx: 8192, num_predict: 2, temperature: 0.1}
  }')
  curl --fail --silent --show-error --max-time 300 \
    --request POST http://127.0.0.1:11434/api/generate \
    --header 'Content-Type: application/json' \
    --data "$payload" >/dev/null
}

use_model() {
  local id=$1
  local record model fim old_model fcitx_backup warm_backup
  local had_fcitx_dropin=false had_warm_dropin=false
  record=$(model_record "$id")
  model=$(jq -r '.model' <<<"$record")
  fim=$(jq -r 'if .fim then 1 else 0 end' <<<"$record")
  old_model=$(environment_value TILDE_MODEL "$(service_environment)")

  if ! ollama show "$model" >/dev/null 2>&1; then
    echo "Model is not downloaded: $id" >&2
    return 3
  fi

  warm_selected_model "$model" "$fim"
  fcitx_backup=$(mktemp)
  warm_backup=$(mktemp)
  if [[ -f $fcitx_model_dropin ]]; then
    cp "$fcitx_model_dropin" "$fcitx_backup"
    had_fcitx_dropin=true
  fi
  if [[ -f $warm_model_dropin ]]; then
    cp "$warm_model_dropin" "$warm_backup"
    had_warm_dropin=true
  fi

  write_model_dropins "$model" "$fim"
  if ! systemctl --user daemon-reload ||
     ! systemctl --user restart "$service" ||
     ! systemctl --user restart tilde-model-warm.timer; then
    if [[ $had_fcitx_dropin == true ]]; then
      install -Dm644 "$fcitx_backup" "$fcitx_model_dropin"
    else
      rm -f "$fcitx_model_dropin"
    fi
    if [[ $had_warm_dropin == true ]]; then
      install -Dm644 "$warm_backup" "$warm_model_dropin"
    else
      rm -f "$warm_model_dropin"
    fi
    systemctl --user daemon-reload || true
    systemctl --user restart "$service" || true
    rm -f "$fcitx_backup" "$warm_backup"
    echo "Could not switch models; Tilde restored the previous configuration." >&2
    return 5
  fi
  rm -f "$fcitx_backup" "$warm_backup"

  if [[ -n $old_model && $old_model != "$model" ]]; then
    ollama stop "$old_model" >/dev/null 2>&1 || true
  fi
  printf 'Now using %s\n' "$(jq -r '.label' <<<"$record")"
}

install_model() {
  local id=$1
  local record model
  record=$(model_record "$id")
  model=$(jq -r '.model' <<<"$record")

  if ! ollama show "$model" >/dev/null 2>&1; then
    if ! ollama pull "$model" >/dev/null; then
      if [[ $(jq -r '.requires_terms' <<<"$record") == true ]]; then
        echo "Gemma download needs the Google model terms accepted on Hugging Face." >&2
      fi
      return 4
    fi
  fi
  use_model "$id"
}

show_status() {
  local json=$1
  jq -r '
    "Tilde: " + (if .enabled then "on" else "off" end),
    "Service: " + .service,
    "Model: " + .model,
    "Loaded: " + (if .model_loaded then "yes" else "no" end),
    "Context: " + (.context_length | tostring),
    "Latency p50: " + ((.telemetry.latency_ms.p50 // "n/a") | tostring) + (if .telemetry.latency_ms.p50 then " ms" else "" end)
  ' <<<"$json"
}

enable_tilde() {
  rm -f "$disabled_file"
}

disable_tilde() {
  install -m 600 /dev/null "$disabled_file"
}

command=${1:-status}
shift || true

mkdir -p -m 700 "$state_dir"

case "$command" in
  status)
    status=$(status_json)
    if [[ ${1:-} == --json ]]; then
      printf '%s\n' "$status"
    else
      show_status "$status"
    fi
    ;;
  enable)
    enable_tilde
    ;;
  disable)
    disable_tilde
    ;;
  toggle)
    if [[ -e $disabled_file ]]; then
      enable_tilde
    else
      disable_tilde
    fi
    ;;
  warm)
    systemctl --user start --no-block tilde-model-warm.service
    ;;
  restart)
    systemctl --user restart "$service"
    ;;
  models)
    environment=$(service_environment)
    current_model=$(environment_value TILDE_MODEL "$environment")
    models=$(models_json "$current_model")
    if [[ ${1:-} == --json ]]; then
      printf '%s\n' "$models"
    else
      jq -r '.[] | (if .current then "* " else "  " end) + .label + (if .installed then " · installed" else "" end)' <<<"$models"
    fi
    ;;
  model)
    action=${1:-}
    id=${2:-}
    [[ -n $action && -n $id ]] || { usage >&2; exit 2; }
    case "$action" in
      install) install_model "$id" ;;
      use) use_model "$id" ;;
      *) usage >&2; exit 2 ;;
    esac
    ;;
  doctor)
    status=$(status_json)
    show_status "$status"
    jq -e '.installed and .service == "active" and .model_loaded' <<<"$status" >/dev/null
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
