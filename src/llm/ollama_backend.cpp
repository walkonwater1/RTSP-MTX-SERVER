/**
 * OllamaBackend implementation — curl + Ollama HTTP API.
 *
 * Extracted from PipelineBridge to provide a clean backend abstraction.
 * The original code paths (CallLlm / CallLlmChat / CallLlmChatStream) are
 * preserved verbatim where possible so behaviour is 100% unchanged.
 */

#include "llm/ollama_backend.h"

#include <nlohmann/json.hpp>

#include "utils/logger.h"

using json = nlohmann::json;

namespace rtsp_server {

// ---------------------------------------------------------------------------
// curl helpers (same as pipeline_bridge.cpp)
// ---------------------------------------------------------------------------

namespace {

size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
  size_t total = size * nmemb;
  userp->append(static_cast<char*>(contents), total);
  return total;
}

int CurlProgressCallback(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                         curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  if (clientp) {
    auto* cancel = static_cast<std::atomic<bool>*>(clientp);
    if (cancel->load()) return 1;  // non-zero aborts
  }
  return 0;
}

// State for streaming curl write callback
struct LlmStreamState {
  LlmTokenCallback on_token;
  std::atomic<bool>* cancel;
  std::string full_response;
  std::string buffer;  // partial NDJSON line buffer
};

size_t LlmStreamWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  auto* state = static_cast<LlmStreamState*>(userp);
  size_t total = size * nmemb;

  if (state->cancel && state->cancel->load()) return 0;

  state->buffer.append(static_cast<char*>(contents), total);

  // Process complete NDJSON lines
  size_t pos;
  while ((pos = state->buffer.find('\n')) != std::string::npos) {
    std::string line = state->buffer.substr(0, pos);
    state->buffer.erase(0, pos + 1);

    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    try {
      auto j = json::parse(line);
      bool done = j.value("done", false);

      if (!done) {
        std::string token = j.value("message", json::object()).value("content", "");
        if (!token.empty()) {
          state->full_response += token;
          if (state->on_token) state->on_token(token, false);
        }
      } else {
        if (state->on_token) state->on_token("", true);
      }
    } catch (const json::exception& e) {
      LOG_DEBUG("[Ollama] stream parse error: {} — line: {}", e.what(), line);
    }
  }

  return total;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

OllamaBackend::OllamaBackend(const std::string& host,
                               const std::string& model,
                               int timeout_sec)
    : host_(host)
    , model_(model)
    , timeout_sec_(timeout_sec) {

  // Quick connectivity check
  std::string ping_result = Ping();
  if (ping_result == "ok") {
    available_ = true;
    LOG_INFO("[Ollama] backend ready — host={} model={}", host_, model_);
  } else {
    LOG_WARN("[Ollama] backend connectivity check failed: {}", ping_result);
  }
}

OllamaBackend::~OllamaBackend() = default;

// ---------------------------------------------------------------------------
// Ping — connectivity check via /api/tags
// ---------------------------------------------------------------------------

std::string OllamaBackend::Ping() {
  CURL* curl = curl_easy_init();
  if (!curl) return "error: curl_init failed";

  std::string url = host_ + "/api/tags";
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

  std::string response;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (res == CURLE_OK) return "ok";
  return "error: " + std::string(curl_easy_strerror(res));
}

// ---------------------------------------------------------------------------
// Generate — simple prompt via /api/generate
// ---------------------------------------------------------------------------

std::string OllamaBackend::Generate(const std::string& prompt,
                                     const std::string& context,
                                     std::atomic<bool>* cancel) {
  // Special case: "ping" is handled by the Ping() method above.
  // PipelineBridge::CallLlm had "ping" as a special case hitting /api/tags;
  // we keep that legacy behaviour here so nothing breaks.
  if (prompt == "ping") {
    return Ping();
  }

  if (cancel && cancel->load()) return "";

  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string url = host_ + "/api/generate";

  json body;
  body["model"] = model_;
  body["prompt"] = prompt;
  body["stream"] = false;
  body["options"]["temperature"] = 0.7;
  body["options"]["num_predict"] = 128;
  body["keep_alive"] = -1;

  if (!context.empty()) {
    // Ollama accepts a "context" field for continuing from previous state
    try {
      body["context"] = json::parse(context);
    } catch (...) {
      // ignore invalid context
    }
  }

  std::string body_str = body.dump();

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec_));

  if (cancel) {
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  }

  std::string response;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    if (res == CURLE_ABORTED_BY_CALLBACK) {
      LOG_INFO("[Ollama] generate cancelled by interrupt");
    } else {
      LOG_ERROR("[Ollama] curl error: {}", curl_easy_strerror(res));
    }
    return "";
  }

  try {
    auto j = json::parse(response);
    std::string reply = j.value("response", "");
    auto start = reply.find_first_not_of(" \t\n\r");
    auto end = reply.find_last_not_of(" \t\n\r");
    if (start != std::string::npos)
      reply = reply.substr(start, end - start + 1);
    return reply;
  } catch (const json::exception& e) {
    LOG_ERROR("[Ollama] JSON parse error: {}", e.what());
    return "";
  }
}

