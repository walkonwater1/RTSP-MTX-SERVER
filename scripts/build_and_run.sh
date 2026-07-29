#!/bin/bash
# ============================================================================
# RTSP Voice Interaction Server — Build & Run Script
# ============================================================================
#
# Usage:
#   ./scripts/build_and_run.sh              # Build + run with config.json
#   ./scripts/build_and_run.sh --build-only # Only compile, don't run
#   ./scripts/build_and_run.sh --run-only   # Skip build, just run
#   ./scripts/build_and_run.sh --clean      # Clean rebuild + run
#   ./scripts/build_and_run.sh --debug      # Debug build + run
#
# Options:
#   --build-only     Compile only, skip server launch
#   --run-only       Skip compilation, launch existing binary
#   --clean          Remove build/ dir and rebuild from scratch
#   --debug          CMake Debug build (default: Release)
#   --no-pipeline    Skip ASR-LLM-TTS pipeline build
#   --no-mediamtx    Don't auto-launch MediaMTX (use external)
#   --port-ws N      Override WebSocket port
#   --port-rtsp N    Override RTSP port
#   -h, --help       Show this help
# ============================================================================

set -euo pipefail

# --- Paths ---
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
CONFIG_FILE="$PROJECT_DIR/config.json"
PIPELINE_DIR="/eir/lixin/ASR-LLM-TTS"
PIPELINE_BUILD_DIR="$PIPELINE_DIR/src/build"

# --- Default settings ---
BUILD_TYPE="Release"
ACTION="full"       # full | build | run
DO_CLEAN=false
BUILD_PIPELINE=true
PASS_THROUGH_ARGS=()

# --- Parse arguments ---
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-only) ACTION="build"; shift ;;
    --run-only)   ACTION="run"; shift ;;
    --clean)      DO_CLEAN=true; shift ;;
    --debug)      BUILD_TYPE="Debug"; shift ;;
    --no-pipeline) BUILD_PIPELINE=false; shift ;;
    --help|-h)
      sed -n '2,/^$/p' "$0" | head -n -1
      exit 0
      ;;
    *) PASS_THROUGH_ARGS+=("$1"); shift ;;
  esac
done

# --- Helper functions ---
log()  { echo -e "\033[1;34m[BUILD]\033[0m $*"; }
ok()   { echo -e "\033[1;32m[OK]\033[0m    $*"; }
warn() { echo -e "\033[1;33m[WARN]\033[0m  $*"; }
err()  { echo -e "\033[1;31m[ERR]\033[0m   $*"; }

# --- Check dependencies ---
check_deps() {
  local missing=()

  for cmd in cmake g++ make pkg-config; do
    command -v "$cmd" &>/dev/null || missing+=("$cmd")
  done

  # Check dev libraries via header files
  if ! find /usr/include -name "json.hpp" -path "*/nlohmann/*" 2>/dev/null | grep -q .; then
    missing+=("nlohmann-json3-dev")
  fi
  if [[ ! -f /usr/include/spdlog/spdlog.h ]]; then
    missing+=("libspdlog-dev")
  fi
  if [[ ! -f /usr/include/curl/curl.h ]] && [[ ! -f /usr/include/x86_64-linux-gnu/curl/curl.h ]]; then
    missing+=("libcurl4-openssl-dev")
  fi

  if [[ ${#missing[@]} -gt 0 ]]; then
    err "Missing dependencies: ${missing[*]}"
    echo "  Install with: sudo apt install ${missing[*]}"
    exit 1
  fi
  ok "System dependencies OK"
}

# --- Build ASR-LLM-TTS pipeline (optional) ---
build_pipeline() {
  if [[ "$BUILD_PIPELINE" != "true" ]]; then
    warn "Skipping ASR-LLM-TTS pipeline (--no-pipeline)"
    return 0
  fi

  if [[ ! -d "$PIPELINE_BUILD_DIR" ]]; then
    log "Creating pipeline build directory..."
    mkdir -p "$PIPELINE_BUILD_DIR"
  fi

  log "Building ASR-LLM-TTS pipeline (${BUILD_TYPE})..."
  cd "$PIPELINE_BUILD_DIR"

  cmake .. \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_TESTS=OFF \
    -DBUILD_BENCHMARKS=OFF \
    2>&1 | sed 's/^/  [cmake] /'

  cmake --build . -j"$(nproc)" 2>&1 | sed 's/^/  [make]  /'

  if [[ -f "$PIPELINE_BUILD_DIR/libvoice_pipeline.a" ]]; then
    ok "ASR-LLM-TTS pipeline built successfully"
  else
    warn "Pipeline library not found — server will run in stub mode"
  fi
}

# --- Build RTSP Server ---
build_server() {
  if [[ "$DO_CLEAN" == "true" ]]; then
    log "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
  fi

  if [[ ! -d "$BUILD_DIR" ]]; then
    mkdir -p "$BUILD_DIR"
  fi

  log "Building RTSP server (${BUILD_TYPE})..."
  cd "$BUILD_DIR"

  local cmake_args=(
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    "-DPIPELINE_DIR=$PIPELINE_DIR"
  )

  cmake .. "${cmake_args[@]}" 2>&1 | sed 's/^/  [cmake] /'
  cmake --build . -j"$(nproc)" 2>&1 | sed 's/^/  [make]  /'

  if [[ -x "$BUILD_DIR/rtsp_server" ]]; then
    ok "RTSP server built: $BUILD_DIR/rtsp_server"
  else
    err "Build failed — binary not found"
    exit 1
  fi
}

# --- Run server ---
run_server() {
  if [[ ! -x "$BUILD_DIR/rtsp_server" ]]; then
    err "Binary not found at $BUILD_DIR/rtsp_server"
    err "Run without --run-only to build first."
    exit 1
  fi

  if [[ ! -f "$CONFIG_FILE" ]]; then
    warn "Config file not found: $CONFIG_FILE"
    warn "Running with defaults..."
  fi

  # Create runtime dirs
  mkdir -p /tmp/rtsp-server/debug
  mkdir -p /tmp/rtsp-server/tts-cache

  log "Starting RTSP server..."
  log "  Config:      $CONFIG_FILE"
  log "  WebSocket:   ws://0.0.0.0:$(grep -oP '"ws_port"\s*:\s*\K\d+' "$CONFIG_FILE" 2>/dev/null || echo 8090)/ws/rtsp"
  log "  RTSP base:   $(grep -oP '"rtsp_base_url"\s*:\s*"\K[^"]+' "$CONFIG_FILE" 2>/dev/null || echo rtsp://0.0.0.0:8554)"
  log "  Log file:    $PROJECT_DIR/rtsp_server.log"
  echo   ""

  cd "$PROJECT_DIR"
  exec "$BUILD_DIR/rtsp_server" "$CONFIG_FILE" "${PASS_THROUGH_ARGS[@]}"
}

# ============================================================================
# Main
# ============================================================================

echo ""
echo "  ╔══════════════════════════════════════════════════╗"
echo "  ║   RTSP Voice Interaction Server                  ║"
echo "  ║   Build & Run Script                             ║"
echo "  ╚══════════════════════════════════════════════════╝"
echo ""

check_deps

case "$ACTION" in
  full)
    build_pipeline
    build_server
    run_server
    ;;
  build)
    build_pipeline
    build_server
    ok "Build complete. Binary: $BUILD_DIR/rtsp_server"
    ;;
  run)
    run_server
    ;;
esac
