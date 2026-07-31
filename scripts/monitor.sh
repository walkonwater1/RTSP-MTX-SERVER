#!/bin/bash
#
# RTSP Server Health Monitor — Feishu Webhook Push
#
# Checks:
#   1. Process alive   (rtsp-mtx-server, mediamtx)
#   2. Port listening  (WS:8090, RTSP:8554)
#   3. Active sessions & WS connections
#   4. CPU / Memory usage
#   5. Recent pipeline latency (ASR→LLM→TTS from logs)
#   6. System load & disk
#
# Usage:
#   # One-shot check
#   ./monitor.sh
#
#   # With custom webhook URL
#   FEISHU_WEBHOOK=https://open.feishu.cn/open-apis/bot/v2/hook/xxx ./monitor.sh
#
#   # Cron (every 2 minutes)
#   */2 * * * * /home/lixin/eir/lixin/rtsp-server/scripts/monitor.sh >> /tmp/rtsp-monitor.log 2>&1
#
# Configuration:
#   Set FEISHU_WEBHOOK in ~/.rtsp-monitor.conf or as environment variable
#   Set ALERT_MENTION to @all or specific open_id to mention on alerts

set -euo pipefail

# ─── Configuration ───────────────────────────────────────────────────────────

# Load config file if exists
CONFIG_FILE="${HOME}/.rtsp-monitor.conf"
if [ -f "$CONFIG_FILE" ]; then
    source "$CONFIG_FILE"
fi

# Feishu webhook URL (REQUIRED — set in ~/.rtsp-monitor.conf or env)
FEISHU_WEBHOOK="${FEISHU_WEBHOOK:-}"

# Who to @mention on alerts: "all" for @everyone, or a specific open_id
ALERT_MENTION="${ALERT_MENTION:-all}"

# Paths
BINARY_NAME="rtsp-mtx-server"
MEDIAMTX_BIN="mediamtx"
LOG_FILE="/home/lixin/eir/lixin/rtsp-server/build/rtsp_server.log"
PID_FILE="/tmp/rtsp-server/monitor.pid"

# Ports to check
WS_PORT=8090
RTSP_PORT=8554

# Thresholds
CPU_WARN_PCT=80
MEM_WARN_MB=2048
LATENCY_WARN_MS=5000      # 5 seconds pipeline latency triggers warning
SESSION_WARN_COUNT=0       # warn if 0 sessions (optional, depends on usage)
LOG_LINES_LOOKBACK=200     # how many log lines to scan for latency

# State file for tracking status changes (avoid duplicate alerts)
STATE_FILE="/tmp/rtsp-server/monitor-state.json"

# ─── Helpers ─────────────────────────────────────────────────────────────────

now_ts() { date +%s; }
now_str() { date '+%Y-%m-%d %H:%M:%S'; }

# Write JSON-friendly string (escape special chars)
json_esc() {
    local s="${1-}"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    s="${s//$'\n'/\\n}"
    s="${s//$'\r'/\\r}"
    s="${s//$'\t'/\\t}"
    echo -n "$s"
}

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC}  $(now_str) $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $(now_str) $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $(now_str) $*"; }

# ─── Checks ──────────────────────────────────────────────────────────────────

check_process() {
    # Returns PID if process is running, empty string otherwise
    pgrep -x "$1" 2>/dev/null | head -1
}

check_port() {
    # Returns "LISTEN" if port is being listened on
    ss -tlnp 2>/dev/null | grep -q ":$1 " && echo "LISTEN" || echo "CLOSED"
}

get_process_stats() {
    # Get CPU% and RSS (MB) for a given PID
    local pid="$1"
    if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then
        local cpu rss_mb uptime_sec
        cpu=$(ps -p "$pid" -o %cpu --no-headers 2>/dev/null | tr -d ' ' || echo "0")
        rss_mb=$(awk '/^VmRSS:/ {printf "%.1f", $2/1024}' "/proc/$pid/status" 2>/dev/null || echo "0")
        uptime_sec=$(awk '{print int($1)}' "/proc/$pid/stat" 2>/dev/null || true)
        # uptime from /proc/stat uses CLK_TCK, approximate
        local now_sec
        now_sec=$(awk '{print int($1)}' /proc/uptime 2>/dev/null)
        if [ -n "$uptime_sec" ] && [ -n "$now_sec" ]; then
            local start_ticks clk_tck
            clk_tck=$(getconf CLK_TCK 2>/dev/null || echo "100")
            start_ticks=$uptime_sec
            uptime_sec=$(( (now_sec * 100 - start_ticks) / clk_tck ))
        fi
        echo "${cpu}|${rss_mb}|${uptime_sec:-0}"
    else
        echo "0|0|0"
    fi
}

