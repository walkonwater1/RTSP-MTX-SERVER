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

// Direct sherpa-onnx C API (avoids popen overhead)
#ifdef SHERPA_ONNX_AVAILABLE
#include "sherpa-onnx/c-api/c-api.h"
#endif

// Direct espeak-ng library (avoids system() overhead)
#ifdef ESPEAK_NG_AVAILABLE
#include "speech/espeak_min.h"
#endif

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
// Direct TTS helpers (espeak-ng callback + WAV writer)
// ---------------------------------------------------------------------------

#ifdef ESPEAK_NG_AVAILABLE
// Thread-local audio buffer for espeak callback (not thread-safe, but TTS
// calls are serialized by the pipeline design).
static std::vector<int16_t> g_tts_audio;

static int tts_audio_callback(short* wav, int numsamples, espeak_EVENT* /*events*/) {
  if (wav && numsamples > 0) {
    g_tts_audio.insert(g_tts_audio.end(), wav, wav + numsamples);
  }
  return 0;
}
#endif

// ---------------------------------------------------------------------------
// WriteWavFile — write int16 PCM as WAV (used by direct espeak TTS)
// ---------------------------------------------------------------------------

bool PipelineBridge::WriteWavFile(const std::string& path,
                                   const std::vector<int16_t>& samples,
                                   int sample_rate,
                                   int num_channels) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;

  uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
  uint32_t chunk_size = 36 + data_size;
  uint16_t bits_per_sample = 16;
  uint32_t byte_rate = static_cast<uint32_t>(sample_rate) * num_channels * bits_per_sample / 8;
  uint16_t block_align = static_cast<uint16_t>(num_channels * bits_per_sample / 8);

  out.write("RIFF", 4);
  out.write(reinterpret_cast<const char*>(&chunk_size), 4);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  uint32_t fmt_size = 16;
  uint16_t audio_format = 1;
  out.write(reinterpret_cast<const char*>(&fmt_size), 4);
  out.write(reinterpret_cast<const char*>(&audio_format), 2);
  out.write(reinterpret_cast<const char*>(&num_channels), 2);
  out.write(reinterpret_cast<const char*>(&sample_rate), 4);
  out.write(reinterpret_cast<const char*>(&byte_rate), 4);
  out.write(reinterpret_cast<const char*>(&block_align), 2);
  out.write(reinterpret_cast<const char*>(&bits_per_sample), 2);
  out.write("data", 4);
  out.write(reinterpret_cast<const char*>(&data_size), 4);
  out.write(reinterpret_cast<const char*>(samples.data()), data_size);
  return true;
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

// CURL progress callback — checks cancellation flag and aborts transfer.
// Returns non-zero to abort. Signature: int(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
int CurlProgressCallback(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                         curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  if (clientp) {
    auto* cancel = static_cast<std::atomic<bool>*>(clientp);
    if (cancel->load()) {
      return 1;  // non-zero aborts the transfer
    }
  }
  return 0;
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
#ifdef SHERPA_ONNX_AVAILABLE
  if (asr_recognizer_) {
    SherpaOnnxDestroyOfflineRecognizer(asr_recognizer_);
    asr_recognizer_ = nullptr;
  }
#endif
#ifdef ESPEAK_NG_AVAILABLE
  if (espeak_initialized_) {
    espeak_Terminate();
    espeak_initialized_ = false;
  }
#endif
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
  LOG_DEBUG("[Pipeline] piper available: {}", piper_available_);

  // Create runtime directories
  system("mkdir -p /tmp/rtsp-server/debug");
  system("mkdir -p /dev/shm/rtsp-server");

  // ── Initialize direct sherpa-onnx ASR (avoid popen) ──
#ifdef SHERPA_ONNX_AVAILABLE
  {
    std::string model_dir = cfg_.asr_model_path.empty()
        ? "/eir/lixin/ASR-LLM-TTS/src/third_party/sherpa-onnx/zipformer-ctc-zh"
        : cfg_.asr_model_path;

    SherpaOnnxOfflineRecognizerConfig asr_cfg;
    memset(&asr_cfg, 0, sizeof(asr_cfg));
    asr_cfg.feat_config.sample_rate = 16000;
    asr_cfg.feat_config.feature_dim = 80;

    std::string model_file = model_dir + "/model.int8.onnx";
    std::string tokens_file = model_dir + "/tokens.txt";

    asr_cfg.model_config.zipformer_ctc.model = model_file.c_str();
    asr_cfg.model_config.tokens = tokens_file.c_str();
    asr_cfg.model_config.provider = "cpu";
    asr_cfg.model_config.num_threads = 4;
    asr_cfg.decoding_method = "greedy_search";

    asr_recognizer_ = SherpaOnnxCreateOfflineRecognizer(&asr_cfg);
    if (asr_recognizer_) {
      LOG_INFO("[Pipeline] sherpa-onnx ASR recognizer initialized (direct C API)");
    } else {
      LOG_WARN("[Pipeline] sherpa-onnx ASR recognizer creation failed — will use popen fallback");
    }
  }
#endif

  // ── Initialize direct espeak-ng TTS (avoid system()) ──
#ifdef ESPEAK_NG_AVAILABLE
  {
    int sr = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0, nullptr, 0);
    if (sr > 0) {
      espeak_sample_rate_ = sr;
      espeak_SetVoiceByName(cfg_.tts_voice.c_str());
      espeak_SetParameter(espeakRATE, cfg_.tts_rate, 0);
      espeak_initialized_ = true;
      LOG_INFO("[Pipeline] espeak-ng initialized ({}Hz, voice={}, rate={}) — direct library",
               espeak_sample_rate_, cfg_.tts_voice, cfg_.tts_rate);
    } else {
      LOG_WARN("[Pipeline] espeak-ng init failed (returned {}) — will use system() fallback", sr);
    }
  }
