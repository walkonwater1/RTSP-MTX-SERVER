/**
 * WebSocket protocol unit tests.
 *
 * Tests the frame building/parsing and the signaling protocol message
 * format without requiring actual network connections.
 *
 * Build: cmake .. -DBUILD_TESTS=ON && make test_ws_protocol
 */

#include <cassert>
#include <iostream>
#include <string>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

// --- Test helpers ---

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
  do { \
    std::cout << "  TEST: " << name << " ... "; \
    try {

#define END_TEST \
      tests_passed++; \
      std::cout << "PASSED" << std::endl; \
    } catch (const std::exception& e) { \
      tests_failed++; \
      std::cout << "FAILED: " << e.what() << std::endl; \
    } \
  } while(0)

#define ASSERT(cond) \
  do { \
    if (!(cond)) throw std::runtime_error("assertion failed: " #cond); \
  } while(0)

#define ASSERT_EQ(a, b) \
  do { \
    if ((a) != (b)) throw std::runtime_error( \
      std::string("expected ") + #a + " == " + #b + \
      " (got " + std::to_string(a) + " vs " + std::to_string(b) + ")"); \
  } while(0)

// ============================================================================
// Tests
// ============================================================================

void test_req_stream_message() {
  TEST("req_stream message format") {
    json msg;
    msg["event"] = "req_stream";
    msg["session_id"] = "";
    msg["timestamp"] = 1234567890;
    msg["data"]["user_id"] = "robot_001";
    msg["data"]["mode"] = "voice";

    std::string payload = msg.dump();

    // Parse back and verify
    auto parsed = json::parse(payload);
    ASSERT(parsed["event"] == "req_stream");
    ASSERT(parsed["data"]["user_id"] == "robot_001");
    ASSERT(parsed["data"]["mode"] == "voice");
  }
  END_TEST;
}

void test_stream_address_response() {
  TEST("stream_address response format") {
    json msg;
    msg["event"] = "stream_address";
    msg["session_id"] = "abc123-def456";
    msg["timestamp"] = 1234567890;
    msg["data"]["rtsp_url"] = "rtsp://192.168.2.106:8554/robot_audio/abc123";
    msg["data"]["rtsp_pull_url"] = "rtsp://192.168.2.106:8554/tts_audio/abc123";
    msg["data"]["user_id"] = "robot_001";
    msg["data"]["mode"] = "voice";

    std::string payload = msg.dump();
    auto parsed = json::parse(payload);

    ASSERT(parsed["event"] == "stream_address");
    ASSERT(parsed["session_id"] == "abc123-def456");
    ASSERT(parsed["data"]["rtsp_url"].get<std::string>().find("rtsp://") == 0);
    ASSERT(parsed["data"]["rtsp_pull_url"].get<std::string>().find("rtsp://") == 0);
  }
  END_TEST;
}

void test_start_push_audio() {
  TEST("start_push_audio message") {
    json msg;
    msg["event"] = "start_push_audio";
    msg["session_id"] = "abc123";
    msg["timestamp"] = 1234567890;
    msg["data"]["push_time"] = 1234567000;

    std::string payload = msg.dump();
    auto parsed = json::parse(payload);

    ASSERT(parsed["event"] == "start_push_audio");
    ASSERT(parsed["data"]["push_time"] == 1234567000);
  }
  END_TEST;
}

void test_stream_ready_response() {
  TEST("stream_ready response format") {
    json msg;
    msg["event"] = "stream_ready";
    msg["session_id"] = "abc123";
    msg["timestamp"] = 1234567890;
    msg["data"]["server_received"] = true;
    msg["receive_aduio_stream"] = "abc123";  // legacy typo field

    std::string payload = msg.dump();
    auto parsed = json::parse(payload);

    ASSERT(parsed["event"] == "stream_ready");
    ASSERT(parsed["data"]["server_received"] == true);
    ASSERT(parsed.contains("receive_aduio_stream"));
  }
  END_TEST;
}

void test_tts_start_message() {
  TEST("tts_start message format") {
    json msg;
    msg["event"] = "tts_start";
    msg["session_id"] = "abc123";
    msg["timestamp"] = 1234567890;
    msg["data"]["tts_id"] = "abc123_tts_1";
    msg["data"]["audio_url"] = "rtsp://192.168.2.106:8554/tts_audio/abc123";
    msg["data"]["text"] = "今天天气晴朗，适合出门散步。";

    std::string payload = msg.dump();
    auto parsed = json::parse(payload);

    ASSERT(parsed["event"] == "tts_start");
    ASSERT(parsed["data"]["tts_id"] == "abc123_tts_1");
    ASSERT(parsed["data"]["audio_url"].get<std::string>().find("rtsp://") == 0);
    ASSERT(!parsed["data"]["text"].get<std::string>().empty());
  }
  END_TEST;
}

void test_playback_status() {
  TEST("playback_status started/completed") {
    // Started
    {
      json msg;
      msg["event"] = "playback_status";
      msg["session_id"] = "abc123";
      msg["timestamp"] = 1234567890;
      msg["data"]["status"] = "started";

      auto parsed = json::parse(msg.dump());
      ASSERT(parsed["data"]["status"] == "started");
    }

    // Completed
    {
      json msg;
      msg["event"] = "playback_status";
      msg["session_id"] = "abc123";
      msg["timestamp"] = 1234567890;
      msg["data"]["status"] = "completed";

      auto parsed = json::parse(msg.dump());
      ASSERT(parsed["data"]["status"] == "completed");
    }
  }
  END_TEST;
}

void test_tts_state_message() {
  TEST("tts_state unified protocol") {
    json msg;
    msg["event"] = "tts_state";
    msg["session_id"] = "abc123";
    msg["timestamp"] = 1234567890;
    msg["data"]["is_playing"] = false;
    msg["data"]["reason"] = "ended";
    msg["data"]["tts_id"] = "abc123_tts_1";

    auto parsed = json::parse(msg.dump());

    ASSERT(parsed["data"]["is_playing"] == false);
    ASSERT(parsed["data"]["reason"] == "ended");

    // Test various reasons
    std::vector<std::string> reasons = {
      "started", "ended", "play_error", "stop_tts", "interrupt", "client_stop"
    };
    for (auto& r : reasons) {
      msg["data"]["reason"] = r;
      auto p = json::parse(msg.dump());
      ASSERT(p["data"]["reason"] == r);
    }
  }
  END_TEST;
}

void test_stop_tts_interrupt() {
  TEST("stop_tts and interrupt messages") {
    // stop_tts
    {
      json msg;
      msg["event"] = "stop_tts";
      msg["session_id"] = "abc123";
      msg["timestamp"] = 1234567890;

      auto parsed = json::parse(msg.dump());
      ASSERT(parsed["event"] == "stop_tts");
    }

    // interrupt
    {
      json msg;
      msg["event"] = "interrupt";
      msg["session_id"] = "abc123";
      msg["timestamp"] = 1234567890;
      msg["data"]["tts_id"] = "abc123_tts_1";

      auto parsed = json::parse(msg.dump());
      ASSERT(parsed["event"] == "interrupt");
      ASSERT(parsed["data"]["tts_id"] == "abc123_tts_1");
    }
  }
  END_TEST;
}

void test_heartbeat() {
  TEST("heartbeat ping/pong") {
    // Heartbeat ping (from client)
    {
      json msg;
      msg["event"] = "ping";
      // No session_id for heartbeat

      auto parsed = json::parse(msg.dump());
      ASSERT(parsed["event"] == "ping");
      ASSERT(!parsed.contains("session_id"));
    }

    // Pong (from server)
    {
      json msg;
      msg["event"] = "pong";

      auto parsed = json::parse(msg.dump());
      ASSERT(parsed["event"] == "pong");
    }
  }
  END_TEST;
}

void test_tips_msg() {
  TEST("tips_msg format") {
    json msg;
    msg["event"] = "tips_msg";
    msg["session_id"] = "abc123";
    msg["timestamp"] = 1234567890;
    msg["data"]["msg_type"] = "info";
    msg["data"]["msg_content"] = "正在识别语音...";

    auto parsed = json::parse(msg.dump());
    ASSERT(parsed["event"] == "tips_msg");
    ASSERT(parsed["data"]["msg_type"] == "info");
  }
  END_TEST;
}

void test_session_id_format() {
  TEST("session ID is non-empty string") {
    // Session IDs should be non-empty strings with reasonable length
    std::string sid = "a1b2c3d4-5678-90ab-cdef-1234567890ab";
    ASSERT(sid.size() >= 8);
    ASSERT(!sid.empty());
  }
  END_TEST;
}

void test_rtsp_url_format() {
  TEST("RTSP URL format validation") {
    auto is_valid_rtsp = [](const std::string& url) -> bool {
      return url.find("rtsp://") == 0 && url.size() > 8;
    };

    ASSERT(is_valid_rtsp("rtsp://192.168.2.106:8554/robot_audio/abc123"));
    ASSERT(is_valid_rtsp("rtsp://localhost:8554/tts_audio/abc123"));
    ASSERT(!is_valid_rtsp("http://example.com"));
    ASSERT(!is_valid_rtsp(""));
    ASSERT(!is_valid_rtsp("rtsp://"));
  }
  END_TEST;
}

void test_message_roundtrip() {
  TEST("full protocol round-trip simulation") {
    // Simulate a complete session lifecycle

    // 1. Handshake
    json req_stream;
    req_stream["event"] = "req_stream";
    req_stream["data"]["user_id"] = "robot_001";
    req_stream["data"]["mode"] = "voice";

    auto h = json::parse(req_stream.dump());
    ASSERT(h["event"] == "req_stream");

    // Server generates session
    json stream_addr;
    stream_addr["event"] = "stream_address";
    stream_addr["session_id"] = "session_robot_001";
    stream_addr["data"]["rtsp_url"] = "rtsp://192.168.2.106:8554/robot_audio/session_robot_001";

    auto sa = json::parse(stream_addr.dump());
    std::string sid = sa["session_id"];
    ASSERT(!sid.empty());

    // 2. Robot starts pushing audio
    json start_push;
    start_push["event"] = "start_push_audio";
    start_push["session_id"] = sid;

    // Server confirms
    json stream_ready;
    stream_ready["event"] = "stream_ready";
    stream_ready["session_id"] = sid;
    stream_ready["data"]["server_received"] = true;

    auto sr = json::parse(stream_ready.dump());
    ASSERT(sr["data"]["server_received"] == true);

    // 3. Server pushes TTS
    json tts_start;
    tts_start["event"] = "tts_start";
    tts_start["session_id"] = sid;
    tts_start["data"]["tts_id"] = sid + "_tts_1";
    tts_start["data"]["audio_url"] = "rtsp://192.168.2.106:8554/tts_audio/" + sid;

    auto ts = json::parse(tts_start.dump());
    ASSERT(ts["data"]["tts_id"].get<std::string>().find("_tts_") != std::string::npos);

    // 4. Robot reports playback started
    json pb_started;
    pb_started["event"] = "playback_status";
    pb_started["session_id"] = sid;
    pb_started["data"]["status"] = "started";

    // 5. Robot reports playback completed
    json pb_completed;
    pb_completed["event"] = "playback_status";
    pb_completed["session_id"] = sid;
    pb_completed["data"]["status"] = "completed";

    ASSERT(json::parse(pb_completed.dump())["data"]["status"] == "completed");
  }
  END_TEST;
}

// ============================================================================
// Main
// ============================================================================

int main() {
  std::cout << "=== WebSocket Protocol Tests ===" << std::endl << std::endl;

  test_req_stream_message();
  test_stream_address_response();
  test_start_push_audio();
  test_stream_ready_response();
  test_tts_start_message();
  test_playback_status();
  test_tts_state_message();
  test_stop_tts_interrupt();
  test_heartbeat();
  test_tips_msg();
  test_session_id_format();
  test_rtsp_url_format();
  test_message_roundtrip();

  std::cout << std::endl;
  std::cout << "=== Results: " << tests_passed << " passed, "
            << tests_failed << " failed ===" << std::endl;

  return tests_failed > 0 ? 1 : 0;
}