parse_log_latency() {
    # Scan recent log lines for ASR/LLM/TTS timing patterns
    # Returns: asr_count|llm_count|tts_count|asr_text_sample|avg_llm_ms|avg_tts_ms|avg_total_ms
    local asr_count=0 llm_count=0 tts_count=0 last_asr=""
    local llm_ms_total=0 llm_ms_count=0 tts_ms_total=0 tts_ms_count=0 total_ms_total=0 total_ms_count=0

    while IFS= read -r line; do
        if [[ "$line" == *"[ASR] recognized:"* ]]; then
            asr_count=$((asr_count + 1))
            last_asr=$(echo "$line" | sed -n 's/.*recognized: "\(.*\)" for session.*/\1/p')
        fi
        if [[ "$line" == *"[LLM]"* ]] || [[ "$line" == *"llm_result"* ]]; then
            llm_count=$((llm_count + 1))
        fi
        if [[ "$line" == *"[TTS]"* ]]; then
            tts_count=$((tts_count + 1))
        fi
        # Parse [Timing] LLM=Xms TTS=Yms TOTAL=Zms
        if [[ "$line" == *"[Timing]"* ]]; then
            local llm_ms tts_ms total_ms
            llm_ms=$(echo "$line" | sed -n 's/.*LLM=\([0-9]\+\)ms.*/\1/p')
            tts_ms=$(echo "$line" | sed -n 's/.*TTS=\([0-9]\+\)ms.*/\1/p')
            total_ms=$(echo "$line" | sed -n 's/.*TOTAL=\([0-9]\+\)ms.*/\1/p')
            if [ -n "$llm_ms" ]; then
                llm_ms_total=$((llm_ms_total + llm_ms))
                llm_ms_count=$((llm_ms_count + 1))
            fi
            if [ -n "$tts_ms" ]; then
                tts_ms_total=$((tts_ms_total + tts_ms))
                tts_ms_count=$((tts_ms_count + 1))
            fi
            if [ -n "$total_ms" ]; then
                total_ms_total=$((total_ms_total + total_ms))
                total_ms_count=$((total_ms_count + 1))
            fi
        fi
    done < <(tail -n "$LOG_LINES_LOOKBACK" "$LOG_FILE" 2>/dev/null)

    local avg_llm=0 avg_tts=0 avg_total=0
    [ "$llm_ms_count" -gt 0 ] && avg_llm=$((llm_ms_total / llm_ms_count))
    [ "$tts_ms_count" -gt 0 ] && avg_tts=$((tts_ms_total / tts_ms_count))
    [ "$total_ms_count" -gt 0 ] && avg_total=$((total_ms_total / total_ms_count))

    echo "${asr_count}|${llm_count}|${tts_count}|$(json_esc "${last_asr:-N/A}")|${avg_llm}|${avg_tts}|${avg_total}"
}

get_session_count() {
    # Parse "X active sessions, Y WS connections" from recent log lines
    tail -n 20 "$LOG_FILE" 2>/dev/null \
        | grep '\[Status\]' | tail -1 \
        | sed -n 's/.*\[Status\] \(.*\) active.*/\1/p' \
        || echo "0"
}

get_ws_connection_count() {
    tail -n 20 "$LOG_FILE" 2>/dev/null \
        | grep '\[Status\]' | tail -1 \
        | sed -n 's/.*, \(.*\) WS connections.*/\1/p' \
        || echo "0"
}

get_system_load() {
    local load cpu_pct mem_pct disk_pct
    load=$(awk '{print $1", "$2", "$3}' /proc/loadavg 2>/dev/null || echo "N/A")
    cpu_pct=$(top -bn1 | grep "Cpu(s)" | awk '{print 100-$8}' 2>/dev/null || echo "N/A")
    mem_pct=$(awk '/^MemTotal:/{t=$2} /^MemAvailable:/{a=$2} END{if(t>0) printf "%.1f", (t-a)/t*100}' /proc/meminfo 2>/dev/null || echo "N/A")
    disk_pct=$(df -h / | awk 'NR==2 {print $5}' 2>/dev/null || echo "N/A")
    echo "${load}|${cpu_pct}|${mem_pct}|${disk_pct}"
}

# ─── Status Assessment ───────────────────────────────────────────────────────

