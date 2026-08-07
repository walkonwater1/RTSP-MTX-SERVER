#pragma once
/**
 * OllamaBackend — LLM via Ollama HTTP API (/api/generate, /api/chat).
 *
 * Extracted from PipelineBridge's private CallLlm / CallLlmChat /
 * CallLlmChatStream methods.  All curl details live here so the
 * PipelineBridge is provider-agnostic.
 *
 * API endpoints used:
 *   GET  /api/tags          — connectivity check (Ping)
 *   POST /api/generate      — simple single-shot completion
 *   POST /api/chat          — multi-turn chat (non-streaming + streaming)
 */

#include "llm/llm_backend.h"

#include <curl/curl.h>
#include <string>

namespace rtsp_server {

class OllamaBackend : public LLMBackend {
public:
  /**
   * @param host       Ollama server URL (e.g. "http://192.168.2.107:11434")
   * @param model      model name (e.g. "qwen2.5:3b")
   * @param timeout_sec HTTP timeout
   */
  OllamaBackend(const std::string& host,
                const std::string& model,
                int timeout_sec = 30);

  ~OllamaBackend() override;

  OllamaBackend(const OllamaBackend&) = delete;
  OllamaBackend& operator=(const OllamaBackend&) = delete;

  // ── LLMBackend interface ────────────────────────────────

  bool IsAvailable() const override { return available_; }
  std::string GetModelName() const override { return model_; }
  LlmBackendType GetType() const override { return LlmBackendType::kOllama; }

  std::string Ping() override;

  std::string Generate(const std::string& prompt,
                       const std::string& context = "",
                       std::atomic<bool>* cancel = nullptr) override;

  std::string Chat(const std::string& system_prompt,
                   const std::string& user_text,
                   const std::vector<ChatMessage>& history_msgs,
                   const std::string& skill_context = "",
                   std::atomic<bool>* cancel = nullptr) override;

  std::string ChatStream(const std::string& system_prompt,
                         const std::string& user_text,
                         const std::vector<ChatMessage>& history_msgs,
                         const std::string& skill_context,
                         LlmTokenCallback on_token,
                         std::atomic<bool>* cancel = nullptr) override;

private:
  std::string host_;
  std::string model_;
  int timeout_sec_;
  bool available_ = false;
};

} // namespace rtsp_server