#endif

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
  // Add MemorySkill with a placeholder store — the actual per-user store
  // will be switched dynamically in ProcessText() via set_current_user_memory().
  skill_mgr_.add_skill(std::make_unique<MemorySkill>(nullptr));

  // Per-user memory loading is now lazy: GetUserMemory() loads from disk
  // the first time a given user_id is accessed.

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
      LOG_DEBUG("[Pipeline] LLM connectivity OK");
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
                                            const std::string& session_id,
                                            std::atomic<bool>* cancel) {
  PipelineResult result;
  auto t_total_start = std::chrono::steady_clock::now();

  if (!ready_.load()) {
    result.error = "pipeline not initialized";
    return result;
  }

  // Check cancellation before any heavy work
  if (cancel && cancel->load()) {
    result.error = "cancelled";
    return result;
  }

  LOG_DEBUG("[Pipeline] processing text: \"{}\"", text);

  // ── 0. Filter semantically empty filler words ──────────
  // "嗯", "啊", "哦" etc. carry no intent — skip LLM to avoid
  // free-form generation that contradicts prior context.
  {
    static const std::vector<std::string> kFillers = {
      "嗯", "嗯嗯", "啊", "哦", "呃", "噢", "诶", "唔",
      "嗯。", "嗯？", "嗯！", "嗯嗯嗯",
      "ah", "uh", "um", "hmm", "oh", "eh",
    };
    std::string trimmed = text;
    // Remove common ASCII punctuation and whitespace
    trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(),
        [](unsigned char c) { return c == '.' || c == '?' || c == '!'
            || c == ',' || c == ' ' || c == '\t' || c == '\n'; }),
        trimmed.end());
    // Remove Chinese punctuation (multi-byte UTF-8 sequences)
    for (const char* punct : {"。", "？", "！", "，", "…", "~"}) {
      size_t pos;
      while ((pos = trimmed.find(punct)) != std::string::npos) {
        trimmed.erase(pos, strlen(punct));
      }
    }
    for (const auto& f : kFillers) {
      if (trimmed == f) {
        LOG_DEBUG("[Pipeline] filler word detected \"{}\", skipping LLM", text);
        result.asr_text = text;
        result.llm_response = "嗯？";  // minimal acknowledgment, no free-form generation
        result.skill_direct = true;
        result.ok = true;
        return result;
      }
    }
  }

  // ── Resolve user_id for per-user isolation ──────────────
  std::string user_id;
  if (!session_id.empty()) {
    auto it = session_user_map_.find(session_id);
    if (it != session_user_map_.end()) {
      user_id = it->second;
    }
  }

  // ── 1. Try Skill System ────────────────────────────────
  auto t_skill_start = std::chrono::steady_clock::now();
  SkillResult sr;  // declared here so accessible for LLM context injection below
  if (cfg_.skills_enabled) {
    // Switch MemorySkill to the current user's store before detection
    if (!user_id.empty()) {
      UserMemoryStore* um = GetUserMemory(user_id);
      skill_mgr_.set_current_user_memory(um);
      skill_mgr_.clear_memory_dirty();
    }

    sr = skill_mgr_.detect_and_execute(text);

    // Auto-persist user memory if modified by skill execution
    if (skill_mgr_.is_memory_dirty() && !user_id.empty() &&
        cfg_.memory_enabled && !cfg_.memory_persist_dir.empty()) {
      std::string mem_path = cfg_.memory_persist_dir + "/user_memory_" + user_id + ".json";
      UserMemoryStore* um = GetUserMemory(user_id);
      if (um) um->save_to_file(mem_path);
    }

    if (sr.hit) {
      auto t_skill_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t_skill_start).count();
      LOG_DEBUG("[Pipeline] skill '{}' handled request ({}ms)", sr.skill_name, t_skill_ms);

      if (sr.direct) {
        // Direct response: skill result IS the final reply (skip LLM)
        result.asr_text = text;
        result.llm_response = sr.result_text;
        result.skill_direct = true;

        // Check cancellation before TTS synthesis
        if (cancel && cancel->load()) {
          result.error = "cancelled before TTS (skill)";
          return result;
        }

        // Still synthesize TTS for direct responses
        std::string wav_path;
        if (!session_id.empty()) {
          wav_path = "/dev/shm/rtsp-server/tts_" + session_id + "_" +
                     std::to_string(
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count()) +
                     ".wav";
        } else {
          wav_path = "/dev/shm/rtsp-server/tts_output.wav";
        }

        if (SynthesizeTts(result.llm_response, wav_path, session_id)) {
          result.tts_audio_path = wav_path;
        }
        result.ok = true;
        return result;
      }

      // Non-direct: inject skill result as context for LLM
      LOG_DEBUG("[Pipeline] skill result injected as LLM context");
      // Fall through to LLM with the skill result as extra context
    }
  }

  // ── 2. Get conversation history ────────────────────────
  std::vector<ChatMessage> history_msgs;
  if (cfg_.memory_enabled && !session_id.empty()) {
    ChatMemory* mem = GetSessionMemory(session_id, user_id);
    if (mem) {
      history_msgs = mem->get_messages();
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
  // Cache key includes user_id for per-user isolation.
  if (!sr.hit && history_msgs.empty() && skill_context.empty()) {
    std::string cache_text = user_id + "|||" + text;
    size_t cache_key = std::hash<std::string>{}(cache_text);
    {
      std::lock_guard<std::mutex> lock(llm_cache_mutex_);
      auto it = llm_cache_.find(cache_key);
      if (it != llm_cache_.end()) {
        result.llm_response = it->second;
        LOG_DEBUG("[Pipeline] LLM cache hit: \"{}\"", text);
        // Fall through to TTS (skip LLM call below)
      }
    }
  }

  if (result.llm_response.empty()) {
    // Check cancellation before expensive LLM call
    if (cancel && cancel->load()) {
      result.error = "cancelled before LLM";
      return result;
    }
    result.llm_response = CallLlmChat(system_prompt, text, history_msgs, skill_context, cancel);

    // Check if LLM was cancelled
    if (cancel && cancel->load()) {
      result.error = "cancelled during LLM";
      return result;
    }

    // Cache the result for frequent static queries
    if (!sr.hit && history_msgs.empty() && skill_context.empty() && !result.llm_response.empty()) {
      std::string cache_text = user_id + "|||" + text;
      size_t cache_key = std::hash<std::string>{}(cache_text);
      std::lock_guard<std::mutex> lock(llm_cache_mutex_);
      if (llm_cache_.size() >= kMaxLlmCacheEntries) {
        llm_cache_.clear();
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

  // ── 4.5. Strip emoji & invisible characters ─────────────
  // LLM models sometimes generate emoji, variation selectors,
  // and other Unicode symbols that TTS cannot pronounce.
  {
    std::string filtered;
    filtered.reserve(result.llm_response.size());
    for (size_t i = 0; i < result.llm_response.size(); ) {
      // Decode UTF-8 codepoint
      uint32_t cp = 0;
      int len = 0;
      unsigned char c = static_cast<unsigned char>(result.llm_response[i]);
      if ((c & 0x80) == 0)      { cp = c;          len = 1; }
      else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
      else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
      else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
      else { i++; continue; }  // invalid byte, skip

      if (i + len > result.llm_response.size()) break;
      for (int j = 1; j < len; j++) {
        cp = (cp << 6) | (static_cast<unsigned char>(result.llm_response[i + j]) & 0x3F);
      }

      // Keep codepoint unless it's in an emoji / decorative range
      bool keep = true;
      if (cp == 0x200D || cp == 0xFE0F || cp == 0xFE00) keep = false;        // ZWJ, variation selectors
      else if (cp >= 0x1F600 && cp <= 0x1F64F) keep = false;  // emoticons
      else if (cp >= 0x1F300 && cp <= 0x1F5FF) keep = false;  // symbols & pictographs
      else if (cp >= 0x1F680 && cp <= 0x1F6FF) keep = false;  // transport & map
      else if (cp >= 0x1F1E0 && cp <= 0x1F1FF) keep = false;  // flags
      else if (cp >= 0x1F900 && cp <= 0x1F9FF) keep = false;  // supplemental symbols
      else if (cp >= 0x1FA00 && cp <= 0x1FAFF) keep = false;  // chess & extended
      else if (cp >= 0x2600 && cp <= 0x27BF) keep = false;    // misc symbols / dingbats
      else if (cp >= 0x1F3FB && cp <= 0x1F3FF) keep = false;  // skin tone modifiers
      else if (cp >= 0xE000 && cp <= 0xF8FF) keep = false;    // private use area

      if (keep) {
        for (int j = 0; j < len; j++) filtered += result.llm_response[i + j];
      }
      i += len;
    }
    if (filtered.size() != result.llm_response.size()) {
      LOG_DEBUG("[Pipeline] stripped {} emoji chars from LLM response",
               result.llm_response.size() - filtered.size());
      result.llm_response = std::move(filtered);
    }
  }

  // ── 5. Update conversation memory ──────────────────────
  if (cfg_.memory_enabled && !session_id.empty()) {
    ChatMemory* mem = GetSessionMemory(session_id, user_id);
    if (mem) {
      // Persist skill result alongside user/assistant so future
      // turns can see facts (e.g. weather, time) even without re-triggering the skill.
      if (sr.hit && !sr.direct && !sr.result_text.empty()) {
        mem->add(text, result.llm_response, sr.result_text);
      } else {
        mem->add(text, result.llm_response);
      }

      // Auto-persist by user_id for cross-session recovery
      if (!user_id.empty() && !cfg_.memory_persist_dir.empty()) {
        std::string chat_path = cfg_.memory_persist_dir + "/chat_" + user_id + ".json";
        mem->save_to_file(chat_path);
      }
    }
  }

  // ── 6. TTS ─────────────────────────────────────────────
  // Check cancellation before expensive TTS synthesis
  if (cancel && cancel->load()) {
    result.error = "cancelled before TTS";
    return result;
  }

  auto t_tts_start = std::chrono::steady_clock::now();
  std::string wav_path;
  if (!session_id.empty()) {
    wav_path = "/dev/shm/rtsp-server/tts_" + session_id + "_" +
               std::to_string(
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count()) +
               ".wav";
  } else {
    wav_path = "/dev/shm/rtsp-server/tts_output.wav";
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
  result.llm_ms = t_llm_ms;
  result.tts_ms = t_tts_ms;
  result.total_ms = t_total_ms;
  return result;
}

// ---------------------------------------------------------------------------
// Process Text (Streaming: Skills → LLM stream → TTS stream)
// ---------------------------------------------------------------------------

PipelineResult PipelineBridge::ProcessTextStream(
    const std::string& text,
    const std::string& session_id,
    std::atomic<bool>* cancel,
    LlmTokenCallback on_llm_token,
    TtsAudioCallback on_tts_audio) {

  PipelineResult result;
  auto t_total_start = std::chrono::steady_clock::now();

  if (!ready_.load()) {
    result.error = "pipeline not initialized";
    return result;
  }

  if (cancel && cancel->load()) {
    result.error = "cancelled";
    return result;
  }

  LOG_DEBUG("[Pipeline] streaming text: \"{}\"", text);

  // ── 0. Filter filler words ──────────────────────────
  if (text.size() <= 3) {
    std::string trimmed = text;
    auto p = trimmed.find_first_not_of(" \t\n\r");
    if (p != std::string::npos) trimmed = trimmed.substr(p);
    if (trimmed == "嗯" || trimmed == "啊" || trimmed == "哦" ||
        trimmed == "嗯嗯" || trimmed == "啊啊" || trimmed == "哦哦" ||
        trimmed == "嗯。") {
      LOG_DEBUG("[Pipeline] filler word detected \"{}\", skipping LLM", text);

      // Synthesize a short acknowledgement via streaming TTS
      if (on_tts_audio) {
        std::string filler_reply = "嗯？";
        SynthesizeTtsStream(filler_reply, on_tts_audio, session_id);
      }

      result.asr_text = text;
      result.llm_response = "嗯？";
      result.ok = true;
      auto t_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t_total_start).count();
      result.total_ms = t_total_ms;
      return result;
    }
  }

  // ── 1. Extract session info ─────────────────────────
  std::string user_id;
  {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    auto it = session_user_map_.find(session_id);
    if (it != session_user_map_.end()) user_id = it->second;
  }

  // ── 2. Check skills ────────────────────────────────
  SkillResult sr;
  if (cfg_.skills_enabled) {
    sr = skill_mgr_.detect_and_execute(text);
    if (sr.hit && sr.direct) {
      // Direct skill response — synthesize via streaming TTS
      result.llm_response = sr.result_text;
      result.skill_direct = true;
      result.asr_text = text;

      if (cancel && cancel->load()) {
        result.error = "cancelled before TTS (skill)";
        return result;
      }

      if (on_tts_audio && !result.llm_response.empty()) {
        SynthesizeTtsStream(result.llm_response, on_tts_audio, session_id);
      }

      result.ok = true;
      auto t_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t_total_start).count();
      result.total_ms = t_total_ms;
      return result;
    }
  }

  // Build skill context for LLM
  std::string skill_context;
  if (sr.hit && !sr.direct && !sr.result_text.empty()) {
    skill_context = sr.result_text;
  }

  // ── 3. Build system prompt ──────────────────────────
  std::string system_prompt = cfg_.llm_system_prompt;
  if (system_prompt.empty()) {
    system_prompt = "你是小千，一个18岁女大学生，性格活泼开朗。回复控制在2-3句话，约50字。";
  }
  std::string sys_ctx = SkillManager::get_system_context();
  if (!sys_ctx.empty()) {
    system_prompt = system_prompt + "\n\n" + sys_ctx;
  }

  // ── 4. Get conversation history ─────────────────────
  std::vector<ChatMessage> history_msgs;
  if (cfg_.memory_enabled && !session_id.empty()) {
    ChatMemory* mem = GetSessionMemory(session_id, user_id);
    if (mem) history_msgs = mem->get_messages();
  }

  // ── 5. Streaming LLM ────────────────────────────────
  auto t_llm_start = std::chrono::steady_clock::now();

  if (cancel && cancel->load()) {
    result.error = "cancelled before LLM";
    return result;
  }

  result.llm_response = CallLlmChatStream(system_prompt, text, history_msgs,
                                          skill_context, on_llm_token, cancel);

  auto t_llm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t_llm_start).count();
  result.llm_ms = t_llm_ms;

  if (cancel && cancel->load()) {
    result.error = "cancelled during LLM";
    return result;
  }

  if (result.llm_response.empty()) {
    result.error = "LLM returned empty response";
    LOG_WARN("[Pipeline] LLM empty response for: {}", text);
    return result;
  }

  // ── 5.5. Strip emoji ────────────────────────────────
  {
    std::string filtered;
    filtered.reserve(result.llm_response.size());
    for (size_t i = 0; i < result.llm_response.size(); ) {
      uint32_t cp = 0;
      int len = 0;
      unsigned char c = static_cast<unsigned char>(result.llm_response[i]);
      if ((c & 0x80) == 0)      { cp = c;          len = 1; }
      else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
      else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
      else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
      else { i++; continue; }

      if (i + len > result.llm_response.size()) break;
      for (int j = 1; j < len; j++) {
        cp = (cp << 6) | (static_cast<unsigned char>(result.llm_response[i + j]) & 0x3F);
      }

      bool keep = true;
      if (cp == 0x200D || cp == 0xFE0F || cp == 0xFE00) keep = false;
      else if (cp >= 0x1F600 && cp <= 0x1F64F) keep = false;
      else if (cp >= 0x1F300 && cp <= 0x1F5FF) keep = false;
      else if (cp >= 0x1F680 && cp <= 0x1F6FF) keep = false;
      else if (cp >= 0x1F1E0 && cp <= 0x1F1FF) keep = false;
      else if (cp >= 0x1F900 && cp <= 0x1F9FF) keep = false;
      else if (cp >= 0x1FA00 && cp <= 0x1FAFF) keep = false;
      else if (cp >= 0x2600 && cp <= 0x27BF) keep = false;
      else if (cp >= 0x1F3FB && cp <= 0x1F3FF) keep = false;
      else if (cp >= 0xE000 && cp <= 0xF8FF) keep = false;

      if (keep) {
        for (int j = 0; j < len; j++) filtered += result.llm_response[i + j];
      }
      i += len;
    }
    if (filtered.size() != result.llm_response.size()) {
      result.llm_response = std::move(filtered);
    }
  }

  // ── 6. Update conversation memory ──────────────────
  if (cfg_.memory_enabled && !session_id.empty()) {
    ChatMemory* mem = GetSessionMemory(session_id, user_id);
    if (mem) {
      if (sr.hit && !sr.direct && !sr.result_text.empty()) {
        mem->add(text, result.llm_response, sr.result_text);
      } else {
        mem->add(text, result.llm_response);
      }
    }
  }

  // ── 7. Streaming TTS ────────────────────────────────
  auto t_tts_start = std::chrono::steady_clock::now();

  if (on_tts_audio && !result.llm_response.empty()) {
    SynthesizeTtsStream(result.llm_response, on_tts_audio, session_id);
  }

  auto t_tts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t_tts_start).count();

  auto t_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t_total_start).count();

  result.asr_text = text;
  result.ok = true;
  result.tts_ms = t_tts_ms;
  result.total_ms = t_total_ms;
  return result;
}

