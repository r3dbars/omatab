#!/usr/bin/env bash
set -euo pipefail

state_dir=${XDG_STATE_HOME:-$HOME/.local/state}/omatab
log_path=${OMATAB_LOG_PATH:-$state_dir/events.jsonl}
disabled_file=${OMATAB_DISABLED_FILE:-$state_dir/disabled}
service=omarchy-fcitx5.service
warm_timer=omatab-model-warm.timer
default_model=hf.co/mradermacher/Qwen3.5-4B-Base-GGUF:Q8_0
default_model_id=qwen-balanced
config_file=${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/conf/omatab.conf
addon_dropin=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/omarchy-fcitx5.service.d/10-omatab-addon-path.conf
fcitx_dropin_dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/omarchy-fcitx5.service.d
warm_dropin_dir=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/omatab-model-warm.service.d
fcitx_model_dropin=$fcitx_dropin_dir/30-omatab-selected-model.conf
warm_model_dropin=$warm_dropin_dir/30-omatab-selected-model.conf

usage() {
  cat <<'EOF'
Usage: omatab <command> [options]

Every command that reports something accepts --json for machine-readable
output, and every command exits non-zero on failure.

Commands:
  status [--json]         Installation, model, settings, and quality status
  enable | disable        Turn suggestions on or off globally, instantly
  toggle                  Flip suggestions on or off
  telemetry [on|off]      Show or set the private local suggestion log
  ocr [on|off]            Show or set screen-text (OCR) context capture
  key full-accept [KEY..] Show or set the key(s) that accept a whole suggestion
                          (Tab always accepts one word). Examples: Shift+Tab,
                          or several: grave asciitilde
  config [--json]         Show all settings
  warm                    Warm the configured model without blocking
  restart                 Restart the Fcitx service that hosts Oma Tab
  models [--json]         List supported model choices
  model install ID        Download, verify, and use a supported model
  model use ID            Use an already-downloaded supported model
  doctor [--json]         Health check; exit 0 only when fully working
  demo [--port N]         Open the local playground page for trying it out
  update                  Pull the latest source and rebuild, in place
  uninstall [--purge]     Remove Oma Tab from this user account
                          (--purge also deletes settings and the telemetry log)
  commands [--json]       List commands for agents and shell completion
  help                    Show this text

Settings live in ~/.config/fcitx5/conf/omatab.conf and apply live.
EOF
}

commands_json() {
  jq -nc '[
    {command: "status", args: ["--json"], mutates: false, description: "Installation, model, settings, and quality status"},
    {command: "enable", args: [], mutates: true, description: "Turn suggestions on globally"},
    {command: "disable", args: [], mutates: true, description: "Turn suggestions off globally"},
    {command: "toggle", args: [], mutates: true, description: "Flip suggestions on or off"},
    {command: "telemetry", args: ["on|off", "--json"], mutates: true, description: "Show or set the private local suggestion log"},
    {command: "ocr", args: ["on|off", "--json"], mutates: true, description: "Show or set screen-text (OCR) context capture"},
    {command: "key", args: ["full-accept", "KEY...", "--json"], mutates: true, description: "Show or set the whole-suggestion accept key(s)"},
    {command: "config", args: ["--json"], mutates: false, description: "Show all settings"},
    {command: "warm", args: [], mutates: false, description: "Warm the configured model"},
    {command: "restart", args: [], mutates: true, description: "Restart the Fcitx service"},
    {command: "models", args: ["--json"], mutates: false, description: "List supported model choices"},
    {command: "model", args: ["install|use", "ID"], mutates: true, description: "Download and/or switch to a supported model"},
    {command: "doctor", args: ["--json"], mutates: false, description: "Health check with non-zero exit on problems"},
    {command: "demo", args: ["--port N", "--no-open"], mutates: false, description: "Serve the local playground page on loopback and open it"},
    {command: "update", args: [], mutates: true, description: "Pull the latest source and rebuild in place"},
    {command: "uninstall", args: ["--purge", "--json"], mutates: true, description: "Remove Oma Tab from this user account"},
    {command: "commands", args: ["--json"], mutates: false, description: "This list"}
  ]'
}

# ---- settings file (Fcitx ini, flat keys) ----

# Fcitx writes scalar options as top-level KEY=VALUE lines and list options
# (such as key bindings) as a section: [FullAcceptKey] with 0=Shift+Tab.
config_value() {
  local key=$1 fallback=$2
  local value=
  if [[ -f $config_file ]]; then
    value=$(sed -n '/^\[/q; s/^'"$key"'=//p' "$config_file" | tail -n 1)
  fi
  printf '%s' "${value:-$fallback}"
}

