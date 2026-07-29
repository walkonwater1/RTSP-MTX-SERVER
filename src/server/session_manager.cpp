#include "server/session_manager.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

#include "utils/logger.h"

namespace rtsp_server {

// --- UUID-like session ID generation ---
std::string SessionManager::GenerateSessionId() {
  // Use a combination of counter + random for uniqueness
  session_counter_++;

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 15);
  std::uniform_int_distribution<> dis2(8, 11);

  auto now = std::chrono::system_clock::now();
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');

  // Time-based prefix (8 hex chars)
  oss << std::setw(8) << (now_ms & 0xFFFFFFFF);

  // Version 4 UUID format for remaining
  oss << "-4";
  for (int i = 0; i < 3; i++) oss << dis(gen);
  oss << "-" << dis2(gen);
  for (int i = 0; i < 3; i++) oss << dis(gen);
  oss << "-";
  for (int i = 0; i < 12; i++) oss << dis(gen);

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

  // Assign RTSP paths
  session->rtsp_push_path = "/robot_audio/" + session->session_id;
  session->rtsp_pull_path = "/tts_audio/" + session->session_id;

  // Build full RTSP URLs: rtsp://host:port/path
  // Strip trailing slash from rtsp_base if present
  std::string base = rtsp_base;
  while (!base.empty() && base.back() == '/') base.pop_back();
  session->rtsp_push_url = base + session->rtsp_push_path;
  session->rtsp_pull_url = base + session->rtsp_pull_path;

  // Timestamps
  session->created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  session->last_activity_ms = session->created_ms;

  Session* ptr = session.get();
  sessions_.push_back(std::move(session));

  LOG_INFO("[SessionMgr] created session {} for user '{}', push_url={}, pull_url={}",
           ptr->session_id, ptr->user_id, ptr->rtsp_push_url, ptr->rtsp_pull_url);

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
