#pragma once
/**
 * LLM Backend — Abstract Interface
 *
 * Decouples the voice pipeline from any specific LLM provider (Ollama,
 * TensorRT-LLM, vLLM, OpenAI API, etc.), enabling dual-backend operation
 * on the same server binary.
 *
 * Architecture:
 *
 *   ┌─ PipelineBridge ───────────────────────────────────────┐
 *   │  Uses LLMBackend*  (doesn't know which implementation)  │
 *   │                                                         │
 *   │  ┌─ OllamaBackend ──────┐  ┌─ TensorRTBackend ───────┐ │
 *   │  │ HTTP /api/chat        │  │ TensorRT-LLM C++ runtime│ │
 *   │  │ (remote Orin NX)      │  │ (local Orin GPU)        │ │
 *   │  └───────────────────────┘  └─────────────────────────┘ │
 *   └─────────────────────────────────────────────────────────┘
 *
 * Usage:
 *   auto backend = CreateLlmBackend(LlmBackendType::kOllama,
 *                                   "http://192.168.2.107:11434",
 *                                   "qwen2.5:3b", 30);
 *   if (backend->IsAvailable()) {
 *     auto reply = backend->Chat(system_prompt, user_text, {}, "");
 *   }
 *
 * Adding a new backend:
 *   1. Subclass LLMBackend, implement all virtual methods
 *   2. Add enum value to LlmBackendType
 *   3. Add case in CreateLlmBackend() factory
 *   4. Optional: add cmake conditional for custom deps
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "memory/chat_memory.h"

namespace rtsp_server {

// ── Streaming callback ───────────────────────────────────
using LlmTokenCallback = std::function<void(const std::string& token, bool is_final)>;

// ── Backend type enum ────────────────────────────────────
enum class LlmBackendType {
  kOllama,     // Ollama HTTP API (default)
  kTensorRT,   // TensorRT-LLM C++ runtime (NVIDIA Jetson Orin)
};

// ── Abstract backend interface ───────────────────────────
class LLMBackend {
public:
  virtual ~LLMBackend() = default;

  // ── Metadata ───────────────────────────────────────────

  /// Returns true if the backend successfully initialized and can serve requests.
  virtual bool IsAvailable() const = 0;

  /// Returns the model identifier string (for logging / display).
  virtual std::string GetModelName() const = 0;

  /// Returns the backend type (for logging / config display).
  virtual LlmBackendType GetType() const = 0;

  // ── Simple ping / warm-up ──────────────────────────────

  /**
   * @brief Quick connectivity + liveness check.
   *        For HTTP backends this hits /api/tags;
   *        for in-process backends this runs a tiny inference.
   * @return "ok" on success, or "error: <reason>"
   */
  virtual std::string Ping() = 0;

  // ── Generate (simple prompt, no chat history) ──────────

  /**
   * @brief Generate text from a simple prompt (no multi-turn chat).
   *        Used for model warm-up and simple single-shot queries.
   * @param prompt  input prompt
   * @param context optional context string (Ollama "context" field)
   * @param cancel  optional atomic flag — set to true to abort
   * @return generated text, or empty on failure/cancellation
   */
  virtual std::string Generate(const std::string& prompt,
                               const std::string& context = "",
                               std::atomic<bool>* cancel = nullptr) = 0;

  // ── Chat (multi-turn conversation) ─────────────────────

  /**
   * @brief Send a chat completion request with full conversation context.
   *        Non-streaming — blocks until the full response is received.
   * @param system_prompt personality / behaviour instructions
   * @param user_text    current user message
   * @param history_msgs prior conversation turns
   * @param skill_context optional skill result to inject as system context
   * @param cancel       optional atomic flag for cancellation
   * @return assistant reply text, or empty on failure/cancellation
   */
  virtual std::string Chat(const std::string& system_prompt,
                           const std::string& user_text,
                           const std::vector<ChatMessage>& history_msgs,
                           const std::string& skill_context = "",
                           std::atomic<bool>* cancel = nullptr) = 0;

  // ── Chat (streaming) ───────────────────────────────────

  /**
   * @brief Streaming chat — invokes `on_token` for each token as it arrives.
   *        Blocks until the full response is received (or cancelled).
   * @param on_token  callback invoked with each token and is_final flag
   * @return accumulated full response text, or empty on failure/cancellation
   */
  virtual std::string ChatStream(const std::string& system_prompt,
                                 const std::string& user_text,
                                 const std::vector<ChatMessage>& history_msgs,
                                 const std::string& skill_context,
                                 LlmTokenCallback on_token,
                                 std::atomic<bool>* cancel = nullptr) = 0;
};

// ── Config struct (passed to factory) ─────────────────────

struct LlmBackendConfig {
  LlmBackendType type = LlmBackendType::kTensorRT;

  // Common
  std::string model;         // model name (e.g. "qwen2.5:3b")
  int timeout_sec = 30;

  // Ollama-specific
  std::string ollama_host;   // e.g. "http://192.168.2.107:11434"

  // TensorRT-specific
  std::string trt_engine_path;    // path to built TensorRT engine
  std::string trt_tokenizer_path; // path to tokenizer model / vocab
};

// ── Factory function ──────────────────────────────────────

/**
 * @brief Create an LLM backend instance based on configuration.
 *        Returns nullptr if the requested backend cannot be initialized.
 */
std::shared_ptr<LLMBackend> CreateLlmBackend(const LlmBackendConfig& cfg);

} // namespace rtsp_server
