#!/bin/bash
# ============================================================================
# RTSP Voice Interaction Server — 一键编译启动
# ============================================================================
#
# Usage:
#   ./start.sh               # 编译 + 启动 (默认)
#   ./start.sh --build-only  # 仅编译
#   ./start.sh --clean       # 清理重编
#   ./start.sh --debug       # Debug 构建
#   ./start.sh --no-mediamtx # 不自动启动 MediaMTX
#   ./start.sh --help        # 帮助
#
# ============================================================================

set -euo pipefail

# --- 路径 ---
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
BUILD_DIR="$PROJECT_DIR/build"
BINARY="$BUILD_DIR/rtsp-mtx-server"
CONFIG_FILE="$PROJECT_DIR/config.json"
PIPELINE_DIR="/eir/lixin/ASR-LLM-TTS"

# --- 默认值 ---
BUILD_TYPE="Release"
NO_MEDIAMTX=false
DO_CLEAN=false
ACTION="full"
START_TIME=$(date +%s)

# --- 颜色 ---
BOLD="\033[1m"
BLUE="\033[1;34m"
GREEN="\033[1;32m"
YELLOW="\033[1;33m"
RED="\033[1;31m"
CYAN="\033[1;36m"
RESET="\033[0m"

log()    { echo -e "${BLUE}[BUILD]${RESET}  $*"; }
ok()     { echo -e "${GREEN}[  OK]${RESET}  $*"; }
warn()   { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
err()    { echo -e "${RED}[ ERR]${RESET}  $*"; }
info()   { echo -e "${CYAN}[INFO]${RESET}  $*"; }
elapsed() { echo "$((($(date +%s) - START_TIME) / 60))m$((($(date +%s) - START_TIME) % 60))s"; }

# --- 参数解析 ---
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-only) ACTION="build"; shift ;;
    --clean)      DO_CLEAN=true; shift ;;
    --debug)      BUILD_TYPE="Debug"; shift ;;
    --no-mediamtx) NO_MEDIAMTX=true; shift ;;
    --help|-h)
      echo "Usage:"
      echo "  ./start.sh               # 编译 + 启动 (默认)"
      echo "  ./start.sh --build-only  # 仅编译"
      echo "  ./start.sh --clean       # 清理重编"
      echo "  ./start.sh --debug       # Debug 构建"
      echo "  ./start.sh --no-mediamtx # 不自动启动 MediaMTX"
      echo "  ./start.sh --help        # 帮助"
      exit 0
      ;;
    *) shift ;;
  esac
done