// ---------------------------------------------------------------------------
// Process Audio (ASR → LLM → TTS)
// ---------------------------------------------------------------------------

PipelineResult PipelineBridge::ProcessAudio(const int16_t* pcm_data,
                                              int sample_count,
                                              const std::string& session_id,
                                              std::atomic<bool>* cancel) {
  PipelineResult result;

  if (!ready_.load()) {
    result.error = "pipeline not initialized";
    return result;
  }

  // 1. ASR: write PCM to WAV file and call transcribe
  std::string wav_path = "/dev/shm/rtsp-server/asr_input_" +
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

  result.asr_text = TranscribeAudio(pcm_data, sample_count, session_id);

  if (result.asr_text.empty()) {
    result.error = "ASR produced no text";
    return result;
  }

  LOG_DEBUG("[Pipeline] ASR result: \"{}\"", result.asr_text);

  // 2. LLM + TTS (via ProcessText which now has skills + memory)
  auto llm_result = ProcessText(result.asr_text, session_id, cancel);
  result.llm_response = llm_result.llm_response;
  result.tts_audio_path = llm_result.tts_audio_path;
  result.ok = !llm_result.llm_response.empty();

  return result;
}

// ---------------------------------------------------------------------------
// ASR Transcription (stub: uses system whisper or returns empty)
// ---------------------------------------------------------------------------

