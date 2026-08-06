/**
 * RTSP Voice Interaction Server — Main Entry Point
 *
 * A self-contained RTSP + WebSocket server that integrates ASR, LLM, and TTS
 * for robot voice interaction. Replaces the colleague's Python signaling
 * service + MediaMTX setup.
 *
 * Architecture:
 *
 *   ┌─ RTSP Server (Main Thread) ───────────────────────────────┐
 *   │                                                            │
 *   │  ┌─ WebSocket Signaling Server (port 8090) ─────────────┐  │
 *   │  │  Thread: accept loop → per-client read/send threads  │  │
 *   │  │  Protocol: req_stream, start_push_audio, tts_start,  │  │
 *   │  │            playback_status, tts_state, stop_tts, ... │  │
 *   │  └──────────────────────────────────────────────────────┘  │
 *   │                                                            │
 *   │  ┌─ Session Manager ────────────────────────────────────┐  │
 *   │  │  Per-robot session: state machine, TTS queue,        │  │
 *   │  │  ASR accumulation                                    │  │
 *   │  └──────────────────────────────────────────────────────┘  │
 *   │                                                            │
 *   │  ┌─ RTSP Media Manager ─────────────────────────────────┐  │
 *   │  │  MediaMTX subprocess (port 8554)                     │  │
 *   │  │  ffmpeg pull: robot audio → PCM                      │  │
 *   │  │  ffmpeg push: TTS audio → RTSP                       │  │
 *   │  └──────────────────────────────────────────────────────┘  │
 *   │                                                            │
 *   │  ┌─ Voice Pipeline Bridge ──────────────────────────────┐  │
 *   │  │  ASR (Zipformer CTC) → LLM (Ollama qwen2.5) → TTS   │  │
 *   │  │  (Piper / espeak-ng)                                 │  │
 *   │  └──────────────────────────────────────────────────────┘  │
 *   │                                                            │
 *   │  ┌─ Interaction Engine ─────────────────────────────────┐  │
 *   │  │  Audio accumulation → VAD → ASR final → LLM → TTS   │  │
 *   │  │  → push to RTSP → send tts_start via WebSocket       │  │
 *   │  └──────────────────────────────────────────────────────┘  │
 *   └────────────────────────────────────────────────────────────┘
 *
 * Protocol flow (matching existing robot-side implementation):
 *
 *   Robot                          Server
 *     │                               │
 *     │── req_stream ───────────────→│  (WS handshake)
 *     │←─ stream_address ────────────│  (session_id + rtsp_url)
 *     │                               │
 *     │── ffmpeg push audio ────────→│  (RTSP)
 *     │── start_push_audio ─────────→│  (WS: notify push started)
 *     │←─ stream_ready ──────────────│  (WS: confirm receiving)
 *     │                               │
 *     │   [... audio streaming ...]   │
 *     │   [server does ASR → LLM]     │
 *     │                               │
 *     │←─ tts_start ─────────────────│  (WS: TTS audio URL)
 *     │── ffmpeg pull audio ←────────│  (RTSP: robot pulls TTS)
 *     │── playback_status(started) ─→│  (WS: suppress VAD)
 *     │   [... robot plays audio ...] │
 *     │── playback_status(completed)→│  (WS: resume ASR)
 *     │                               │
 *     │←─ interrupt ─────────────────│  (WS: stop playback)
 *     │── playback_status(completed)→│  (WS: confirm stopped)
 *
 * Usage:
 *   ./rtsp_server [config.json]
 *   ./rtsp_server config.json --port-ws 8090 --port-rtsp 8554
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"

#include "server/ws_server.h"
#include "server/session_manager.h"
#include "server/rtsp_manager.h"
#include "pipeline/pipeline_bridge.h"
#include "utils/logger.h"
#include "utils/feishu_notifier.h"

using json = nlohmann::json;
using namespace rtsp_server;

// --- Global state for signal handling ---
static std::atomic<bool> g_running{true};
static WsSignalingServer* g_ws_server = nullptr;
static RtspManager* g_rtsp_manager = nullptr;
static SessionManager* g_session_mgr = nullptr;
static PipelineBridge* g_pipeline = nullptr;
static std::string g_rtsp_base_url = "rtsp://192.168.2.110:8554";
static std::string g_http_base_url = "http://192.168.2.110:8090";  // for TTS file download
static std::unique_ptr<FeishuNotifier> g_feishu;

// --- Configuration ---
struct ServerConfig {
  // WebSocket
  int ws_port = 8090;
  std::string ws_bind = "0.0.0.0";
  std::string ws_path = "/ws/rtsp";
  int ws_ping_interval_sec = 15;

  // RTSP
  std::string rtsp_base_url = "rtsp://192.168.2.110:8554";
  int rtsp_port = 8554;
  std::string mediamtx_bin = "mediamtx";
  bool auto_launch_mediamtx = true;

  // Session
  int max_sessions = 32;
  int session_timeout_sec = 300;

  // Pipeline
  PipelineBridgeConfig pipeline;

  // Audio debug
  bool debug_dump_audio = false;
  std::string debug_dump_dir = "/tmp/rtsp-server/debug";

  // Feishu
  std::string feishu_webhook_url;
};

// --- Signal handler ---
static void SignalHandler(int sig) {
  // ASYNC-SIGNAL-SAFE: only set the flag. The main loop handles actual shutdown.
  // Calling Stop() from a signal handler risks deadlock (mutexes, thread joins).
  g_running.store(false);
}

// --- Config loading ---
static bool LoadConfig(const std::string& path, ServerConfig& cfg) {
  std::ifstream f(path);
  if (!f) {
    LOG_WARN("Cannot open config file: {}, using defaults", path);
    return false;
  }

  try {
    auto j = json::parse(f);

    // Server section
    if (j.contains("server")) {
      auto& s = j["server"];
      cfg.ws_port = s.value("ws_port", cfg.ws_port);
      cfg.ws_bind = s.value("bind_address", cfg.ws_bind);
      cfg.ws_path = s.value("ws_path", cfg.ws_path);
      cfg.max_sessions = s.value("max_sessions", cfg.max_sessions);
      cfg.session_timeout_sec = s.value("session_timeout_sec", cfg.session_timeout_sec);
      cfg.ws_ping_interval_sec = s.value("heartbeat_interval_sec", cfg.ws_ping_interval_sec);
      cfg.rtsp_base_url = s.value("rtsp_base_url", cfg.rtsp_base_url);
      cfg.rtsp_port = s.value("rtsp_port", cfg.rtsp_port);
    }

    // MediaMTX section
    if (j.contains("mediamtx")) {
      auto& m = j["mediamtx"];
      cfg.mediamtx_bin = m.value("binary_path", cfg.mediamtx_bin);
      cfg.auto_launch_mediamtx = m.value("auto_launch", cfg.auto_launch_mediamtx);
    }

    // ASR section
    if (j.contains("asr")) {
      auto& a = j["asr"];
      cfg.pipeline.asr_model_path = a.value("model_path", cfg.pipeline.asr_model_path);
      cfg.pipeline.asr_model_type = a.value("model_type", cfg.pipeline.asr_model_type);
    }

    // LLM section
    if (j.contains("llm")) {
      auto& l = j["llm"];
      cfg.pipeline.llm_host = l.value("host", cfg.pipeline.llm_host);
      cfg.pipeline.llm_model = l.value("model", cfg.pipeline.llm_model);
      cfg.pipeline.llm_system_prompt = l.value("system_prompt", cfg.pipeline.llm_system_prompt);
      cfg.pipeline.llm_timeout_sec = l.value("timeout_sec", cfg.pipeline.llm_timeout_sec);
    }

    // TTS section
    if (j.contains("tts")) {
      auto& t = j["tts"];
      cfg.pipeline.tts_backend = t.value("backend", cfg.pipeline.tts_backend);
      cfg.pipeline.tts_rate = t.value("rate", cfg.pipeline.tts_rate);
      cfg.pipeline.tts_voice = t.value("voice", cfg.pipeline.tts_voice);
      cfg.pipeline.tts_piper_model = t.value("piper_model", cfg.pipeline.tts_piper_model);
      cfg.pipeline.tts_edge_tts_script = t.value("edge_tts_script", cfg.pipeline.tts_edge_tts_script);
      cfg.pipeline.tts_edge_tts_voice = t.value("edge_tts_voice", cfg.pipeline.tts_edge_tts_voice);
      cfg.pipeline.tts_cache_enabled = t.value("cache_tts", cfg.pipeline.tts_cache_enabled);
      cfg.pipeline.tts_cache_dir = t.value("cache_dir", cfg.pipeline.tts_cache_dir);
    }

    // VAD section
    if (j.contains("vad")) {
      auto& v = j["vad"];
      cfg.pipeline.vad_energy_threshold = v.value("energy_threshold", cfg.pipeline.vad_energy_threshold);
      cfg.pipeline.vad_min_speech_frames = v.value("min_speech_frames", cfg.pipeline.vad_min_speech_frames);
      cfg.pipeline.vad_min_silence_frames = v.value("min_silence_frames", cfg.pipeline.vad_min_silence_frames);
      cfg.pipeline.vad_adaptive_factor = v.value("adaptive_factor", cfg.pipeline.vad_adaptive_factor);
      cfg.pipeline.vad_max_speech_sec = v.value("max_speech_sec", cfg.pipeline.vad_max_speech_sec);
      cfg.pipeline.vad_min_speech_sec = v.value("min_speech_sec", cfg.pipeline.vad_min_speech_sec);
    }

    // Audio debug
    if (j.contains("audio")) {
      auto& a = j["audio"];
      cfg.debug_dump_dir = a.value("debug_dump_dir", cfg.debug_dump_dir);
    }

    // Skills section
    if (j.contains("skills")) {
      auto& sk = j["skills"];
      cfg.pipeline.skills_enabled = sk.value("enabled", cfg.pipeline.skills_enabled);
      cfg.pipeline.function_calling_enabled = sk.value("function_calling_enabled", cfg.pipeline.function_calling_enabled);
      cfg.pipeline.fc_model = sk.value("fc_model", cfg.pipeline.fc_model);
    }

    // Memory section
    if (j.contains("memory")) {
      auto& m = j["memory"];
      cfg.pipeline.memory_enabled = m.value("enabled", cfg.pipeline.memory_enabled);
      cfg.pipeline.memory_max_rounds = m.value("max_rounds", cfg.pipeline.memory_max_rounds);
      cfg.pipeline.memory_max_tokens = m.value("max_tokens", cfg.pipeline.memory_max_tokens);
      cfg.pipeline.memory_persist_dir = m.value("persist_dir", cfg.pipeline.memory_persist_dir);
    }

    // Feishu section
    if (j.contains("feishu")) {
      auto& f = j["feishu"];
      cfg.feishu_webhook_url = f.value("webhook_url", cfg.feishu_webhook_url);
    }

    // Env var override: FEISHU_WEBHOOK_URL takes precedence over config file.
    // Keeps secrets out of git-committed config.json.
    const char* env_url = std::getenv("FEISHU_WEBHOOK_URL");
    if (env_url && env_url[0] != '\0') {
      cfg.feishu_webhook_url = env_url;
    }

    LOG_INFO("Config loaded from {}", path);
    return true;
  } catch (const json::exception& e) {
    LOG_ERROR("Config parse error: {}", e.what());
    return false;
  }
}

// ============================================================================
// WebSocket Message Handler — implements the full signaling protocol
// ============================================================================

static void HandleWsMessage(int client_fd, const std::string& event,
                            const std::string& raw_json) {
  LOG_DEBUG("[MSG] fd={} event={} payload={}", client_fd, event, raw_json);

  try {
    auto j = json::parse(raw_json);

    // --- req_stream: handshake ---
    if (event == "req_stream") {
      std::string user_id = j.value("data", json::object()).value("user_id", "unknown");
      std::string mode = j.value("data", json::object()).value("mode", "voice");

      LOG_INFO("[MSG] req_stream from user='{}', mode={}", user_id, mode);

      // Use the configured rtsp_base_url (not hardcoded 127.0.0.1)
      Session* session = g_session_mgr->CreateSession(user_id, mode, g_rtsp_base_url);

      if (!session) {
        json err;
        err["event"] = "error";
        err["message"] = "max sessions reached";
        g_ws_server->SendMessage(client_fd, err.dump());
        return;
      }

      session->ws_fd = client_fd;

      // Store client protocol params (with defaults)
      session->llm_type = j.value("data", json::object()).value("llm_type", "offline");
      session->tts_type = j.value("data", json::object()).value("tts_type", "stream");
      session->qa_type = j.value("data", json::object()).value("qa_type", "llm");
      session->interrupt_type = j.value("interrupt_type", 1);
      LOG_DEBUG("[MSG] session {} params: llm={}, tts={}, qa={}, interrupt={}",
               session->session_id, session->llm_type, session->tts_type,
               session->qa_type, session->interrupt_type);

      // Feishu: push connect event
      if (g_feishu) g_feishu->OnConnect(user_id, client_fd);

      // Register session → user mapping for per-user memory isolation
      if (g_pipeline) {
        g_pipeline->RegisterSessionUser(session->session_id, user_id);
      }

      // Send stream_address response
      json resp;
      resp["event"] = "stream_address";
      resp["session_id"] = session->session_id;
      resp["protocol_version"] = "2.0";
      int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      resp["timestamp"] = now_ms;
      resp["data"]["rtsp_url"] = session->rtsp_push_url;
      resp["data"]["user_id"] = user_id;
      resp["data"]["mode"] = mode;

      g_ws_server->SendMessage(client_fd, resp.dump());

      LOG_DEBUG("[MSG] session {} assigned push_url={}",
               session->session_id, session->rtsp_push_url);
      return;
    }

    // --- start_push_audio: robot started pushing mic audio ---
    if (event == "start_push_audio") {
      std::string session_id = j.value("session_id", "");
      Session* session = g_session_mgr->FindSession(session_id);

      if (!session) {
        LOG_WARN("[MSG] start_push_audio for unknown session {}", session_id);
        json err;
        err["event"] = "error";
        err["message"] = "unknown session";
        g_ws_server->SendMessage(client_fd, err.dump());
        return;
      }

      session->audio_streaming.store(true);
      session->push_started_ms.store(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count());

      // Invalidate any in-flight pipeline from a previous wake cycle.
      // The robot is starting fresh audio — old results must not be sent.
      session->NewGeneration();

      // Reset ASR state for new wakeup — stale state from before sleep
      // would prevent recognition from working.
      {
        std::lock_guard<std::mutex> asr_lock(session->asr_mutex);
        session->asr_buffer.clear();
        session->asr_finalized = false;
        session->speech_detected = false;
        session->silence_frames = 0;
      }
      session->llm_triggered = false;  // reset for new wakeup
      session->first_utterance = true;  // first speech after wake skips cooldown
      session->TransitionTo(SessionState::Streaming);
      session->Touch();

      LOG_INFO("[MSG] start_push_audio for session {}", session_id);

      // Start pulling robot audio from RTSP for ASR
      if (g_rtsp_manager) {
        g_rtsp_manager->StartAudioPull(session_id, session->rtsp_push_url,
            [session_id](const int16_t* pcm, int n) {
              // Audio accumulation callback
              Session* s = g_session_mgr->FindSession(session_id);
              if (!s || !s->audio_streaming.load()) return;

              // Suppress ASR accumulation while TTS is playing — the mic
              // captures speaker output + ambient noise, not user speech.
              if (s->GetState() == SessionState::Playing) return;

              // Post-TTS guard: discard audio for kPostTtsGuardMs after TTS ends,
              // preventing residual speaker echo from false-triggering ASR.
              {
                int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                int64_t guard = s->tts_end_ms.load();
                if (guard > 0 && (now_ms - guard) < Session::kPostTtsGuardMs) return;
              }

              // Keep session alive during active audio streaming — prevents
              // premature timeout during long silent/idle periods.
              s->Touch();

              std::lock_guard<std::mutex> lock(s->asr_mutex);

              // Append PCM to buffer
              s->asr_buffer.append(reinterpret_cast<const char*>(pcm), n * sizeof(int16_t));

              static constexpr int kMaxBufferSamples = 16000 * 60; // 60 seconds max
              static constexpr int kCooldownMs = 800;              // min interval between ASR triggers
              static constexpr int kMinAudioSamples = 16000 / 2;   // min 0.5s audio

              int current_samples = s->asr_buffer.size() / sizeof(int16_t);

              // Calculate RMS of current chunk
              float sum_sq = 0;
              for (int i = 0; i < n; i++) {
                float sample = pcm[i] / 32768.0f;
                sum_sq += sample * sample;
              }
              float rms = std::sqrt(sum_sq / n);

              // Adaptive VAD: track noise floor via EMA, derive speech/silence
              // thresholds dynamically.  Clamped to minimums so quiet-room
              // sensitivity never lets faint TTS echo through.
              if (!s->speech_detected && rms < s->noise_baseline * 2.0f) {
                s->noise_baseline = 0.95f * s->noise_baseline + 0.05f * rms;
              }
              float speech_rms  = std::max(s->noise_baseline * 3.0f, Session::kMinSpeechRms);
              float silence_rms = std::max(s->noise_baseline * 2.0f, Session::kMinSilenceRms);

              // Speech detection: high energy → someone is talking
              if (rms > speech_rms) {
                bool was_speech = s->speech_detected;
                s->speech_detected = true;
                s->silence_frames = 0;  // reset silence counter on speech

                // Interrupt: new speech detected while TTS is playing
                if (!was_speech && s->GetState() == SessionState::Playing &&
                    s->interrupt_type == 1 && g_ws_server) {
                  std::string tts_id;
                  {
                    std::lock_guard<std::mutex> tlock(s->tts_mutex);
                    if (!s->tts_queue.empty()) {
                      tts_id = s->tts_queue.front().tts_id;
                    } else if (!s->current_tts_id.empty()) {
                      tts_id = s->current_tts_id;
                    }
                  }
                  if (!tts_id.empty()) {
                    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                    // Invalidate in-flight pipeline processing —
                    // any background thread from the previous generation will
                    // detect the generation_id change and discard its results.
                    s->NewGeneration();

                    // Protocol: stop_tts first, then interrupt
                    json stop_msg;
                    stop_msg["event"] = "stop_tts";
                    stop_msg["session_id"] = session_id;
                    stop_msg["timestamp"] = now_ms;
                    stop_msg["data"]["tts_id"] = tts_id;
                    stop_msg["data"]["reason"] = "valid_interrupt";
                    g_ws_server->SendMessage(s->ws_fd, stop_msg.dump());

                    json intr_msg;
                    intr_msg["event"] = "interrupt";
                    intr_msg["session_id"] = session_id;
                    intr_msg["timestamp"] = now_ms;
                    intr_msg["data"]["tts_id"] = tts_id;
                    g_ws_server->SendMessage(s->ws_fd, intr_msg.dump());

                    LOG_INFO("[INTERRUPT] session {} interrupted tts_id={}, generation={}",
                             session_id, tts_id, s->generation_id.load());

                    // Clear TTS queue
                    {
                      std::lock_guard<std::mutex> tlock(s->tts_mutex);
                      s->tts_queue.clear();
                    }
                    s->current_tts_id.clear();

                    // Reset ASR state for new utterance
                    {
                      std::lock_guard<std::mutex> asr_lock(s->asr_mutex);
                      s->asr_buffer.clear();
                      s->asr_finalized = false;
                      s->speech_detected = false;
                      s->silence_frames = 0;
                    }
                    s->llm_triggered = false;
                    s->first_utterance = true;
                  }
                }
              }

              // End-of-speech detection: speech was detected, then silence follows.
              // Use a silence frame counter (hysteresis) to avoid premature trigger
              // on brief pauses between syllables.
              bool is_silence = (rms < silence_rms);
              if (is_silence && s->speech_detected) {
                s->silence_frames++;
              }

              bool has_speech = s->speech_detected;
              bool enough_audio = (current_samples > kMinAudioSamples);  // min 0.5s
              bool enough_silence = (s->silence_frames >= Session::kSilenceFramesThreshold);
              bool cooldown_ok = true;
              if (!s->first_utterance) {
                int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                cooldown_ok = (now_ms - s->last_asr_finalized_ms) > kCooldownMs;
              }

              if (is_silence && has_speech && enough_audio && enough_silence && !s->asr_finalized && cooldown_ok) {
                s->asr_finalized = true;
                auto asr_trigger_tp = std::chrono::steady_clock::now();
                LOG_DEBUG("[ASR] speech segment end for session {} ({} samples, rms={:.4f})",
                         session_id, current_samples, rms);

                // Process ASR in a background thread to not block the audio callback.
                // Use generation_id to detect staleness: if a new utterance starts
                // (interrupt, new speech) while this thread runs, its results are discarded.
                std::thread([session_id, asr_trigger_tp]() {
                  Session* s = g_session_mgr->FindSession(session_id);
                  if (!s) return;

                  // Capture the generation this work belongs to.
                  // If generation_id changes (interrupt / new utterance), we discard results.
                  int64_t my_generation = s->generation_id.load();

                  // Mark pipeline as running and reset cancellation
                  {
                    std::lock_guard<std::mutex> plock(s->pipeline_mutex);
                    s->pipeline_running.store(true);
                    s->ResetPipelineCancel();
                  }

                  int64_t asr_ms = 0;
                  std::string asr_text;
                  {
                    std::lock_guard<std::mutex> lock(s->asr_mutex);
                    if (g_pipeline && g_pipeline->IsReady()) {
                      auto* pcm_ptr = reinterpret_cast<const int16_t*>(s->asr_buffer.data());
                      int pcm_count = s->asr_buffer.size() / sizeof(int16_t);
                      asr_text = g_pipeline->TranscribeAudio(pcm_ptr, pcm_count, session_id);
                      asr_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - asr_trigger_tp).count();
                    }
                    s->asr_buffer.clear();
                    s->asr_finalized = false;
                    s->speech_detected = false;  // reset for next utterance
                    s->silence_frames = 0;
                    s->first_utterance = false;  // subsequent utterances need cooldown
                    s->last_asr_finalized_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                  }

                  // Check if interrupted during ASR
                  if (s->generation_id.load() != my_generation) {
                    LOG_DEBUG("[ASR] generation changed, discarding for session {}", session_id);
                    s->pipeline_running.store(false);
                    return;
                  }

                  if (asr_text.empty()) {
                    LOG_DEBUG("[ASR] no text recognized for session {}", session_id);
                    s->pipeline_running.store(false);
                    return;
                  }

                  // Dedup: only one LLM call per conversation turn
                  if (s->llm_triggered) {
                    LOG_DEBUG("[ASR] LLM already triggered for session {}, skipping", session_id);
                    s->pipeline_running.store(false);
                    return;
                  }
                  s->llm_triggered = true;

                    LOG_INFO("[ASR] \"{}\"", asr_text);

                    // Feishu: push ASR result
                    if (g_feishu) g_feishu->OnAsrResult(s->user_id, asr_text);

                    // Send ASR result to robot
                    if (g_ws_server) {
                      json asr_msg;
                      asr_msg["event"] = "asr_result";
                      asr_msg["session_id"] = s->session_id;
                      asr_msg["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
                      asr_msg["data"]["text"] = asr_text;
                      g_ws_server->SendMessage(s->ws_fd, asr_msg.dump());
                    }

                    // Check generation before expensive LLM call
                    if (s->generation_id.load() != my_generation) {
                      LOG_DEBUG("[ASR] generation changed before LLM for session {}", session_id);
                      s->pipeline_running.store(false);
                      return;
                    }

                    // Run LLM → TTS with cancellation support.
                    // The cancel_pipeline flag is tied to the session and set by
                    // NewGeneration() when an interrupt occurs.
                    if (g_pipeline && g_pipeline->IsReady()) {
                      auto result = g_pipeline->ProcessText(asr_text, session_id,
                                                            &s->cancel_pipeline);

                      // Re-check: session may have been removed by disconnect handler
                      // while ProcessText was running.
                      s = g_session_mgr->FindSession(session_id);
                      if (!s) {
                        LOG_DEBUG("[ASR] session {} gone after LLM/TTS", session_id);
                        return;
                      }

                      // Check if result is stale (new utterance started while we were processing)
                      if (s->generation_id.load() != my_generation) {
                        LOG_DEBUG("[ASR] generation changed, discarding stale result for session {}",
                                 session_id);
                        s->pipeline_running.store(false);
                        return;
                      }

                      // Check if the pipeline result itself indicates cancellation
                      if (!result.ok && result.error.find("cancelled") != std::string::npos) {
                        LOG_DEBUG("[ASR] pipeline cancelled for session {}", session_id);
                        s->pipeline_running.store(false);
                        return;
                      }

                      if (!result.llm_response.empty()) {
                        // Final generation check before sending to client
                        if (s->generation_id.load() != my_generation) {
                          LOG_DEBUG("[ASR] generation changed before sending, discarding");
                          s->pipeline_running.store(false);
                          return;
                        }

                        // ── Consolidated latency (ASR + LLM + TTS) ──────────
                        int64_t total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - asr_trigger_tp).count();
                        // Color: green <2s, yellow <4s, red >=4s
                        const char* c = total_ms < 2000 ? "\033[1;32m" :
                                        total_ms < 4000 ? "\033[1;33m" : "\033[1;31m";
                        LOG_INFO("\033[1;36m[Pipeline]\033[0m {} | ASR={}ms LLM={}ms TTS={}ms {}TOTAL={}ms\033[0m",
                                 session_id, asr_ms, result.llm_ms, result.tts_ms, c, total_ms);

                        // Send LLM result to robot
                        if (g_ws_server) {
                          json llm_msg;
                          llm_msg["event"] = "llm_result";
                          llm_msg["session_id"] = s->session_id;
                          llm_msg["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
                          llm_msg["data"]["text"] = result.llm_response;
                          g_ws_server->SendMessage(s->ws_fd, llm_msg.dump());
                        }

                        // Feishu: push pipeline result with latency
                        if (g_feishu) {
                          g_feishu->OnPipelineResult(s->user_id, asr_text,
                              result.llm_response, result.llm_ms,
                              result.tts_ms, result.total_ms);
                        }

                        // Queue TTS — serve WAV file via HTTP download
                        if (!result.tts_audio_path.empty()) {
                          std::lock_guard<std::mutex> tlock(s->tts_mutex);
                          TtsItem item;
                          item.tts_id = s->GenerateTtsId();
                          item.text = result.llm_response;
                          item.audio_path = result.tts_audio_path;

                          // Build HTTP download URL from audio_path basename
                          // File: /dev/shm/rtsp-server/tts_<sid>_<ts>.wav
                          // Token: tts_<sid>_<ts> (filename without .wav)
                          std::string fname = item.audio_path;
                          auto last_slash = fname.find_last_of('/');
                          if (last_slash != std::string::npos) fname = fname.substr(last_slash + 1);
                          if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".wav") {
                            fname = fname.substr(0, fname.size() - 4);
                          }
                          item.audio_url = g_http_base_url + "/file?token=" + fname;
                          item.created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
                          s->tts_queue.push_back(item);

                          // Send tts_start with HTTP download URL
                          if (g_ws_server) {
                            json tts_msg;
                            tts_msg["event"] = "tts_start";
                            tts_msg["session_id"] = s->session_id;
                            tts_msg["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            tts_msg["data"]["tts_id"] = item.tts_id;
                            tts_msg["data"]["audio_url"] = item.audio_url;
                            tts_msg["data"]["audio_path"] = item.audio_path;
                            tts_msg["data"]["text"] = result.llm_response;
                            g_ws_server->SendMessage(s->ws_fd, tts_msg.dump());
                            LOG_DEBUG("[TTS] tts_start sent to session {}: tts_id={}, url={}",
                                     session_id, item.tts_id, item.audio_url);
                          }

                          s->TransitionTo(SessionState::Playing);
                          s->current_tts_id = item.tts_id;
                        }
                      }
                    }
                    s->pipeline_running.store(false);
                  }).detach();
                }

                // Reset if buffer gets too large without finalization
                if (current_samples > kMaxBufferSamples) {
                  LOG_WARN("[ASR] buffer overflow for session {}, resetting", session_id);
                  s->asr_buffer.clear();
                  s->asr_finalized = false;
                  s->speech_detected = false;
                }
            });
      }

      // Send stream_ready response
      int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      int64_t latency_ms = now_ms - session->push_started_ms.load();
      json ready;
      ready["event"] = "stream_ready";
      ready["session_id"] = session_id;
      ready["timestamp"] = now_ms;
      ready["data"]["rtsp_url"] = session->rtsp_push_url;
      ready["data"]["server_received"] = now_ms;
      ready["data"]["latency_ms"] = latency_ms;
      ready["receive_aduio_stream"] = session_id;  // legacy typo from old protocol
      g_ws_server->SendMessage(client_fd, ready.dump());

      return;
    }

    // --- playback_status: robot reports TTS playback state ---
    if (event == "playback_status") {
      std::string session_id = j.value("session_id", "");
      std::string status = j.value("data", json::object()).value("status", "");

      Session* session = g_session_mgr->FindSession(session_id);
      if (!session) {
        LOG_WARN("[MSG] playback_status for unknown session {}", session_id);
        return;
      }

      session->Touch();
      LOG_DEBUG("[MSG] playback_status({}) from session {}", status, session_id);

      if (status == "completed") {
        // Robot finished playing TTS (or starting a new speech turn).
        // Always reset these flags — the client sends this message both at
        // TTS-end and at speech-start (handle_speech_start), and both are
        // valid "new turn" signals.
        session->llm_triggered = false;  // re-arm for next speech turn
        session->first_utterance = true;  // first speech after TTS skips cooldown

        // Guard period: only set when TTS was actually playing.
        // The speech-start path (handle_speech_start) sends "completed"
        // while already in Streaming — we must NOT start a guard then,
        // or the first 500ms of real user speech is discarded.
        if (session->GetState() == SessionState::Playing) {
          session->tts_end_ms.store(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count());
        }

        session->TransitionTo(SessionState::Streaming);
        session->current_tts_id.clear();

        // Process next TTS in queue (HTTP download, no RTSP push needed)
        std::lock_guard<std::mutex> tlock(session->tts_mutex);
        if (!session->tts_queue.empty()) {
          session->tts_queue.pop_front();
        }
        if (!session->tts_queue.empty()) {
          auto& next = session->tts_queue.front();
          session->current_tts_id = next.tts_id;

          // Send next tts_start with HTTP download URL
          json tts_msg;
          tts_msg["event"] = "tts_start";
          tts_msg["session_id"] = session_id;
          tts_msg["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
          tts_msg["data"]["tts_id"] = next.tts_id;
          tts_msg["data"]["audio_url"] = next.audio_url;
          tts_msg["data"]["audio_path"] = next.audio_path;
          tts_msg["data"]["text"] = next.text;
          g_ws_server->SendMessage(client_fd, tts_msg.dump());

          session->TransitionTo(SessionState::Playing);
        }
      } else if (status == "started") {
        // Robot started playing TTS — discard stale audio and suppress VAD
        {
          std::lock_guard<std::mutex> lock(session->asr_mutex);
          session->asr_buffer.clear();
          session->speech_detected = false;
          session->asr_finalized = false;
        }
        session->TransitionTo(SessionState::Playing);
      }

      return;
    }

    // --- tts_state: unified TTS protocol ---
    if (event == "tts_state") {
      std::string session_id = j.value("session_id", "");
      bool is_playing = j.value("data", json::object()).value("is_playing", false);
      std::string reason = j.value("data", json::object()).value("reason", "");

      Session* session = g_session_mgr->FindSession(session_id);
      if (!session) {
        LOG_WARN("[MSG] tts_state for unknown session {}", session_id);
        return;
      }

      session->Touch();
      LOG_DEBUG("[MSG] tts_state(is_playing={}, reason={}) from session {}",
               is_playing, reason, session_id);

      if (!is_playing && (reason == "ended" || reason == "stop_tts" ||
                          reason == "interrupt" || reason == "client_stop")) {
        session->llm_triggered = false;  // re-arm for next speech turn
        session->first_utterance = true;  // first speech after TTS skips cooldown
        session->TransitionTo(SessionState::Streaming);
      }

      return;
    }

    // --- ping: keep-alive heartbeat ---
    if (event == "ping") {
      // Handled in ws_server.cpp ClientReadLoop — responds with pong
      // Update heartbeat timestamp
      Session* session = g_session_mgr->FindSessionByFd(client_fd);
      if (session) {
        session->Touch();
      }
      return;
    }

    // --- interrupt: client requests server to cancel current processing ---
    if (event == "interrupt" || event == "cancel_pipeline" ||
        event == "stop_generation") {
      std::string session_id = j.value("session_id", "");
      Session* session = g_session_mgr->FindSession(session_id);
      if (!session) {
        LOG_WARN("[MSG] {} for unknown session {}", event, session_id);
        return;
      }

      session->Touch();
      LOG_INFO("[MSG] {} from session {} — cancelling pipeline", event, session_id);

      // Invalidate current generation — any in-flight background thread will
      // see the generation_id mismatch and discard its results.
      session->NewGeneration();

      // Clear TTS queue
      {
        std::lock_guard<std::mutex> tlock(session->tts_mutex);
        session->tts_queue.clear();
      }
      session->current_tts_id.clear();

      // Reset ASR state
      {
        std::lock_guard<std::mutex> asr_lock(session->asr_mutex);
        session->asr_buffer.clear();
        session->asr_finalized = false;
        session->speech_detected = false;
        session->silence_frames = 0;
      }
      session->llm_triggered = false;
      session->first_utterance = true;

      // Acknowledge the interrupt
      int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      json ack;
      ack["event"] = "interrupt_ack";
      ack["session_id"] = session_id;
      ack["timestamp"] = now_ms;
      g_ws_server->SendMessage(client_fd, ack.dump());

      return;
    }

    // --- stop_tts: client requests server to stop TTS playback ---
    if (event == "stop_tts") {
      std::string session_id = j.value("session_id", "");
      Session* session = g_session_mgr->FindSession(session_id);
      if (!session) {
        LOG_WARN("[MSG] stop_tts for unknown session {}", session_id);
        return;
      }

      session->Touch();
      LOG_INFO("[MSG] stop_tts from session {}", session_id);

      // Cancel any in-flight generation
      session->NewGeneration();

      // Clear TTS queue
      {
        std::lock_guard<std::mutex> tlock(session->tts_mutex);
        session->tts_queue.clear();
      }
      session->current_tts_id.clear();

      // Reset for new speech turn
      session->llm_triggered = false;
      session->first_utterance = true;
      session->TransitionTo(SessionState::Streaming);

      return;
    }

    // Unknown event
    LOG_DEBUG("[MSG] unhandled event '{}' from fd={}", event, client_fd);

  } catch (const json::exception& e) {
    LOG_ERROR("[MSG] JSON parse error in handler: {}", e.what());
  }
}

// ============================================================================
// Client Connect / Disconnect Handlers
// ============================================================================

static void HandleWsConnect(int client_fd) {
  LOG_INFO("[CONN] client connected, fd={}", client_fd);
}

static void HandleWsDisconnect(int client_fd) {
  LOG_INFO("[CONN] client disconnected, fd={}", client_fd);

  // Find and cleanup session
  Session* session = g_session_mgr->FindSessionByFd(client_fd);
  if (session) {
    // Feishu: push disconnect event
    if (g_feishu) g_feishu->OnDisconnect(session->user_id);

    LOG_DEBUG("[CONN] cleaning up session {}", session->session_id);

    // Stop RTSP pipelines
    if (g_rtsp_manager) {
      g_rtsp_manager->StopAudioPull(session->session_id);
    }

    g_session_mgr->RemoveSession(session->session_id);
  }
}

// ============================================================================
// Periodic Tasks
// ============================================================================

static void SessionCleanupLoop(ServerConfig& cfg) {
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(30));

    // Collect expired session IDs before purging so we can stop their RTSP pipelines.
    // PurgeExpired previously removed sessions without cleaning up RTSP,
    // leaving stale ffmpeg processes running and consuming resources.
    auto expired_ids = g_session_mgr->GetExpiredSessionIds(cfg.session_timeout_sec * 1000);
    for (const auto& sid : expired_ids) {
      if (g_rtsp_manager) {
        g_rtsp_manager->StopAudioPull(sid);
      }
      g_session_mgr->RemoveSession(sid);
    }
    if (!expired_ids.empty()) {
      LOG_INFO("[Cleanup] purged {} expired sessions, {} remaining",
               expired_ids.size(), g_session_mgr->SessionCount());
    }
  }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
  // --- Parse args ---
  std::string config_path;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << "RTSP Voice Interaction Server\n"
                << "Usage: " << argv[0] << " [config.json] [--port-ws N] [--port-rtsp N]\n"
                << "  config.json   Server configuration file\n"
                << "  --port-ws N   Override WebSocket port (default: 8090)\n"
                << "  --port-rtsp N Override RTSP port (default: 8554)\n"
                << "  --no-mediamtx Don't auto-launch MediaMTX (use external)\n"
                << "  --help, -h    Show this help\n";
      return 0;
    } else if (arg.find("--") == 0) {
      // Handle flag overrides
      if (arg == "--no-mediamtx") {
        // Will be applied after config load
      }
    } else {
      config_path = arg;
    }
  }

  // --- Init logging ---
  rtsp_server::init_logger("rtsp_server.log", "info");

  LOG_INFO("==============================================================");
  LOG_INFO("  RTSP Voice Interaction Server v0.1.0");
  LOG_INFO("==============================================================");

  // --- Load config ---
  ServerConfig cfg;
  if (!config_path.empty()) {
    LoadConfig(config_path, cfg);
  }

  // CLI overrides
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--port-ws" && i + 1 < argc) {
      cfg.ws_port = std::atoi(argv[++i]);
    } else if (arg == "--port-rtsp" && i + 1 < argc) {
      cfg.rtsp_port = std::atoi(argv[++i]);
      cfg.rtsp_base_url = "rtsp://192.168.2.110:" + std::to_string(cfg.rtsp_port);
    } else if (arg == "--no-mediamtx") {
      cfg.auto_launch_mediamtx = false;
    }
  }

  // Make rtsp_base_url available to the WebSocket message handler
  g_rtsp_base_url = cfg.rtsp_base_url;
  // Build HTTP base URL from rtsp_base_url (same host, WS port)
  {
    // Extract host from rtsp://host:port → http://host:ws_port
    std::string host = cfg.rtsp_base_url;
    auto scheme_end = host.find("://");
    if (scheme_end != std::string::npos) host = host.substr(scheme_end + 3);
    auto port_pos = host.find(':');
    if (port_pos != std::string::npos) host = host.substr(0, port_pos);
    g_http_base_url = "http://" + host + ":" + std::to_string(cfg.ws_port);
  }
  LOG_INFO("  RTSP base: {}", g_rtsp_base_url);
  LOG_INFO("  HTTP base: {}", g_http_base_url);

  // Init Feishu notifier (fire-and-forget, disabled if no webhook URL)
  if (!cfg.feishu_webhook_url.empty()) {
    g_feishu = std::make_unique<FeishuNotifier>(cfg.feishu_webhook_url);
    LOG_INFO("  Feishu:    webhook enabled");
  }

  // Create directories
  system("mkdir -p /tmp/rtsp-server/debug");
  system("mkdir -p /dev/shm/rtsp-server/tts-cache");
  system("mkdir -p /dev/shm/rtsp-server");

  // --- Init pipeline ---
  PipelineBridge pipeline(cfg.pipeline);
  g_pipeline = &pipeline;

  if (!pipeline.Initialize()) {
    LOG_ERROR("Failed to initialize voice pipeline");
    return 1;
  }

  // --- Init session manager ---
  SessionManager session_mgr;
  session_mgr.SetMaxSessions(cfg.max_sessions);
  g_session_mgr = &session_mgr;

  // --- Init RTSP manager ---
  RtspManager rtsp_mgr;
  g_rtsp_manager = &rtsp_mgr;

  rtsp_mgr.Configure(cfg.mediamtx_bin, cfg.rtsp_port, cfg.auto_launch_mediamtx);
  if (!rtsp_mgr.Start()) {
    LOG_WARN("RTSP media manager failed to start (continuing with WebSocket only)");
  }

  // --- Init WebSocket server ---
  WsServerConfig ws_cfg;
  ws_cfg.port = cfg.ws_port;
  ws_cfg.bind_address = cfg.ws_bind;
  ws_cfg.ws_path = cfg.ws_path;
  ws_cfg.ping_interval_sec = cfg.ws_ping_interval_sec;
  ws_cfg.max_connections = cfg.max_sessions;

  WsSignalingServer ws_server(ws_cfg);
  g_ws_server = &ws_server;

  ws_server.SetMessageCallback(HandleWsMessage);
  ws_server.SetConnectCallback(HandleWsConnect);
  ws_server.SetDisconnectCallback(HandleWsDisconnect);

  if (!ws_server.Start()) {
    LOG_ERROR("Failed to start WebSocket signaling server");
    return 1;
  }

  // --- Signal handlers ---
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  // --- Start cleanup thread ---
  std::thread cleanup_thread(SessionCleanupLoop, std::ref(cfg));

  // --- Print status ---
  LOG_INFO("==============================================================");
  LOG_INFO("  Server is running");
  LOG_INFO("  WebSocket: ws://{}:{}{}",
           cfg.ws_bind, cfg.ws_port, cfg.ws_path);
  LOG_INFO("  RTSP:      {} (MediaMTX: {})",
           cfg.rtsp_base_url, cfg.auto_launch_mediamtx ? "managed" : "external");
  LOG_INFO("  LLM:       {} ({})", cfg.pipeline.llm_host, cfg.pipeline.llm_model);
  LOG_INFO("  Sessions:  max {}", cfg.max_sessions);
  LOG_INFO("==============================================================");
  LOG_INFO("  Waiting for robot connections...");
  LOG_INFO("");

  // --- Main loop ---
  // Poll at 100ms for responsive shutdown (signals may not interrupt sleep_for).
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Periodic status
    static int tick = 0;
    tick++;
    if (tick % 600 == 0) {  // every 60 seconds (600 * 100ms)
      LOG_INFO("[Status] {} active sessions, {} WS connections",
               session_mgr.SessionCount(), ws_server.ConnectionCount());
    }
  }

  // --- Shutdown ---
  LOG_INFO("Shutting down...");

  ws_server.Stop();
  rtsp_mgr.Stop();

  g_running.store(false);
  if (cleanup_thread.joinable()) cleanup_thread.join();

  LOG_INFO("Server stopped. Goodbye!");
  return 0;
}
