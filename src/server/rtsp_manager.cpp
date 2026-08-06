#include "server/rtsp_manager.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include "utils/logger.h"

namespace rtsp_server {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

RtspManager::~RtspManager() {
  Stop();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

bool RtspManager::Configure(const std::string& mediamtx_bin,
                            int rtsp_port,
                            bool auto_launch) {
  mediamtx_bin_ = mediamtx_bin;
  rtsp_port_ = rtsp_port;
  auto_launch_ = auto_launch;
  LOG_INFO("[RTSP] configured: mediamtx={}, port={}, auto_launch={}",
           mediamtx_bin_, rtsp_port_, auto_launch_);
  return true;
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

bool RtspManager::Start() {
  if (auto_launch_) {
    if (!LaunchMediaMtx()) {
      LOG_ERROR("[RTSP] failed to launch MediaMTX");
      return false;
    }
  } else {
    LOG_INFO("[RTSP] MediaMTX auto-launch disabled — assuming external instance on port {}",
             rtsp_port_);
    mediamtx_running_.store(true);
  }
  return true;
}

void RtspManager::Stop() {
  if (stopped_.exchange(true)) return;  // idempotent

  LOG_INFO("[RTSP] stopping all pipelines...");

  // Signal all pipelines to exit.
  // IMPORTANT: do NOT pclose before joining — PullLoop's fread will unblock
  // when the ffmpeg child exits.
  {
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    for (auto& p : pull_pipelines_) {
      p->need_exit.store(true);
    }
  }

  // Join all threads (now safe — loops will exit on need_exit or broken pipe)
  {
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    for (auto& p : pull_pipelines_) {
      if (p->pull_thread.joinable()) p->pull_thread.join();
    }
    // Fallback: pclose any pipes the loops didn't clean up themselves
    for (auto& p : pull_pipelines_) {
      if (p->ffmpeg_pipe) {
        pclose(p->ffmpeg_pipe);
        p->ffmpeg_pipe = nullptr;
      }
    }
    pull_pipelines_.clear();
  }

  KillMediaMtx();
  LOG_INFO("[RTSP] all pipelines stopped");
}

// ---------------------------------------------------------------------------
// MediaMTX Subprocess
// ---------------------------------------------------------------------------

bool RtspManager::LaunchMediaMtx() {
  if (mediamtx_bin_.empty()) {
    LOG_ERROR("[RTSP] MediaMTX binary path not set");
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    LOG_ERROR("[RTSP] fork failed: {}", strerror(errno));
    return false;
  }

  if (pid == 0) {
    // Child: exec MediaMTX
    // Build a minimal config via environment variables
    setenv("MTX_RTSPADDRESS", ":8554", 1);
    setenv("MTX_RTPSDISABLE", "yes", 1);    // disable RTSPS
    setenv("MTX_RTMPDISABLE", "yes", 1);    // disable RTMP
    setenv("MTX_HLSDISABLE", "yes", 1);     // disable HLS
    setenv("MTX_WEBRTCDISABLE", "yes", 1);  // disable WebRTC
    setenv("MTX_SRTDISABLE", "yes", 1);     // disable SRT
    setenv("MTX_LOGLEVEL", "info", 1);
    setenv("MTX_PATHS_ALL_SOURCE", "publisher", 1);  // allow push on any path

    // Redirect stdout/stderr to log
    freopen("/tmp/rtsp-server/mediamtx.log", "w", stdout);
    freopen("/tmp/rtsp-server/mediamtx.log", "w", stderr);

    execlp(mediamtx_bin_.c_str(), mediamtx_bin_.c_str(), nullptr);

    // execlp only returns on error
    LOG_ERROR("[RTSP] execlp failed: {}", strerror(errno));
    _exit(1);
  }

  mediamtx_pid_ = pid;
  LOG_INFO("[RTSP] MediaMTX launched: pid={}", pid);

  // Wait a bit for MediaMTX to start
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  // Check if still alive
  int status;
  pid_t result = waitpid(pid, &status, WNOHANG);
  if (result == pid) {
    LOG_ERROR("[RTSP] MediaMTX exited immediately (exit_code={})",
              WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    mediamtx_pid_ = -1;
    return false;
  }

  mediamtx_running_.store(true);
  return true;
}

void RtspManager::KillMediaMtx() {
  if (mediamtx_pid_ > 0) {
    LOG_INFO("[RTSP] stopping MediaMTX (pid={})", mediamtx_pid_);
    kill(mediamtx_pid_, SIGTERM);

    // Wait up to 5 seconds
    for (int i = 0; i < 50; i++) {
      int status;
      if (waitpid(mediamtx_pid_, &status, WNOHANG) == mediamtx_pid_) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Force kill if still alive
    if (waitpid(mediamtx_pid_, nullptr, WNOHANG) == 0) {
      LOG_WARN("[RTSP] MediaMTX didn't exit, sending SIGKILL");
      kill(mediamtx_pid_, SIGKILL);
      waitpid(mediamtx_pid_, nullptr, 0);
    }

    mediamtx_pid_ = -1;
    mediamtx_running_.store(false);
  }
}

// ---------------------------------------------------------------------------
// Audio Pull Pipeline (RTSP → PCM callback)
// ---------------------------------------------------------------------------

bool RtspManager::StartAudioPull(const std::string& session_id,
                                 const std::string& rtsp_url,
                                 std::function<void(const int16_t*, int)> on_pcm) {
  // Stop any existing pull pipeline for this session before starting a new one.
  // This handles sleep→wake cycles where start_push_audio is sent again
  // for the same session but with a potentially new RTSP URL.
  StopAudioPull(session_id);

  auto pipeline = std::make_unique<AudioPullPipeline>();
  pipeline->session_id = session_id;
  pipeline->rtsp_url = rtsp_url;
  pipeline->on_pcm_data = std::move(on_pcm);
  pipeline->state.store(RtspPipelineState::Starting);

  AudioPullPipeline* ptr = pipeline.get();

  {
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    pull_pipelines_.push_back(std::move(pipeline));
  }

  ptr->pull_thread = std::thread(&RtspManager::PullLoop, ptr);

  LOG_INFO("[RTSP-PULL] started for session {}, url={}", session_id, rtsp_url);
  return true;
}

void RtspManager::StopAudioPull(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(pipelines_mutex_);
  auto it = std::find_if(pull_pipelines_.begin(), pull_pipelines_.end(),
      [&](const auto& p) { return p->session_id == session_id; });
  if (it != pull_pipelines_.end()) {
    auto* p = it->get();
    p->need_exit.store(true);

    // Do NOT pclose here — PullLoop will pclose its own pipe after breaking
    // out of the fread loop. Calling pclose before joining races with
    // PullLoop's fread/pclose and causes double-free.

    if (p->pull_thread.joinable()) p->pull_thread.join();

    // Fallback cleanup after thread has definitely exited
    if (p->ffmpeg_pipe) {
      pclose(p->ffmpeg_pipe);
      p->ffmpeg_pipe = nullptr;
    }
    pull_pipelines_.erase(it);
    LOG_INFO("[RTSP-PULL] stopped for session {}", session_id);
  }
}

void RtspManager::PullLoop(AudioPullPipeline* pipeline) {
  LOG_INFO("[RTSP-PULL] loop started for session {}", pipeline->session_id);

  int retry_count = 0;
  constexpr int kMaxRetries = 10000;  // effectively infinite (~5.5h at 2s intervals)
  constexpr int kBaseDelayMs = 500;
  constexpr int kMaxDelayMs = 5000;
  // After this many consecutive EOFs, robot is likely sleeping (no RTSP publisher).
  // Use a short retry interval so recovery on wake is near-instant.
  constexpr int kFastRetryLimit = 3;
  constexpr int kSleepRetryDelayMs = 2000;

  while (!pipeline->need_exit.load() && retry_count < kMaxRetries) {
    if (!LaunchPullFfmpeg(pipeline)) {
      LOG_ERROR("[RTSP-PULL] failed to launch ffmpeg pull for {}",
                pipeline->session_id);
      pipeline->state.store(RtspPipelineState::Error);
      if (pipeline->on_disconnect) pipeline->on_disconnect(pipeline->session_id);
      return;
    }

    pipeline->state.store(RtspPipelineState::Running);

    // Read decoded PCM from ffmpeg stdout
    std::vector<int16_t> buf(1024);  // 64ms at 16kHz (was 4096=256ms)

    while (!pipeline->need_exit.load()) {
      size_t n_read = fread(buf.data(), sizeof(int16_t), buf.size(), pipeline->ffmpeg_pipe);
      if (n_read == 0) {
        if (ferror(pipeline->ffmpeg_pipe)) {
          LOG_WARN("[RTSP-PULL] read error for session {}: {}",
                   pipeline->session_id, strerror(errno));
        } else if (retry_count < kFastRetryLimit) {
          LOG_INFO("[RTSP-PULL] ffmpeg EOF for session {} (retry {}/{})",
                   pipeline->session_id, retry_count, kMaxRetries);
        }
        break;
      }

      if (pipeline->on_pcm_data) {
        pipeline->on_pcm_data(buf.data(), static_cast<int>(n_read));
      }
      retry_count = 0;  // reset retry on successful read
    }

    // Close current ffmpeg
    if (pipeline->ffmpeg_pipe) {
      pclose(pipeline->ffmpeg_pipe);
      pipeline->ffmpeg_pipe = nullptr;
    }

    if (pipeline->need_exit.load()) break;

    // Retry with backoff. After kFastRetryLimit consecutive failures,
    // assume robot is sleeping and use a long retry interval.
    retry_count++;
    int delay;
    if (retry_count <= kFastRetryLimit) {
      delay = std::min(kBaseDelayMs * (1 << std::min(retry_count, 4)), kMaxDelayMs);
      LOG_INFO("[RTSP-PULL] reconnecting in {} ms (attempt {}/{})",
               delay, retry_count, kMaxRetries);
    } else {
      delay = kSleepRetryDelayMs;
      LOG_DEBUG("[RTSP-PULL] no publisher, slow retry in {}s (attempt {}/{})",
                delay / 1000, retry_count, kMaxRetries);
    }

    for (int waited = 0; waited < delay && !pipeline->need_exit.load(); waited += 100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  pipeline->state.store(RtspPipelineState::Idle);

  LOG_INFO("[RTSP-PULL] loop exited for session {}", pipeline->session_id);
  if (pipeline->on_disconnect) {
    pipeline->on_disconnect(pipeline->session_id);
  }
}

bool RtspManager::LaunchPullFfmpeg(AudioPullPipeline* pipeline) {
  // ffmpeg -i rtsp://... -vn -acodec pcm_s16le -ar 16000 -ac 1 -f s16le pipe:1
  std::ostringstream cmd;
  cmd << "ffmpeg"
      << " -rtsp_transport tcp"
      << " -i " << pipeline->rtsp_url
      << " -vn"                           // no video
      << " -acodec pcm_s16le"             // decode to raw PCM
      << " -ar 16000"                     // 16 kHz
      << " -ac 1"                         // mono
      << " -f s16le"                      // raw format
      << " pipe:1"                        // stdout
      << " -loglevel warning"
      << " 2>>/tmp/rtsp-server/ffmpeg_pull.log";

  LOG_INFO("[RTSP-PULL] launching: {}", cmd.str());

  // Block SIGINT/SIGTERM during popen
  sigset_t old_mask, block_mask;
  sigemptyset(&block_mask);
  sigaddset(&block_mask, SIGINT);
  sigaddset(&block_mask, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &block_mask, &old_mask);

  pipeline->ffmpeg_pipe = popen(cmd.str().c_str(), "r");

  pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);

  if (!pipeline->ffmpeg_pipe) {
    LOG_ERROR("[RTSP-PULL] popen failed: {}", strerror(errno));
    return false;
  }

  // Disable buffering for low-latency
  setvbuf(pipeline->ffmpeg_pipe, nullptr, _IONBF, 0);

  LOG_INFO("[RTSP-PULL] ffmpeg launched for {}", pipeline->session_id);
  return true;
}

} // namespace rtsp_server
