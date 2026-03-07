#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  translategemma_gpu_translate.sh \
    --model /path/to/model.gguf \
    --source-lang en \
    --target-lang zh \
    --text "Hello world"

Options:
  --model PATH           GGUF model path (required)
  --source-lang CODE     source language code (required)
  --target-lang CODE     target language code (required)
  --text TEXT            text to translate (required)
  --server-bin PATH      llama-server binary path
  --host HOST            server host (default: 127.0.0.1)
  --port PORT            server port (default: 18087)
  --n-predict N          max generated tokens (default: 96)
  --ngl N                GPU layers (default: 999)
  -h, --help             show this help
EOF
}

json_escape() {
  local value="${1-}"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//$'\n'/\\n}"
  value="${value//$'\r'/\\r}"
  printf '%s' "$value"
}

MODEL=""
SOURCE_LANG=""
TARGET_LANG=""
TEXT=""
SERVER_BIN="${LLAMA_SERVER_BIN:-}"
HOST="127.0.0.1"
PORT="18087"
N_PREDICT="96"
NGL="999"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --model)
      MODEL="${2-}"
      shift 2
      ;;
    --source-lang)
      SOURCE_LANG="${2-}"
      shift 2
      ;;
    --target-lang)
      TARGET_LANG="${2-}"
      shift 2
      ;;
    --text)
      TEXT="${2-}"
      shift 2
      ;;
    --server-bin)
      SERVER_BIN="${2-}"
      shift 2
      ;;
    --host)
      HOST="${2-}"
      shift 2
      ;;
    --port)
      PORT="${2-}"
      shift 2
      ;;
    --n-predict)
      N_PREDICT="${2-}"
      shift 2
      ;;
    --ngl)
      NGL="${2-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$MODEL" || -z "$SOURCE_LANG" || -z "$TARGET_LANG" || -z "$TEXT" ]]; then
  echo "Missing required arguments." >&2
  usage >&2
  exit 2
fi

if [[ -z "$SERVER_BIN" ]]; then
  for candidate in \
    "/tmp/llama-old-build/bin/llama-server" \
    "/tmp/llama.cpp-build/bin/llama-server"
  do
    if [[ -x "$candidate" ]]; then
      SERVER_BIN="$candidate"
      break
    fi
  done
fi

if [[ -z "$SERVER_BIN" || ! -x "$SERVER_BIN" ]]; then
  echo "Unable to find executable llama-server. Set --server-bin or LLAMA_SERVER_BIN." >&2
  exit 3
fi

if [[ ! -f "$MODEL" ]]; then
  echo "Model file not found: $MODEL" >&2
  exit 4
fi

LOG_FILE="$(mktemp /tmp/translategemma-gpu-server.XXXXXX)"
SERVER_PID=""

cleanup() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

"$SERVER_BIN" \
  -m "$MODEL" \
  --no-jinja \
  --chat-template chatml \
  --host "$HOST" \
  --port "$PORT" \
  -ngl "$NGL" \
  --no-warmup \
  >"$LOG_FILE" 2>&1 &
SERVER_PID="$!"

READY=0
for _ in $(seq 1 120); do
  if curl -sf "http://$HOST:$PORT/health" >/dev/null; then
    READY=1
    break
  fi
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    break
  fi
  sleep 0.25
done

if [[ "$READY" != "1" ]]; then
  echo "Server failed to start. Log tail:" >&2
  tail -n 80 "$LOG_FILE" >&2 || true
  exit 5
fi

PROMPT="<|im_start|>system
You are a translation engine. Return only translated text.
<|im_end|>
<|im_start|>user
Translate from ${SOURCE_LANG} to ${TARGET_LANG}. Preserve line breaks when possible.

${TEXT}
<|im_end|>
<|im_start|>assistant
"
PROMPT_ESCAPED="$(json_escape "$PROMPT")"
PAYLOAD="{\"prompt\":\"${PROMPT_ESCAPED}\",\"n_predict\":${N_PREDICT},\"temperature\":0,\"stream\":false,\"stop\":[\"<|im_end|>\"]}"

RESPONSE="$(curl -sS "http://$HOST:$PORT/completion" \
  -H 'Content-Type: application/json' \
  -d "$PAYLOAD")"

if command -v jq >/dev/null 2>&1; then
  printf '%s\n' "$RESPONSE" | jq -r '.content'
else
  printf '%s\n' "$RESPONSE"
fi
