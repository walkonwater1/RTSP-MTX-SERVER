#pragma once
/**
 * Simple spdlog wrapper — consistent with ASR-LLM-TTS logging style.
 */
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <memory>
#include <string>

namespace rtsp_server {

inline std::shared_ptr<spdlog::logger> g_logger;

inline void init_logger(const std::string& log_file = "rtsp_server.log",
                        const std::string& level = "info") {
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, true);
  file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

  spdlog::sinks_init_list sinks = {console_sink, file_sink};
  g_logger = std::make_shared<spdlog::logger>("rtsp-server", sinks);

  auto lvl = spdlog::level::from_str(level);
  g_logger->set_level(lvl);
  g_logger->flush_on(spdlog::level::info);
  spdlog::set_default_logger(g_logger);
}

} // namespace rtsp_server

// Convenience macros
#define LOG_TRACE(...)    spdlog::trace(__VA_ARGS__)
#define LOG_DEBUG(...)    spdlog::debug(__VA_ARGS__)
#define LOG_INFO(...)     spdlog::info(__VA_ARGS__)
#define LOG_WARN(...)     spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...)    spdlog::error(__VA_ARGS__)
#define LOG_CRITICAL(...) spdlog::critical(__VA_ARGS__)