std::string PipelineBridge::TranscribeAudio(const int16_t* pcm_data,
                                              int sample_count,
                                              const std::string& session_id) {
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
    LOG_DEBUG("[ASR] audio too quiet (RMS={:.4f}), skipping", rms);
    return "";
  }

  auto t_asr_start = std::chrono::steady_clock::now();

#ifdef SHERPA_ONNX_AVAILABLE
  // ── Direct sherpa-onnx C API (no fork/exec, model stays in memory) ──
  if (asr_recognizer_) {
    // Convert int16 → float (sherpa-onnx expects float samples)
    std::vector<float> float_samples(sample_count);
    for (int i = 0; i < sample_count; i++) {
      float_samples[i] = pcm_data[i] / 32768.0f;
    }

    const SherpaOnnxOfflineStream* stream =
        SherpaOnnxCreateOfflineStream(asr_recognizer_);
    if (stream) {
      SherpaOnnxAcceptWaveformOffline(stream, 16000,
                                       float_samples.data(), sample_count);
      SherpaOnnxDecodeOfflineStream(asr_recognizer_, stream);

      const SherpaOnnxOfflineRecognizerResult* result =
          SherpaOnnxGetOfflineStreamResult(stream);
      std::string text;
      if (result && result->text) {
        text = result->text;
      }
      if (result) {
        SherpaOnnxDestroyOfflineRecognizerResult(result);
      }
      SherpaOnnxDestroyOfflineStream(stream);

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
          LOG_DEBUG("[ASR] \"{}\" ({} samples, {}ms, RMS={:.4f}) [direct C API]",
                   text, sample_count, t_asr_total_ms, rms);
          return text;
        }
      }
    }

    LOG_DEBUG("[ASR] direct C API returned empty ({} samples)", sample_count);
  }
