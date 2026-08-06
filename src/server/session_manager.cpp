#include "server/session_manager.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

#include "utils/logger.h"

namespace rtsp_server {

// --- 32-char hex session ID (matching client protocol) ---
std::string SessionManager::GenerateSessionId() {
  session_counter_++;

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 15);

  auto now = std::chrono::system_clock::now();
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');

  // Time-based prefix (8 hex chars)
  oss << std::setw(8) << (now_ms & 0xFFFFFFFF);

  // Remaining 24 hex chars: random
  for (int i = 0; i < 24; i++) oss << dis(gen);

  return oss.str();
}

Session* SessionManager::CreateSession(const std::string& user_id,
                                       const std::string& mode,
                                       const std::string& rtsp_base) {
  std::lock_guard<std::mutex> lock(mutex_);

  if ((int)sessions_.size() >= max_sessions_) {
    LOG_WARN("[SessionMgr] max sessions reached ({}), rejecting {}", max_sessions_, user_id);
    return nullptr;
  }

  auto session = std::make_unique<Session>();
  session->session_id = GenerateSessionId();
  session->user_id = user_id;
  session->mode = mode;

  // Assign RTSP paths (flat format matching client protocol)
  session->rtsp_push_path = "robot_speech_" + session->session_id;

  // Build full RTSP URLs: rtsp://host:port/path
  // Strip trailing slash from rtsp_base if present
  std::string base = rtsp_base;
  while (!base.empty() && base.back() == '/') base.pop_back();
  session->rtsp_push_url = base + "/" + session->rtsp_push_path;

  // Timestamps
  session->created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  session->last_activity_ms = session->created_ms;

  Session* ptr = session.get();
  sessions_.push_back(std::move(session));

  LOG_INFO("[SessionMgr] created session {} for user '{}', push_url={}",
           ptr->session_id, ptr->user_id, ptr->rtsp_push_url);

  return ptr;
}

void SessionManager::RemoveSession(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(sessions_.begin(), sessions_.end(),
      [&](const auto& s) { return s->session_id == session_id; });
  if (it != sessions_.end()) {
    LOG_INFO("[SessionMgr] removing session {}", session_id);
    sessions_.erase(it);
  }
}

Session* SessionManager::FindSession(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(sessions_.begin(), sessions_.end(),
      [&](const auto& s) { return s->session_id == session_id; });
  return (it != sessions_.end()) ? it->get() : nullptr;
}

Session* SessionManager::FindSessionByFd(int fd) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(sessions_.begin(), sessions_.end(),
      [fd](const auto& s) { return s->ws_fd == fd; });
  return (it != sessions_.end()) ? it->get() : nullptr;
}

std::vector<Session*> SessionManager::GetAllSessions() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Session*> result;
  result.reserve(sessions_.size());
  for (auto& s : sessions_) result.push_back(s.get());
  return result;
}

std::vector<std::string> SessionManager::GetExpiredSessionIds(int64_t timeout_ms) const {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  std::vector<std::string> ids;
  for (const auto& s : sessions_) {
    if ((now - s->last_activity_ms) > timeout_ms) {
      ids.push_back(s->session_id);
    }
  }
  return ids;
}

int SessionManager::PurgeExpired(int64_t timeout_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  int purged = 0;
  sessions_.erase(
      std::remove_if(sessions_.begin(), sessions_.end(),
          [&](const auto& s) {
            bool expired = (now - s->last_activity_ms) > timeout_ms;
            if (expired) {
              LOG_INFO("[SessionMgr] purging expired session {} (idle {}s)",
                       s->session_id, (now - s->last_activity_ms) / 1000);
              purged++;
            }
            return expired;
          }),
      sessions_.end());

  if (purged > 0) {
    LOG_INFO("[SessionMgr] purged {} expired sessions", purged);
  }
  return purged;
}

size_t SessionManager::SessionCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

bool SessionManager::CanAccept() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return (int)sessions_.size() < max_sessions_;
}

} // namespace rtsp_server
