#include "server/ws_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#include "nlohmann/json.hpp"
#include "utils/base64.h"
#include "utils/logger.h"

namespace rtsp_server {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

WsSignalingServer::WsSignalingServer(const WsServerConfig& cfg)
    : cfg_(cfg) {}

WsSignalingServer::~WsSignalingServer() {
  Stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool WsSignalingServer::Start() {
  if (!CreateListenSocket()) {
    LOG_ERROR("[WS] failed to create listen socket");
    return false;
  }

  running_.store(true);
  accept_thread_ = std::thread(&WsSignalingServer::AcceptLoop, this);

  LOG_INFO("[WS] signaling server started on {}:{}", cfg_.bind_address, cfg_.port);
  return true;
}

void WsSignalingServer::Stop() {
  if (stopped_.exchange(true)) return;  // idempotent
  running_.store(false);

  LOG_INFO("[WS] stopping signaling server...");

  // Close listen socket to unblock accept()
  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    listen_fd_ = -1;
  }

  // Close all client connections and move them out of the map.
  // We must NOT hold conn_mutex_ while joining threads, because
  // ClientReadLoop also tries to lock conn_mutex_ during cleanup.
  std::vector<std::unique_ptr<WsConnection>> pending_cleanup;
  {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    for (auto& [fd, conn] : connections_) {
      conn->need_exit.store(true);
      conn->active.store(false);
      shutdown(fd, SHUT_RDWR);
      close(fd);
    }
    // Move all connections out — ClientReadLoop won't find them
    // and will skip its own erase, avoiding double-join.
    for (auto& [fd, conn] : connections_) {
      pending_cleanup.push_back(std::move(conn));
    }
    connections_.clear();
    connection_count_.store(0);
  }

  // Join threads OUTSIDE the lock to avoid deadlock with ClientReadLoop
  if (accept_thread_.joinable()) accept_thread_.join();

  for (auto& conn : pending_cleanup) {
    if (conn->read_thread.joinable()) conn->read_thread.join();
    if (conn->send_thread.joinable()) conn->send_thread.join();
  }
  // pending_cleanup destroyed here — WsConnection dtors safe (threads already joined)

  LOG_INFO("[WS] signaling server stopped");
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

bool WsSignalingServer::SendMessage(int fd, const std::string& json_msg) {
  std::lock_guard<std::mutex> lock(conn_mutex_);
  auto it = connections_.find(fd);
  if (it == connections_.end() || !it->second->active.load()) {
    return false;
  }

  std::string frame = BuildFrame(json_msg);
  return EnqueueFrame(it->second.get(), std::move(frame));
}

void WsSignalingServer::BroadcastMessage(const std::string& json_msg) {
  std::string frame = BuildFrame(json_msg);
  std::lock_guard<std::mutex> lock(conn_mutex_);
  for (auto& [fd, conn] : connections_) {
    if (conn->active.load()) {
      EnqueueFrame(conn.get(), frame);  // copy for broadcast
    }
  }
}

void WsSignalingServer::CloseClient(int fd) {
  std::lock_guard<std::mutex> lock(conn_mutex_);
  auto it = connections_.find(fd);
  if (it != connections_.end()) {
    auto* conn = it->second.get();
    conn->need_exit.store(true);
    conn->active.store(false);

    // Send close frame (best-effort)
    std::string close_frame = BuildCloseFrame();
    WriteAll(fd, close_frame.data(), close_frame.size(), 1);

    shutdown(fd, SHUT_RDWR);
    close(fd);

    if (conn->read_thread.joinable()) conn->read_thread.join();
    if (conn->send_thread.joinable()) conn->send_thread.join();

    connections_.erase(it);
    connection_count_--;
    LOG_INFO("[WS] client {} disconnected (server-initiated)", fd);

    if (on_disconnect_) on_disconnect_(fd);
  }
}

// ---------------------------------------------------------------------------
// Internal: Enqueue for send thread
// ---------------------------------------------------------------------------

bool WsSignalingServer::EnqueueFrame(WsConnection* conn, std::string frame) {
  std::lock_guard<std::mutex> lock(conn->send_mutex);
  if (conn->send_queue.size() >= WsConnection::kMaxQueueSize) {
    LOG_WARN("[WS] send queue full for fd={}, dropping message", conn->fd);
    return false;
  }
  conn->send_queue.push_back(std::move(frame));
  return true;
}

// ---------------------------------------------------------------------------
// Listen Socket
// ---------------------------------------------------------------------------

bool WsSignalingServer::CreateListenSocket() {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    LOG_ERROR("[WS] socket() failed: {}", strerror(errno));
    return false;
  }

  int opt = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg_.port);
  if (cfg_.bind_address == "0.0.0.0") {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else {
    inet_pton(AF_INET, cfg_.bind_address.c_str(), &addr.sin_addr);
  }

  if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    LOG_ERROR("[WS] bind() failed: {}", strerror(errno));
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  if (listen(listen_fd_, SOMAXCONN) < 0) {
    LOG_ERROR("[WS] listen() failed: {}", strerror(errno));
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  LOG_INFO("[WS] listening on {}:{}", cfg_.bind_address, cfg_.port);
  return true;
}

// ---------------------------------------------------------------------------
// Accept Loop
// ---------------------------------------------------------------------------

void WsSignalingServer::AcceptLoop() {
  LOG_DEBUG("[WS] accept loop started");

  while (running_.load()) {
    struct pollfd pfd;
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, 1000);
    if (ret < 0) {
      if (errno == EINTR) continue;
      LOG_ERROR("[WS] accept poll error: {}", strerror(errno));
      break;
    }
    if (ret == 0) continue;

    struct sockaddr_in client_addr {};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      LOG_ERROR("[WS] accept() failed: {}", strerror(errno));
      continue;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    LOG_INFO("[WS] new TCP connection from {}:{} (fd={})",
             client_ip, ntohs(client_addr.sin_port), client_fd);

    // Check connection limit
    if (connection_count_.load() >= cfg_.max_connections) {
      LOG_WARN("[WS] max connections reached, rejecting {}", client_ip);
      close(client_fd);
      continue;
    }

    // Set TCP_NODELAY
    {
      int opt = 1;
      setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    }

    // Set socket timeout for handshake
    {
      struct timeval tv;
      tv.tv_sec = cfg_.io_timeout_sec;
      tv.tv_usec = 0;
      setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    // Read HTTP request header
    std::string http_request = ReadHttpRequest(client_fd);
    if (http_request.empty()) {
      LOG_WARN("[WS] failed to read HTTP request for fd={}", client_fd);
      close(client_fd);
      continue;
    }

    // Check if it's a plain HTTP GET (TTS file download)
    if (http_request.find("GET /file?") == 0 && cfg_.enable_http_files) {
      HandleHttpFileRequest(client_fd, http_request);
      close(client_fd);
      continue;
    }

    // WebSocket upgrade handshake
    if (!PerformHttpHandshake(client_fd, cfg_.ws_path, http_request)) {
      LOG_WARN("[WS] HTTP handshake failed for fd={}", client_fd);
      close(client_fd);
      continue;
    }

    LOG_DEBUG("[WS] WebSocket upgrade complete for fd={}", client_fd);

    // Clear receive timeout for long-lived connection
    {
      struct timeval tv {};
      tv.tv_sec = 0;
      tv.tv_usec = 0;
      setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    // Create connection
    auto conn = std::make_unique<WsConnection>();
    conn->fd = client_fd;
    conn->active.store(true);
    conn->last_heartbeat_ms.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    WsConnection* conn_ptr = conn.get();
    {
      std::lock_guard<std::mutex> lock(conn_mutex_);
      connections_[client_fd] = std::move(conn);
    }

    // Start threads
    conn_ptr->read_thread = std::thread(&WsSignalingServer::ClientReadLoop, this, conn_ptr);
    conn_ptr->send_thread = std::thread(&WsSignalingServer::ClientSendLoop, this, conn_ptr);

    connection_count_++;
    LOG_DEBUG("[WS] client fd={} ready (total connections: {})",
             client_fd, connection_count_.load());

    if (on_connect_) on_connect_(client_fd);
  }

  LOG_DEBUG("[WS] accept loop exited");
}

// ---------------------------------------------------------------------------
// Client Read Loop
// ---------------------------------------------------------------------------

void WsSignalingServer::ClientReadLoop(WsConnection* conn) {
  LOG_DEBUG("[WS] read loop started for fd={}", conn->fd);

  auto last_heartbeat = std::chrono::steady_clock::now();

  while (conn->active.load() && !conn->need_exit.load()) {
    // Poll for data with 1s timeout (for heartbeat)
    struct pollfd pfd;
    pfd.fd = conn->fd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, 1000);

    if (ret < 0) {
      if (errno == EINTR) continue;
      LOG_WARN("[WS] poll error for fd={}: {}", conn->fd, strerror(errno));
      break;
    }

    // Heartbeat check
    auto now = std::chrono::steady_clock::now();
    if (now - last_heartbeat >= std::chrono::seconds(cfg_.ping_interval_sec)) {
      // Send ping as a text frame (consistent with existing robot-side client)
      // The robot-side client sends {"event":"ping"}, but as a server we
      // should be receiving pings and responding. Here we're passive.
      last_heartbeat = now;
    }

    if (ret == 0) continue;  // timeout, no data

    // Read frame
    WsFrame frame;
    if (!ReceiveFrame(conn->fd, frame, cfg_.max_message_size)) {
      if (conn->active.load()) {
        LOG_WARN("[WS] connection lost for fd={}", conn->fd);
        conn->active.store(false);
      }
      break;
    }

    // Reset heartbeat on data
    last_heartbeat = std::chrono::steady_clock::now();
    conn->last_heartbeat_ms.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    if (frame.payload.empty()) continue;

    // Handle ping frame internally
    try {
      auto j = json::parse(frame.payload);
      std::string event = j.value("event", "");
      if (event == "ping") {
        // Respond with pong
        json pong;
        pong["event"] = "pong";
        std::string pong_frame = BuildFrame(pong.dump());
        EnqueueFrame(conn, std::move(pong_frame));
        LOG_DEBUG("[WS] ping → pong for fd={}", conn->fd);
        continue;
      }
    } catch (...) {
      // Not JSON — might be binary audio data, pass through
    }

    // Deliver to application layer
    if (on_message_ && !frame.payload.empty()) {
      std::string event;
      try {
        auto j = json::parse(frame.payload);
        event = j.value("event", "");
      } catch (...) {}

      on_message_(conn->fd, event, frame.payload);
    }
  }

  LOG_DEBUG("[WS] read loop exited for fd={}", conn->fd);

  // Signal send thread to stop and wait for it to finish.
  // The send thread only locks conn->send_mutex, not conn_mutex_,
  // so joining here is safe (no deadlock).
  conn->active.store(false);
  conn->need_exit.store(true);

  if (conn->send_thread.joinable()) {
    conn->send_thread.join();
  }

  int saved_fd = conn->fd;

  // CRITICAL: We are running INSIDE conn->read_thread. If we erase the
  // connection from the map, the WsConnection destructor destroys
  // std::thread read_thread while it's still joinable → std::terminate().
  // Detach ourselves first so the destructor is safe.
  conn->read_thread.detach();

  // Close socket and remove from registry
  {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = connections_.find(saved_fd);
    if (it != connections_.end()) {
      shutdown(saved_fd, SHUT_RDWR);
      close(saved_fd);
      connections_.erase(it);
      connection_count_--;
    }
  }

  // Call disconnect handler OUTSIDE the lock to avoid deadlock
  if (on_disconnect_) {
    on_disconnect_(saved_fd);
  }
}

// ---------------------------------------------------------------------------
// Client Send Loop
// ---------------------------------------------------------------------------

void WsSignalingServer::ClientSendLoop(WsConnection* conn) {
  LOG_DEBUG("[WS] send loop started for fd={}", conn->fd);

  while (!conn->need_exit.load()) {
    std::string frame_data;

    {
      std::unique_lock<std::mutex> lock(conn->send_mutex);
      // Wait for data or exit
      while (conn->send_queue.empty() && !conn->need_exit.load()) {
        // Use a short sleep to avoid tight loop (not great but simple)
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        lock.lock();
      }

      if (conn->need_exit.load() && conn->send_queue.empty()) break;
      if (conn->send_queue.empty()) continue;

      frame_data = std::move(conn->send_queue.front());
      conn->send_queue.pop_front();
    }

    if (!conn->active.load()) {
      LOG_DEBUG("[WS] send thread: fd={} inactive, discarding", conn->fd);
      continue;
    }

    if (!WriteAll(conn->fd, frame_data.data(), frame_data.size(), 5)) {
      LOG_WARN("[WS] send failed for fd={}, exiting send loop", conn->fd);
      conn->active.store(false);
      break;
    }
  }

  // Drain on exit
  {
    std::lock_guard<std::mutex> lock(conn->send_mutex);
    if (!conn->send_queue.empty()) {
      LOG_INFO("[WS] discarding {} queued frames for fd={}",
               conn->send_queue.size(), conn->fd);
      conn->send_queue.clear();
    }
  }

  LOG_DEBUG("[WS] send loop exited for fd={}", conn->fd);
}

// ---------------------------------------------------------------------------
// HTTP Upgrade Handshake
// ---------------------------------------------------------------------------

bool WsSignalingServer::PerformHttpHandshake(int fd, const std::string& expected_path,
                                             const std::string& request) {
  (void)fd;  // already read by caller (ReadHttpRequest)

  LOG_DEBUG("[WS] HTTP request:\n{}", request);

  // Extract headers
  std::string ws_key;
  std::string ws_version;

  // Find Sec-WebSocket-Key
  auto key_pos = request.find("Sec-WebSocket-Key:");
  if (key_pos != std::string::npos) {
    auto end = request.find("\r\n", key_pos);
    ws_key = request.substr(key_pos + 19, end - (key_pos + 19));
    // Trim whitespace
    auto start = ws_key.find_first_not_of(" \t");
    auto last = ws_key.find_last_not_of(" \t");
    if (start != std::string::npos && last != std::string::npos) {
      ws_key = ws_key.substr(start, last - start + 1);
    }
  }

  // Find Sec-WebSocket-Version
  auto ver_pos = request.find("Sec-WebSocket-Version:");
  if (ver_pos != std::string::npos) {
    auto end = request.find("\r\n", ver_pos);
    ws_version = request.substr(ver_pos + 22, end - (ver_pos + 22));
    auto start = ws_version.find_first_not_of(" \t");
    if (start != std::string::npos) {
      ws_version = ws_version.substr(start);
      auto last = ws_version.find_last_not_of(" \t\r\n");
      if (last != std::string::npos) ws_version = ws_version.substr(0, last + 1);
    }
  }

  if (ws_key.empty()) {
    LOG_WARN("[WS] missing Sec-WebSocket-Key");
    return false;
  }

  // Check path (optional — we accept any path for flexibility)
  // In strict mode we could verify the GET path matches expected_path

  // Build response
  std::string accept = GenerateWebSocketAccept(ws_key);

  std::ostringstream response;
  response << "HTTP/1.1 101 Switching Protocols\r\n"
           << "Upgrade: websocket\r\n"
           << "Connection: Upgrade\r\n"
           << "Sec-WebSocket-Accept: " << accept << "\r\n"
           << "\r\n";

  std::string resp_str = response.str();
  LOG_DEBUG("[WS] HTTP response:\n{}", resp_str);

  return WriteAll(fd, resp_str.data(), resp_str.size(), 5);
}

// ---------------------------------------------------------------------------
// Read HTTP Request
// ---------------------------------------------------------------------------

std::string WsSignalingServer::ReadHttpRequest(int fd) {
  std::string request;
  char buf[4096];
  while (true) {
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
      LOG_DEBUG("[WS] recv during HTTP read: {}", n == 0 ? "EOF" : strerror(errno));
      return "";
    }
    buf[n] = '\0';
    request += buf;

    if (request.find("\r\n\r\n") != std::string::npos) break;
    if (request.size() > 16384) {
      LOG_WARN("[WS] HTTP request too large");
      return "";
    }
  }
  return request;
}

// ---------------------------------------------------------------------------
// HTTP File Request Handler (TTS audio download)
// ---------------------------------------------------------------------------

bool WsSignalingServer::HandleHttpFileRequest(int fd, const std::string& request) {
  // Parse: GET /file?token=<filename> HTTP/1.1
  auto path_start = request.find(" /");
  if (path_start == std::string::npos) return false;
  path_start++;  // skip space
  auto path_end = request.find(" ", path_start);
  if (path_end == std::string::npos) return false;
  std::string path = request.substr(path_start, path_end - path_start);

  // Extract token from query string
  auto token_pos = path.find("token=");
  if (token_pos == std::string::npos) {
    LOG_DEBUG("[WS] HTTP GET without token: {}", path);
    std::string not_found = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    WriteAll(fd, not_found.data(), not_found.size(), 3);
    return false;
  }

  std::string token = path.substr(token_pos + 6);
  // Strip trailing params if any
  auto amp_pos = token.find("&");
  if (amp_pos != std::string::npos) token = token.substr(0, amp_pos);
  // Strip HTTP version suffix that may have bled in
  auto space_pos = token.find(" ");
  if (space_pos != std::string::npos) token = token.substr(0, space_pos);

  // Security: only allow alphanumeric, underscore, dash in token
  for (char c : token) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
      LOG_WARN("[WS] HTTP file request: invalid token '{}'", token);
      std::string bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      WriteAll(fd, bad.data(), bad.size(), 3);
      return false;
    }
  }

  std::string file_path = cfg_.http_file_dir + "/" + token + ".wav";
  LOG_DEBUG("[WS] HTTP file request: token={}, path={}", token, file_path);

  // Open file
  std::ifstream file(file_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    LOG_WARN("[WS] HTTP file not found: {}", file_path);
    std::string not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    WriteAll(fd, not_found.data(), not_found.size(), 3);
    return false;
  }

  size_t file_size = file.tellg();
  file.seekg(0, std::ios::beg);

  // Send HTTP response header
  std::ostringstream header;
  header << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: audio/wav\r\n"
         << "Content-Length: " << file_size << "\r\n"
         << "Connection: close\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "\r\n";

  std::string header_str = header.str();
  if (!WriteAll(fd, header_str.data(), header_str.size(), 5)) {
    return false;
  }

  // Send file contents
  std::vector<char> file_data(file_size);
  file.read(file_data.data(), file_size);
  bool ok = WriteAll(fd, file_data.data(), file_size, 15);
  LOG_DEBUG("[WS] HTTP file served: {} ({} bytes)", token, file_size);
  return ok;
}