#endif

  // ── Fallback: popen to sherpa-onnx-offline binary ──────
  {
    // Write PCM to WAV file
    std::string wav_path = session_id.empty()
        ? "/dev/shm/rtsp-server/asr_latest.wav"
        : ("/dev/shm/rtsp-server/asr_" + session_id + ".wav");
    {
      std::ofstream f(wav_path, std::ios::binary);
      if (!f) {
        LOG_DEBUG("[ASR] no speech recognized ({} samples, {:.1f}s audio)",
                  sample_count, sample_count / 16000.0f);
        return "";
      }

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

    static const char* kSherpaBin =
        "/eir/lixin/ASR-LLM-TTS/src/third_party/sherpa-onnx/bin/sherpa-onnx-offline";
    static const char* kModelDir =
        "/eir/lixin/ASR-LLM-TTS/src/third_party/sherpa-onnx/zipformer-ctc-zh";
    static const char* kSherpaLib =
        "/eir/lixin/ASR-LLM-TTS/src/third_party/sherpa-onnx/lib";
    static bool sherpa_bin_avail = (access(kSherpaBin, X_OK) == 0);

    if (sherpa_bin_avail) {
      std::ostringstream cmd;
      cmd << "LD_LIBRARY_PATH=" << kSherpaLib << ":$LD_LIBRARY_PATH "
          << kSherpaBin
          << " --tokens=" << kModelDir << "/tokens.txt"
          << " --zipformer-ctc-model=" << kModelDir << "/model.int8.onnx"
          << " --model-type=zipformer_ctc"
          << " --num-threads=4"
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

      if (!output.empty()) {
        try {
          auto j = nlohmann::json::parse(output);
          std::string text = j.value("text", "");
          if (!text.empty()) {
            auto start = text.find_first_not_of(" \t\n\r");
            auto end = text.find_last_not_of(" \t\n\r");
            if (start != std::string::npos) {
              text = text.substr(start, end - start + 1);
            }
            if (!text.empty()) {
              text = CtcDedup(text);
              auto t_asr_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t_asr_start).count();
              LOG_DEBUG("[ASR] \"{}\" ({} samples, {}ms, RMS={:.4f}) [popen]",
                       text, sample_count, t_asr_total_ms, rms);
              return text;
            }
          }
        } catch (...) {
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
                  LOG_DEBUG("[ASR] \"{}\" (RMS={:.4f}) [popen]", text, rms);
                  return text;
                }
              }
            } catch (...) {}
          }
        }
      }
    }
  }

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

  // ── Remote TTS (Orin NX GPU) ──────────────────────
  if (!cfg_.tts_remote_host.empty()) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = "http://" + cfg_.tts_remote_host + "/tts";
    nlohmann::json req_body;
    req_body["text"] = text;
    std::string req_str = req_body.dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_str.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    // Write response to file
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return false;
    }

    // Use a simple lambda-capture struct for the file stream
    struct WriteCtx {
      std::ofstream* stream;
    } ctx = {&out};

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     +[](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                       auto* ctx = static_cast<WriteCtx*>(userdata);
                       size_t total = size * nmemb;
                       ctx->stream->write(static_cast<char*>(ptr), total);
                       return total;
                     });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    out.close();

    if (res == CURLE_OK && http_code == 200) {
      // Resample from 22050 → target sample rate if needed
      if (cfg_.tts_sample_rate != 22050) {
        std::string tmp_path = output_path + ".22050.wav";
        if (std::rename(output_path.c_str(), tmp_path.c_str()) == 0) {
          std::ostringstream ffmpeg_cmd;
          ffmpeg_cmd << "ffmpeg -y -i " << tmp_path
                     << " -ar " << cfg_.tts_sample_rate << " -ac 1 -sample_fmt s16"
                     << " -loglevel error "
                     << output_path << " 2>/dev/null";
          int ret = system(ffmpeg_cmd.str().c_str());
          if (ret != 0) {
            LOG_WARN("[TTS] remote: ffmpeg resample failed, keeping original rate");
            std::rename(tmp_path.c_str(), output_path.c_str());
          } else {
            std::remove(tmp_path.c_str());
          }
        }
      }
      LOG_DEBUG("[TTS] remote synthesis OK: \"{}\" → {}", text, output_path);
      // Cache the result locally
      if (cfg_.tts_cache_enabled) {
        std::string cached = GetCachePath(text);
        std::ifstream src(output_path, std::ios::binary);
        std::ofstream dst(cached, std::ios::binary);
        if (src && dst) { dst << src.rdbuf(); }
      }
      return true;
    }

    LOG_ERROR("[TTS] remote synthesis failed: curl={} http={}", (int)res, http_code);
    return false;
  }

  // Check cache
  if (cfg_.tts_cache_enabled) {
    std::string cached = GetCachePath(text);
    std::ifstream check(cached, std::ios::binary);
    if (check.good()) {
      check.close();
      std::ifstream src(cached, std::ios::binary);
      std::ofstream dst(output_path, std::ios::binary);
      if (src && dst) {
        dst << src.rdbuf();
        LOG_DEBUG("[TTS] cache hit: \"{}\"", text);
        return true;
      }
    }
  }

  bool ok = false;

  // ── Direct espeak-ng library (fastest, no fork/exec) ──
