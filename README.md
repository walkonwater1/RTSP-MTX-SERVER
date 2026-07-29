# RTSP Voice Interaction Server

全链路 RTSP + WebSocket 语音交互服务器。替代原有的 Python 信令服务 + MediaMTX 方案，整合 ASR → LLM → TTS 全链路处理。

## 架构

```
┌─ RTSP Server ───────────────────────────────────────────────────┐
│                                                                  │
│  ┌─ WebSocket Signaling (port 8090) ──────────────────────────┐ │
│  │  RFC 6455 自实现，per-client 读写线程                       │ │
│  │  协议: req_stream → stream_address → start_push_audio      │ │
│  │        → tts_start → playback_status → tts_state           │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌─ RTSP Media (port 8554, MediaMTX) ────────────────────────┐ │
│  │  ffmpeg pull: robot audio → PCM → ASR                      │ │
│  │  ffmpeg push: TTS PCM → RTSP → robot pull                  │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌─ Voice Pipeline ───────────────────────────────────────────┐ │
│  │  ASR (Zipformer CTC) → LLM (Ollama qwen2.5) → TTS (Piper) │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌─ Session Manager ──────────────────────────────────────────┐ │
│  │  每机器人一个 Session: 状态机 + TTS 队列 + ASR 累积        │ │
│  └────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

## 协议

完全兼容现有机器人端 `ehr_ros_app` 的 WebSocket + RTSP 协议。

### WebSocket 信令协议

| 方向 | event | 说明 |
|------|-------|------|
| C→S | `req_stream` | 握手请求，携带 `user_id` 和 `mode` |
| S→C | `stream_address` | 返回 `session_id`、`rtsp_url` (推流地址)、`rtsp_pull_url` (拉流地址) |
| C→S | `start_push_audio` | 机器人开始推流 |
| S→C | `stream_ready` | 服务端确认收到音频流 |
| S→C | `asr_result` | ASR 识别结果 |
| S→C | `llm_result` | LLM 回复文本 |
| S→C | `tts_start` | TTS 音频就绪，含 `audio_url` |
| C→S | `playback_status` | 播放状态: `started`(抑制VAD) / `completed`(恢复ASR) |
| C→S | `tts_state` | TTS 状态（新统一协议） |
| S→C | `stop_tts` | 停止当前 TTS |
| S→C | `interrupt` | 打断播放 |
| C→S | `ping` | 心跳 (每15s) |
| S→C | `pong` | 心跳响应 |

### RTSP 音频流

```
机器人 mic → ffmpeg → RTSP push → MediaMTX → ffmpeg pull → PCM → ASR
TTS WAV → ffmpeg push → MediaMTX → RTSP pull → ffmpeg → 机器人 speaker
```

## 依赖

```bash
# 系统库
sudo apt install libcurl4-openssl-dev libspdlog-dev nlohmann-json3-dev

# MediaMTX (RTSP 媒体服务器)
wget https://github.com/bluenviron/mediamtx/releases/download/v1.8.0/mediamtx_v1.8.0_linux_amd64.tar.gz
tar xzf mediamtx_v1.8.0_linux_amd64.tar.gz
sudo cp mediamtx /usr/local/bin/

# ffmpeg
sudo apt install ffmpeg

# Ollama (LLM)
curl -fsSL https://ollama.com/install.sh | sh
ollama pull qwen2.5:3b

# espeak-ng (TTS fallback)
sudo apt install espeak-ng

# 可选: Piper TTS (更高质量)
# pip install piper-tts
```

## 编译

```bash
cd /eir/lixin/rtsp-server
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

链接 ASR-LLM-TTS pipeline (可选):
```bash
# 先编译 ASR-LLM-TTS
cd /eir/lixin/ASR-LLM-TTS/src/build
cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . -j$(nproc)

# 再编译 RTSP Server (自动检测并链接)
cd /eir/lixin/rtsp-server/build
cmake .. -DPIPELINE_DIR=/eir/lixin/ASR-LLM-TTS
cmake --build . -j$(nproc)
```

## 运行

