#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  translategemma_regression.sh --model /path/to/model.gguf [options]

Purpose:
  Run a regression suite for TranslateGemma + llamafile packaging:
  1) dev binary text/image translation
  2) packaged .llamafile text/image translation (implicit and explicit /zip)
  3) --translate-messages-json text/image (local path + data URI)
  4) optional dev + packaged server smoke tests (/health, /v1/models, /completion, /v1/chat/completions)

Options:
  --model PATH              GGUF model path (required)
  --mmproj PATH             vision model path (optional; required for image cases)
  --image PATH              image path for image/messages-image cases (optional)
  --repo-root PATH          llamafile repo root (default: script_dir/..)
  --llamafile-bin PATH      dev binary path (default: <repo>/o//llamafile/llamafile)
  --convert-bin PATH        llamafile-convert path (default: <repo>/build/llamafile-convert)
  --zipalign-bin PATH       zipalign path (default: <repo>/o/third_party/zipalign/zipalign)
  --packaged PATH           existing packaged .llamafile (skip auto-pack step)
  --source-lang CODE        source language (default: en)
  --target-lang CODE        target language (default: zh)
  --text TEXT               text for regression prompt (default: Hello world)
  --expect SUBSTR           expected substring in translation output (optional)
  --n-predict N             max generated tokens (default: 8)
  --runs N                  repeat count for packaged implicit path (default: 2)
  --server-smoke            run optional server smoke tests for TranslateGemma HTTP integration
  --timeout-sec N           timeout for each run (default: 120)
  --require-metal yes|no    require Metal backend evidence in stderr (default: yes on macOS)
  --keep-artifacts          keep temporary packaging directory
  -h, --help                show help
USAGE
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

find_timeout_bin() {
  local candidate=""
  for candidate in /opt/homebrew/bin/timeout timeout gtimeout; do
    if command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

strip_cr() {
  tr -d '\r'
}

json_escape_inline() {
  local value="${1-}"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//$'\n'/\\n}"
  value="${value//$'\r'/\\r}"
  printf '%s' "$value"
}

mime_from_path() {
  case "${1##*.}" in
    jpg|jpeg|JPG|JPEG) echo "image/jpeg" ;;
    png|PNG) echo "image/png" ;;
    webp|WEBP) echo "image/webp" ;;
    gif|GIF) echo "image/gif" ;;
    *) echo "application/octet-stream" ;;
  esac
}

image_to_data_uri() {
  local path="$1"
  local mime
  mime="$(mime_from_path "$path")"
  local b64
  b64="$(base64 <"$path" | tr -d '\n')"
  printf 'data:%s;base64,%s' "$mime" "$b64"
}

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODEL=""
MMPROJ=""
IMAGE=""
LLAMAFILE_BIN=""
CONVERT_BIN=""
ZIPALIGN_BIN=""
PACKAGED=""
SOURCE_LANG="en"
TARGET_LANG="zh"
TEXT="Hello world"
EXPECT_SUBSTR=""
N_PREDICT="8"
RUNS="2"
RUN_SERVER_SMOKE="0"
TIMEOUT_SEC="120"
KEEP_ARTIFACTS="0"
REQUIRE_METAL="auto"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --model) MODEL="${2-}"; shift 2 ;;
    --mmproj) MMPROJ="${2-}"; shift 2 ;;
    --image) IMAGE="${2-}"; shift 2 ;;
    --repo-root) REPO_ROOT="${2-}"; shift 2 ;;
    --llamafile-bin) LLAMAFILE_BIN="${2-}"; shift 2 ;;
    --convert-bin) CONVERT_BIN="${2-}"; shift 2 ;;
    --zipalign-bin) ZIPALIGN_BIN="${2-}"; shift 2 ;;
    --packaged) PACKAGED="${2-}"; shift 2 ;;
    --source-lang) SOURCE_LANG="${2-}"; shift 2 ;;
    --target-lang) TARGET_LANG="${2-}"; shift 2 ;;
    --text) TEXT="${2-}"; shift 2 ;;
    --expect) EXPECT_SUBSTR="${2-}"; shift 2 ;;
    --n-predict) N_PREDICT="${2-}"; shift 2 ;;
    --runs) RUNS="${2-}"; shift 2 ;;
    --server-smoke) RUN_SERVER_SMOKE="1"; shift ;;
    --timeout-sec) TIMEOUT_SEC="${2-}"; shift 2 ;;
    --require-metal) REQUIRE_METAL="${2-}"; shift 2 ;;
    --keep-artifacts) KEEP_ARTIFACTS="1"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[[ -n "$MODEL" ]] || die "--model is required"
