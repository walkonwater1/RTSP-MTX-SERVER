# RTSP Voice Interaction Server

**C++17 | WebSocket RFC 6455 + RTSP + MediaMTX | MIT License**

全链路 RTSP + WebSocket 语音交互服务器。替代原有的 Python 信令服务 + MediaMTX 方案，整合 ASR → Skills/FC → LLM → TTS 全链路处理，支持飞书机器人实时通知。

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
│  │  ASR (Zipformer CTC) → Skills → LLM (TensorRT/Ollama) → TTS   │ │
│  │  ┌─ LLM Backend (双后端抽象层) ────────────────────────────┐  │ │
│  │  │  TensorRT-LLM (Orin NX GPU):    Qwen3-4B, ~500ms       │  │ │
│  │  │  Ollama (fallback, 远程 CPU):   Qwen3-4B, ~2-4s        │  │ │
│  │  │  自动降级: TensorRT 不可用时 → Ollama                   │  │ │
│  │  └────────────────────────────────────────────────────────┘  │ │
│  │  ┌─ Skill System ────────────────────────────────────────────┐ │ │
│  │  │  9 个技能: 时间/天气/计算器/娱乐/谜语/运势/诗词/游戏/记忆  │ │ │
│  │  │  双策略: Function Calling (LLM驱动) + Keyword Match (兜底) │ │ │
│  │  └───────────────────────────────────────────────────────────┘ │ │
│  │  ┌─ Memory System (per-user isolation) ─────────────────────┐ │ │
│  │  │  会话记忆 (ChatMemory): 多轮对话上下文，按 user_id 持久化  │ │ │
│  │  │  用户记忆 (UserMemory): 长期偏好/事实，按 user_id 隔离存储 │ │ │
│  │  └───────────────────────────────────────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────────┘ │
│                                                                      │
│  ┌─ Session Manager ──────────────────────────────────────────────┐ │
│  │  每机器人一个 Session: 状态机 + TTS 队列 + ASR 累积 + 打断     │ │
│  └────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────┘
```

## 延迟与性能

### 甜点配置：TensorRT + Qwen3-4B

在当前硬件 (NVIDIA Jetson Orin NX) 上的最佳实践：

```
                    ┌────────── 端到端延迟 ≤1s（目标）──────────┐
                    │                                           │
  [ASR] ~100ms  →  [LLM TensorRT Qwen3-4B] ~500ms  →  [TTS 本地] ~200ms
                    │                                           │
                    └── 当前 TTS 用 Edge 云服务 ~1-3s ──────────┘
```

| 阶段 | 甜点方案 | 延迟 | 说明 |
|------|---------|------|------|
| **ASR** | Zipformer CTC (sherpa-onnx, 直接 C API) | ~100ms | CPU 推理，4 线程 |
| **LLM** | **TensorRT-LLM + Qwen3-4B** (Orin NX GPU) | **~500ms** | 当前最佳性价比，W4A16 量化 |
| **LLM** (fallback) | Ollama + Qwen3-4B (远程 192.168.2.107) | ~2-4s | 网络往返 + CPU 推理 |
| **TTS** (当前) | Edge-TTS (zh-CN-XiaoxiaoNeural, 微软云) | **~1-3s** | 音质好但延迟高，受网络波动影响 |
| **TTS** (计划) | ChatTTS / Piper 本地部署 | **~200ms** | 目标：总延迟控制在 1s 内 |

### LLM 后端

项目支持双后端架构，运行时自动选择：

```cpp
// llm_backend.h — 工厂模式
enum class LlmBackendType { kOllama, kTensorRT };
auto backend = CreateLlmBackend(cfg);  // 自动选择可用后端
```

- **TensorRT-LLM** (`src/llm/tensorrt_backend.cpp`): 在 Orin NX 上编译后启用，`qwen3:4b-w4a16` W4A16 量化 ~500ms/回复
- **Ollama** (`src/llm/ollama_backend.cpp`): 远程 HTTP API，自动 fallback，支持流式 (`ChatStream`)
- 配置中设置 `llm.backend = "tensorrt"` 即可优先使用 TensorRT，不可用时自动降级

### TTS 当前状态

```
当前:  Edge-TTS (zh-CN-XiaoxiaoNeural) → 微软云 TTS，音色温暖自然
       延迟: 1-3s（网络 + 合成 + ffmpeg 转码）
       瓶颈: 网络延迟不可控，是端到端延迟的主要来源

