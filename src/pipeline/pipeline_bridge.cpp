#include "pipeline/pipeline_bridge.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unistd.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "llm/function_caller.h"
#include "brain/skills/skill_memory.h"
#include "utils/logger.h"

using json = nlohmann::json;

namespace rtsp_server {

// ---------------------------------------------------------------------------
// CTC dedup: collapse consecutive identical CJK characters
// ---------------------------------------------------------------------------

static std::string CtcDedup(const std::string& text) {
  if (text.empty()) return text;
  std::string result;
  result.reserve(text.size());
  // Helper: check if a UTF-8 byte starts a multi-byte CJK character
  auto is_cjk_lead = [](unsigned char c) -> bool {
    return (c >= 0xE4 && c <= 0xE9);  // U+4E00 ~ U+9FFF first byte range
  };
  auto char_len = [](unsigned char c) -> int {
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
  };

  std::string prev_char;
  for (size_t i = 0; i < text.size(); ) {
    int clen = char_len(static_cast<unsigned char>(text[i]));
    if (i + clen > text.size()) clen = 1;
    std::string cur = text.substr(i, clen);

    // Only dedup CJK characters; keep ASCII/other as-is
    if (clen == 3 && is_cjk_lead(static_cast<unsigned char>(text[i]))) {
      if (cur != prev_char) {
        result += cur;
        prev_char = cur;
      }
      // else: skip duplicate
    } else {
      result += cur;
      prev_char.clear();  // reset for non-CJK
    }
    i += clen;
  }
  return result;
}

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

  // Cache piper availability (avoid forking shell every TTS call)
  piper_available_ = (system("which piper >/dev/null 2>&1") == 0);
  LOG_INFO("[Pipeline] piper available: {}", piper_available_);

  // Create debug dump dir
  system("mkdir -p /tmp/rtsp-server/debug");

  // Create memory persist dir
  if (cfg_.memory_enabled && !cfg_.memory_persist_dir.empty()) {
    std::ostringstream cmd;
    cmd << "mkdir -p " << cfg_.memory_persist_dir;
    system(cmd.str().c_str());
  }

  // --- Set up Function Calling ---
  if (cfg_.skills_enabled && cfg_.function_calling_enabled) {
    std::string fc_model = cfg_.fc_model.empty() ? cfg_.llm_model : cfg_.fc_model;
    function_caller_ = std::make_shared<FunctionCaller>(cfg_.llm_host, fc_model);
    skill_mgr_.set_function_caller(function_caller_);
    skill_mgr_.set_function_calling_enabled(true);
    LOG_INFO("[Pipeline] function calling enabled (model={})", fc_model);
  } else {
    skill_mgr_.set_function_calling_enabled(false);
    LOG_INFO("[Pipeline] function calling disabled");
  }

  // --- Set up Memory ---
  skill_mgr_.set_memory_store(&user_memory_);

  // Add MemorySkill now that memory store is available
  skill_mgr_.add_skill(std::make_unique<MemorySkill>(&user_memory_));

  // Load long-term memory from disk
  if (cfg_.memory_enabled && !cfg_.memory_persist_dir.empty()) {
    std::string mem_path = cfg_.memory_persist_dir + "/user_memory.json";
    user_memory_.load_from_file(mem_path);
  }

#ifdef HAS_VOICE_PIPELINE
  LOG_INFO("[Pipeline] full voice pipeline mode");
#else
  LOG_INFO("[Pipeline] stub mode (LLM + Skills via Ollama HTTP API)");
#endif

