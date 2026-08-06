#pragma once
/**
 * Voice Pipeline Bridge — integrates ASR-LLM-TTS pipeline into the RTSP server.
 *
 * Two modes:
 *   1. HAS_VOICE_PIPELINE: links against the real ASR-LLM-TTS libraries
 *   2. Stub mode: text-only (for testing without the full pipeline)
 *
 * The bridge provides:
 *   - ASR: audio PCM → text
 *   - LLM: text → response text (with skills, memory, function calling)
 *   - TTS: text → audio file (WAV)
 *
 * All processing is synchronous (blocking). The caller is responsible for
 * running these in background threads.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include <curl/curl.h>

#include "brain/skill_manager.h"
#include "memory/chat_memory.h"
#include "memory/user_memory.h"

// Forward declarations for direct library integration
#ifdef SHERPA_ONNX_AVAILABLE
struct SherpaOnnxOfflineRecognizer;
#endif

namespace rtsp_server {

// --- Pipeline configuration ---
struct PipelineBridgeConfig {
  // ASR
  std::string asr_model_path;
  std::string asr_model_type = "zipformer_ctc";

  // LLM
  std::string llm_host = "http://192.168.2.107:11434";   // Orin NX
  std::string llm_model = "qwen2.5:3b";
  std::string llm_system_prompt;
  int llm_timeout_sec = 30;
  bool llm_streaming = false;

  // TTS
  std::string tts_backend = "edge_tts";  // "edge_tts" | "piper" | "espeak"
  bool tts_streaming = false;
  std::string tts_remote_host;           // e.g. "192.168.2.107:8765" for Orin NX
  std::string tts_piper_model;
  std::string tts_edge_tts_script = "scripts/edge_tts_cli.py";
  std::string tts_edge_tts_voice = "zh-CN-XiaoxiaoNeural";
  int tts_rate = 200;
  std::string tts_voice = "cmn+f3";
  int tts_sample_rate = 16000;
  bool tts_cache_enabled = true;
  std::string tts_cache_dir = "/tmp/rtsp-server/tts-cache";

  // VAD
  float vad_energy_threshold = 0.008f;
  int vad_min_speech_frames = 15;
  int vad_min_silence_frames = 15;
  int vad_pre_speech_frames = 15;
  float vad_adaptive_factor = 7.0f;
  float vad_min_energy = 0.025f;
  int vad_cooldown_frames = 25;
  float vad_max_speech_sec = 60.0f;
  float vad_min_speech_sec = 0.5f;

  // Skills
  bool skills_enabled = true;
  bool function_calling_enabled = true;
  std::string fc_model;  // empty = use llm_model

  // Memory
  bool memory_enabled = true;
  int memory_max_rounds = 10;
  int memory_max_tokens = 1536;
  std::string memory_persist_dir = "/tmp/rtsp-server/memory";
};

// --- Streaming callbacks ---
using LlmTokenCallback = std::function<void(const std::string& token, bool is_final)>;
using TtsAudioCallback = std::function<void(const int16_t* pcm, int sample_count, bool is_final)>;

// --- Processing result ---
struct PipelineResult {
  std::string asr_text;      // recognized text
  std::string llm_response;  // LLM reply
  std::string tts_audio_path; // path to generated WAV file
  std::string error;          // non-empty on failure
  bool ok = false;
  bool skill_direct = false;  // skill result, bypassed LLM

  // Timing (ms) — populated by ProcessText
  long llm_ms = 0;
  long tts_ms = 0;
  long total_ms = 0;
};

// --- Voice Pipeline Bridge ---
class PipelineBridge {
public:
  explicit PipelineBridge(const PipelineBridgeConfig& cfg = {});
  ~PipelineBridge();

  PipelineBridge(const PipelineBridge&) = delete;
  PipelineBridge& operator=(const PipelineBridge&) = delete;

  /**
   * @brief Initialize the pipeline (load models, set up skills, memory).
   *        Blocks until models are loaded (may download on first run).
   * @return true on success
   */
  bool Initialize();

  /**
   * @brief Whether the pipeline is initialized and ready.
   */
  bool IsReady() const { return ready_.load(); }

  /// Whether streaming TTS mode is enabled in config
  bool IsStreamingTts() const { return cfg_.tts_streaming; }

  /// Whether streaming LLM mode is enabled in config
  bool IsStreamingLlm() const { return cfg_.llm_streaming; }

  /**
   * @brief Process text through Skill detection → LLM (with memory) → TTS.
   *        Synchronous, blocking call.
   * @param text      user input text
   * @param session_id optional session context (for caching and per-session memory)
   * @param cancel     optional atomic flag — when set to true, aborts in-flight
   *                   LLM and TTS as soon as possible
   * @return result with LLM response and TTS audio path (result.ok=false if cancelled)
   */
  PipelineResult ProcessText(const std::string& text,
                              const std::string& session_id = "",
                              std::atomic<bool>* cancel = nullptr);

  /**
   * @brief Process audio PCM data through ASR → LLM → TTS.
   *        Synchronous, blocking call.
   * @param pcm_data    raw PCM samples (16kHz mono S16LE)
   * @param sample_count number of samples
   * @param session_id  optional session context
   * @param cancel      optional atomic flag for cancellation
   * @return result with ASR text, LLM response, and TTS audio path
   */
  PipelineResult ProcessAudio(const int16_t* pcm_data,
                               int sample_count,
                               const std::string& session_id = "",
                               std::atomic<bool>* cancel = nullptr);

  /**
   * @brief Run ASR only (no LLM/TTS).
   *        Used for streaming partial results.
   * @param pcm_data    raw PCM samples
   * @param sample_count number of samples
   * @return recognized text (may be empty)
   */
  std::string TranscribeAudio(const int16_t* pcm_data, int sample_count,
                               const std::string& session_id = "");

  /**
   * @brief Generate TTS audio from text.
   * @param text        text to synthesize
   * @param output_path output WAV file path
   * @param session_id  optional session context (for caching)
   * @return true on success
   */
  bool SynthesizeTts(const std::string& text,
                     const std::string& output_path,
                     const std::string& session_id = "");

  // ── Streaming pipeline ──────────────────────────

  PipelineResult ProcessTextStream(const std::string& text,
                                   const std::string& session_id,
                                   std::atomic<bool>* cancel,
                                   LlmTokenCallback on_llm_token,
                                   TtsAudioCallback on_tts_audio);

  bool SynthesizeTtsStream(const std::string& text,
                           TtsAudioCallback on_audio,
                           const std::string& session_id = "");

  /**
   * @brief Reload configuration at runtime.
   */
  void ReloadConfig(const PipelineBridgeConfig& cfg);

  /**
   * @brief Get per-session chat memory (for external access).
   */
  ChatMemory* GetSessionMemory(const std::string& session_id,
                                const std::string& user_id = "");

  /**
   * @brief Register session → user mapping (call on req_stream handshake).
   *        Enables per-user memory isolation and cross-session chat recovery.
   */
  void RegisterSessionUser(const std::string& session_id,
                           const std::string& user_id);

  /**
   * @brief Get or create the UserMemoryStore for a given user_id.
   *        Loads from per-user persist file on first access.
   */
  UserMemoryStore* GetUserMemory(const std::string& user_id);