[[ -f "$MODEL" ]] || die "model file not found: $MODEL"
if [[ -n "$MMPROJ" ]]; then
  [[ -f "$MMPROJ" ]] || die "mmproj file not found: $MMPROJ"
fi
if [[ -n "$IMAGE" ]]; then
  [[ -f "$IMAGE" ]] || die "image file not found: $IMAGE"
  [[ -n "$MMPROJ" ]] || die "--image requires --mmproj"
fi
[[ "$RUNS" =~ ^[0-9]+$ ]] || die "--runs must be an integer"
[[ "$TIMEOUT_SEC" =~ ^[0-9]+$ ]] || die "--timeout-sec must be an integer"

if [[ -z "$LLAMAFILE_BIN" ]]; then
  LLAMAFILE_BIN="$REPO_ROOT/o//llamafile/llamafile"
fi
[[ -x "$LLAMAFILE_BIN" ]] || die "llamafile binary not executable: $LLAMAFILE_BIN"

if [[ -z "$CONVERT_BIN" ]]; then
  CONVERT_BIN="$REPO_ROOT/build/llamafile-convert"
fi

if [[ -z "$ZIPALIGN_BIN" ]]; then
  ZIPALIGN_BIN="$REPO_ROOT/o/third_party/zipalign/zipalign"
fi

if [[ "$REQUIRE_METAL" == "auto" ]]; then
  if [[ "$(uname -s)" == "Darwin" ]]; then
    REQUIRE_METAL="yes"
  else
    REQUIRE_METAL="no"
  fi
fi

TIMEOUT_BIN="$(find_timeout_bin || true)"

TMP_DIR=""
if [[ -z "$PACKAGED" ]]; then
  [[ -x "$CONVERT_BIN" ]] || die "llamafile-convert not executable: $CONVERT_BIN"
  [[ -x "$ZIPALIGN_BIN" ]] || die "zipalign not executable: $ZIPALIGN_BIN"
  TMP_DIR="$(mktemp -d /tmp/translategemma-regression.XXXXXX)"
  if [[ "$KEEP_ARTIFACTS" != "1" ]]; then
    trap 'rm -rf "$TMP_DIR"' EXIT
  fi
  (
    cd "$TMP_DIR"
    PATH="$(dirname "$LLAMAFILE_BIN"):$(dirname "$ZIPALIGN_BIN"):$PATH" \
      "$CONVERT_BIN" "$MODEL" >/dev/null
  )
  PACKAGED="$TMP_DIR/$(basename "${MODEL%.gguf}.llamafile")"
  if [[ -n "$MMPROJ" ]]; then
    "$ZIPALIGN_BIN" -j0 "$PACKAGED" "$MMPROJ" >/dev/null
  fi
fi

[[ -x "$PACKAGED" ]] || die "packaged llamafile not executable: $PACKAGED"

PASS_COUNT=0
FAIL_COUNT=0

run_translation_case() {
  local name="$1"
  shift
  local stdout_file stderr_file rc output_text
  stdout_file="$(mktemp /tmp/${name}.stdout.XXXXXX)"
  stderr_file="$(mktemp /tmp/${name}.stderr.XXXXXX)"

  if [[ -n "$TIMEOUT_BIN" ]]; then
    if "$TIMEOUT_BIN" "$TIMEOUT_SEC" "$@" >"$stdout_file" 2>"$stderr_file"; then
      rc=0
    else
      rc=$?
    fi
  else
    if "$@" >"$stdout_file" 2>"$stderr_file"; then
      rc=0
    else
      rc=$?
    fi
  fi

  if [[ "$rc" -ne 0 ]]; then
    echo "FAIL [$name]: exit code $rc" >&2
    tail -n 60 "$stderr_file" >&2 || true
    FAIL_COUNT=$((FAIL_COUNT + 1))
    rm -f "$stdout_file" "$stderr_file"
    return
  fi

  if grep -Eqi 'failed to load model|error loading model|failed to open .*gguf|^error:' "$stderr_file"; then
    echo "FAIL [$name]: detected load error in stderr" >&2
    tail -n 60 "$stderr_file" >&2 || true
    FAIL_COUNT=$((FAIL_COUNT + 1))
    rm -f "$stdout_file" "$stderr_file"
    return
  fi

  if [[ "$REQUIRE_METAL" == "yes" ]]; then
    if ! grep -Eq 'registered backend MTL|ggml_metal_device_init' "$stderr_file"; then
      echo "FAIL [$name]: Metal backend evidence not found in stderr" >&2
      tail -n 60 "$stderr_file" >&2 || true
      FAIL_COUNT=$((FAIL_COUNT + 1))
      rm -f "$stdout_file" "$stderr_file"
      return
    fi
  fi

  output_text="$(awk 'NF { line=$0 } END { print line }' "$stdout_file" | strip_cr)"
  if [[ -z "$output_text" ]]; then
    echo "FAIL [$name]: empty translation output" >&2
    tail -n 40 "$stdout_file" >&2 || true
    FAIL_COUNT=$((FAIL_COUNT + 1))
    rm -f "$stdout_file" "$stderr_file"
    return
  fi

  if [[ -n "$EXPECT_SUBSTR" ]] && [[ "$output_text" != *"$EXPECT_SUBSTR"* ]]; then
    echo "FAIL [$name]: output '$output_text' does not include '$EXPECT_SUBSTR'" >&2
    FAIL_COUNT=$((FAIL_COUNT + 1))
    rm -f "$stdout_file" "$stderr_file"
    return
  fi

  echo "PASS [$name]: $output_text"
  PASS_COUNT=$((PASS_COUNT + 1))
  rm -f "$stdout_file" "$stderr_file"
}

