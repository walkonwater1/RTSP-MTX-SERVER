#include "pipeline/pipeline_bridge.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "utils/logger.h"

using json = nlohmann::json;

namespace rtsp_server {

// ---------------------------------------------------------------------------
// libcurl helper
// ---------------------------------------------------------------------------

namespace {

size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
  size_t total = size * nmemb;
  userp->append(static_cast<char*>(contents), total);
  return total;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PipelineBridge::PipelineBridge(const PipelineBridgeConfig& cfg)
    : cfg_(cfg) {
  curl_global_init(CURL_GLOBAL_ALL);
}

PipelineBridge::~PipelineBridge() {
  curl_global_cleanup();
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

bool PipelineBridge::Initialize() {
  LOG_INFO("[Pipeline] initializing...");

  // Create TTS cache directory
  if (cfg_.tts_cache_enabled && !cfg_.tts_cache_dir.empty()) {
    std::ostringstream cmd;
    cmd << "mkdir -p " << cfg_.tts_cache_dir;
    system(cmd.str().c_str());
  }

  // Create debug dump dir
  system("mkdir -p /tmp/rtsp-server/debug");

#ifdef HAS_VOICE_PIPELINE
  LOG_INFO("[Pipeline] full voice pipeline mode");
  // TODO: Initialize sherpa-onnx ASR, Piper TTS, etc.
  // When linked against the real libraries:
  //   asr_.Initialize(cfg_.asr_model_path);
  //   tts_.Initialize(cfg_.tts_piper_model);
#else
  LOG_INFO("[Pipeline] stub mode (LLM-only via Ollama HTTP API)");
#endif

  // Test LLM connectivity
  {
    std::string test_result = CallLlm("ping");
    if (test_result.empty() || test_result.find("error") != std::string::npos) {
      LOG_WARN("[Pipeline] LLM connectivity test failed: {}", test_result);
      // Don't fail — LLM might come up later
    } else {
      LOG_INFO("[Pipeline] LLM connectivity OK");
    }
  }

  ready_.store(true);
  LOG_INFO("[Pipeline] initialized");
  return true;
}

// ---------------------------------------------------------------------------
// Process Text (LLM → TTS)
// ---------------------------------------------------------------------------

PipelineResult PipelineBridge::ProcessText(const std::string& text,
                                            const std::string& session_id) {
  PipelineResult result;

  if (!ready_.load()) {
    result.error = "pipeline not initialized";
    return result;
  }

  LOG_INFO("[Pipeline] processing text: \"{}\"", text);

  // 1. LLM
  std::string system_prompt = cfg_.llm_system_prompt;
  if (system_prompt.empty()) {
    system_prompt = "你是小千，一个18岁女大学生，性格活泼开朗。回复控制在2-3句话，约50字。";
  }

  std::string prompt = system_prompt + "\n\n用户: " + text + "\n助手:";
  result.llm_response = CallLlm(prompt);

  if (result.llm_response.empty()) {
    result.error = "LLM returned empty response";
    LOG_WARN("[Pipeline] LLM empty response for: {}", text);
    return result;
  }

  LOG_INFO("[Pipeline] LLM response: \"{}\"", result.llm_response);

  // 2. TTS
  std::string wav_path;
  if (!session_id.empty()) {
    wav_path = "/tmp/rtsp-server/tts_" + session_id + "_" +
               std::to_string(
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count()) +
               ".wav";
  } else {
    wav_path = "/tmp/rtsp-server/tts_output.wav";
  }

  if (SynthesizeTts(result.llm_response, wav_path, session_id)) {
    result.tts_audio_path = wav_path;
  }

  result.asr_text = text;
  result.ok = true;
  return result;
}

// ---------------------------------------------------------------------------
// Process Audio (ASR → LLM → TTS)
// ---------------------------------------------------------------------------

PipelineResult PipelineBridge::ProcessAudio(const int16_t* pcm_data,
                                              int sample_count,
                                              const std::string& session_id) {
  PipelineResult result;

  if (!ready_.load()) {
    result.error = "pipeline not initialized";
    return result;
  }

  // 1. ASR: write PCM to WAV file and call transcribe
  std::string wav_path = "/tmp/rtsp-server/asr_input_" +
      (session_id.empty() ? "default" : session_id) + ".wav";

  // Write minimal WAV header
  {
    std::ofstream f(wav_path, std::ios::binary);
    if (!f) {
      result.error = "failed to create WAV file for ASR";
      return result;
    }

    uint32_t data_size = sample_count * sizeof(int16_t);
    uint32_t file_size = 36 + data_size;

    // RIFF header
    f.write("RIFF", 4);
    f.write(reinterpret_cast<const char*>(&file_size), 4);
    f.write("WAVE", 4);

    // fmt chunk
    f.write("fmt ", 4);
    uint32_t fmt_size = 16;
    uint16_t audio_fmt = 1;  // PCM
    uint16_t num_ch = 1;
    uint32_t sample_rate = 16000;
    uint32_t byte_rate = 16000 * 1 * 2;
    uint16_t block_align = 2;
    uint16_t bps = 16;

    f.write(reinterpret_cast<const char*>(&fmt_size), 4);
    f.write(reinterpret_cast<const char*>(&audio_fmt), 2);
    f.write(reinterpret_cast<const char*>(&num_ch), 2);
    f.write(reinterpret_cast<const char*>(&sample_rate), 4);
    f.write(reinterpret_cast<const char*>(&byte_rate), 4);
    f.write(reinterpret_cast<const char*>(&block_align), 2);
    f.write(reinterpret_cast<const char*>(&bps), 2);

    // data chunk
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&data_size), 4);
    f.write(reinterpret_cast<const char*>(pcm_data), data_size);
  }