  // Test LLM connectivity & warm up model (preload into GPU memory)
  {
    std::string test_result = CallLlm("ping");
    if (test_result.empty() || test_result.find("error") != std::string::npos) {
      LOG_WARN("[Pipeline] LLM connectivity test failed: {}", test_result);
    } else {
      LOG_INFO("[Pipeline] LLM connectivity OK");
    }

    // Warm-up: send a tiny inference to load model into GPU memory.
    // Without this, the first real query pays a 1.6s+ cold-start penalty.
    auto t_warm = std::chrono::steady_clock::now();
    std::string warm_result = CallLlm("hi");
    auto t_warm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_warm).count();
    LOG_INFO("[Pipeline] model warm-up complete ({}ms)", t_warm_ms);
  }

  ready_.store(true);
  LOG_INFO("[Pipeline] initialized ({} skills registered)", 9);
  return true;
}

// ---------------------------------------------------------------------------
// Process Text (Skills → LLM with Memory → TTS)
// ---------------------------------------------------------------------------

PipelineResult PipelineBridge::ProcessText(const std::string& text,
                                            const std::string& session_id) {
  PipelineResult result;
  auto t_total_start = std::chrono::steady_clock::now();

  if (!ready_.load()) {
    result.error = "pipeline not initialized";
    return result;
  }

  LOG_INFO("[Pipeline] processing text: \"{}\"", text);

  // ── 1. Try Skill System ────────────────────────────────
  auto t_skill_start = std::chrono::steady_clock::now();
  SkillResult sr;  // declared here so accessible for LLM context injection below
  if (cfg_.skills_enabled) {
    sr = skill_mgr_.detect_and_execute(text);

    if (sr.hit) {
      auto t_skill_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t_skill_start).count();
      LOG_INFO("[Pipeline] skill '{}' handled request ({}ms)", sr.skill_name, t_skill_ms);

      if (sr.direct) {
        // Direct response: skill result IS the final reply (skip LLM)
        result.asr_text = text;
        result.llm_response = sr.result_text;
        result.skill_direct = true;

        // Still synthesize TTS for direct responses
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
        result.ok = true;
        return result;
      }

      // Non-direct: inject skill result as context for LLM
      LOG_INFO("[Pipeline] skill result injected as LLM context");
      // Fall through to LLM with the skill result as extra context
    }
  }

  // ── 2. Get conversation history ────────────────────────
  std::string history_context;
  if (cfg_.memory_enabled && !session_id.empty()) {
    ChatMemory* mem = GetSessionMemory(session_id);
    if (mem) {
      history_context = mem->get_context();
    }
  }

  // ── 3. Build system prompt ─────────────────────────────
  std::string system_prompt = cfg_.llm_system_prompt;
  if (system_prompt.empty()) {
    system_prompt = "你是小千，一个18岁女大学生，性格活泼开朗。回复控制在2-3句话，约50字。";
  }

  // Add system context (time, etc.)
  std::string sys_ctx = SkillManager::get_system_context();
  if (!sys_ctx.empty()) {
    system_prompt = system_prompt + "\n\n" + sys_ctx;
  }

  // ── 4. Call LLM with conversation context ──────────────
  auto t_llm_start = std::chrono::steady_clock::now();

  // Inject skill result into LLM context (previously was logged but not passed!)
  std::string skill_context;
  if (sr.hit && !sr.direct && !sr.result_text.empty()) {
    skill_context = sr.result_text;
  }

  // LLM response cache: skip LLM call for frequent static queries ("你好", etc.)
  // Only cache non-skill, non-history queries where the response is deterministic.
  if (!sr.hit && history_context.empty() && skill_context.empty()) {
    size_t cache_key = std::hash<std::string>{}(text);
    {
      std::lock_guard<std::mutex> lock(llm_cache_mutex_);
      auto it = llm_cache_.find(cache_key);
      if (it != llm_cache_.end()) {
        result.llm_response = it->second;
        LOG_INFO("[Pipeline] LLM cache hit: \"{}\"", text);
        // Fall through to TTS (skip LLM call below)
      }
    }
  }

  if (result.llm_response.empty()) {
    result.llm_response = CallLlmChat(system_prompt, text, history_context, skill_context);

    // Cache the result for future use (only simple queries)
    if (!sr.hit && history_context.empty() && skill_context.empty() && !result.llm_response.empty()) {
      size_t cache_key = std::hash<std::string>{}(text);
      std::lock_guard<std::mutex> lock(llm_cache_mutex_);
      if (llm_cache_.size() >= kMaxLlmCacheEntries) {
        llm_cache_.clear();  // simple LRU: clear all when full
      }
      llm_cache_[cache_key] = result.llm_response;
    }
  }
  auto t_llm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t_llm_start).count();

  if (result.llm_response.empty()) {
    result.error = "LLM returned empty response";
    LOG_WARN("[Pipeline] LLM empty response for: {}", text);
    return result;
  }

  LOG_INFO("[Pipeline] LLM response ({}ms): \"{}\"", t_llm_ms, result.llm_response);

  // ── 5. Update conversation memory ──────────────────────
  if (cfg_.memory_enabled && !session_id.empty()) {
    ChatMemory* mem = GetSessionMemory(session_id);
    if (mem) {
      mem->add(text, result.llm_response);
    }
  }

  // ── 6. TTS ─────────────────────────────────────────────
  auto t_tts_start = std::chrono::steady_clock::now();
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
  auto t_tts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t_tts_start).count();

  auto t_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t_total_start).count();

  result.asr_text = text;
  result.ok = true;
  LOG_INFO("[Timing] LLM={}ms TTS={}ms TOTAL={}ms", t_llm_ms, t_tts_ms, t_total_ms);
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

  // 2. LLM + TTS (via ProcessText which now has skills + memory)
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
  if (sample_count < 8000) {
    LOG_DEBUG("[Pipeline] TranscribeAudio: too few samples ({}), skipping", sample_count);
    return "";
  }

  // Calculate RMS energy — skip if too quiet (echo/silence)
  double sum_sq = 0;
  for (int i = 0; i < sample_count; i++) {
    double s = pcm_data[i] / 32768.0;
    sum_sq += s * s;
  }
  double rms = std::sqrt(sum_sq / sample_count);
  if (rms < 0.005) {
    LOG_INFO("[ASR] audio too quiet (RMS={:.4f}), skipping ASR to avoid hallucination", rms);
    return "";
  }

  // Write PCM to WAV file
  std::string wav_path = "/tmp/rtsp-server/asr_latest.wav";
  {
    std::ofstream f(wav_path, std::ios::binary);
    if (!f) return "";

    uint32_t data_size = sample_count * sizeof(int16_t);
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
    f.write(reinterpret_cast<const char*>(pcm_data), data_size);
  }

  // Try real ASR via sherpa-onnx-offline
  static const char* kSherpaBin =
      "/eir/lixin/ASR-LLM-TTS/src/third_party/sherpa-onnx/bin/sherpa-onnx-offline";
  static const char* kModelDir =
      "/eir/lixin/ASR-LLM-TTS/src/third_party/sherpa-onnx/zipformer-ctc-zh";
  static const char* kSherpaLib =
      "/eir/lixin/ASR-LLM-TTS/src/third_party/sherpa-onnx/lib";

  // Check if sherpa-onnx binary exists
  static bool sherpa_available = (access(kSherpaBin, X_OK) == 0);

  if (sherpa_available) {
    auto t_asr_start = std::chrono::steady_clock::now();
    std::ostringstream cmd;
    cmd << "LD_LIBRARY_PATH=" << kSherpaLib << ":$LD_LIBRARY_PATH "
        << kSherpaBin
        << " --tokens=" << kModelDir << "/tokens.txt"
        << " --zipformer-ctc-model=" << kModelDir << "/model.int8.onnx"
        << " --model-type=zipformer_ctc"
        << " --num-threads=2"
        << " --decoding-method=greedy_search"
        << " " << wav_path
        << " 2>/dev/null";

    std::string output;
    FILE* p = popen(cmd.str().c_str(), "r");
    if (p) {
      char buf[4096];
      while (fgets(buf, sizeof(buf), p)) {
        output += buf;
      }
      pclose(p);
    }

    // Parse JSON output: extract "text" field
    if (!output.empty()) {
      try {
        // Find the last JSON line (sherpa-onnx outputs config + result lines)
        auto j = nlohmann::json::parse(output);
        std::string text = j.value("text", "");
        if (!text.empty()) {
          // Trim
          auto start = text.find_first_not_of(" \t\n\r");
          auto end = text.find_last_not_of(" \t\n\r");
          if (start != std::string::npos) {
            text = text.substr(start, end - start + 1);
          }
          if (!text.empty()) {
            text = CtcDedup(text);
            auto t_asr_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_asr_start).count();
            LOG_INFO("[ASR] recognized: \"{}\" ({} samples, {:.1f}s, ASR={}ms, RMS={:.4f})",
                     text, sample_count, sample_count / 16000.0f, t_asr_total_ms, rms);
            return text;
          }
        }
      } catch (...) {
        // JSON parse failed — fall through to empty check
      }

      // If JSON parsing failed but we got output, try to extract text manually
      // sherpa-onnx output format has the last line as JSON
      size_t last_brace = output.rfind('{');
      if (last_brace != std::string::npos) {
        try {
          auto j = nlohmann::json::parse(output.substr(last_brace));
          std::string text = j.value("text", "");
          if (!text.empty()) {
            auto start = text.find_first_not_of(" \t\n\r");
            auto end = text.find_last_not_of(" \t\n\r");
            if (start != std::string::npos) {
              text = text.substr(start, end - start + 1);
            }
            if (!text.empty()) {
              text = CtcDedup(text);
              LOG_INFO("[ASR] recognized: \"{}\" (RMS={:.4f})", text, rms);
              return text;
            }
          }
        } catch (...) {}
      }
    }

    LOG_DEBUG("[ASR] sherpa-onnx returned empty or no speech detected ({} samples)", sample_count);
  }

  // No real ASR available or no speech detected — return empty
  LOG_DEBUG("[ASR] no speech recognized ({} samples, {:.1f}s audio)",
            sample_count, sample_count / 16000.0f);
  return "";
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
    std::ifstream check(cached, std::ios::binary);
    if (check.good()) {
      check.close();
      // Copy from cache via file streams (avoids shell fork)
      std::ifstream src(cached, std::ios::binary);
      std::ofstream dst(output_path, std::ios::binary);
      if (src && dst) {
        dst << src.rdbuf();
        LOG_INFO("[TTS] cache hit: \"{}\"", text);
        return true;
      }
    }
  }

  // Generate TTS using the configured backend.
  // When linked with HAS_VOICE_PIPELINE, this uses the native Piper library.
  std::ostringstream cmd;

