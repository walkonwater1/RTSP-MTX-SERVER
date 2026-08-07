#pragma once
/**
 * TensorRTBackend — LLM via NVIDIA TensorRT-LLM C++ runtime.
 *
 * Designed for Jetson Orin (or any NVIDIA GPU with TensorRT-LLM).
 * Uses the TensorRT-LLM C++ executor API for in-process inference.
 *
 * Prerequisites (Orin):
 *   1. Build TensorRT-LLM engine from a quantized model (e.g. INT4 Qwen2.5)
 *   2. Install TensorRT-LLM C++ runtime libraries
 *   3. Set -DTENSORRT_LLM_AVAILABLE=ON in cmake (auto-detected if headers found)
 *
 * Without TENSORRT_LLM_AVAILABLE, this class compiles as a stub:
 *   - IsAvailable() returns false
 *   - All methods return empty / error strings
 *   - Server falls back to Ollama seamlessly
 *
 * Integration notes:
 *   - TensorRT-LLM executor API: tensorrt_llm::executor::Executor
 *   - Tokenizer: typically HuggingFace tokenizer via tokenizers_cpp or similar
 *   - This backend handles the full chat pipeline: tokenize → infer → detokenize
 *   - Streaming is supported natively via executor callback
 */

#include "llm/llm_backend.h"

#include <string>

namespace rtsp_server {

class TensorRTBackend : public LLMBackend {
public:
  /**
   * @param engine_path    path to built TensorRT-LLM engine directory
   * @param tokenizer_path path to tokenizer.json / vocab
   * @param timeout_sec    inference timeout
   */
  TensorRTBackend(const std::string& engine_path,
                  const std::string& tokenizer_path,
                  int timeout_sec = 30);

  ~TensorRTBackend() override;

  TensorRTBackend(const TensorRTBackend&) = delete;
  TensorRTBackend& operator=(const TensorRTBackend&) = delete;

  // ── LLMBackend interface ────────────────────────────────

  bool IsAvailable() const override { return available_; }
  std::string GetModelName() const override { return engine_path_; }
  LlmBackendType GetType() const override { return LlmBackendType::kTensorRT; }

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
  std::string engine_path_;
  std::string tokenizer_path_;
  int timeout_sec_;
  bool available_ = false;

  // Build a chat-formatted prompt from messages.
  // Uses the Qwen2.5 chat template: <|im_start|>system\n...<|im_end|>\n...
  static std::string BuildChatPrompt(const std::string& system_prompt,
                                     const std::string& user_text,
                                     const std::vector<ChatMessage>& history_msgs,
                                     const std::string& skill_context);

#ifdef TENSORRT_LLM_AVAILABLE
  // TensorRT-LLM executor handle (opaque, defined in .cpp)
  struct TrtImpl;
  std::unique_ptr<TrtImpl> impl_;
#endif
};

} // namespace rtsp_server