private:
  PipelineBridgeConfig cfg_;
  std::atomic<bool> ready_{false};

  // Skills & memory
  SkillManager skill_mgr_;
  std::shared_ptr<FunctionCaller> function_caller_;
  std::mutex memory_mutex_;
  std::map<std::string, UserMemoryStore> user_memories_;  // per-user
  std::map<std::string, std::string> session_user_map_;   // session_id → user_id
  std::map<std::string, ChatMemory> session_memories_;    // per-session

  // In stub mode, we use curl to call Ollama directly
  std::string CallLlm(const std::string& prompt,
                      const std::string& context = "",
                      std::atomic<bool>* cancel = nullptr);
  std::string CallLlmChat(const std::string& system_prompt,
                          const std::string& user_text,
                          const std::vector<ChatMessage>& history_msgs,
                          const std::string& skill_context = "",
                          std::atomic<bool>* cancel = nullptr);

  std::string CallLlmChatStream(const std::string& system_prompt,
                                const std::string& user_text,
                                const std::vector<ChatMessage>& history_msgs,
                                const std::string& skill_context,
                                LlmTokenCallback on_token,
                                std::atomic<bool>* cancel = nullptr);

  std::string EscapeJson(const std::string& s);

  // Cached TTS backend check (avoid forking shell every TTS call)
  bool piper_available_ = false;

  // ── Direct sherpa-onnx ASR (avoid popen) ──────────
#ifdef SHERPA_ONNX_AVAILABLE
  const SherpaOnnxOfflineRecognizer* asr_recognizer_ = nullptr;
#endif

  // ── Direct espeak-ng TTS (avoid system()) ──────────
  bool espeak_initialized_ = false;
  int espeak_sample_rate_ = 22050;
  // Helper: write WAV from int16 samples (used by espeak callback)
  static bool WriteWavFile(const std::string& path,
                           const std::vector<int16_t>& samples,
                           int sample_rate,
                           int num_channels = 1);

  // LLM response cache: hash(user_text) → response
  // Avoids redundant LLM calls for frequent queries like "你好", "你叫什么"
  std::mutex llm_cache_mutex_;
  std::unordered_map<size_t, std::string> llm_cache_;
  static constexpr int kMaxLlmCacheEntries = 64;

  // Cache
  std::string GetCachePath(const std::string& text);
};

} // namespace rtsp_server