计划:  ChatTTS (本地 GPU) / Piper (本地 CPU)
       延迟: ~200ms
       目标: ASR(100ms) + LLM(500ms) + TTS(200ms) ≈ 800ms 端到端
```

> ChatTTS CLI 已就绪（`scripts/chattts_cli.py`），待 Orin NX 上 GPU 推理调通后切换。

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
sudo apt install libcurl4-openssl-dev libspdlog-dev nlohmann-json3-dev ffmpeg

# MediaMTX (RTSP 媒体服务器)
wget https://github.com/bluenviron/mediamtx/releases/download/v1.8.0/mediamtx_v1.8.0_linux_amd64.tar.gz
tar xzf mediamtx_v1.8.0_linux_amd64.tar.gz
sudo cp mediamtx /usr/local/bin/

# LLM 运行时（二选一）
## 方案A: TensorRT-LLM on Orin NX（甜点，~500ms）
# 编译时启用 -DTENSORRT_LLM_AVAILABLE=ON
## 方案B: 远程 Ollama（fallback）
curl -fsSL https://ollama.com/install.sh | sh
ollama pull qwen3:4b

# TTS 运行时
## 当前: Edge-TTS (微软云)
pip install edge-tts
## 计划: ChatTTS (本地 GPU TTS)
pip install chattts

# espeak-ng (TTS fallback, 最低延迟备选)
sudo apt install espeak-ng
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
./build/rtsp-mtx-server config.json

# 指定端口
./build/rtsp-mtx-server config.json --port-ws 8090 --port-rtsp 8554

# 使用外部 MediaMTX (不自动启动)
./build/rtsp-mtx-server config.json --no-mediamtx
```

## 配置

`config.json`:

```json
{
  "server": {
    "ws_port": 8090,
    "ws_path": "/ws/rtsp",
    "rtsp_port": 8554,
    "rtsp_base_url": "rtsp://192.168.2.110:8554",
    "bind_address": "0.0.0.0",
    "max_sessions": 32,
    "heartbeat_interval_sec": 15,
    "session_timeout_sec": 300
  },
  "mediamtx": {
    "binary_path": "/home/lixin/.local/bin/mediamtx",
    "auto_launch": true,
    "rtmp_disable": true,
    "hls_disable": true,
    "webrtc_disable": true,
    "srt_disable": true
  },
  "asr": {
    "model_path": "/eir/lixin/ASR-LLM-TTS/src/third_party/sherpa-onnx/zipformer-ctc-zh",
    "model_type": "zipformer_ctc",
    "sample_rate": 16000
  },
  "llm": {
    "host": "http://192.168.2.107:11434",
    "model": "qwen3:4b-w4a16",
    "system_prompt": "你是小希，一个活泼开朗的少女，说话可爱俏皮。回复控制在三句话以内。不要复读用户原话，直接回应问题本身。",
    "timeout_sec": 60,
    "max_response_tokens": 256
  },
  "tts": {
    "backend": "edge_tts",
    "edge_tts_script": "/home/lixin/eir/lixin/RTSP-MTX-SERVER/scripts/edge_tts_cli.py",
    "edge_tts_voice": "zh-CN-XiaoxiaoNeural",
    "output_sample_rate": 16000,
    "cache_tts": true,
    "cache_dir": "/dev/shm/rtsp-server/tts-cache"
  },
  "vad": {
    "backend": "adaptive",
    "energy_threshold": 0.008,
    "min_speech_frames": 15,
    "min_silence_frames": 15,
    "pre_speech_frames": 15,
    "adaptive_factor": 7.0,
    "min_energy": 0.025,
    "cooldown_frames": 25,
    "max_speech_sec": 60,
    "min_speech_sec": 0.5
  },
  "skills": {
    "enabled": true,
    "function_calling_enabled": false,
    "fc_model": ""
  },
  "memory": {
    "enabled": true,
    "max_rounds": 10,
    "max_tokens": 1536,
    "persist_dir": "/tmp/rtsp-server/memory"
  },
  "audio": {
    "debug_dump_dir": "/tmp/rtsp-server/debug"
  },
  "feishu": {
    "webhook_url": "https://open.feishu.cn/open-apis/bot/v2/hook/xxx"
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
| | `backend` | LLM 后端类型：`"tensorrt"`（Orin NX GPU, ~500ms）或默认 Ollama |
| | `system_prompt` | 系统人设 prompt，会自动附加当前时间上下文 |
| `tts` | `backend` | TTS 后端：`"edge_tts"`（当前，微软云）/ `"piper"`（本地）/ `"chattts"`（计划中） |
| | `edge_tts_voice` | Edge-TTS 音色：`zh-CN-XiaoxiaoNeural`（温暖） / `zh-CN-XiaoyiNeural`（明亮） / `zh-CN-YunxiNeural`（男声） |
| | `cache_dir` | TTS 缓存目录（建议 `/dev/shm` 内存文件系统，重启后自动清空） |
| `skills` | `function_calling_enabled` | 启用 LLM 驱动的工具选择（需要额外一次 FC LLM 调用） |
| | `fc_model` | FC 专用模型，为空则复用 `llm.model` |
| `memory` | `max_rounds` | 对话记忆保留轮数 |
| | `persist_dir` | 记忆持久化目录（按 user_id 分隔文件，跨重连恢复） |
| `audio` | `debug_dump_dir` | ASR/VAD 调试音频 dump 目录 |
| `feishu` | `webhook_url` | 飞书机器人 Webhook 地址（connect/disconnect/ASR/Pipeline 事件推送） |
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
│   ├── start_mediamtx.sh                # MediaMTX 启动脚本
│   ├── edge_tts_cli.py                  # Edge TTS CLI (微软云 TTS)
│   ├── chattts_cli.py                   # ChatTTS CLI (本地 GPU TTS，计划中)
│   ├── build_and_run.sh                 # 一键编译+运行
│   ├── monitor.sh                       # 服务监控脚本
│   └── rtsp-monitor.conf.example        # 监控配置模板
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
│   │   ├── llm_backend.h/cpp             # LLM 后端抽象接口（工厂模式）
│   │   ├── ollama_backend.h/cpp          # Ollama HTTP 后端 (Chat + ChatStream)
│   │   ├── tensorrt_backend.h/cpp        # TensorRT-LLM 后端 (Orin NX GPU)
│   │   └── function_caller.h/cpp         # Function Calling: LLM 驱动工具选择
│   ├── memory/
│   │   ├── chat_memory.h/cpp             # 会话对话记忆 (多轮上下文)
│   │   ├── user_memory.h/cpp             # 用户长期记忆 (偏好/事实)
│   │   ├── token_counter.h/cpp           # 简单 token 计数
│   │   └── vector_store.h                # 向量存储 (记忆检索)
│   └── utils/
│       ├── base64.h                      # Base64 编解码
│       ├── http_client.h                 # HTTP 客户端 (libcurl)
│       ├── feishu_notifier.h             # 飞书 Webhook 通知 (connect/disconnect/ASR/Pipeline)
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

## 多用户隔离

服务器支持多个机器人同时连接，每个用户的对话和记忆完全隔离：

| 组件 | 隔离方式 |
|------|---------|
| **Session** | 每个连接独立的 session_id，独立状态机、TTS 队列、ASR 缓冲区 |
| **ChatMemory** | 按 `user_id` 分片存储，同一用户重连后自动恢复对话历史 |
| **UserMemory** | 按 `user_id` 分片存储，用户 A 的记忆（如"我叫小明"）对用户 B 不可见 |
| **LLM Cache** | 缓存键包含 `user_id`，确保不同用户的相同问题返回各自缓存 |
| **ASR WAV** | 临时文件按 `session_id` 命名，避免并发 ASR 时的文件覆盖竞争 |
| **RTSP 流** | 每个 session 独立的推流/拉流路径（`/robot_audio/<uuid>`、`/tts_audio/<uuid>`） |

所有持久化文件也按 `user_id` 分隔：
- `user_memory_<user_id>.json` — 用户长期记忆
- `chat_<user_id>.json` — 对话历史（跨重连恢复）

**注意**：多个机器人使用**相同** `user_id` 连接时，它们将共享同一份长期记忆和对话历史。如需完全隔离，请为每个机器人分配不同的 `user_id`。

## 飞书通知

服务器支持通过飞书 Webhook 实时推送关键事件到飞书群聊，所有推送均为 fire-and-forget 非阻塞模式：

| 事件 | 触发时机 | 推送内容 |
|------|---------|---------|
| `OnConnect` | 客户端 WebSocket 连接建立 | user_id、session_id |
| `OnDisconnect` | 客户端断开连接 | user_id、连接时长 |
| `OnAsrResult` | ASR 识别完成 | 识别文本、user_id |
| `OnPipelineResult` | 管线处理完成 | LLM 延迟(ms)、TTS 延迟(ms)、总延迟(ms) |

配置方式：在 `config.json` 中设置 `feishu.webhook_url` 即可启用。

```json
"feishu": {
  "webhook_url": "https://open.feishu.cn/open-apis/bot/v2/hook/your-hook-id"
}
```

推送为独立线程执行，Webhook 不可达时不会阻塞主服务。

## 路线图

### 当前瓶颈：TTS 延迟

```
端到端延迟分解（当前）:
  ASR       ~100ms  ✅
  LLM       ~500ms  ✅ (TensorRT + Qwen3-4B on Orin NX)
  TTS       ~1-3s   ❌ (Edge-TTS 微软云，网络延迟不可控)
  ─────────────────
  总计      ~1.6-3.6s