SERVER_PORT_NEXT=18090

stop_server() {
  local pid="$1"
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 20); do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      sleep 0.25
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null || true
    fi
  fi
  wait "$pid" >/dev/null 2>&1 || true
}

extract_completion_content() {
  local path="$1"
  if command -v jq >/dev/null 2>&1; then
    jq -r '.content // empty' "$path"
    return
  fi
  python3 - "$path" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)
print(data.get("content", ""))
PY
}

extract_chat_content() {
  local path="$1"
  if command -v jq >/dev/null 2>&1; then
    jq -r '.choices[0].message.content // empty' "$path"
    return
  fi
  python3 - "$path" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    data = json.load(fh)
choices = data.get("choices") or []
message = choices[0].get("message", {}) if choices else {}
print(message.get("content", ""))
PY
}

run_server_case() {
  local name="$1"
  shift

  local port="$SERVER_PORT_NEXT"
  SERVER_PORT_NEXT=$((SERVER_PORT_NEXT + 1))

  local log_file body_file http_code completion_text chat_text
  log_file="$(mktemp /tmp/${name}.server.XXXXXX)"
  body_file="$(mktemp /tmp/${name}.body.XXXXXX)"

  "$@" \
    --server \
    --no-warmup \
    --host 127.0.0.1 \
    --port "$port" \
    >"$log_file" 2>&1 &
  local server_pid="$!"

  local ready=0
  for _ in $(seq 1 240); do
    if curl -sf "http://127.0.0.1:$port/health" >/dev/null; then
      ready=1
      break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
      break
    fi
    sleep 0.25
  done

  if [[ "$ready" != "1" ]]; then
    echo "FAIL [$name]: server failed to start" >&2
    tail -n 80 "$log_file" >&2 || true
    FAIL_COUNT=$((FAIL_COUNT + 1))
    stop_server "$server_pid"
    rm -f "$log_file" "$body_file"
    return
  fi

  if [[ "$REQUIRE_METAL" == "yes" ]]; then
    if ! grep -Eq 'registered backend MTL|ggml_metal_device_init' "$log_file"; then
      echo "FAIL [$name]: Metal backend evidence not found in server log" >&2
      tail -n 80 "$log_file" >&2 || true
      FAIL_COUNT=$((FAIL_COUNT + 1))
      stop_server "$server_pid"
      rm -f "$log_file" "$body_file"
      return
    fi
  fi

  http_code="$(curl -sS -o "$body_file" -w '%{http_code}' "http://127.0.0.1:$port/v1/models" || true)"
  if [[ "$http_code" != "200" ]] || ! grep -q '"models"' "$body_file"; then
    echo "FAIL [$name]: /v1/models returned HTTP $http_code" >&2
    cat "$body_file" >&2 || true
    FAIL_COUNT=$((FAIL_COUNT + 1))
    stop_server "$server_pid"
    rm -f "$log_file" "$body_file"
    return
  fi

  local completion_payload
  completion_payload="{\"prompt\":\"$(json_escape_inline "Say hello briefly.")\",\"n_predict\":${N_PREDICT},\"temperature\":0,\"stream\":false}"
  http_code="$(curl -sS -o "$body_file" -w '%{http_code}' \
    -H 'Content-Type: application/json' \
    -d "$completion_payload" \
    "http://127.0.0.1:$port/completion" || true)"
  completion_text="$(extract_completion_content "$body_file" | strip_cr)"
  if [[ "$http_code" != "200" ]] || [[ -z "$completion_text" ]]; then
    echo "FAIL [$name]: /completion returned HTTP $http_code with empty content" >&2
    cat "$body_file" >&2 || true
    FAIL_COUNT=$((FAIL_COUNT + 1))
    stop_server "$server_pid"
    rm -f "$log_file" "$body_file"
    return
  fi

  local chat_payload
  chat_payload="{\"messages\":[{\"role\":\"user\",\"content\":\"Reply with a short greeting.\"}],\"max_tokens\":${N_PREDICT},\"temperature\":0,\"stream\":false}"
  http_code="$(curl -sS -o "$body_file" -w '%{http_code}' \
    -H 'Content-Type: application/json' \
    -d "$chat_payload" \
    "http://127.0.0.1:$port/v1/chat/completions" || true)"
  chat_text="$(extract_chat_content "$body_file" | strip_cr)"
  if [[ "$http_code" != "200" ]] || [[ -z "$chat_text" ]]; then
    echo "FAIL [$name]: /v1/chat/completions returned HTTP $http_code with empty content" >&2
    cat "$body_file" >&2 || true
    FAIL_COUNT=$((FAIL_COUNT + 1))
    stop_server "$server_pid"
    rm -f "$log_file" "$body_file"
    return
  fi

  local has_mmproj_arg=0
  local arg=""
  for arg in "$@"; do
    if [[ "$arg" == "--mmproj" ]] || [[ "$arg" == --mmproj=* ]]; then
      has_mmproj_arg=1
      break
    fi
  done

  if [[ "$has_mmproj_arg" == "1" ]] && [[ -n "$IMAGE" ]]; then
    local image_data_uri image_payload
    image_data_uri="$(image_to_data_uri "$IMAGE")"
    image_payload="{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Describe this image briefly.\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"$(json_escape_inline "$image_data_uri")\"}}]}],\"max_tokens\":${N_PREDICT},\"temperature\":0,\"stream\":false}"
    http_code="$(curl -sS -o "$body_file" -w '%{http_code}' \
      -H 'Content-Type: application/json' \
      -d "$image_payload" \
      "http://127.0.0.1:$port/v1/chat/completions" || true)"
    chat_text="$(extract_chat_content "$body_file" | strip_cr)"
    if [[ "$http_code" != "200" ]] || [[ -z "$chat_text" ]]; then
      echo "FAIL [$name]: multimodal /v1/chat/completions returned HTTP $http_code with empty content" >&2
      cat "$body_file" >&2 || true
      FAIL_COUNT=$((FAIL_COUNT + 1))
      stop_server "$server_pid"
      rm -f "$log_file" "$body_file"
      return
    fi
  fi

  echo "PASS [$name]: completion/chat routes responded"
  PASS_COUNT=$((PASS_COUNT + 1))
  stop_server "$server_pid"
  rm -f "$log_file" "$body_file"
}

