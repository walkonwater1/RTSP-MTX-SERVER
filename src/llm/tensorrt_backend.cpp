/**
 * TensorRTBackend implementation.
 *
 * Two code paths controlled by TENSORRT_LLM_AVAILABLE:
 *
 *   WITH TensorRT-LLM:  Uses executor API for real GPU inference.
 *                        Tokenize → infer → detokenize pipeline, all in-process.
 *
 *   WITHOUT (stub):     All methods return empty / error. The server will
 *                        log a warning and fall back to Ollama.
 *
 * To enable on Orin:
 *   1. Install TensorRT-LLM C++ libraries
 *      (typically /usr/local/tensorrt_llm/lib/)
 *   2. Build with -DTENSORRT_LLM_ROOT=/path/to/tensorrt_llm
 *   3. Place built engine at the configured path
 */

#include "llm/tensorrt_backend.h"

#include <sstream>

#include "utils/logger.h"

namespace rtsp_server {

// ---------------------------------------------------------------------------
// Chat prompt building (Qwen2.5 chat template)
// ---------------------------------------------------------------------------

std::string TensorRTBackend::BuildChatPrompt(
    const std::string& system_prompt,
    const std::string& user_text,
    const std::vector<ChatMessage>& history_msgs,
    const std::string& skill_context) {

  std::ostringstream ss;

  // System prompt
  ss << "<|im_start|>system\n" << system_prompt;
  if (!skill_context.empty()) {
    ss << "\n\n[工具返回结果]\n" << skill_context
       << "\n\n请严格根据以上工具返回的事实信息回答用户问题，不要凭训练数据猜测。";
  }
  ss << "<|im_end|>\n";

  // History
  for (const auto& msg : history_msgs) {
    ss << "<|im_start|>" << msg.role << "\n"
       << msg.content << "<|im_end|>\n";
  }

  // Current user message
  ss << "<|im_start|>user\n" << user_text << "<|im_end|>\n";
  ss << "<|im_start|>assistant\n";

  return ss.str();
}

// ===========================================================================
//  STUB IMPLEMENTATION (no TensorRT-LLM available)
// ===========================================================================

#ifdef TENSORRT_LLM_AVAILABLE

// ===========================================================================
//  REAL IMPLEMENTATION (TensorRT-LLM C++ runtime)
// ===========================================================================
//
// Example integration with TensorRT-LLM executor API:
//
//   #include <tensorrt_llm/executor/executor.h>
//   #include <tensorrt_llm/plugins/api/tllmPlugin.h>
//
//   struct TensorRTBackend::TrtImpl {
//     std::unique_ptr<tensorrt_llm::executor::Executor> executor;
//     // tokenizer wrapper (HF tokenizer via tokenizers_cpp or custom)
//     std::unique_ptr<Tokenizer> tokenizer;
//     tensorrt_llm::executor::ExecutorConfig config;
//   };
//
// Initialization:
//   - Load TensorRT-LLM plugins: initTrtLlmPlugins()
//   - Create ExecutorConfig with engine path
//   - Create Executor from config
//   - Load tokenizer from tokenizer_path
//
// Inference:
//   - Build chat prompt via BuildChatPrompt()
//   - Tokenize prompt → Vec<Int32> input_ids
//   - Create Request with input_ids, max_new_tokens, streaming flag
//   - executor->enqueueRequest(request)
//   - Poll or await responses; for streaming use per-token callback
//   - Detokenize output tokens → string
//
// Cancellation:
//   - executor->cancelRequest(request_id)
//
// Notes:
//   - INT4 AWQ/SmoothQuant quantized models achieve ~2x speedup on Orin
//   - Max context length depends on engine build config (4K–8K typical)
//   - The chat template should match the one used during training
//
// For reference, the full integration would look like:
//
//   bool TensorRTBackend::InitializeTrt() {
//     initTrtLlmPlugins();
//     tensorrt_llm::executor::ExecutorConfig config;
//     config.setMaxBeamWidth(1);
//     // ... set kv cache config, scheduler config, etc.
//     impl_->executor = std::make_unique<...>(engine_path_, config);
//     impl_->tokenizer = Tokenizer::FromFile(tokenizer_path_);
//     return true;
//   }
//
// When you're ready to integrate TensorRT-LLM for real:
//   1. Install the SDK: https://github.com/NVIDIA/TensorRT-LLM
//   2. Uncomment the includes above
//   3. Fill in the InitializeTrt() method
//   4. Implement Generate/Chat/ChatStream using the executor
//   5. Build with: cmake -DTENSORRT_LLM_AVAILABLE=ON ..

TensorRTBackend::TensorRTBackend(const std::string& engine_path,
                                 const std::string& tokenizer_path,
                                 int timeout_sec)
    : engine_path_(engine_path)
    , tokenizer_path_(tokenizer_path)
    , timeout_sec_(timeout_sec) {

  LOG_INFO("[TensorRT] initializing — engine={} tokenizer={}",
           engine_path_, tokenizer_path_);

  // TODO: Initialize TensorRT-LLM executor
  // available_ = InitializeTrt();
  //
  // For now, mark available if the engine path exists:
  // available_ = (access(engine_path_.c_str(), F_OK) == 0);

  if (available_) {
    LOG_INFO("[TensorRT] backend ready — engine={}", engine_path_);
  } else {
    LOG_WARN("[TensorRT] engine not found at {} — backend unavailable", engine_path_);
  }
}

TensorRTBackend::~TensorRTBackend() = default;

std::string TensorRTBackend::Ping() {
  if (!available_) return "error: TensorRT backend not initialized";
  // Quick inference test with minimal prompt
  std::string result = Generate("hi", "", nullptr);
  return result.empty() ? "error: inference test failed" : "ok";
}

std::string TensorRTBackend::Generate(const std::string& prompt,
                                       const std::string& /*context*/,
                                       std::atomic<bool>* cancel) {
  if (!available_) return "";
  if (cancel && cancel->load()) return "";

  // TODO: Real inference via executor
  LOG_WARN("[TensorRT] Generate not yet implemented (stub)");
  return "";
}

std::string TensorRTBackend::Chat(const std::string& system_prompt,
                                   const std::string& user_text,
                                   const std::vector<ChatMessage>& history_msgs,
                                   const std::string& skill_context,
                                   std::atomic<bool>* cancel) {
  if (!available_) return "";
  if (cancel && cancel->load()) return "";

  std::string prompt = BuildChatPrompt(system_prompt, user_text,
                                        history_msgs, skill_context);

  // TODO: Real chat inference via executor
  LOG_WARN("[TensorRT] Chat not yet implemented (stub)");
  return "";
}

std::string TensorRTBackend::ChatStream(const std::string& system_prompt,
                                         const std::string& user_text,
                                         const std::vector<ChatMessage>& history_msgs,
                                         const std::string& skill_context,
                                         LlmTokenCallback on_token,
                                         std::atomic<bool>* cancel) {
  if (!available_) return "";
  if (cancel && cancel->load()) return "";

  std::string prompt = BuildChatPrompt(system_prompt, user_text,
                                        history_msgs, skill_context);

  // TODO: Real streaming inference via executor
  LOG_WARN("[TensorRT] ChatStream not yet implemented (stub)");
  return "";
}

#else // !TENSORRT_LLM_AVAILABLE

// ===========================================================================
//  STUB — no TensorRT-LLM SDK installed
// ===========================================================================

TensorRTBackend::TensorRTBackend(const std::string& engine_path,
                                 const std::string& tokenizer_path,
                                 int timeout_sec)
    : engine_path_(engine_path)
    , tokenizer_path_(tokenizer_path)
    , timeout_sec_(timeout_sec)
    , available_(false) {

  LOG_WARN("[TensorRT] compiled without TensorRT-LLM support — stub mode");
  LOG_WARN("[TensorRT] to enable: install TensorRT-LLM SDK and rebuild with "
           "-DTENSORRT_LLM_AVAILABLE=ON");
}

TensorRTBackend::~TensorRTBackend() = default;

std::string TensorRTBackend::Ping() {
  return "error: TensorRT-LLM not available (compiled without TENSORRT_LLM_AVAILABLE)";
}

std::string TensorRTBackend::Generate(const std::string& /*prompt*/,
                                       const std::string& /*context*/,
                                       std::atomic<bool>* /*cancel*/) {
  return "";
}

std::string TensorRTBackend::Chat(const std::string& /*system_prompt*/,
                                   const std::string& /*user_text*/,
                                   const std::vector<ChatMessage>& /*history_msgs*/,
                                   const std::string& /*skill_context*/,
                                   std::atomic<bool>* /*cancel*/) {
  return "";
}

std::string TensorRTBackend::ChatStream(const std::string& /*system_prompt*/,
                                         const std::string& /*user_text*/,
                                         const std::vector<ChatMessage>& /*history_msgs*/,
                                         const std::string& /*skill_context*/,
                                         LlmTokenCallback /*on_token*/,
                                         std::atomic<bool>* /*cancel*/) {
  return "";
}

#endif // TENSORRT_LLM_AVAILABLE

} // namespace rtsp_server