// ---------------------------------------------------------------------------
// WebSocket Frame Receive
// ---------------------------------------------------------------------------

bool WsSignalingServer::ReceiveFrame(int fd, WsFrame& frame, int max_size) {
  frame = WsFrame{};

  uint8_t header[2];
  if (!ReadExact(fd, header, 2)) return false;

  uint8_t opcode = header[0] & 0x0F;
  bool masked = (header[1] & 0x80) != 0;
  uint64_t payload_len = header[1] & 0x7F;

  // Extended payload length
  if (payload_len == 126) {
    uint8_t ext[2];
    if (!ReadExact(fd, ext, 2)) return false;
    payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
  } else if (payload_len == 127) {
    uint8_t ext[8];
    if (!ReadExact(fd, ext, 8)) return false;
    payload_len = 0;
    for (int i = 0; i < 8; i++) {
      payload_len = (payload_len << 8) | ext[i];
    }
  }

  if (payload_len > static_cast<uint64_t>(max_size)) {
    LOG_WARN("[WS] payload too large: {} > {}", payload_len, max_size);
    return false;
  }

  // Masking key
  uint8_t mask_key[4] = {0};
  if (masked) {
    if (!ReadExact(fd, mask_key, 4)) return false;
  }

  // Payload
  std::vector<uint8_t> buf;
  if (payload_len > 0) {
    buf.resize(payload_len);
    if (!ReadExact(fd, buf.data(), payload_len)) return false;

    if (masked) {
      for (uint64_t i = 0; i < payload_len; i++) {
        buf[i] ^= mask_key[i % 4];
      }
    }
  }

  // Handle control frames
  if (opcode == 0x08) {
    // Close frame
    LOG_DEBUG("[WS] received close frame, payload size={}", payload_len);
    frame.is_close = true;
    frame.payload.assign(reinterpret_cast<const char*>(buf.data()), payload_len);
    return false;  // signal disconnect
  }

  if (opcode == 0x09) {
    // Ping → respond with pong (must be masked per RFC 6455)
    LOG_DEBUG("[WS] received ping, sending pong");

    std::string pong_frame;
    pong_frame.push_back(static_cast<char>(0x8A));  // FIN + Pong

    if (payload_len < 126) {
      pong_frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(payload_len)));
    } else if (payload_len <= 65535) {
      pong_frame.push_back(static_cast<char>(0x80 | 126));
      pong_frame.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
      pong_frame.push_back(static_cast<char>(payload_len & 0xFF));
    } else {
      pong_frame.push_back(static_cast<char>(0x80 | 127));
      for (int i = 7; i >= 0; i--) {
        pong_frame.push_back(static_cast<char>((payload_len >> (i * 8)) & 0xFF));
      }
    }

    // Masking key + masked application data
    std::string mask = GenerateMaskingKey();
    pong_frame += mask;
    for (uint64_t i = 0; i < payload_len; i++) {
      pong_frame.push_back(buf[i] ^ mask[i % 4]);
    }

    WriteAll(fd, pong_frame.data(), pong_frame.size(), 5);
    return ReceiveFrame(fd, frame, max_size);  // recurse to get next frame
  }

  if (opcode == 0x0A) {
    // Pong — ignore
    LOG_DEBUG("[WS] received pong");
    return ReceiveFrame(fd, frame, max_size);
  }

  // Text or binary frame
  frame.is_text = (opcode == 0x01);
  if (payload_len > 0) {
    frame.payload.assign(reinterpret_cast<const char*>(buf.data()), payload_len);
  }

  return true;
}