#ifdef HAS_VOICE_PIPELINE
  // Native Piper TTS
  cmd << "echo '" << EscapeJson(text) << "' | "
      << "piper --model " << cfg_.tts_piper_model
      << " --output_file " << output_path;
#else
  // Stub mode: use external Piper or espeak-ng depending on config
  if (cfg_.tts_backend == "piper" && !cfg_.tts_piper_model.empty()) {
    if (piper_available_) {
      // Piper model sample rate (huayan = 22050)
      // Pipe through ffmpeg to resample to 16kHz for robot compatibility.
      cmd << "echo " << std::quoted(text) << " | "
          << "piper --model " << cfg_.tts_piper_model
          << " --output-raw 2>/dev/null"
          << " | ffmpeg -f s16le -ar 22050 -ac 1 -i pipe:0"
          << " -ar " << cfg_.tts_sample_rate << " -ac 1"
          << " -y " << output_path
          << " 2>/dev/null";
    } else {
      LOG_WARN("[TTS] piper requested but not installed, falling back to espeak-ng");
    }
  }

  // Fallback to espeak-ng
  if (cmd.str().empty()) {
    cmd << "espeak-ng -v " << cfg_.tts_voice
        << " -s " << cfg_.tts_rate
        << " --stdout"
        << " \"" << EscapeJson(text) << "\""
        << " 2>/dev/null"
        << " | ffmpeg -f s16le -ar 22050 -ac 1 -i pipe:0"
        << " -ar " << cfg_.tts_sample_rate << " -ac 1"
        << " -y " << output_path
        << " 2>/dev/null";

    // Check if espeak-ng is available
    if (system("which espeak-ng >/dev/null 2>&1") != 0) {
      LOG_WARN("[TTS] espeak-ng not found, creating empty WAV placeholder");
      // Create a minimal silent WAV
      std::ofstream f(output_path, std::ios::binary);
      if (f) {
        uint32_t data_size = 16000;
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
    std::ifstream src(output_path, std::ios::binary);
    std::ofstream dst(cached, std::ios::binary);
    if (src && dst) {
      dst << src.rdbuf();
    }
  }

  LOG_INFO("[TTS] synthesized: \"{}\" → {}", text, output_path);
  return true;
}

// ---------------------------------------------------------------------------
// LLM Call (Ollama /api/generate — used for ping and simple prompts)
// ---------------------------------------------------------------------------

std::string PipelineBridge::CallLlm(const std::string& prompt,
                                    const std::string& context) {
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
  body["keep_alive"] = -1;  // keep model loaded in Ollama memory

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
// LLM Call via /api/chat (supports conversation history as messages)
// ---------------------------------------------------------------------------

std::string PipelineBridge::CallLlmChat(const std::string& system_prompt,
                                         const std::string& user_text,
                                         const std::string& history_context,
                                         const std::string& skill_context) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string url = cfg_.llm_host + "/api/chat";

  // Build messages array
  json messages = json::array();

  // System message
  messages.push_back({
      {"role", "system"},
      {"content", system_prompt}
  });

  // Skill context (tool result injected for LLM to use)
  if (!skill_context.empty()) {
    messages.push_back({
        {"role", "system"},
        {"content", "[工具返回结果]\n" + skill_context + "\n\n请严格根据以上工具返回的事实信息回答用户问题，不要凭训练数据猜测。"}
    });
  }

  // History context (if any)
  if (!history_context.empty()) {
    messages.push_back({
        {"role", "user"},
        {"content", "(以下为历史对话记录)\n" + history_context + "\n请基于以上历史对话继续回复。"}
    });
  }

  // Current user message
  messages.push_back({
      {"role", "user"},
      {"content", user_text}
  });

  json body;
  body["model"] = cfg_.llm_model;
  body["messages"] = messages;
  body["stream"] = false;
  body["options"]["temperature"] = 0.7;
  body["options"]["num_predict"] = 256;
  body["keep_alive"] = -1;  // keep model loaded in Ollama memory

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

  // Parse Ollama /api/chat response
  try {
    auto j = json::parse(response);
    std::string reply = j.value("message", json::object()).value("content", "");
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
// Session Memory
// ---------------------------------------------------------------------------

ChatMemory* PipelineBridge::GetSessionMemory(const std::string& session_id) {
  if (session_id.empty()) return nullptr;

  std::lock_guard<std::mutex> lock(memory_mutex_);

  auto it = session_memories_.find(session_id);
  if (it == session_memories_.end()) {
    // Create new session memory
    auto [inserted_it, ok] = session_memories_.emplace(
        session_id,
        ChatMemory(cfg_.memory_max_rounds, cfg_.memory_max_tokens));

    // Try loading from disk
    if (!cfg_.memory_persist_dir.empty()) {
      std::string path = cfg_.memory_persist_dir + "/chat_" + session_id + ".json";
      inserted_it->second.load_from_file(path);
    }

    return &inserted_it->second;
  }

  return &it->second;
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