#ifdef ESPEAK_NG_AVAILABLE
  if (espeak_initialized_ && (cfg_.tts_backend == "espeak" || cfg_.tts_backend.empty())) {
    auto t_tts_start = std::chrono::steady_clock::now();

    g_tts_audio.clear();
    espeak_SetSynthCallback(tts_audio_callback);

    espeak_ERROR err = espeak_Synth(text.c_str(), text.size() + 1,
                                    0, POS_CHARACTER, 0,
                                    espeakCHARS_UTF8, nullptr, nullptr);
    if (err == EE_OK) {
      espeak_Synchronize();

      if (!g_tts_audio.empty()) {
        // If sample rate doesn't match target, resample via ffmpeg pipe
        if (espeak_sample_rate_ != cfg_.tts_sample_rate) {
          std::ostringstream ffmpeg_cmd;
          ffmpeg_cmd << "ffmpeg -f s16le -ar " << espeak_sample_rate_
                     << " -ac 1 -i pipe:0"
                     << " -ar " << cfg_.tts_sample_rate << " -ac 1"
                     << " -y " << output_path
                     << " 2>/dev/null";

          FILE* ffmpeg = popen(ffmpeg_cmd.str().c_str(), "w");
          if (ffmpeg) {
            fwrite(g_tts_audio.data(), sizeof(int16_t), g_tts_audio.size(), ffmpeg);
            int ff_ret = pclose(ffmpeg);
            if (ff_ret == 0) {
              auto t_tts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t_tts_start).count();
              LOG_DEBUG("[TTS] espeak {}ms: \"{}\"", t_tts_ms, text);
              ok = true;
            }
          }
          if (!ok) {
            LOG_WARN("[TTS] espeak direct ffmpeg resample failed, trying WAV write");
          }
        }

        // Direct WAV write (no resample needed, or ffmpeg fallback)
        if (!ok) {
          if (WriteWavFile(output_path, g_tts_audio, espeak_sample_rate_)) {
            auto t_tts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_tts_start).count();
            LOG_DEBUG("[TTS] espeak {}ms: \"{}\"", t_tts_ms, text);
            ok = true;
          }
        }
      }
    }

    if (ok) {
      // Cache and return
      if (cfg_.tts_cache_enabled) {
        std::string cached = GetCachePath(text);
        std::ifstream src(output_path, std::ios::binary);
        std::ofstream dst(cached, std::ios::binary);
        if (src && dst) {
          dst << src.rdbuf();
        }
      }
      return true;
    }

    LOG_WARN("[TTS] espeak direct synthesis failed, falling back to system()");
  }
#endif

  // ── system()-based TTS (fallback / edge_tts / piper) ──

  // Generate TTS using the configured backend.
  std::ostringstream cmd;