assess_status() {
    local proc_ok="$1" mtx_ok="$2" ws_ok="$3" rtsp_ok="$4"
    local sessions="$5" cpu_pct="$6"

    if [ "$proc_ok" != "running" ]; then
        echo "CRITICAL"
    elif [ "$mtx_ok" != "running" ] || [ "$ws_ok" != "LISTEN" ] || [ "$rtsp_ok" != "LISTEN" ]; then
        echo "WARNING"
    elif [ -n "$cpu_pct" ] && [ "$cpu_pct" != "N/A" ] && \
         awk "BEGIN {exit !($cpu_pct > $CPU_WARN_PCT)}"; then
        echo "WARNING"
    else
        echo "OK"
    fi
}

# ─── Feishu Card Builder ─────────────────────────────────────────────────────

build_card_json() {
    local status="$1"          # OK / WARNING / CRITICAL
    local timestamp="$2"

    local proc_pid="$3" mtx_pid="$4"
    local ws_port_state="$5" rtsp_port_state="$6"
    local cpu="$7" mem="$8" uptime_sec="$9"
    local sessions="${10}" ws_conns="${11}"
    local asr_count="${12}" llm_count="${13}" tts_count="${14}" last_asr="${15}"
    local avg_llm_ms="${16}" avg_tts_ms="${17}" avg_total_ms="${18}"
    local load="${19}" sys_cpu="${20}" sys_mem="${21}" disk="${22}"

    # Pick color and icon
    local color icon title
    case "$status" in
        OK)
            color="green"
            icon="✅"
            title="RTSP 服务器运行正常"
            ;;
        WARNING)
            color="orange"
            icon="⚠️"
            title="RTSP 服务器异常警告"
            ;;
        CRITICAL)
            color="red"
            icon="🚨"
            title="RTSP 服务器故障"
            ;;
    esac

    # Format uptime
    local uptime_str
    if [ -n "$uptime_sec" ] && [ "$uptime_sec" -gt 0 ] 2>/dev/null; then
        local h=$((uptime_sec / 3600))
        local m=$(((uptime_sec % 3600) / 60))
        uptime_str="${h}h${m}m"
    else
        uptime_str="N/A"
    fi

    # Status line for each component
    local proc_line mtx_line ws_line rtsp_line
    if [ "$proc_pid" != "0" ]; then
        proc_line="🟢 rtsp-mtx-server: PID $proc_pid | CPU ${cpu}% | MEM ${mem}MB | uptime ${uptime_str}"
    else
        proc_line="🔴 rtsp-mtx-server: **已停止**"
    fi

    if [ "$mtx_pid" != "0" ]; then
        mtx_line="🟢 MediaMTX (RTSP): PID $mtx_pid"
    else
        mtx_line="🔴 MediaMTX (RTSP): **已停止**"
    fi

    if [ "$ws_port_state" = "LISTEN" ]; then
        ws_line="🟢 WebSocket: 端口 $WS_PORT 正常监听"
    else
        ws_line="🔴 WebSocket: 端口 $WS_PORT **未监听**"
    fi

    if [ "$rtsp_port_state" = "LISTEN" ]; then
        rtsp_line="🟢 RTSP: 端口 $RTSP_PORT 正常监听"
    else
        rtsp_line="🔴 RTSP: 端口 $RTSP_PORT **未监听**"
    fi

    # Mention string for alerts
    local mention_line=""
    if [ "$status" != "OK" ]; then
        if [ "$ALERT_MENTION" = "all" ]; then
            mention_line="<at id=all></at> "
        elif [ -n "$ALERT_MENTION" ]; then
            mention_line="<at id=${ALERT_MENTION}></at> "
        fi
    fi

    # Build Feishu interactive card JSON
    cat <<CARDEOF
{
  "msg_type": "interactive",
  "card": {
    "header": {
      "title": {
        "tag": "plain_text",
        "content": "${icon} ${title} — ${timestamp}"
      },
      "template": "${color}"
    },
    "elements": [
      {
        "tag": "div",
        "text": {
          "tag": "lark_md",
          "content": "${mention_line}**服务状态**\n${proc_line}\n${mtx_line}\n${ws_line}\n${rtsp_line}"
        }
      },
      {
        "tag": "hr"
      },
      {
        "tag": "div",
        "text": {
          "tag": "lark_md",
          "content": "**会话信息**\n活跃会话: ${sessions} | WS 连接: ${ws_conns}\n\n**Pipeline 活动** (近 ${LOG_LINES_LOOKBACK} 行日志)\nASR 识别: ${asr_count} 次 | LLM 响应: ${llm_count} 次 | TTS 合成: ${tts_count} 次\n最近识别: _${last_asr}_\n\n**平均延迟**\nLLM: ${avg_llm_ms}ms | TTS: ${avg_tts_ms}ms | 总链路: ${avg_total_ms}ms"
        }
      },
      {
        "tag": "hr"
      },
      {
        "tag": "div",
        "text": {
          "tag": "lark_md",
          "content": "**系统资源**\nCPU: ${sys_cpu}% | 内存: ${sys_mem}% | 磁盘: ${disk}\n负载: ${load}"
        }
      },
      {
        "tag": "note",
        "elements": [
          {
            "tag": "plain_text",
            "content": "rtsp-server monitor | $(hostname) | $(date '+%Y-%m-%d %H:%M:%S')"
          }
        ]
      }
    ]
  }
}
CARDEOF
}

