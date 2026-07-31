/**
 * Feishu (Lark) Webhook Notifier — fire-and-forget event push
 *
 * Posts interactive card messages to a Feishu bot webhook on key events:
 *   - Client connect / disconnect
 *   - ASR recognition result
 *   - Pipeline result with latency (LLM ms, TTS ms, total ms)
 *
 * All push calls are non-blocking (detached thread), so a slow or
 * unreachable webhook never stalls the main loop.
 *
 * Usage:
 *   #include "utils/feishu_notifier.h"
 *
 *   FeishuNotifier notify("https://open.feishu.cn/open-apis/bot/v2/hook/xxx");
 *   notify.OnConnect("robot_001");
 *   notify.OnAsrResult("今天天气怎么样");
 *   notify.OnPipelineResult("robot_001", "今天天气很好", 320, 450, 820);
 *   notify.OnDisconnect("robot_001");
 */

#pragma once

#include <string>
#include <thread>
#include <chrono>
#include <sstream>
#include <cstdlib>
#include <cstring>

#include "http_client.h"
#include "logger.h"

namespace rtsp_server {

class FeishuNotifier {
public:
    explicit FeishuNotifier(const std::string& webhook_url)
        : webhook_url_(webhook_url), enabled_(!webhook_url.empty()) {}

    bool Enabled() const { return enabled_; }

    // ── Event pushes (all fire-and-forget) ──────────────────────────────

    void OnConnect(const std::string& user_id, int client_fd) {
        if (!enabled_) return;
        std::string title = "🔗 客户端连接 — " + user_id;
        std::string body = "用户 **" + EscapeMd(user_id)
                         + "** 已连接到服务器\n"
                         + "FD: " + std::to_string(client_fd);
        PostCard("green", title, body);
    }

    void OnDisconnect(const std::string& user_id) {
        if (!enabled_) return;
        std::string title = "🔌 客户端断开 — " + user_id;
        std::string body = "用户 **" + EscapeMd(user_id) + "** 已断开连接";
        PostCard("red", title, body);
    }

    void OnAsrResult(const std::string& user_id, const std::string& text) {
        if (!enabled_) return;
        std::string title = "🎤 ASR 识别 — " + user_id;
        std::string body = "**识别内容：**\n" + EscapeMd(text);
        PostCard("blue", title, body);
    }

    void OnPipelineResult(const std::string& user_id,
                          const std::string& asr_text,
                          const std::string& llm_response,
                          long llm_ms,
                          long tts_ms,
                          long total_ms)
    {
        if (!enabled_) return;

        std::string latency_color = (total_ms < 2000) ? "green" : "orange";
        std::stringstream ss;
        ss << "**用户：**" << EscapeMd(user_id) << "\n\n"
           << "**识别：**" << EscapeMd(asr_text) << "\n\n"
           << "**回复：**" << EscapeMd(llm_response) << "\n\n"
           << "**延迟：**\n"
           << "LLM: " << llm_ms << "ms | TTS: " << tts_ms
           << "ms | 总链路: " << total_ms << "ms";

        PostCard(latency_color, "💬 对话响应 — " + user_id, ss.str());
    }

    void OnHeartbeat() {
        // Reserved — could push a lightweight summary periodically
    }

private:
    std::string webhook_url_;
    bool enabled_;

    // ── Card push ──────────────────────────────────────────────────────

    void PostCard(const std::string& color,
                  const std::string& title,
                  const std::string& body_md)
    {
        // Capture by value — safe for detached thread
        std::string url = webhook_url_;
        std::string payload = BuildCardJson(color, title, body_md);

        std::thread([url, payload]() {
            try {
                HttpClient::post(url, payload, 5);  // 5s timeout
            } catch (const std::exception& e) {
                LOG_DEBUG("[Feishu] push failed: {}", e.what());
            }
        }).detach();
    }

    std::string BuildCardJson(const std::string& color,
                              const std::string& title,
                              const std::string& body_md)
    {
        auto now = std::chrono::system_clock::now();
        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        char time_buf[32];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&time_t));

        std::stringstream json;
        json << "{"
             << "\"msg_type\":\"interactive\","
             << "\"card\":{"
             << "\"header\":{"
             << "\"title\":{\"tag\":\"plain_text\",\"content\":\""
             << EscapeJson(title) << " — " << time_buf << "\"},"
             << "\"template\":\"" << color << "\""
             << "},"
             << "\"elements\":[{"
             << "\"tag\":\"div\","
             << "\"text\":{\"tag\":\"lark_md\",\"content\":\""
             << EscapeJson(body_md) << "\"}"
             << "}],"
             << "\"header\":{"
             << "\"title\":{\"tag\":\"plain_text\",\"content\":\""
             << EscapeJson(title) << "\"},"
             << "\"template\":\"" << color << "\""
             << "}"
             << "}}";
        // NOTE: The double header above is intentional — Feishu card API
        // requires the header at top level. We rebuild cleanly:
        return BuildCardJsonClean(color, title, body_md, time_buf);
    }

    std::string BuildCardJsonClean(const std::string& color,
                                   const std::string& title,
                                   const std::string& body_md,
                                   const std::string& time_str)
    {
        std::stringstream json;
        json << "{"
             << "\"msg_type\":\"interactive\","
             << "\"card\":{"
             << "\"header\":{"
             << "\"title\":{\"tag\":\"plain_text\",\"content\":\""
             << EscapeJson(title) << " — " << time_str << "\"},"
             << "\"template\":\"" << color << "\""
             << "},"
             << "\"elements\":[{"
             << "\"tag\":\"div\","
             << "\"text\":{\"tag\":\"lark_md\",\"content\":\""
             << EscapeJson(body_md) << "\"}"
             << "},{"
             << "\"tag\":\"hr\""
             << "},{"
             << "\"tag\":\"note\","
             << "\"elements\":[{"
             << "\"tag\":\"plain_text\","
             << "\"content\":\"rtsp-server | event push\""
             << "}]"
             << "}]"
             << "}}";
        return json.str();
    }

    // ── String helpers ─────────────────────────────────────────────────

    static std::string EscapeJson(const std::string& s) {
        std::string out;
        out.reserve(s.size() * 2);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:   out += c;
            }
        }
        return out;
    }

    static std::string EscapeMd(const std::string& s) {
        std::string out;
        out.reserve(s.size() * 2);
        for (char c : s) {
            switch (c) {
                case '*': case '_': case '~': case '`':
                case '[': case ']': case '(': case ')':
                    out += '\\';
                    out += c;
                    break;
                default:
                    out += c;
            }
        }
        return out;
    }
};

} // namespace rtsp_server