# Prints the keys of a list option, space separated (0=grave 1=asciitilde).
config_key_value() {
  local section=$1 fallback=$2
  local value=
  if [[ -f $config_file ]]; then
    value=$(awk -v section="[$section]" '
      $0 == section {inside = 1; next}
      /^\[/ {inside = 0}
      inside && sub(/^[0-9]+=/, "") {printf "%s%s", sep, $0; sep = " "}' "$config_file")
  fi
  printf '%s' "${value:-$fallback}"
}

config_bool() {
  local key=$1 fallback=$2
  local value
  value=$(config_value "$key" "$fallback")
  [[ ${value,,} == true ]] && echo true || echo false
}

reload_addon_config() {
  fcitx_bus_available || return 0
  busctl --user call org.fcitx.Fcitx5 /controller org.fcitx.Fcitx.Controller1 \
    ReloadAddonConfig s omatab >/dev/null 2>&1 || true
}

# Rewrites the whole settings file from the current values with one override.
write_setting() {
  local key=$1 value=$2
  local telemetry ocr full_accept
  telemetry=$(config_value Telemetry False)
  ocr=$(config_value ScreenContext False)
  full_accept=$(config_key_value FullAcceptKey Shift+Tab)
  case "$key" in
    Telemetry) telemetry=$value ;;
    ScreenContext) ocr=$value ;;
    FullAcceptKey) full_accept=$value ;;
  esac
  mkdir -p "$(dirname "$config_file")"
  {
    printf 'ScreenContext=%s\nTelemetry=%s\n\n[FullAcceptKey]\n' "$ocr" "$telemetry"
    local index=0
    for key in $full_accept; do
      printf '%d=%s\n' "$index" "$key"
      index=$((index + 1))
    done
  } | install -m 600 /dev/stdin "$config_file"
  reload_addon_config
}

parse_switch() {
  case "${1,,}" in
    on|true|1|yes) echo True ;;
    off|false|0|no) echo False ;;
    *) echo "Expected on or off, got: $1" >&2; return 2 ;;
  esac
}

config_json() {
  jq -nc \
    --arg file "$config_file" \
    --argjson telemetry "$(config_bool Telemetry false)" \
    --argjson ocr "$(config_bool ScreenContext false)" \
    --arg fullAccept "$(config_key_value FullAcceptKey Shift+Tab)" \
    --arg logPath "$log_path" \
    '{config_file: $file, telemetry: $telemetry, ocr: $ocr,
      word_accept_key: "Tab", full_accept_keys: ($fullAccept | split(" ")),
      telemetry_log: $logPath}'
}

setting_command() {
  # setting_command KEY LABEL [on|off] [--json]
  local key=$1 label=$2; shift 2
  local value= json=false
  for arg in "$@"; do
    case "$arg" in
      --json) json=true ;;
      *) value=$arg ;;
    esac
  done
  if [[ -n $value ]]; then
    local normalized
    normalized=$(parse_switch "$value") || return 2
    write_setting "$key" "$normalized"
  fi
  local current
  current=$(config_bool "$key" false)
  if [[ $json == true ]]; then
    jq -nc --arg key "$key" --argjson enabled "$current" '{setting: $key, enabled: $enabled}'
  else
    printf '%s: %s\n' "$label" "$([[ $current == true ]] && echo on || echo off)"
  fi
}