build_simple_text() {
    # Fallback simple text message
    local msg="$1"
    cat <<EOF
{
  "msg_type": "text",
  "content": {
    "text": "$msg"
  }
}
EOF
}

# ─── Send to Feishu ──────────────────────────────────────────────────────────

send_to_feishu() {
    local payload="$1"
    if [ -z "$FEISHU_WEBHOOK" ]; then
        log_warn "FEISHU_WEBHOOK not set — skipping push"
        return 1
    fi

    local http_code
    http_code=$(curl -s -o /dev/null -w "%{http_code}" \
        -X POST "$FEISHU_WEBHOOK" \
        -H 'Content-Type: application/json' \
        -d "$payload" 2>/dev/null || echo "000")

    if [ "$http_code" = "200" ]; then
        log_info "Pushed to Feishu (HTTP $http_code)"
        return 0
    else
        log_error "Feishu push failed (HTTP $http_code)"
        return 1
    fi
}

# ─── State Tracking ──────────────────────────────────────────────────────────

load_state() {
    if [ -f "$STATE_FILE" ]; then
        cat "$STATE_FILE"
    else
        echo '{"last_status":"UNKNOWN","last_alert_ts":0,"consecutive_failures":0}'
    fi
}

save_state() {
    local status="$1"
    local ts="$2"
    local failures="$3"
    mkdir -p "$(dirname "$STATE_FILE")"
    cat > "$STATE_FILE" <<EOF
{"last_status":"${status}","last_alert_ts":${ts},"consecutive_failures":${failures}}
EOF
}

# ─── Main ────────────────────────────────────────────────────────────────────