// ---------------------------------------------------------------------------
// Chat — multi-turn conversation via /api/chat (non-streaming)
// ---------------------------------------------------------------------------

std::string OllamaBackend::Chat(const std::string& system_prompt,
                                 const std::string& user_text,
                                 const std::vector<ChatMessage>& history_msgs,
                                 const std::string& skill_context,
                                 std::atomic<bool>* cancel) {
  if (cancel && cancel->load()) return "";

  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string url = host_ + "/api/chat";

  // Build messages array
  json messages = json::array();

  messages.push_back({{"role", "system"}, {"content", system_prompt}});

  if (!skill_context.empty()) {
    messages.push_back({
        {"role", "system"},
        {"content", "[工具返回结果]\n" + skill_context +
                    "\n\n请严格根据以上工具返回的事实信息回答用户问题，不要凭训练数据猜测。"}
    });
  }

  for (const auto& msg : history_msgs) {
    messages.push_back({{"role", msg.role}, {"content", msg.content}});
  }

  messages.push_back({{"role", "user"}, {"content", user_text}});

  json body;
  body["model"] = model_;
  body["messages"] = messages;
  body["stream"] = false;
  body["options"]["temperature"] = 0.7;
  body["options"]["num_predict"] = 128;
  body["keep_alive"] = -1;

  std::string body_str = body.dump();

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec_));

  if (cancel) {
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  }

  std::string response;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    if (res == CURLE_ABORTED_BY_CALLBACK) {
      LOG_INFO("[Ollama] chat cancelled by interrupt");
    } else {
      LOG_ERROR("[Ollama] curl error: {}", curl_easy_strerror(res));
    }
    return "";
  }

  try {
    auto j = json::parse(response);
    std::string reply = j.value("message", json::object()).value("content", "");
    auto start = reply.find_first_not_of(" \t\n\r");
    auto end = reply.find_last_not_of(" \t\n\r");
    if (start != std::string::npos)
      reply = reply.substr(start, end - start + 1);
    return reply;
  } catch (const json::exception& e) {
    LOG_ERROR("[Ollama] JSON parse error: {}", e.what());
    return "";
  }
}

// ---------------------------------------------------------------------------
// ChatStream — streaming chat via /api/chat (NDJSON)
// ---------------------------------------------------------------------------

std::string OllamaBackend::ChatStream(const std::string& system_prompt,
                                       const std::string& user_text,
                                       const std::vector<ChatMessage>& history_msgs,
                                       const std::string& skill_context,
                                       LlmTokenCallback on_token,
                                       std::atomic<bool>* cancel) {
  if (cancel && cancel->load()) return "";

  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string url = host_ + "/api/chat";

  json messages = json::array();
  messages.push_back({{"role", "system"}, {"content", system_prompt}});

  if (!skill_context.empty()) {
    messages.push_back({
        {"role", "system"},
        {"content", "[工具返回结果]\n" + skill_context +
                    "\n\n请严格根据以上工具返回的事实信息回答用户问题，不要凭训练数据猜测。"}
    });
  }

  for (const auto& msg : history_msgs) {
    messages.push_back({{"role", msg.role}, {"content", msg.content}});
  }
  messages.push_back({{"role", "user"}, {"content", user_text}});

  json body;
  body["model"] = model_;
  body["messages"] = messages;
  body["stream"] = true;
  body["options"]["temperature"] = 0.7;
  body["options"]["num_predict"] = 128;
  body["keep_alive"] = -1;

  std::string body_str = body.dump();

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec_));

  LlmStreamState state;
  state.on_token = std::move(on_token);
  state.cancel = cancel;

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, LlmStreamWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);

  if (cancel) {
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  }

  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    if (res == CURLE_ABORTED_BY_CALLBACK) {
      LOG_INFO("[Ollama] stream cancelled by interrupt");
    } else {
      LOG_ERROR("[Ollama] stream curl error: {}", curl_easy_strerror(res));
    }
    return "";
  }

  // Trim
  auto start = state.full_response.find_first_not_of(" \t\n\r");
  auto end = state.full_response.find_last_not_of(" \t\n\r");
  if (start != std::string::npos)
    state.full_response = state.full_response.substr(start, end - start + 1);

  return state.full_response;
}

} // namespace rtsp_server