MODEL_BASENAME="$(basename "$MODEL")"
MMPROJ_BASENAME="$(basename "$MMPROJ")"

echo "=== TranslateGemma regression ==="
echo "model:        $MODEL"
if [[ -n "$MMPROJ" ]]; then
  echo "mmproj:       $MMPROJ"
fi
if [[ -n "$IMAGE" ]]; then
  echo "image:        $IMAGE"
fi
echo "dev binary:   $LLAMAFILE_BIN"
echo "packaged:     $PACKAGED"
echo "requireMetal: $REQUIRE_METAL"
echo "serverSmoke:  $RUN_SERVER_SMOKE"
echo

if [[ "$RUN_SERVER_SMOKE" != "1" ]]; then
  echo "note: server smoke is skipped by default because it launches long-running local HTTP server processes"
  echo
fi

run_translation_case dev-text \
  "$LLAMAFILE_BIN" \
  -m "$MODEL" \
  --translate-text "$TEXT" \
  --source-lang "$SOURCE_LANG" \
  --target-lang "$TARGET_LANG" \
  -n "$N_PREDICT"

for i in $(seq 1 "$RUNS"); do
  run_translation_case "packaged-implicit-text-${i}" \
    "$PACKAGED" \
    --translate-text "$TEXT" \
    --source-lang "$SOURCE_LANG" \
    --target-lang "$TARGET_LANG" \
    -n "$N_PREDICT"
