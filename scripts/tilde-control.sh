#!/usr/bin/env bash
set -euo pipefail

state_dir=${XDG_STATE_HOME:-$HOME/.local/state}/tilde
log_path=${TILDE_LOG_PATH:-$state_dir/events.jsonl}
disabled_file=${TILDE_DISABLED_FILE:-$state_dir/disabled}
service=omarchy-fcitx5.service
warm_timer=tilde-model-warm.timer
default_model=qwen2.5-coder:1.5b-base

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
  doctor           Run a concise local health check
EOF
}

service_environment() {
  systemctl --user show "$service" --property=Environment --value 2>/dev/null || true
}

environment_value() {
  local key=$1
  local environment=$2
  tr ' ' '\n' <<<"$environment" | sed -n "s/^${key}=//p" | tail -n 1
}

input_method_state() {
  fcitx5-remote 2>/dev/null || true
}

input_method_name() {
  fcitx5-remote -n 2>/dev/null || true
}

status_json() {
  local environment model context input_state input_name service_state timer_state
  local installed=false enabled=true active_input=false telemetry_enabled=false
  local ollama_json='{"models":[]}' telemetry_json='{}'

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
        telemetry_enabled: $telemetryEnabled,
        telemetry_log: $logPath,
        telemetry: $telemetry
      }'
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