key_command() {
  local which=${1:-}; shift || true
  [[ $which == full-accept ]] || { echo "Only the full-accept key is configurable; Tab always accepts one word." >&2; return 2; }
  local keys=() json=false
  for arg in "$@"; do
    case "$arg" in
      --json) json=true ;;
      *) keys+=("$arg") ;;
    esac
  done
  if ((${#keys[@]} > 0)); then
    for key in "${keys[@]}"; do
      if [[ ! $key =~ ^((Control|Ctrl|Shift|Alt|Super|Hyper)\+)*[A-Za-z0-9_]+$ ]]; then
        echo "Key must look like Shift+Tab, Control+Return, grave, or asciitilde" >&2
        return 2
      fi
    done
    write_setting FullAcceptKey "${keys[*]}"
  fi
  local current
  current=$(config_key_value FullAcceptKey Shift+Tab)
  if [[ $json == true ]]; then
    jq -nc --arg keys "$current" '{word_accept_key: "Tab", full_accept_keys: ($keys | split(" "))}'
  else
    printf 'Word accept: Tab\nFull accept: %s\n' "$current"
  fi
}

uninstall_omatab() {
  local purge=false json=false
  for arg in "$@"; do
    case "$arg" in
      --purge) purge=true ;;
      --json) json=true ;;
    esac
  done
  local removed=()
  systemctl --user disable --now "$warm_timer" >/dev/null 2>&1 || true
  local paths=(
    "$HOME/.local/lib/fcitx5/libomatab.so"
    "$HOME/.local/share/fcitx5/addon/omatab.conf"
    "$HOME/.local/bin/omatab-telemetry-report"
    "$HOME/.local/libexec/omatab-warm-model"
    "$HOME/.local/share/systemd/user/omatab-model-warm.service"
    "$HOME/.local/share/systemd/user/omatab-model-warm.timer"
    "$addon_dropin"
    "$fcitx_model_dropin"
    "$warm_dropin_dir"
  )
  if [[ $purge == true ]]; then
    paths+=("$config_file" "$state_dir")
  fi
  for path in "${paths[@]}"; do
    if [[ -e $path ]]; then
      rm -rf "$path"
      removed+=("$path")
    fi
  done
  rmdir "$fcitx_dropin_dir" 2>/dev/null || true
  systemctl --user daemon-reload || true
  systemctl --user restart "$service" >/dev/null 2>&1 || true
  # Remove this script last so the loop above could still use its helpers.
  local self="$HOME/.local/bin/omatab"
  if [[ -e $self ]]; then
    rm -f "$self"
    removed+=("$self")
  fi
  if [[ $json == true ]]; then
    printf '%s\n' "${removed[@]}" | jq -Rsc --argjson purge "$purge" \
      '{uninstalled: true, purged: $purge, removed: (split("\n") | map(select(length > 0)))}'
  else
    printf 'Removed Oma Tab (%d paths).\n' "${#removed[@]}"
    if [[ $purge != true ]]; then
      echo "Settings and telemetry were kept; rerun with --purge to delete them."
    fi
    echo "Remove the Oma Tab entry from your Fcitx input-method group if it is still listed."
  fi
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
    || { echo "Unknown Oma Tab model: $id" >&2; return 2; }
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

# The bootstrap script records its current stage; "running" is true only
# while that script is still alive, so a stale file never shows as progress.
setup_progress_json() {
  local file=$state_dir/setup.json pid running=false
  [[ -s $file ]] || { printf 'null'; return; }
  pid=$(jq -r '.pid // 0' "$file" 2>/dev/null || echo 0)
  if [[ $pid =~ ^[0-9]+$ ]] && ((pid > 0)) && kill -0 "$pid" 2>/dev/null; then
    running=true
  fi
  jq -c --argjson running "$running" '{stage, detail, updated, running: $running}' "$file" 2>/dev/null ||
    printf 'null'
}

status_json() {
  local environment model context input_state input_name service_state timer_state
  local installed=false enabled=true active_input=false telemetry_enabled=false
  local ollama_json='{"models":[]}' telemetry_json='{}' catalog='[]'

  environment=$(service_environment)
  model=$(environment_value OMATAB_MODEL "$environment")
  context=$(environment_value OMATAB_NUM_CTX "$environment")
  [[ -n $model ]] || model=$default_model
  [[ $context =~ ^[0-9]+$ ]] || context=8192

  if [[ -f $HOME/.local/share/fcitx5/addon/omatab.conf ]] &&
     [[ -f $HOME/.local/lib/fcitx5/libomatab.so ]]; then
    installed=true
  fi

  input_state=$(input_method_state)
  input_name=$(input_method_name)
  if [[ -e $disabled_file ]]; then
    enabled=false
  fi
  if [[ $input_state == 2 && $input_name == omatab ]]; then
    active_input=true
  fi

  service_state=$(systemctl --user is-active "$service" 2>/dev/null || true)
  timer_state=$(systemctl --user is-active "$warm_timer" 2>/dev/null || true)

  ollama_json=$(curl -fsS --max-time 1 http://127.0.0.1:11434/api/ps 2>/dev/null || printf '{"models":[]}')
  catalog=$(models_json "$model")

  setup_json=$(setup_progress_json)
  source_dir=$(cat "$state_dir/source_dir" 2>/dev/null || true)

  telemetry_enabled=$(config_bool Telemetry false)
  if [[ -s $log_path ]]; then
    telemetry_json=$(omatab-telemetry-report "$log_path" 2>/dev/null || printf '{}')
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
    --argjson telemetry "$telemetry_json" \
    --argjson settings "$(config_json)" \
    --argjson setup "$setup_json" \
    --arg sourceDir "$source_dir" '
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
        settings: $settings,
        telemetry_enabled: $telemetryEnabled,
        telemetry_log: $logPath,
        telemetry: $telemetry,
        setup: $setup,
        source_dir: (if $sourceDir == "" then null else $sourceDir end)
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

  printf '[Service]\nEnvironment=OMATAB_MODEL=%s\nEnvironment=OMATAB_CONTEXT_MODEL=%s\nEnvironment=OMATAB_FIM=%s\n' \
    "$model" "$model" "$fim" >"$fcitx_tmp"
  printf '[Service]\nEnvironment=OMATAB_MODEL=%s\nEnvironment=OMATAB_NUM_CTX=8192\n' \
    "$model" >"$warm_tmp"

  install -Dm644 "$fcitx_tmp" "$fcitx_model_dropin"
  install -Dm644 "$warm_tmp" "$warm_model_dropin"
  rm -f "$fcitx_tmp" "$warm_tmp"
}

warm_selected_model() {
  local model=$1
  local fim=$2
  local prompt='Oma Tab local autocomplete validation'
  if [[ $fim == 1 ]]; then
    prompt='<|fim_prefix|>Oma Tab local autocomplete val<|fim_suffix|><|fim_middle|>'
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
  old_model=$(environment_value OMATAB_MODEL "$(service_environment)")

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
     ! systemctl --user restart omatab-model-warm.timer; then
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
    echo "Could not switch models; Oma Tab restored the previous configuration." >&2
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
    "Oma Tab: " + (if .enabled then "on" else "off" end),
    "Service: " + .service,
    "Model: " + .model,
    "Loaded: " + (if .model_loaded then "yes" else "no" end),
    "Context: " + (.context_length | tostring),
    "OCR context: " + (if .settings.ocr then "on" else "off" end),
    "Telemetry: " + (if .settings.telemetry then "on" else "off" end),
    "Keys: Tab = word, " + (.settings.full_accept_keys | join(" or ")) + " = full",
    "Latency p50: " + ((.telemetry.latency_ms.p50 // "n/a") | tostring) + (if .telemetry.latency_ms.p50 then " ms" else "" end)
  ' <<<"$json"
}

enable_omatab() {
  rm -f "$disabled_file"
}

disable_omatab() {
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
    enable_omatab
    ;;
  disable)
    disable_omatab
    ;;
  toggle)
    if [[ -e $disabled_file ]]; then
      enable_omatab
    else
      disable_omatab
    fi
    ;;
  telemetry)
    setting_command Telemetry "Telemetry" "$@"
    ;;
  ocr)
    setting_command ScreenContext "OCR context" "$@"
    ;;
  key)
    key_command "$@"
    ;;
  config)
    if [[ ${1:-} == --json ]]; then
      config_json
    else
      config_json | jq -r 'to_entries[] | "\(.key): \(.value)"'
    fi
    ;;
  uninstall)
    uninstall_omatab "$@"
    ;;
  commands)
    if [[ ${1:-} == --json ]]; then
      commands_json
    else
      commands_json | jq -r '.[] | .command'
    fi
    ;;
  warm)
    systemctl --user start --no-block omatab-model-warm.service
    ;;
  restart)
    systemctl --user restart "$service"
    ;;
  models)
    environment=$(service_environment)
    current_model=$(environment_value OMATAB_MODEL "$environment")
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
    healthy=$(jq -c '{installed, service_active: (.service == "active"), model_loaded, healthy: (.installed and .service == "active" and .model_loaded)}' <<<"$status")
    if [[ ${1:-} == --json ]]; then
      printf '%s\n' "$healthy"
    else
      show_status "$status"
    fi
    jq -e '.healthy' <<<"$healthy" >/dev/null
    ;;
  update)
    source_dir=$(cat "$state_dir/source_dir" 2>/dev/null || true)
    [[ -n $source_dir ]] || source_dir=$HOME/.local/src/omatab
    if [[ ! -f $source_dir/scripts/bootstrap.sh ]]; then
      echo "Source checkout not found at $source_dir; clone https://github.com/r3dbars/tilde-linux there first." >&2
      exit 3
    fi
    if [[ -d $source_dir/.git ]]; then
      git -C "$source_dir" pull --ff-only
    fi
    exec "$source_dir/scripts/bootstrap.sh"
    ;;
  demo)
    demo_dir=$HOME/.local/share/omatab/demo
    [[ -f $demo_dir/serve.py ]] || { echo "Demo files are not installed; rerun the installer." >&2; exit 3; }
    port=8765
    open_flag=--open
    while (($# > 0)); do
      case "$1" in
        --port) port=${2:-8765}; shift 2 ;;
        --no-open) open_flag=; shift ;;
        *) shift ;;
      esac
    done
    exec python3 "$demo_dir/serve.py" --port "$port" $open_flag
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