# ============================================================================
# 1. 环境检查
# ============================================================================
check_env() {
  echo ""
  echo -e "${BOLD}  ╔══════════════════════════════════════════════╗${RESET}"
  echo -e "${BOLD}  ║   RTSP Voice Interaction Server              ║${RESET}"
  echo -e "${BOLD}  ║   一键编译启动                                 ║${RESET}"
  echo -e "${BOLD}  ╚══════════════════════════════════════════════╝${RESET}"
  echo ""

  log "检查系统环境..."

  # 编译工具链
  local missing=()
  for cmd in cmake g++ make; do
    command -v "$cmd" &>/dev/null || missing+=("$cmd")
  done

  # 系统库 (pkg-config)
  pkg-config --exists nlohmann_json 2>/dev/null || missing+=("nlohmann-json3-dev")
  pkg-config --exists spdlog 2>/dev/null         || missing+=("libspdlog-dev")
  pkg-config --exists libcurl 2>/dev/null        || missing+=("libcurl4-openssl-dev")

  if [[ ${#missing[@]} -gt 0 ]]; then
    err "缺少依赖: ${missing[*]}"
    echo "  安装: sudo apt install ${missing[*]}"
    exit 1
  fi
  ok "编译工具链: cmake $(cmake --version 2>/dev/null | awk 'NR==1{print $NF}'), g++ $(g++ -dumpversion)"

  # MediaMTX
  if [[ "$NO_MEDIAMTX" != "true" ]]; then
    local mtx_bin=""
    for candidate in /usr/local/bin/mediamtx /usr/bin/mediamtx "$HOME/.local/bin/mediamtx"; do
      [[ -x "$candidate" ]] && { mtx_bin="$candidate"; break; }
    done
    if [[ -n "$mtx_bin" ]]; then
      ok "MediaMTX:   $mtx_bin"
    else
      warn "MediaMTX 未找到，将使用 --no-mediamtx 模式"
      NO_MEDIAMTX=true
    fi
  fi

  # Ollama
  local ollama_host=$(grep -oP '"host"\s*:\s*"\K[^"]+' "$CONFIG_FILE" 2>/dev/null || echo "http://localhost:11434")
  if curl -s --max-time 3 "${ollama_host}/api/tags" &>/dev/null; then
    local ollama_model=$(grep -oP '"model"\s*:\s*"\K[^"]+' "$CONFIG_FILE" 2>/dev/null || echo "?")
    ok "Ollama:     $ollama_host (模型: $ollama_model)"
  else
    warn "Ollama 未响应 ($ollama_host) — LLM 功能不可用"
  fi

  # ASR 模型
  local asr_model=$(grep -oP '"model_path"\s*:\s*"\K[^"]+' "$CONFIG_FILE" 2>/dev/null || echo "")
  if [[ -n "$asr_model" && -f "$asr_model/model.int8.onnx" ]]; then
    ok "ASR 模型:   $asr_model"
  elif [[ -n "$asr_model" ]]; then
    warn "ASR 模型不完整: $asr_model"
  fi

  # 配置文件
  [[ -f "$CONFIG_FILE" ]] && ok "配置文件:   $CONFIG_FILE" || warn "配置文件不存在: $CONFIG_FILE"
}

# ============================================================================
# 2. 编译
# ============================================================================
build() {
  log "开始编译 (${BUILD_TYPE})..."

  if [[ "$DO_CLEAN" == "true" ]]; then
    log "清理构建目录..."
    rm -rf "$BUILD_DIR"
  fi

  mkdir -p "$BUILD_DIR"
  cd "$BUILD_DIR"

  # CMake 配置
  log "CMake 配置..."
  cmake .. \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DPIPELINE_DIR="$PIPELINE_DIR" \
    2>&1 | tail -3

  # 编译
  log "编译中 (并行: $(nproc) 线程)..."
  cmake --build . -j"$(nproc)" 2>&1 | tail -5

  if [[ -x "$BINARY" ]]; then
    local bin_size=$(du -h "$BINARY" | cut -f1)
    ok "编译成功 — $BINARY ($bin_size) | 耗时: $(elapsed)"
  else
    err "编译失败 — 二进制文件未生成"
    exit 1
  fi
}

# ============================================================================
# 3. 启动
# ============================================================================
run() {
  # 停止已有实例
  local pids=$(pgrep -f "rtsp-mtx-server" 2>/dev/null || true)
  if [[ -n "$pids" ]]; then
    warn "发现运行中的实例 (PID: $pids)，正在停止..."
    kill $pids 2>/dev/null || true
    sleep 1
    pids=$(pgrep -f "rtsp-mtx-server" 2>/dev/null || true)
    [[ -n "$pids" ]] && kill -9 $pids 2>/dev/null || true
    ok "已停止旧实例"
  fi

  # 创建运行时目录
  mkdir -p /tmp/rtsp-server/debug
  mkdir -p /tmp/rtsp-server/tts-cache
  mkdir -p /tmp/rtsp-server/memory
  mkdir -p /dev/shm/rtsp-server/tts-cache 2>/dev/null || true

  # 读取配置
  local ws_port=$(grep -oP '"ws_port"\s*:\s*\K\d+' "$CONFIG_FILE" 2>/dev/null || echo "8090")
  local rtsp_url=$(grep -oP '"rtsp_base_url"\s*:\s*"\K[^"]+' "$CONFIG_FILE" 2>/dev/null || echo "rtsp://0.0.0.0:8554")

  echo ""
  info "启动参数:"
  info "  WebSocket:  ws://0.0.0.0:${ws_port}/ws/rtsp"
  info "  RTSP:       ${rtsp_url}"
  info "  日志文件:    ${PROJECT_DIR}/rtsp-mtx-server.log"
  echo ""

  cd "$PROJECT_DIR"

  # 启动
  "$BINARY" "$CONFIG_FILE" 2>&1 | tee "$PROJECT_DIR/rtsp-mtx-server.log" &

  local server_pid=$!
  sleep 2

  # 健康检查
  if kill -0 "$server_pid" 2>/dev/null; then
    ok "服务已启动 (PID: $server_pid) | 总耗时: $(elapsed)"
    echo ""
    info "交互命令:"
    info "  tail -f rtsp-mtx-server.log   # 查看日志"
    info "  kill $server_pid              # 停止服务"
    info "  ./start.sh --clean            # 清理重编"
    echo ""
    info "按 Ctrl+C 停止服务"
    echo ""

    # Trap 清理
    trap 'echo ""; warn "停止服务..."; kill $server_pid 2>/dev/null; ok "已停止 (运行时长: $(elapsed))"; exit 0' SIGINT SIGTERM

    wait "$server_pid" 2>/dev/null || true
  else
    err "服务启动失败，查看日志: tail -50 $PROJECT_DIR/rtsp-mtx-server.log"
    exit 1
  fi
}

# ============================================================================
# Main
# ============================================================================
case "$ACTION" in
  full)
    check_env
    build
    run
    ;;
  build)
    check_env
    build
    ok "编译完成。运行 ./start.sh 启动服务"
    ;;
esac
