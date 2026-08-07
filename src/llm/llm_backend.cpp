/**
 * LLM Backend factory — creates the correct backend based on config.
 *
 * Fallback strategy:
 *   TensorRT requested → try TensorRT → on failure, auto-fallback to Ollama
 *   Ollama requested   → try Ollama   → on failure, return nullptr
 */

#include "llm/llm_backend.h"
#include "llm/ollama_backend.h"
#include "llm/tensorrt_backend.h"

#include "utils/logger.h"

namespace rtsp_server {

std::shared_ptr<LLMBackend> CreateLlmBackend(const LlmBackendConfig& cfg) {
  switch (cfg.type) {

    case LlmBackendType::kOllama: {
      LOG_INFO("[LLM] creating Ollama backend — host={} model={}",
               cfg.ollama_host, cfg.model);
      auto backend = std::make_shared<OllamaBackend>(
          cfg.ollama_host, cfg.model, cfg.timeout_sec);

      if (backend->IsAvailable()) {
        return backend;
      }

      LOG_ERROR("[LLM] Ollama backend created but not available");
      return nullptr;
    }

    case LlmBackendType::kTensorRT: {
      LOG_INFO("[LLM] creating TensorRT backend — engine={} tokenizer={}",
               cfg.trt_engine_path, cfg.trt_tokenizer_path);
      auto trt_backend = std::make_shared<TensorRTBackend>(
          cfg.trt_engine_path, cfg.trt_tokenizer_path, cfg.timeout_sec);

      if (trt_backend->IsAvailable()) {
        return trt_backend;
      }

      // TensorRT not available — automatically fall back to Ollama
      LOG_WARN("[LLM] TensorRT backend not available, automatically falling back to Ollama");

      if (cfg.ollama_host.empty()) {
        LOG_ERROR("[LLM] fallback failed: no ollama_host configured");
        return nullptr;
      }

      auto ollama_backend = std::make_shared<OllamaBackend>(
          cfg.ollama_host, cfg.model, cfg.timeout_sec);

      if (ollama_backend->IsAvailable()) {
        LOG_INFO("[LLM] fallback to Ollama OK — host={} model={}",
                 cfg.ollama_host, cfg.model);
        return ollama_backend;
      }

      LOG_ERROR("[LLM] fallback to Ollama also failed");
      return nullptr;
    }
  }

  LOG_ERROR("[LLM] unknown backend type");
  return nullptr;
}

} // namespace rtsp_server
