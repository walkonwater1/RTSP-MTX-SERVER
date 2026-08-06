#pragma once
/**
 * WebSocket Signaling Server — RFC 6455 compliant, single-threaded event loop.
 *
 * Replaces the colleague's Python signaling service. Runs on a dedicated
 * thread and communicates with the main server via callbacks.
 *
 * Supported protocol messages (JSON over WebSocket text frames):
 *
 *   Client → Server:
 *     req_stream         — handshake: request session + RTSP URL
 *     start_push_audio   — robot started pushing mic audio
 *     playback_status    — TTS playback state (started/completed)
 *     tts_state          — TTS state per new unified protocol
 *     ping               — heartbeat keep-alive
 *
 *   Server → Client:
 *     stream_address     — session info with rtsp_url
 *     stream_ready       — server confirmed receiving audio
 *     start_pull_stream  — server starting to pull for ASR
 *     tts_start          — new TTS audio ready for robot
 *     tts_finish         — TTS round ended (legacy)
 *     stop_tts           — server stops current TTS
 *     interrupt          — interrupt current playback
 *     asr_result         — ASR recognition result
 *     llm_result         — LLM response text
 *     tips_msg           — UI tip message
 */

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rtsp_server {

// --- WebSocket frame ---
struct WsFrame {
  std::string payload;  // decoded payload
  bool is_text = true;  // text vs binary
  bool is_close = false;
};

// --- Per-connection state ---
struct WsConnection {
  int fd = -1;
  std::string session_id;     // set after handshake
  std::atomic<bool> active{true};
  std::atomic<bool> need_exit{false};
  std::atomic<int64_t> last_heartbeat_ms{0};

  // Send queue (shared between server thread and connection send thread)
  std::mutex send_mutex;
  std::deque<std::string> send_queue;
  static constexpr size_t kMaxQueueSize = 64;

  // Threads
  std::thread read_thread;
  std::thread send_thread;
};

// --- Server configuration ---
struct WsServerConfig {
  int port = 8090;
  std::string bind_address = "0.0.0.0";
  std::string ws_path = "/ws/rtsp";     // HTTP upgrade path
  int ping_interval_sec = 15;
  int connect_timeout_ms = 5000;
  int io_timeout_sec = 5;
  int max_connections = 64;
  int max_message_size = 256 * 1024;    // 256 KB

  // HTTP file serving (for TTS audio download)
  bool enable_http_files = true;
  std::string http_file_dir = "/dev/shm/rtsp-server";
};

// --- Callback types ---
using MessageCallback = std::function<void(int client_fd, const std::string& event,
                                           const std::string& json_message)>;
using ConnectCallback = std::function<void(int client_fd)>;
using DisconnectCallback = std::function<void(int client_fd)>;

// --- WebSocket Signaling Server ---
class WsSignalingServer {
public:
  explicit WsSignalingServer(const WsServerConfig& cfg = {});
  ~WsSignalingServer();

  WsSignalingServer(const WsSignalingServer&) = delete;
  WsSignalingServer& operator=(const WsSignalingServer&) = delete;

  // --- Callbacks ---
  void SetMessageCallback(MessageCallback cb) { on_message_ = std::move(cb); }
  void SetConnectCallback(ConnectCallback cb) { on_connect_ = std::move(cb); }
  void SetDisconnectCallback(DisconnectCallback cb) { on_disconnect_ = std::move(cb); }

  // --- Lifecycle ---
  bool Start();
  void Stop();
  bool IsRunning() const { return running_.load(); }
  int ConnectionCount() const { return connection_count_.load(); }

  // --- Send ---
  /**
   * @brief Send a JSON message to a specific client.
   *        Thread-safe — enqueues the frame in the client's send queue.
   * @param fd       client socket fd
   * @param json_msg JSON string payload
   * @return true if enqueued successfully
   */
  bool SendMessage(int fd, const std::string& json_msg);

  /**
   * @brief Broadcast a JSON message to all connected clients.
   */
  void BroadcastMessage(const std::string& json_msg);

  /**
   * @brief Close a client connection gracefully.
   */
  void CloseClient(int fd);

private:
  // --- Network ---
  bool CreateListenSocket();
  void AcceptLoop();
  void ClientReadLoop(WsConnection* conn);
  void ClientSendLoop(WsConnection* conn);

  // --- HTTP ---
  static std::string ReadHttpRequest(int fd);
  bool HandleHttpFileRequest(int fd, const std::string& request);
  bool HandleWsUpgrade(int fd);

  // --- WebSocket protocol ---
  static bool PerformHttpHandshake(int fd, const std::string& expected_path, const std::string& request);
  static bool ReceiveFrame(int fd, WsFrame& frame, int max_size);
  static std::string BuildFrame(const std::string& payload, bool text = true);
  static std::string BuildCloseFrame();

  // --- Helpers ---
  static bool ReadExact(int fd, void* buf, size_t n, int timeout_sec = 5);
  static bool WriteAll(int fd, const void* buf, size_t n, int timeout_sec = 5);
  static std::string GenerateWebSocketAccept(const std::string& key);
  static std::string GenerateMaskingKey();

  // --- Internal send (for accept loop / broadcast) ---
  bool EnqueueFrame(WsConnection* conn, std::string frame);

  WsServerConfig cfg_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopped_{false};
  std::atomic<int> connection_count_{0};

  int listen_fd_ = -1;
  std::thread accept_thread_;

  // Connection registry
  mutable std::mutex conn_mutex_;
  std::unordered_map<int, std::unique_ptr<WsConnection>> connections_;

  // Callbacks
  MessageCallback on_message_;
  ConnectCallback on_connect_;
  DisconnectCallback on_disconnect_;
};

} // namespace rtsp_server
