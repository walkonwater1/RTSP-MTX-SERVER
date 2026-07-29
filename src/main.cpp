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

using json = nlohmann::json;
using namespace rtsp_server;

// --- Global state for signal handling ---
static std::atomic<bool> g_running{true};
static WsSignalingServer* g_ws_server = nullptr;
static RtspManager* g_rtsp_manager = nullptr;
static SessionManager* g_session_mgr = nullptr;
static PipelineBridge* g_pipeline = nullptr;
static std::string g_rtsp_base_url = "rtsp://127.0.0.1:8554";

// --- Configuration ---
struct ServerConfig {
  // WebSocket
  int ws_port = 8090;
  std::string ws_bind = "0.0.0.0";
  std::string ws_path = "/ws/rtsp";
  int ws_ping_interval_sec = 15;

  // RTSP
  std::string rtsp_base_url = "rtsp://0.0.0.0:8554";
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

      // Send stream_address response
      json resp;
      resp["event"] = "stream_address";
      resp["session_id"] = session->session_id;
      int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      resp["timestamp"] = now_ms;
      resp["data"]["rtsp_url"] = session->rtsp_push_url;
      resp["data"]["rtsp_pull_url"] = session->rtsp_pull_url;
      resp["data"]["user_id"] = user_id;
      resp["data"]["mode"] = mode;

      g_ws_server->SendMessage(client_fd, resp.dump());

      // Also store session_id on the connection
      {
        // We access the WsConnection via the session manager
        // (session_id is stored on the Session, the ws_server maps fd→session indirectly)
      }

      LOG_INFO("[MSG] session {} assigned push_url={}, pull_url={}",
               session->session_id, session->rtsp_push_url, session->rtsp_pull_url);
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
      session->llm_triggered = false;  // reset for new wakeup
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

              std::lock_guard<std::mutex> lock(s->asr_mutex);

              // Append PCM to buffer
              s->asr_buffer.append(reinterpret_cast<const char*>(pcm), n * sizeof(int16_t));

              static constexpr int kMaxBufferSamples = 16000 * 60; // 60 seconds max
              static constexpr float kSpeechRms = 0.02f;           // RMS above this = speech
              static constexpr float kSilenceRms = 0.008f;         // RMS below this = silence
              static constexpr int kCooldownMs = 3000;             // min interval between ASR triggers

              int current_samples = s->asr_buffer.size() / sizeof(int16_t);

              // Calculate RMS of current chunk
              float sum_sq = 0;
              for (int i = 0; i < n; i++) {
                float sample = pcm[i] / 32768.0f;
                sum_sq += sample * sample;
              }
              float rms = std::sqrt(sum_sq / n);

              // Speech detection: high energy → someone is talking
              if (rms > kSpeechRms) {
                s->speech_detected = true;
              }

              // End-of-speech detection: speech was detected, then silence follows
              // Only trigger if: (a) speech was detected, (b) current chunk is silent,
              // (c) enough total audio, (d) not already finalized, (e) cooldown elapsed.
              bool is_silence = (rms < kSilenceRms);
              bool has_speech = s->speech_detected;
              bool enough_audio = (current_samples > 16000 * 1);  // at least 1s total
              bool cooldown_ok = true;
              {
                int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                cooldown_ok = (now_ms - s->last_asr_finalized_ms) > kCooldownMs;
              }