main() {
    log_info "=== RTSP Server Health Check ==="
    local ts
    ts=$(now_ts)
    local timestamp
    timestamp=$(now_str)

    # Load previous state
    local prev_state prev_status prev_failures
    prev_state=$(load_state)
    prev_status=$(echo "$prev_state" | python3 -c "import sys,json; print(json.load(sys.stdin).get('last_status','UNKNOWN'))" 2>/dev/null || echo "UNKNOWN")
    prev_failures=$(echo "$prev_state" | python3 -c "import sys,json; print(json.load(sys.stdin).get('consecutive_failures',0))" 2>/dev/null || echo "0")

    # ── 1. Process checks ──
    local proc_pid mtx_pid
    proc_pid=$(check_process "$BINARY_NAME" || true)
    mtx_pid=$(check_process "$MEDIAMTX_BIN" || true)

    if [ -n "$proc_pid" ]; then
        log_info "rtsp-mtx-server: PID $proc_pid ✓"
    else
        log_error "rtsp-mtx-server: NOT RUNNING ✗"
    fi

    if [ -n "$mtx_pid" ]; then
        log_info "mediamtx: PID $mtx_pid ✓"
    else
        log_warn "mediamtx: NOT RUNNING ✗"
    fi

    # ── 2. Port checks ──
    local ws_port_state rtsp_port_state
    ws_port_state=$(check_port "$WS_PORT")
    rtsp_port_state=$(check_port "$RTSP_PORT")

    log_info "WS port $WS_PORT: $ws_port_state"
    log_info "RTSP port $RTSP_PORT: $rtsp_port_state"

    # ── 3. Process stats ──
    local proc_cpu proc_mem proc_uptime
    IFS='|' read -r proc_cpu proc_mem proc_uptime <<< "$(get_process_stats "${proc_pid:-0}")"
    log_info "CPU: ${proc_cpu}% | MEM: ${proc_mem}MB | Uptime: ${proc_uptime}s"

    # ── 4. Parse log ──
    local asr_count llm_count tts_count last_asr avg_llm_ms avg_tts_ms avg_total_ms
    IFS='|' read -r asr_count llm_count tts_count last_asr avg_llm_ms avg_tts_ms avg_total_ms <<< "$(parse_log_latency)"
    log_info "Pipeline: ASR=$asr_count LLM=$llm_count TTS=$tts_count | Last: $last_asr | Latency(avg): LLM=${avg_llm_ms}ms TTS=${avg_tts_ms}ms TOTAL=${avg_total_ms}ms"

    # ── 5. Sessions ──
    local sessions ws_conns
    sessions=$(get_session_count)
    ws_conns=$(get_ws_connection_count)
    log_info "Sessions: $sessions active, $ws_conns WS connections"

    # ── 6. System load ──
    local sys_load sys_cpu sys_mem sys_disk
    IFS='|' read -r sys_load sys_cpu sys_mem sys_disk <<< "$(get_system_load)"
    log_info "System: CPU=${sys_cpu}% MEM=${sys_mem}% DISK=${sys_disk} Load=${sys_load}"

    # ── 7. Assess status ──
    local status
    local proc_ok="running"
    local mtx_ok="running"
    [ -z "$proc_pid" ] && proc_ok="stopped"
    [ -z "$mtx_pid" ] && mtx_ok="stopped"

    status=$(assess_status "$proc_ok" "$mtx_ok" "$ws_port_state" "$rtsp_port_state" "$sessions" "$sys_cpu")
    log_info "Assessment: $status (previous: $prev_status)"

    # ── 8. Track state & suppress duplicate alerts ──
    local failures="${prev_failures:-0}"
    local should_send=true

    if [ "$status" = "OK" ]; then
        failures=0
        # Only send OK every 10 checks (~20 min) or on recovery from non-OK
        if [ "$prev_status" = "OK" ] && [ -f "$STATE_FILE" ]; then
            local last_alert_ts
            last_alert_ts=$(echo "$prev_state" | python3 -c "import sys,json; print(json.load(sys.stdin).get('last_alert_ts',0))" 2>/dev/null || echo "0")
            local since_last=$(( ts - last_alert_ts ))
            if [ "$since_last" -lt 600 ]; then  # 10 minutes
                should_send=false
                log_info "Suppressing OK message (last sent ${since_last}s ago)"
            fi
        fi
    else
        failures=$((failures + 1))
        # Always send on first failure, then throttle to every 5th
        if [ "$failures" -gt 1 ] && [ $((failures % 5)) -ne 0 ]; then
            should_send=false
            log_info "Suppressing repeated alert (failure #${failures})"
        fi
    fi

    save_state "$status" "$ts" "$failures"

    # ── 9. Build and send card ──
    if [ "$should_send" = true ] && [ -n "$FEISHU_WEBHOOK" ]; then
        local card_json
        card_json=$(build_card_json \
            "$status" "$timestamp" \
            "${proc_pid:-0}" "${mtx_pid:-0}" \
            "$ws_port_state" "$rtsp_port_state" \
            "$proc_cpu" "$proc_mem" "$proc_uptime" \
            "$sessions" "$ws_conns" \
            "$asr_count" "$llm_count" "$tts_count" "$last_asr" \
            "${avg_llm_ms:-0}" "${avg_tts_ms:-0}" "${avg_total_ms:-0}" \
            "$sys_load" "$sys_cpu" "$sys_mem" "$sys_disk")

        send_to_feishu "$card_json"
    elif [ "$should_send" = true ] && [ -z "$FEISHU_WEBHOOK" ]; then
        # No webhook configured — just print the card for debugging
        echo "=== Would send to Feishu ==="
        build_card_json \
            "$status" "$timestamp" \
            "${proc_pid:-0}" "${mtx_pid:-0}" \
            "$ws_port_state" "$rtsp_port_state" \
            "$proc_cpu" "$proc_mem" "$proc_uptime" \
            "$sessions" "$ws_conns" \
            "$asr_count" "$llm_count" "$tts_count" "$last_asr" \
            "${avg_llm_ms:-0}" "${avg_tts_ms:-0}" "${avg_total_ms:-0}" \
            "$sys_load" "$sys_cpu" "$sys_mem" "$sys_disk" \
            | python3 -m json.tool 2>/dev/null || true
    fi

    log_info "=== Health check complete ==="
    echo ""
}

main "$@"