  result.asr_text = TranscribeAudio(pcm_data, sample_count);

  if (result.asr_text.empty()) {
    result.error = "ASR produced no text";
    return result;
  }

  LOG_INFO("[Pipeline] ASR result: \"{}\"", result.asr_text);

  // 2. LLM + TTS
  auto llm_result = ProcessText(result.asr_text, session_id);
  result.llm_response = llm_result.llm_response;
  result.tts_audio_path = llm_result.tts_audio_path;
  result.ok = !llm_result.llm_response.empty();

  return result;
}

// ---------------------------------------------------------------------------
// ASR Transcription (stub: uses system whisper or returns empty)
// ---------------------------------------------------------------------------

std::string PipelineBridge::TranscribeAudio(const int16_t* pcm_data,
                                              int sample_count) {
  // In stub mode, ASR is not available — return a test prompt to exercise LLM.
  // When linked with HAS_VOICE_PIPELINE, this would call sherpa-onnx.
#ifdef HAS_VOICE_PIPELINE
  (void)pcm_data;
  (void)sample_count;
  return "";
#else
  if (sample_count < 16000) {
    LOG_DEBUG("[Pipeline] TranscribeAudio: too few samples for stub ({}), skipping", sample_count);
    return "";
  }
  LOG_INFO("[Pipeline] TranscribeAudio: stub mode, returning mock ASR text ({} samples)", sample_count);
  return "你好，今天天气怎么样";
#endif
}

// ---------------------------------------------------------------------------
// TTS Synthesis
// ---------------------------------------------------------------------------

bool PipelineBridge::SynthesizeTts(const std::string& text,
                                    const std::string& output_path,
                                    const std::string& session_id) {
  // Check cache
  if (cfg_.tts_cache_enabled) {
    std::string cached = GetCachePath(text);
    std::ifstream check(cached);
    if (check.good()) {
      check.close();
      // Copy from cache
      std::ostringstream cmd;
      cmd << "cp " << cached << " " << output_path;
      if (system(cmd.str().c_str()) == 0) {
        LOG_INFO("[TTS] cache hit: \"{}\"", text);
        return true;
      }
    }
  }

  // Generate TTS using espeak-ng (simple approach for stub mode)
  // When linked with HAS_VOICE_PIPELINE, this would use Piper.
  std::ostringstream cmd;

#ifdef HAS_VOICE_PIPELINE
  // Piper TTS call
  cmd << "echo '" << EscapeJson(text) << "' | "
      << "piper --model " << cfg_.tts_piper_model
      << " --output_file " << output_path;
#else
  // Fallback: espeak-ng stdout → ffmpeg resample to 16kHz → WAV
  cmd << "espeak-ng -v " << cfg_.tts_voice
      << " -s " << cfg_.tts_rate
      << " --stdout"
      << " \"" << EscapeJson(text) << "\""
      << " 2>/dev/null"
      << " | ffmpeg -f s16le -ar 22050 -ac 1 -i pipe:0"
      << " -ar 16000 -ac 1"
      << " -y " << output_path
      << " 2>/dev/null";

  // Check if espeak-ng is available, otherwise create a minimal WAV
  if (system("which espeak-ng >/dev/null 2>&1") != 0) {
    LOG_WARN("[TTS] espeak-ng not found, creating empty WAV placeholder");
    // Create a minimal silent WAV
    std::ofstream f(output_path, std::ios::binary);
    if (f) {
      uint32_t data_size = 16000;  // 0.5s @ 16kHz
      uint32_t file_size = 36 + data_size;
      f.write("RIFF", 4);
      f.write(reinterpret_cast<const char*>(&file_size), 4);
      f.write("WAVE", 4);
      f.write("fmt ", 4);
      uint32_t fmt_size = 16;
      uint16_t audio_fmt = 1, num_ch = 1, block_align = 2, bps = 16;
      uint32_t sample_rate = 16000, byte_rate = 32000;
      f.write(reinterpret_cast<const char*>(&fmt_size), 4);
      f.write(reinterpret_cast<const char*>(&audio_fmt), 2);
      f.write(reinterpret_cast<const char*>(&num_ch), 2);
      f.write(reinterpret_cast<const char*>(&sample_rate), 4);
      f.write(reinterpret_cast<const char*>(&byte_rate), 4);
      f.write(reinterpret_cast<const char*>(&block_align), 2);
      f.write(reinterpret_cast<const char*>(&bps), 2);
      f.write("data", 4);
      f.write(reinterpret_cast<const char*>(&data_size), 4);
      std::vector<char> silence(data_size, 0);
      f.write(silence.data(), data_size);
    }
    return true;
  }
#endif

  int ret = system(cmd.str().c_str());
  if (ret != 0) {
    LOG_ERROR("[TTS] synthesis failed (exit={}) for: \"{}\"", ret, text);
    return false;
  }

  // Cache the result
  if (cfg_.tts_cache_enabled) {
    std::string cached = GetCachePath(text);
    std::ostringstream cp_cmd;
    cp_cmd << "cp " << output_path << " " << cached;
    system(cp_cmd.str().c_str());
  }

  LOG_INFO("[TTS] synthesized: \"{}\" → {}", text, output_path);
  return true;
}

// ---------------------------------------------------------------------------
// LLM Call (Ollama HTTP API)
// ---------------------------------------------------------------------------

std::string PipelineBridge::CallLlm(const std::string& prompt) {
  if (prompt == "ping") {
    // Just check connectivity
    CURL* curl = curl_easy_init();
    if (!curl) return "error: curl_init failed";

    std::string url = cfg_.llm_host + "/api/tags";
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

  // Actual LLM inference
  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string url = cfg_.llm_host + "/api/generate";

  json body;
  body["model"] = cfg_.llm_model;
  body["prompt"] = prompt;
  body["stream"] = false;
  body["options"]["temperature"] = 0.7;
  body["options"]["num_predict"] = 256;

  std::string body_str = body.dump();

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.llm_timeout_sec));

