#pragma once
/**
 * RTSP Media Manager — manages MediaMTX and ffmpeg audio pipelines.
 *
 * Responsibilities:
 *   1. Launch/manage MediaMTX subprocess (RTSP media server)
 *   2. Start ffmpeg subprocess to pull robot audio from RTSP → PCM pipe
 *   3. Start ffmpeg subprocess to push TTS audio to RTSP
 *   4. Manage per-session RTSP paths
 *
 * Architecture:
 *   MediaMTX (subprocess)
 *     ├─ /robot_audio/<sid>  ← robot pushes here (PCM → AAC)
 *     ├─ /tts_audio/<sid>    → robot pulls from here (AAC → PCM)
 *     └─ ... (one pair per session)
 *
 *   Server-side:
 *     Robot audio: ffmpeg pull rtsp://localhost:8554/robot_audio/<sid> → raw PCM stdout
 *     TTS audio:   ffmpeg push raw PCM → rtsp://localhost:8554/tts_audio/<sid>
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rtsp_server {

// --- RTSP pipeline state ---
enum class RtspPipelineState {
  Idle,
  Starting,
  Running,
  Stopping,
  Error,
};

// --- Audio pull pipeline (server pulls robot audio from RTSP) ---
struct AudioPullPipeline {
  std::string session_id;
  std::string rtsp_url;
  std::atomic<RtspPipelineState> state{RtspPipelineState::Idle};
  std::atomic<bool> need_exit{false};

  FILE* ffmpeg_pipe = nullptr; // popen handle
  std::thread pull_thread;
  std::mutex mutex;

  // Callback when PCM data is available
  // void(int16_t* samples, int n_samples)
  std::function<void(const int16_t*, int)> on_pcm_data;

  // Callback when ffmpeg exits
  std::function<void(const std::string& session_id)> on_disconnect;
};

// --- RTSP Manager ---
class RtspManager {
public:
  RtspManager() = default;
  ~RtspManager();

  RtspManager(const RtspManager&) = delete;
  RtspManager& operator=(const RtspManager&) = delete;

  /**
   * @brief Configure the RTSP server (MediaMTX).
   * @param mediamtx_bin  path to mediamtx binary
   * @param rtsp_port     RTSP listening port
   * @param auto_launch   whether to launch MediaMTX automatically
   * @return true on success
   */
  bool Configure(const std::string& mediamtx_bin,
                 int rtsp_port,
                 bool auto_launch);

  /**
   * @brief Start MediaMTX and prepare for sessions.
   */
  bool Start();

  /**
   * @brief Stop all pipelines and MediaMTX.
   */
  void Stop();

  /**
   * @brief Check if MediaMTX is running.
   */
  bool IsRunning() const { return mediamtx_running_.load(); }

  /**
   * @brief Start pulling audio from an RTSP path (robot audio → PCM callback).
   * @param session_id  session identifier
   * @param rtsp_url    full RTSP URL to pull from
   * @param on_pcm      callback receiving decoded PCM samples
   * @return true if the pipeline was started
   */
  bool StartAudioPull(const std::string& session_id,
                      const std::string& rtsp_url,
                      std::function<void(const int16_t*, int)> on_pcm);

  /**
   * @brief Stop pulling audio for a session.
   */
  void StopAudioPull(const std::string& session_id);

private:
  // MediaMTX subprocess
  bool LaunchMediaMtx();
  void KillMediaMtx();

  // Pull pipeline internals
  static void PullLoop(AudioPullPipeline* pipeline);
  static bool LaunchPullFfmpeg(AudioPullPipeline* pipeline);

  std::string mediamtx_bin_;
  int rtsp_port_ = 8554;
  bool auto_launch_ = false;
  std::atomic<bool> mediamtx_running_{false};
  pid_t mediamtx_pid_ = -1;

  std::mutex pipelines_mutex_;
  std::vector<std::unique_ptr<AudioPullPipeline>> pull_pipelines_;
  std::atomic<bool> stopped_{false};
};

} // namespace rtsp_server
