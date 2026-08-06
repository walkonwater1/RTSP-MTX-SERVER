#pragma once
/**
 * Session Manager — per-robot session state machine.
 *
 * Each connected robot gets one Session with:
 *   - unique session_id (UUID v4)
 *   - dedicated RTSP path on MediaMTX
 *   - state machine tracking the interaction lifecycle
 *   - TTS queue
 *
 * State transitions:
 *   Idle → Streaming    (req_stream + start_push_audio)
 *   Streaming → Playing  (server pushes TTS)
 *   Playing → Streaming  (TTS playback completed/interrupted)
 *   Streaming → Idle     (timeout / disconnect)
 *   Any → Error          (internal error)
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "utils/logger.h"

namespace rtsp_server {

// --- Session state machine ---
enum class SessionState {
  Idle,         // Connected, handshake done, no audio streaming
  Streaming,    // Receiving audio from robot, ASR active
  Playing,      // Server is pushing TTS → robot is playing
  Interrupted,  // TTS was interrupted
  Error,        // Irrecoverable error
};

inline const char* SessionStateName(SessionState s) {
  switch (s) {
    case SessionState::Idle:        return "Idle";
    case SessionState::Streaming:   return "Streaming";
    case SessionState::Playing:     return "Playing";
    case SessionState::Interrupted: return "Interrupted";
    case SessionState::Error:       return "Error";
  }
  return "?";
}

// --- TTS item in the play queue ---
struct TtsItem {
  std::string tts_id;       // unique ID for this TTS utterance
  std::string text;         // the text being spoken
  std::string audio_url;    // URL for robot to pull audio (RTSP or HTTP)
  std::string audio_path;   // local file path (if cached)
  int64_t created_ms;       // epoch ms when created
  bool interrupted = false; // set when robot/client interrupts
};

// --- Session ---
struct Session {
  std::string session_id;
  std::string user_id;
  std::string mode;  // "voice" (speech interaction)

  // RTSP
  std::string rtsp_push_path;  // e.g. "robot_speech_<session_id>"
  std::string rtsp_push_url;   // full RTSP URL for push (client pushes mic here)

  // Client protocol params (from req_stream)
  std::string llm_type = "offline";     // stream/offline
  std::string tts_type = "stream";      // stream/offline
  std::string qa_type = "llm";          // faq/llm
  int interrupt_type = 1;               // 0=disabled, 1=enabled

  // State
  std::atomic<SessionState> state{SessionState::Idle};
  std::atomic<bool> audio_streaming{false};  // robot is pushing audio
  std::atomic<int64_t> last_heartbeat_ms{0};
  std::atomic<int64_t> push_started_ms{0};

  // TTS queue
  std::mutex tts_mutex;
  std::deque<TtsItem> tts_queue;
  std::string current_tts_id;  // currently playing (or awaiting playback)
  int tts_seq = 0;             // auto-increment for tts_id generation

  // ASR accumulation
  std::mutex asr_mutex;
  std::string asr_buffer;          // accumulated raw PCM audio
  bool asr_finalized = false;     // set when speech segment complete
  bool llm_triggered = false;     // dedup: only one LLM call per wakeup session
  bool speech_detected = false;   // set when actual speech energy is detected
  int64_t last_asr_finalized_ms = 0;  // cooldown: prevent rapid re-trigger
  bool first_utterance = true;    // skip cooldown for first speech after wake/idle
  int silence_frames = 0;        // consecutive silent chunks (for VAD hysteresis)
  static constexpr int kSilenceFramesThreshold = 3;  // ~750ms silence before end-of-utterance

  // Post-TTS guard: discard audio briefly after TTS playback ends,
  // preventing residual speaker echo / room reverb from false-triggering ASR.
  std::atomic<int64_t> tts_end_ms{0};
  static constexpr int kPostTtsGuardMs = 500;

  // ── Interrupt / Cancellation support ─────────────────
  // Monotonic generation counter. Incremented on each new utterance;
  // background processing threads check this to detect staleness.
  std::atomic<int64_t> generation_id{0};

  // Set by interrupt handler to cancel in-flight LLM + TTS processing.
  // Checked by CURL progress callback and before TTS synthesis.
  std::atomic<bool> cancel_pipeline{false};

  // True while a background ASR→LLM→TTS thread is running for this session.
  std::atomic<bool> pipeline_running{false};

  // Mutex serializes pipeline start/cancel operations.
  std::mutex pipeline_mutex;

  /// Signal that a new utterance has started — invalidates any in-flight
  /// processing from the previous generation.
  /// @return the new generation_id
  int64_t NewGeneration() {
    cancel_pipeline.store(true);
    int64_t g = generation_id.fetch_add(1) + 1;
    return g;
  }

  /// Reset cancellation for a new pipeline run.
  void ResetPipelineCancel() {
    cancel_pipeline.store(false);
  }

  // Adaptive VAD: EMA-tracked noise baseline for dynamic thresholds.
  float noise_baseline = 0.01f;
  static constexpr float kMinSpeechRms = 0.008f;
  static constexpr float kMinSilenceRms = 0.004f;

  // WebSocket connection fd (for sending messages directly)
  int ws_fd = -1;

  // Timestamps
  int64_t created_ms = 0;
  int64_t last_activity_ms = 0;

  // --- Helpers ---

  std::string GenerateTtsId() {
    tts_seq++;
    return session_id + "_tts_" + std::to_string(tts_seq);
  }

  SessionState GetState() const { return state.load(); }

  bool TransitionTo(SessionState new_state) {
    SessionState old = state.exchange(new_state);
    if (old != new_state) {
      LOG_INFO("[Session {}] state: {} → {}",
               session_id, SessionStateName(old), SessionStateName(new_state));
      return true;
    }
    return false;
  }

  void Touch() {
    last_activity_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
  }

  bool IsExpired(int64_t timeout_ms) const {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (now - last_activity_ms) > timeout_ms;
  }
};

// --- Session Manager ---
class SessionManager {
public:
  SessionManager() = default;
  ~SessionManager() = default;

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  /**
   * @brief Create a new session for a connecting robot.
   * @param user_id   robot user ID
   * @param mode      interaction mode ("voice")
   * @param rtsp_base base RTSP URL (e.g. "rtsp://192.168.2.106:8554")
   * @return pointer to the new Session (owned by SessionManager)
   */
  Session* CreateSession(const std::string& user_id,
                         const std::string& mode,
                         const std::string& rtsp_base);

  /**
   * @brief Remove a session (robot disconnected).
   */
  void RemoveSession(const std::string& session_id);

  /**
   * @brief Find a session by ID.
   * @return pointer or nullptr
   */
  Session* FindSession(const std::string& session_id);

  /**
   * @brief Find a session by WebSocket fd.
   */
  Session* FindSessionByFd(int fd);

  /**
   * @brief Get all active sessions (snapshot).
   */
  std::vector<Session*> GetAllSessions();

  /**
   * @brief Remove expired sessions.
   * @param timeout_ms  session timeout in ms
   * @return number of sessions removed
   */
  int PurgeExpired(int64_t timeout_ms);

  /**
   * @brief Get IDs of expired sessions without removing them.
   *        Caller should stop RTSP pipelines before calling RemoveSession.
   * @param timeout_ms max idle time in milliseconds
   * @return vector of expired session IDs
   */
  std::vector<std::string> GetExpiredSessionIds(int64_t timeout_ms) const;

  /**
   * @brief Number of active sessions.
   */
  size_t SessionCount() const;

  /**
   * @brief Set the maximum allowed sessions.
   */
  void SetMaxSessions(int max) { max_sessions_ = max; }

  /**
   * @brief Whether we can accept a new session.
   */
  bool CanAccept() const;

private:
  std::string GenerateSessionId();

  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<Session>> sessions_;
  int max_sessions_ = 32;
  uint64_t session_counter_ = 0;
};

} // namespace rtsp_server