```

### 计划：本地 TTS 模型

```
端到端延迟分解（目标）:
  ASR       ~100ms  ✅
  LLM       ~500ms  ✅
  TTS       ~200ms  🎯 (ChatTTS / Piper 本地推理)
  ─────────────────
  总计      ~800ms  🎯
```

| 方案 | 模型 | 延迟 | 音质 | 状态 |
|------|------|------|------|------|
| **ChatTTS** | 本地 GPU (Orin NX) | ~200ms | ⭐⭐⭐ 自然流畅 | `scripts/chattts_cli.py` 已就绪，待 GPU 推理调通 |
| Piper | 本地 CPU | ~300ms | ⭐⭐ 清晰可辨 | 已集成，音色偏机械 |
| Edge-TTS | 微软云 | ~1-3s | ⭐⭐⭐ 温暖自然 | 当前使用 |

ChatTTS 是 2.5 亿参数的神经 TTS 大模型，支持笑声/停顿等韵律控制，在 Orin NX GPU 上推理预计 ~200ms/句。切换后将实现 **≤1 秒端到端语音交互**。

```bash
# ChatTTS 测试（当前可手动验证音质）
python3 scripts/chattts_cli.py --text "你好呀，今天天气真不错" --output /tmp/test.wav
```

## 项目关联

| 项目 | 关系 |
|------|------|
| [ASR-LLM-TTS](../ASR-LLM-TTS/) | **核心依赖** — 链接 voice_pipeline/speech/brain/llm/audio/memory 库，提供 ASR/LLM/TTS/Skills 能力 |
| [GATEWAY-MULTI-AGENT](../GATEWAY-MULTI-AGENT/) | Gateway 的 interaction Runtime 可对接本服务的 WebSocket + RTSP 协议 |
| [MIDDLEWARE](../MIDDLEWARE/) | WebSocket/RTSP/MQTT 通信协议的知识参考 |
| [HARNESS](../HARNESS/) | 可用于本服务 C++ 模块的 AI 驱动自动化测试 |