              if (is_silence && has_speech && enough_audio && !s->asr_finalized && cooldown_ok) {
                s->asr_finalized = true;
                LOG_INFO("[ASR] speech segment end for session {} ({} samples, rms={:.4f})",
                         session_id, current_samples, rms);

                // Process ASR in a background thread to not block the audio callback
                std::thread([session_id]() {
                  Session* s = g_session_mgr->FindSession(session_id);
                  if (!s) return;

                  std::string asr_text;
                  {
                    std::lock_guard<std::mutex> lock(s->asr_mutex);
                    if (g_pipeline && g_pipeline->IsReady()) {
                      auto* pcm_ptr = reinterpret_cast<const int16_t*>(s->asr_buffer.data());
                      int pcm_count = s->asr_buffer.size() / sizeof(int16_t);
                      asr_text = g_pipeline->TranscribeAudio(pcm_ptr, pcm_count);
                    }
                    s->asr_buffer.clear();
                    s->asr_finalized = false;
                    s->speech_detected = false;  // reset for next utterance
                    s->last_asr_finalized_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                  }

                  if (asr_text.empty()) {
                    LOG_DEBUG("[ASR] no text recognized for session {}", session_id);
                    return;
                  }

                  // Dedup: only one LLM call per conversation turn
                  if (s->llm_triggered) {
                    LOG_DEBUG("[ASR] LLM already triggered for session {}, skipping", session_id);
                    return;
                  }
                  s->llm_triggered = true;

                    LOG_INFO("[ASR] recognized: \"{}\" for session {}", asr_text, session_id);

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

                    // Run LLM → TTS (can take 1-2s — session may be removed while we wait)
                    if (g_pipeline && g_pipeline->IsReady()) {
                      auto result = g_pipeline->ProcessText(asr_text, session_id);

                      // Re-check: session may have been removed by disconnect handler
                      // while ProcessText was running.  Accessing s after this point
                      // would be use-after-free.
                      s = g_session_mgr->FindSession(session_id);
                      if (!s) {
                        LOG_INFO("[ASR] session {} gone after LLM/TTS, discarding result", session_id);
                        return;
                      }

                      if (!result.llm_response.empty()) {
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

                        // Queue TTS
                        if (!result.tts_audio_path.empty()) {
                          std::lock_guard<std::mutex> tlock(s->tts_mutex);
                          TtsItem item;
                          item.tts_id = s->GenerateTtsId();
                          item.text = result.llm_response;
                          item.audio_url = s->rtsp_pull_url;
                          item.audio_path = result.tts_audio_path;
                          item.created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
                          s->tts_queue.push_back(item);

                          // Start push pipeline (ffmpeg connects to MediaMTX as publisher)
                          AudioPushPipeline* push = nullptr;
                          if (g_rtsp_manager) {
                            push = g_rtsp_manager->StartAudioPush(session_id, s->rtsp_pull_url);
                          }

                          // Send tts_start FIRST — robot needs to start pulling before we push
                          if (g_ws_server) {
                            json tts_msg;
                            tts_msg["event"] = "tts_start";
                            tts_msg["session_id"] = s->session_id;
                            tts_msg["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            tts_msg["data"]["tts_id"] = item.tts_id;
                            tts_msg["data"]["audio_url"] = s->rtsp_pull_url;
                            tts_msg["data"]["text"] = result.llm_response;
                            g_ws_server->SendMessage(s->ws_fd, tts_msg.dump());
                            LOG_INFO("[TTS] tts_start sent to session {}: tts_id={}",
                                     session_id, item.tts_id);
                          }

                          // Wait for robot to start pulling, then feed audio
                          if (push) {
                            // Read WAV file
                            std::ifstream wav(item.audio_path, std::ios::binary);
                            if (wav) {
                              wav.seekg(44);  // skip WAV header
                              std::vector<int16_t> wav_data;
                              int16_t sample;
                              while (wav.read(reinterpret_cast<char*>(&sample), sizeof(sample))) {
                                wav_data.push_back(sample);
                              }
                              // Wait for robot to connect its pull (robot retries may
                              // take up to ~2s: 300+600+1200ms back-off).
                              // Meanwhile, ffmpeg -re pushes at real-time (~6.8s)
                              // so the robot won't miss the stream.
                              std::this_thread::sleep_for(std::chrono::milliseconds(2500));
                              g_rtsp_manager->FeedPushPcm(push, wav_data.data(), wav_data.size());
                              LOG_INFO("[TTS] pushed {} samples to RTSP for session {}",
                                       wav_data.size(), session_id);
                              // -re flag makes ffmpeg read at real-time speed.
                              // Signal EOF now; PushLoop will drain the ring buffer
                              // at 1x speed (~6.8s), then close ffmpeg cleanly.
                              g_rtsp_manager->SignalPushEof(push);
                            }
                          }

                          s->TransitionTo(SessionState::Playing);
                          s->current_tts_id = item.tts_id;
                        }
                      }
                    }
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
      json ready;
      ready["event"] = "stream_ready";
      ready["session_id"] = session_id;
      ready["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      ready["data"]["server_received"] = true;
      ready["receive_aduio_stream"] = session_id;  // typo from old protocol
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
      LOG_INFO("[MSG] playback_status({}) from session {}", status, session_id);

      if (status == "completed") {
        // Robot finished playing TTS → resume ASR
        session->llm_triggered = false;  // re-arm for next speech turn
        session->TransitionTo(SessionState::Streaming);
        session->current_tts_id.clear();

        // Clean up push pipeline
        if (g_rtsp_manager) {
          g_rtsp_manager->StopAudioPush(session_id);
        }

        // Process next TTS in queue
        std::lock_guard<std::mutex> tlock(session->tts_mutex);
        if (!session->tts_queue.empty()) {
          session->tts_queue.pop_front();
        }
        if (!session->tts_queue.empty()) {
          auto& next = session->tts_queue.front();
          session->current_tts_id = next.tts_id;

          // Push next TTS audio
          if (g_rtsp_manager) {
            auto* push = g_rtsp_manager->StartAudioPush(session_id, session->rtsp_pull_url);
            if (push) {
              std::ifstream wav(next.audio_path, std::ios::binary);
              if (wav) {
                wav.seekg(44);
                std::vector<int16_t> wav_data;
                int16_t sample;
                while (wav.read(reinterpret_cast<char*>(&sample), sizeof(sample))) {
                  wav_data.push_back(sample);
                }
                g_rtsp_manager->FeedPushPcm(push, wav_data.data(), wav_data.size());
                g_rtsp_manager->SignalPushEof(push);
              }
            }
          }

          // Send next tts_start
          json tts_msg;
          tts_msg["event"] = "tts_start";
          tts_msg["session_id"] = session_id;
          tts_msg["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
          tts_msg["data"]["tts_id"] = next.tts_id;
          tts_msg["data"]["audio_url"] = session->rtsp_pull_url;
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
      LOG_INFO("[MSG] tts_state(is_playing={}, reason={}) from session {}",
               is_playing, reason, session_id);

      if (!is_playing && (reason == "ended" || reason == "stop_tts" ||
                          reason == "interrupt" || reason == "client_stop")) {
        session->llm_triggered = false;  // re-arm for next speech turn
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
    LOG_INFO("[CONN] cleaning up session {}", session->session_id);

    // Stop RTSP pipelines
    if (g_rtsp_manager) {
      g_rtsp_manager->StopAudioPull(session->session_id);
      g_rtsp_manager->StopAudioPush(session->session_id);
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

    int purged = g_session_mgr->PurgeExpired(cfg.session_timeout_sec * 1000);
    if (purged > 0) {
      LOG_INFO("[Cleanup] purged {} expired sessions, {} remaining",
               purged, g_session_mgr->SessionCount());
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
      cfg.rtsp_base_url = "rtsp://0.0.0.0:" + std::to_string(cfg.rtsp_port);
    } else if (arg == "--no-mediamtx") {
      cfg.auto_launch_mediamtx = false;
    }
  }

  // Make rtsp_base_url available to the WebSocket message handler
  g_rtsp_base_url = cfg.rtsp_base_url;

  // Create directories
  system("mkdir -p /tmp/rtsp-server/debug");
  system("mkdir -p /tmp/rtsp-server/tts-cache");

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