// ---------------------------------------------------------------------------
// WebSocket Frame Building (server → client, NOT masked per RFC 6455 §5.3)
// ---------------------------------------------------------------------------

std::string WsSignalingServer::BuildFrame(const std::string& payload, bool text) {
  std::string frame;

  // Byte 0: FIN=1, RSV=0, opcode
  frame.push_back(static_cast<char>(0x80 | (text ? 0x01 : 0x02)));

  // Byte 1: MASK=0 (server→client is NOT masked)
  size_t len = payload.size();
  if (len < 126) {
    frame.push_back(static_cast<char>(len));
  } else if (len <= 65535) {
    frame.push_back(static_cast<char>(126));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));
  } else {
    frame.push_back(static_cast<char>(127));
    for (int i = 7; i >= 0; i--) {
      frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
  }

  // Payload (no masking for server→client)
  frame += payload;

  return frame;
}

std::string WsSignalingServer::BuildCloseFrame() {
  std::string frame;
  frame.push_back(static_cast<char>(0x88));  // FIN + Close
  frame.push_back(static_cast<char>(0x00));  // zero-length payload
  return frame;
}

// ---------------------------------------------------------------------------
// WebSocket Key Generation
// ---------------------------------------------------------------------------

std::string WsSignalingServer::GenerateWebSocketAccept(const std::string& key) {
  // RFC 6455: base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
  const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::string combined = key + magic;

  // SHA1 over combined
  // Use `sha1sum` as a lightweight approach, or implement inline
  // For simplicity, we use a pipe to openssl sha1
  std::ostringstream cmd;
  cmd << "echo -n \"" << combined << "\" | openssl sha1 -binary | base64";

  FILE* p = popen(cmd.str().c_str(), "r");
  if (!p) {
    LOG_ERROR("[WS] failed to compute WebSocket accept key");
    return "";
  }

  char result[64] = {};
  fgets(result, sizeof(result), p);
  pclose(p);

  // Trim newline
  std::string accept(result);
  while (!accept.empty() && (accept.back() == '\n' || accept.back() == '\r')) {
    accept.pop_back();
  }

  return accept;
}