#ifdef HAS_VOICE_PIPELINE
  // Native Piper TTS
  cmd << "echo '" << EscapeJson(text) << "' | "
      << "piper --model " << cfg_.tts_piper_model
      << " --output_file " << output_path;
#else
  // Stub mode: use external TTS engine depending on config
  if (cfg_.tts_backend == "edge_tts") {
    // Microsoft Edge TTS (neural, best Chinese quality)
    cmd << "python3 " << cfg_.tts_edge_tts_script
        << " --text " << std::quoted(text)
        << " --voice " << cfg_.tts_edge_tts_voice
        << " --output " << output_path
        << " 2>/dev/null";
  } else if (cfg_.tts_backend == "piper" && !cfg_.tts_piper_model.empty()) {
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

  // Fallback to espeak-ng via system()
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

  if (!ok) {
    int ret = system(cmd.str().c_str());
    if (ret != 0) {
      LOG_ERROR("[TTS] synthesis failed (exit={}) for: \"{}\"", ret, text);
      return false;
    }
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

  LOG_DEBUG("[TTS] synthesized: \"{}\"", text);
  return true;
}

// ---------------------------------------------------------------------------
// TTS Streaming (piper --output-raw → PCM chunks via callback)
// ---------------------------------------------------------------------------

bool PipelineBridge::SynthesizeTtsStream(const std::string& text,
                                         TtsAudioCallback on_audio,
                                         const std::string& session_id) {
  if (!on_audio) return false;

  // Check cache first — we can serve cached WAV as a single chunk
  if (cfg_.tts_cache_enabled) {
    std::string cached = GetCachePath(text);
    std::ifstream check(cached, std::ios::binary);
    if (check.good()) {
      check.close();
      // Read cached WAV and strip header to get raw PCM
      std::ifstream src(cached, std::ios::binary);
      if (src) {
        // Skip WAV header (44 bytes)
        src.seekg(44);
        std::vector<int16_t> samples;
        int16_t sample;
        while (src.read(reinterpret_cast<char*>(&sample), sizeof(sample))) {
          samples.push_back(sample);
        }
        if (!samples.empty()) {
          on_audio(samples.data(), static_cast<int>(samples.size()), true);
          LOG_DEBUG("[TTS] stream cache hit: \"{}\" ({} samples)", text, samples.size());
          return true;
        }
      }
    }
  }

  if (!piper_available_) {
    LOG_WARN("[TTS] piper not available for streaming, falling back to file synthesis");
    return false;
  }

  // Build piper command: echo text | piper --model <model> --output-raw
  std::ostringstream cmd;
  cmd << "echo " << std::quoted(text) << " | "
      << "piper --model " << cfg_.tts_piper_model
      << " --output-raw 2>/dev/null";

  FILE* pipe = popen(cmd.str().c_str(), "r");
  if (!pipe) {
    LOG_ERROR("[TTS] piper popen failed for streaming");
    return false;
  }

  // Read PCM chunks from piper's stdout
  // Piper outputs raw S16LE at model's native sample rate (huayan = 22050 Hz)
  // Chunk size: ~186ms of audio at 22050 Hz = 4096 samples = 8192 bytes
  static constexpr int kChunkSamples = 4096;
  std::vector<int16_t> chunk(kChunkSamples);

  int total_samples = 0;
  while (true) {
    size_t n = fread(chunk.data(), sizeof(int16_t), kChunkSamples, pipe);
    if (n == 0) break;
    total_samples += static_cast<int>(n);
    on_audio(chunk.data(), static_cast<int>(n), false);
  }

  int ret = pclose(pipe);
  if (ret != 0) {
    LOG_WARN("[TTS] piper stream exited with code {}", ret);
    if (total_samples == 0) return false;
  }

  // Signal end of stream
  on_audio(nullptr, 0, true);

  LOG_DEBUG("[TTS] stream complete: \"{}\" ({} total samples, {}ms)",
            text, total_samples,
            total_samples * 1000 / 22050);
  return total_samples > 0;
}

// ---------------------------------------------------------------------------
// LLM Call (Ollama /api/generate — used for ping and simple prompts)
// ---------------------------------------------------------------------------

std::string PipelineBridge::CallLlm(const std::string& prompt,
                                    const std::string& context,
                                    std::atomic<bool>* cancel) {
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

  // Check cancellation before starting
  if (cancel && cancel->load()) return "";

  // Actual LLM inference
  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string url = cfg_.llm_host + "/api/generate";

  json body;
  body["model"] = cfg_.llm_model;
  body["prompt"] = prompt;
  body["stream"] = false;
  body["options"]["temperature"] = 0.7;
  body["options"]["num_predict"] = 128;  // ~50 Chinese chars, 128 tokens is plenty
  body["keep_alive"] = -1;  // keep model loaded in Ollama memory

  std::string body_str = body.dump();

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.llm_timeout_sec));

  // Enable cancellation via progress callback
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
      LOG_INFO("[LLM] request cancelled by interrupt");
    } else {
      LOG_ERROR("[LLM] curl error: {}", curl_easy_strerror(res));
    }
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
                                         const std::vector<ChatMessage>& history_msgs,
                                         const std::string& skill_context,
                                         std::atomic<bool>* cancel) {
  // Check cancellation before starting
  if (cancel && cancel->load()) return "";

  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string url = cfg_.llm_host + "/api/chat";

  // Build messages array
  json messages = json::array();

  // System message with personality
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

  // History messages — proper user/assistant alternating with embedded skill facts
  for (const auto& msg : history_msgs) {
    messages.push_back({
        {"role", msg.role},
        {"content", msg.content}
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
  body["options"]["num_predict"] = 128;  // ~50 Chinese chars, 128 tokens is plenty
  body["keep_alive"] = -1;  // keep model loaded in Ollama memory

  std::string body_str = body.dump();

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.llm_timeout_sec));

  // Enable cancellation via progress callback
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
      LOG_INFO("[LLM] chat request cancelled by interrupt");
    } else {
      LOG_ERROR("[LLM] curl error: {}", curl_easy_strerror(res));
    }
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
// LLM Call via /api/chat (streaming — NDJSON chunks)
// ---------------------------------------------------------------------------

namespace {
// State for streaming curl write callback — accumulates partial NDJSON lines
struct LlmStreamState {
  LlmTokenCallback on_token;
  std::atomic<bool>* cancel;
  std::string full_response;  // accumulated complete text
  std::string buffer;         // partial line buffer
};
}  // namespace

static size_t LlmStreamWriteCallback(void* contents, size_t size, size_t nmemb,
                                      void* userp) {
  auto* state = static_cast<LlmStreamState*>(userp);
  size_t total = size * nmemb;

  // Check cancellation
  if (state->cancel && state->cancel->load()) return 0;

  state->buffer.append(static_cast<char*>(contents), total);

  // Process complete lines
  size_t pos;
  while ((pos = state->buffer.find('\n')) != std::string::npos) {
    std::string line = state->buffer.substr(0, pos);
    state->buffer.erase(0, pos + 1);

    // Trim \r
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    try {
      auto j = nlohmann::json::parse(line);
      bool done = j.value("done", false);

      if (!done) {
        std::string token = j.value("message", nlohmann::json::object())
                                .value("content", "");
        if (!token.empty()) {
          state->full_response += token;
          if (state->on_token) state->on_token(token, false);
        }
      } else {
        if (state->on_token) state->on_token("", true);
      }
    } catch (const nlohmann::json::exception& e) {
      LOG_DEBUG("[LLM] stream parse error: {} — line: {}", e.what(), line);
    }
  }

  return total;
}

std::string PipelineBridge::CallLlmChatStream(
    const std::string& system_prompt,
    const std::string& user_text,
    const std::vector<ChatMessage>& history_msgs,
    const std::string& skill_context,
    LlmTokenCallback on_token,
    std::atomic<bool>* cancel) {

  if (cancel && cancel->load()) return "";

  CURL* curl = curl_easy_init();
  if (!curl) return "";

  std::string url = cfg_.llm_host + "/api/chat";

  // Build messages array (same as CallLlmChat)
  nlohmann::json messages = nlohmann::json::array();
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

  nlohmann::json body;
  body["model"] = cfg_.llm_model;
  body["messages"] = messages;
  body["stream"] = true;                       // ← streaming!
  body["options"]["temperature"] = 0.7;
  body["options"]["num_predict"] = 128;
  body["keep_alive"] = -1;

  std::string body_str = body.dump();

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.llm_timeout_sec));

  // Set up streaming state
  LlmStreamState state;
  state.on_token = std::move(on_token);
  state.cancel = cancel;

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, LlmStreamWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);

  // Enable cancellation via progress callback
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
      LOG_INFO("[LLM] stream request cancelled by interrupt");
    } else {
      LOG_ERROR("[LLM] stream curl error: {}", curl_easy_strerror(res));
    }
    return "";
  }

  // Trim response
  auto start = state.full_response.find_first_not_of(" \t\n\r");
  auto end = state.full_response.find_last_not_of(" \t\n\r");
  if (start != std::string::npos) {
    state.full_response = state.full_response.substr(start, end - start + 1);
  }

  return state.full_response;
}

