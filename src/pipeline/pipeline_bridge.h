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
 *   - LLM: text → response text
 *   - TTS: text → audio file (WAV)
 *
 * All processing is synchronous (blocking). The caller is responsible for
 * running these in background threads.
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rtsp_server {

// --- Pipeline configuration ---
struct PipelineBridgeConfig {
  // ASR
  std::string asr_model_path;
  std::string asr_model_type = "zipformer_ctc";

  // LLM
  std::string llm_host = "http://localhost:11434";
  std::string llm_model = "qwen2.5:3b";
  std::string llm_system_prompt;
  int llm_timeout_sec = 30;

  // TTS
  std::string tts_backend = "piper";
  std::string tts_piper_model;
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
};

// --- Processing result ---
struct PipelineResult {
  std::string asr_text;      // recognized text
  std::string llm_response;  // LLM reply
  std::string tts_audio_path; // path to generated WAV file
  std::string error;          // non-empty on failure
  bool ok = false;
};

// --- Voice Pipeline Bridge ---
class PipelineBridge {
public:
  explicit PipelineBridge(const PipelineBridgeConfig& cfg = {});
  ~PipelineBridge();

  PipelineBridge(const PipelineBridge&) = delete;
  PipelineBridge& operator=(const PipelineBridge&) = delete;

  /**
   * @brief Initialize the pipeline (load models).
   *        Blocks until models are loaded (may download on first run).
   * @return true on success
   */
  bool Initialize();

  /**
   * @brief Whether the pipeline is initialized and ready.
   */
  bool IsReady() const { return ready_.load(); }

  /**
   * @brief Process text through LLM → TTS.
   *        Synchronous, blocking call.
   * @param text      user input text
   * @param session_id optional session context (for caching)
   * @return result with LLM response and TTS audio path
   */
  PipelineResult ProcessText(const std::string& text,
                              const std::string& session_id = "");

  /**
   * @brief Process audio PCM data through ASR → LLM → TTS.
   *        Synchronous, blocking call.
   * @param pcm_data    raw PCM samples (16kHz mono S16LE)
   * @param sample_count number of samples
   * @param session_id  optional session context
   * @return result with ASR text, LLM response, and TTS audio path
   */
  PipelineResult ProcessAudio(const int16_t* pcm_data,
                               int sample_count,
                               const std::string& session_id = "");

  /**
   * @brief Run ASR only (no LLM/TTS).
   *        Used for streaming partial results.
   * @param pcm_data    raw PCM samples
   * @param sample_count number of samples
   * @return recognized text (may be empty)
   */
  std::string TranscribeAudio(const int16_t* pcm_data, int sample_count);

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

  /**
   * @brief Reload configuration at runtime.
   */
  void ReloadConfig(const PipelineBridgeConfig& cfg);

private:
  PipelineBridgeConfig cfg_;
  std::atomic<bool> ready_{false};

  // In stub mode, we use curl to call Ollama directly
  std::string CallLlm(const std::string& prompt);
  std::string EscapeJson(const std::string& s);

  // Cache
  std::string GetCachePath(const std::string& text);
};

} // namespace rtsp_server