  std::string response;
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    LOG_ERROR("[LLM] curl error: {}", curl_easy_strerror(res));
    return "";
  }

  // Parse Ollama response
  try {
    auto j = json::parse(response);
    std::string reply = j.value("response", "");
    // Trim whitespace
    auto start = reply.find_first_not_of(" \t\n\r");
    auto end = reply.find_last_not_of(" \t\n\r");
    if (start != std::string::npos) {
      reply = reply.substr(start, end - start + 1);
    }
    return reply;
  } catch (const json::exception& e) {
    LOG_ERROR("[LLM] JSON parse error: {}", e.what());
    return "";
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string PipelineBridge::EscapeJson(const std::string& s) {
  std::string out;
  out.reserve(s.size());
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

std::string PipelineBridge::GetCachePath(const std::string& text) {
  // Simple hash: sum of chars
  uint32_t hash = 5381;
  for (char c : text) hash = ((hash << 5) + hash) + static_cast<uint8_t>(c);

  std::ostringstream path;
  path << cfg_.tts_cache_dir << "/tts_" << std::hex << hash << ".wav";
  return path.str();
}

void PipelineBridge::ReloadConfig(const PipelineBridgeConfig& cfg) {
  cfg_ = cfg;
  LOG_INFO("[Pipeline] config reloaded");
}

} // namespace rtsp_server