done

run_translation_case packaged-explicit-text-zip \
  "$PACKAGED" \
  -m "/zip/$MODEL_BASENAME" \
  --translate-text "$TEXT" \
  --source-lang "$SOURCE_LANG" \
  --target-lang "$TARGET_LANG" \
  -n "$N_PREDICT"

run_translation_case packaged-explicit-text-abs \
  "$PACKAGED" \
  -m "$MODEL" \
  --translate-text "$TEXT" \
  --source-lang "$SOURCE_LANG" \
  --target-lang "$TARGET_LANG" \
  -n "$N_PREDICT"

MSG_TEXT_PAYLOAD="[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"source_lang_code\":\"$(json_escape_inline "$SOURCE_LANG")\",\"target_lang_code\":\"$(json_escape_inline "$TARGET_LANG")\",\"text\":\"$(json_escape_inline "$TEXT")\"}]}]"

run_translation_case dev-messages-text \
  "$LLAMAFILE_BIN" \
  -m "$MODEL" \
  --translate-messages-json "$MSG_TEXT_PAYLOAD" \
  -n "$N_PREDICT"

run_translation_case packaged-messages-text-zip \
  "$PACKAGED" \
  -m "/zip/$MODEL_BASENAME" \
  --translate-messages-json "$MSG_TEXT_PAYLOAD" \
  -n "$N_PREDICT"

if [[ "$RUN_SERVER_SMOKE" == "1" ]]; then
  run_server_case dev-server-text \
    "$LLAMAFILE_BIN" \
    -m "$MODEL"

  run_server_case packaged-server-text \
    "$PACKAGED"
fi

if [[ -n "$IMAGE" ]]; then
  run_translation_case dev-image \
    "$LLAMAFILE_BIN" \
    -m "$MODEL" \
    --mmproj "$MMPROJ" \
    --translate-image "$IMAGE" \
    --source-lang "$SOURCE_LANG" \
    --target-lang "$TARGET_LANG" \
    -n "$N_PREDICT"

  run_translation_case packaged-image-zip \
    "$PACKAGED" \
    -m "/zip/$MODEL_BASENAME" \
    --mmproj "/zip/$MMPROJ_BASENAME" \
    --translate-image "$IMAGE" \
    --source-lang "$SOURCE_LANG" \
    --target-lang "$TARGET_LANG" \
    -n "$N_PREDICT"

  MSG_IMAGE_LOCAL_PAYLOAD="[{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"source_lang_code\":\"$(json_escape_inline "$SOURCE_LANG")\",\"target_lang_code\":\"$(json_escape_inline "$TARGET_LANG")\",\"url\":\"$(json_escape_inline "$IMAGE")\"}]}]"
  run_translation_case dev-messages-image-local \
    "$LLAMAFILE_BIN" \
    -m "$MODEL" \
    --mmproj "$MMPROJ" \
    --translate-messages-json "$MSG_IMAGE_LOCAL_PAYLOAD" \
    -n "$N_PREDICT"

  IMAGE_DATA_URI="$(image_to_data_uri "$IMAGE")"
  MSG_IMAGE_DATA_PAYLOAD="[{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"source_lang_code\":\"$(json_escape_inline "$SOURCE_LANG")\",\"target_lang_code\":\"$(json_escape_inline "$TARGET_LANG")\",\"url\":\"$(json_escape_inline "$IMAGE_DATA_URI")\"}]}]"
  run_translation_case dev-messages-image-datauri \
    "$LLAMAFILE_BIN" \
    -m "$MODEL" \
    --mmproj "$MMPROJ" \
    --translate-messages-json "$MSG_IMAGE_DATA_PAYLOAD" \
    -n "$N_PREDICT"

  if [[ "$RUN_SERVER_SMOKE" == "1" ]]; then
    run_server_case dev-server-mmproj \
      "$LLAMAFILE_BIN" \
      -m "$MODEL" \
      --mmproj "$MMPROJ"

    run_server_case packaged-server-mmproj \
      "$PACKAGED" \
      --mmproj "/zip/$MMPROJ_BASENAME"
  fi
fi

echo
echo "=== Summary ==="
echo "PASS: $PASS_COUNT"
echo "FAIL: $FAIL_COUNT"

if [[ "$FAIL_COUNT" -ne 0 ]]; then
  exit 2
fi
