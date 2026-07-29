# RTSP Voice Interaction Server

全链路 RTSP + WebSocket 语音交互服务器。替代原有的 Python 信令服务 + MediaMTX 方案，整合 ASR → Skills/FC → LLM → TTS 全链路处理。

> 本项目基于 [speech](https://github.com/walkonwater1/speech)（ASR-LLM-TTS 语音交互原型）将服务器部分独立拆分出来，重构为 C++ 单进程 RTSP 服务器，便于机器人端部署。

## 架构

```
┌─ RTSP Server ───────────────────────────────────────────────────────┐
│                                                                      │
│  ┌─ WebSocket Signaling (port 8090) ──────────────────────────────┐ │
│  │  RFC 6455 自实现，per-client 读写线程 + 心跳                    │ │
│  │  协议: req_stream → stream_address → start_push_audio          │ │
│  │        → tts_start → playback_status → tts_state               │ │
│  └────────────────────────────────────────────────────────────────┘ │
│                                                                      │
│  ┌─ RTSP Media (port 8554, MediaMTX) ────────────────────────────┐ │
│  │  ffmpeg pull: robot audio → PCM → VAD → ASR                   │ │
│  │  ffmpeg push: TTS PCM → RTSP → robot pull                      │ │
│  └────────────────────────────────────────────────────────────────┘ │
│                                                                      │
│  ┌─ Voice Pipeline ───────────────────────────────────────────────┐ │
│  │  ASR (Zipformer CTC) → Skills/FC → LLM (Ollama) → TTS (Piper) │ │
│  │  ┌─ Skill System ────────────────────────────────────────────┐ │ │
│  │  │  9 个技能: 时间/天气/计算器/娱乐/谜语/运势/诗词/游戏/记忆  │ │ │
│  │  │  双策略: Function Calling (LLM驱动) + Keyword Match (兜底) │ │ │
│  │  └───────────────────────────────────────────────────────────┘ │ │
│  │  ┌─ Memory System ───────────────────────────────────────────┐ │ │
│  │  │  会话记忆 (ChatMemory): 多轮对话上下文                     │ │ │
│  │  │  用户记忆 (UserMemory): 长期偏好/事实存储                   │ │ │
│  │  └───────────────────────────────────────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────────┘ │
│                                                                      │
│  ┌─ Session Manager ──────────────────────────────────────────────┐ │
│  │  每机器人一个 Session: 状态机 + TTS 队列 + ASR 累积            │ │
│  └────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────┘
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
    "bind_address": "0.0.0.0",
    "max_sessions": 32,
    "heartbeat_interval_sec": 15,
    "session_timeout_sec": 300
  },
  "mediamtx": {
    "binary_path": "/usr/local/bin/mediamtx",
    "auto_launch": true
  },
  "asr": {
    "model_path": "/path/to/sherpa-onnx/zipformer-ctc-zh",
    "model_type": "zipformer_ctc",
    "sample_rate": 16000
  },
  "llm": {
    "host": "http://localhost:11434",
    "model": "qwen2.5:3b",
    "system_prompt": "你是小千，一个18岁女大学生...",
    "timeout_sec": 60,
    "max_response_tokens": 256
  },
  "tts": {
    "backend": "piper",
    "rate": 200,
    "voice": "cmn+f3",
    "piper_model": "/path/to/zh_CN-huayan-medium.onnx",
    "output_sample_rate": 16000,
    "cache_tts": true,
    "cache_dir": "/tmp/rtsp-server/tts-cache"
  },
  "vad": {
    "backend": "adaptive",
    "energy_threshold": 0.008,
    "min_speech_frames": 15,
    "min_silence_frames": 15,
    "pre_speech_frames": 15,
    "max_speech_sec": 60,
    "min_speech_sec": 0.5
  },
  "skills": {
    "enabled": true,
    "function_calling_enabled": true,
    "fc_model": ""
  },
  "memory": {
    "enabled": true,
    "max_rounds": 10,
    "max_tokens": 512,
    "persist_dir": "/tmp/rtsp-server/memory"
  }
}
```

### 配置说明

| 模块 | 关键字段 | 说明 |
|------|---------|------|
| `server` | `ws_port`, `rtsp_port` | WebSocket 和 RTSP 端口 |
| | `rtsp_base_url` | **必须设为机器人可访问的 IP**（不能用 0.0.0.0 或 127.0.0.1） |
| `mediamtx` | `auto_launch` | 是否自动启动 MediaMTX 进程 |
| `llm` | `host`, `model` | Ollama 服务地址和模型名 |
| | `system_prompt` | 系统人设 prompt，会自动附加当前时间上下文 |
| `skills` | `function_calling_enabled` | 启用 LLM 驱动的工具选择（需要额外一次 FC LLM 调用） |
| | `fc_model` | FC 专用模型，为空则复用 `llm.model` |
| `memory` | `max_rounds` | 对话记忆保留轮数 |
| | `persist_dir` | 记忆持久化目录（跨进程重启保留） |
| `vad` | `energy_threshold` | 语音活动检测能量阈值 |

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
│   │   └── pipeline_bridge.h/cpp         # ASR → Skills/FC → LLM → TTS 桥接
│   ├── brain/
│   │   ├── skill_base.h                  # 技能抽象基类
│   │   ├── skill_manager.h/cpp           # 技能注册、调度、Function Calling 分发
│   │   ├── skill_utils.h                 # 技能工具函数
│   │   └── skills/
│   │       ├── skill_time.h/cpp          # 时间/日期查询
│   │       ├── skill_weather.h/cpp       # 天气查询 (wttr.in API)
│   │       ├── skill_calculator.h/cpp    # 数学计算
│   │       ├── skill_entertainment.h/cpp # 娱乐 (笑话/歌曲/故事)
│   │       ├── skill_riddle.h/cpp        # 谜语/脑筋急转弯
│   │       ├── skill_fortune.h/cpp       # 运势/抽签
│   │       ├── skill_poetry.h/cpp        # 诗词接龙/背诵
│   │       ├── skill_games.h/cpp         # 小游戏 (猜数字/成语接龙)
│   │       └── skill_memory.h/cpp        # 用户记忆 (偏好/事实存储)
│   ├── llm/
│   │   └── function_caller.h/cpp         # Function Calling: LLM 驱动工具选择
│   ├── memory/
│   │   ├── chat_memory.h/cpp             # 会话对话记忆 (多轮上下文)
│   │   ├── user_memory.h/cpp             # 用户长期记忆 (偏好/事实)
│   │   ├── token_counter.h/cpp           # 简单 token 计数
│   │   └── vector_store.h                # 向量存储 (记忆检索)
│   └── utils/
│       ├── base64.h                      # Base64 编解码
│       ├── http_client.h                 # HTTP 客户端 (libcurl)
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

## Skills & Function Calling

Pipeline 处理用户输入时分两条路径：

### 策略 1: Function Calling（LLM 驱动）

```
用户输入 → FC LLM (小模型, temperature=0) → 选择工具 + 提取参数
         → 执行 Skill → 结果注入 LLM 上下文 → LLM 生成回复
```

- FC LLM 专门做工具选择，输出 JSON `{"tool": "time", "args": {}}`
- Skill 结果作为 system 消息注入，LLM 据此生成自然语言回复
- 可在 `config.json` 中设置 `skills.fc_model` 使用独立小模型

### 策略 2: Keyword Match（兜底）

```
用户输入 → 关键词匹配 → 执行 Skill → 结果注入 LLM → LLM 生成回复
```

- 当 FC 未命中或 FC 未启用时使用
- 每个 Skill 定义自己的关键词列表（如 `match()` 返回 true）

### 可用技能

| 技能 | FC 名称 | 触发词 | 说明 |
|------|---------|--------|------|
| 时间 | `time` | 几点/时间/日期/星期几 | 获取当前时间和日期 |
| 天气 | `weather` | 天气/气温/下雨 | 查询城市天气 (wttr.in) |
| 计算器 | `calculator` | 计算/等于/加/减/乘/除 | 数学表达式求值 |
| 娱乐 | `entertainment` | 笑话/唱歌/讲故事 | 讲笑话、脑筋急转弯 |
| 谜语 | `riddle` | 谜语/猜谜/出个谜 | 猜谜互动 |
| 运势 | `fortune` | 运势/运气/抽签 | 每日运势 |
| 诗词 | `poetry` | 古诗/诗词/背诗 | 诗词背诵 |
| 游戏 | `games` | 猜数字/成语接龙 | 小游戏 |
| 记忆 | `memory` | 记住/还记得/我的 | 用户偏好和事实存储 |