// ---------------------------------------------------------------------------
// Session Memory
// ---------------------------------------------------------------------------

ChatMemory* PipelineBridge::GetSessionMemory(const std::string& session_id,
                                                 const std::string& user_id) {
  if (session_id.empty()) return nullptr;

  std::lock_guard<std::mutex> lock(memory_mutex_);

  auto it = session_memories_.find(session_id);
  if (it == session_memories_.end()) {
    // Create new session memory
    auto [inserted_it, ok] = session_memories_.emplace(
        session_id,
        ChatMemory(cfg_.memory_max_rounds, cfg_.memory_max_tokens));

    // Try loading from disk: prefer user_id-based (cross-session),
    // fall back to session_id-based (legacy).
    if (!cfg_.memory_persist_dir.empty()) {
      bool loaded = false;
      if (!user_id.empty()) {
        std::string user_path = cfg_.memory_persist_dir + "/chat_" + user_id + ".json";
        loaded = inserted_it->second.load_from_file(user_path);
      }
      if (!loaded) {
        std::string session_path = cfg_.memory_persist_dir + "/chat_" + session_id + ".json";
        inserted_it->second.load_from_file(session_path);
      }
    }

    return &inserted_it->second;
  }

  return &it->second;
}

// ---------------------------------------------------------------------------
// Per-User Memory
// ---------------------------------------------------------------------------

void PipelineBridge::RegisterSessionUser(const std::string& session_id,
                                          const std::string& user_id) {
  std::lock_guard<std::mutex> lock(memory_mutex_);
  session_user_map_[session_id] = user_id;
  LOG_DEBUG("[Pipeline] registered session {} → user {}", session_id, user_id);
}

UserMemoryStore* PipelineBridge::GetUserMemory(const std::string& user_id) {
  if (user_id.empty()) return nullptr;

  std::lock_guard<std::mutex> lock(memory_mutex_);
  auto it = user_memories_.find(user_id);
  if (it == user_memories_.end()) {
    // Create new per-user memory store
    auto [inserted_it, ok] = user_memories_.emplace(
        user_id, UserMemoryStore());

    // Load from per-user persist file
    if (cfg_.memory_enabled && !cfg_.memory_persist_dir.empty()) {
      std::string path = cfg_.memory_persist_dir + "/user_memory_" + user_id + ".json";
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