std::string WsSignalingServer::GenerateMaskingKey() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 255);
  char key[4];
  for (int i = 0; i < 4; i++) {
    key[i] = static_cast<char>(dis(gen));
  }
  return std::string(key, 4);
}

// ---------------------------------------------------------------------------
// Low-Level Socket I/O
// ---------------------------------------------------------------------------

bool WsSignalingServer::ReadExact(int fd, void* buf, size_t n, int timeout_sec) {
  if (timeout_sec > 0) {
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  size_t total = 0;
  auto* p = static_cast<uint8_t*>(buf);
  while (total < n) {
    ssize_t r = recv(fd, p + total, n - total, 0);
    if (r <= 0) {
      if (r == 0) {
        LOG_DEBUG("[WS] recv: connection closed");
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_DEBUG("[WS] recv error: {}", strerror(errno));
      }
      return false;
    }
    total += static_cast<size_t>(r);
  }
  return true;
}

bool WsSignalingServer::WriteAll(int fd, const void* buf, size_t n, int timeout_sec) {
  if (timeout_sec > 0) {
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }

  size_t total = 0;
  auto* p = static_cast<const uint8_t*>(buf);
  while (total < n) {
    ssize_t w = send(fd, p + total, n - total, MSG_NOSIGNAL);
    if (w <= 0) {
      if (w == 0 || errno == EPIPE || errno == ECONNRESET) {
        LOG_DEBUG("[WS] send: connection closed");
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_DEBUG("[WS] send error: {}", strerror(errno));
      }
      return false;
    }
    total += static_cast<size_t>(w);
  }
  return true;
}

} // namespace rtsp_server