```bash
# 使用配置文件
./build/rtsp_server config.json

# 指定端口
./build/rtsp_server config.json --port-ws 8090 --port-rtsp 8554

# 使用外部 MediaMTX (不自动启动)
./build/rtsp_server config.json --no-mediamtx
```

## 配置

`config.json`:

```json
{
  "server": {
    "ws_port": 8090,
    "ws_path": "/ws/rtsp",
    "rtsp_port": 8554,
    "rtsp_base_url": "rtsp://192.168.2.106:8554",
    "max_sessions": 32,
    "session_timeout_sec": 300
  },
  "llm": {
    "host": "http://localhost:11434",
    "model": "qwen2.5:3b",
    "system_prompt": "你是小千..."
  },
  "tts": {
    "backend": "espeak-ng",
    "rate": 200,
    "voice": "cmn+f3"
  }
}
```

**重要**: 将 `rtsp_base_url` 设为机器人可以访问的 IP 地址（不能用 `0.0.0.0` 或 `127.0.0.1`）。

## 机器人端配置

机器人端 (`ehr_ros_app`) 的 `user_settings.json`:

```json
{
  "ws_rtsp": {
    "url": "ws://192.168.2.106:8090/ws/rtsp",
    "user_id": "robot_001",
    "mode": "voice"
  },
  "voice": {
    "audio_input_source": "rtsp"
  }
}
```

## 目录结构

```
rtsp-server/
├── CMakeLists.txt
├── config.json
├── README.md
├── scripts/
│   └── start_mediamtx.sh
├── src/
│   ├── main.cpp                          # 入口 + 信令消息处理 + 交互引擎
│   ├── server/
│   │   ├── ws_server.h/cpp               # WebSocket 服务器 (RFC 6455)
│   │   ├── session_manager.h/cpp         # 会话管理 + 状态机
│   │   └── rtsp_manager.h/cpp            # MediaMTX + ffmpeg 管道管理
│   ├── pipeline/
│   │   └── pipeline_bridge.h/cpp         # ASR/LLM/TTS 桥接
│   └── utils/
│       ├── base64.h                      # Base64 编解码
│       └── logger.h                      # spdlog 封装
└── tests/
    └── test_ws_protocol.cpp              # 协议测试
```

## 时序流程

```
[Robot 连接]
  WebSocket connect → HTTP 101 upgrade
  → C: {"event":"req_stream","data":{"user_id":"robot_001","mode":"voice"}}
  ← S: {"event":"stream_address","session_id":"abc123...","data":{"rtsp_url":"rtsp://..."}}

[Robot 唤醒]
  KWS 检测到唤醒词
  → C: ffmpeg push → RTSP (mic audio)
  → C: {"event":"start_push_audio",...}
  ← S: {"event":"stream_ready",...}

[ASR → LLM → TTS]
  Server: RTSP pull → PCM → VAD → silence detect → ASR → text
  ← S: {"event":"asr_result","data":{"text":"今天天气怎么样"}}
  Server: text → LLM → response
  ← S: {"event":"llm_result","data":{"text":"今天天气晴朗..."}}
  Server: TTS → WAV → ffmpeg push → RTSP
  ← S: {"event":"tts_start","data":{"tts_id":"...","audio_url":"rtsp://..."}}

[Robot 播放 TTS]
  C: ffmpeg pull ← RTSP (TTS audio) → speaker
  → C: {"event":"playback_status","data":{"status":"started"}}
  [... 播放中 ...]
  → C: {"event":"playback_status","data":{"status":"completed"}}

[Robot 休眠]
  超时 → 停止推流 → ffmpeg push kill
```

## 测试

```bash
cd build
cmake .. -DBUILD_TESTS=ON && cmake --build .
./test_ws_protocol

# 手动测试 WebSocket
websocat ws://localhost:8090/ws/rtsp
# 输入: {"event":"req_stream","session_id":"","timestamp":0,"data":{"user_id":"test","mode":"voice"}}
# 期望: {"event":"stream_address","session_id":"...","data":{"rtsp_url":"..."}}
```
