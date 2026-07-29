#pragma once
/**
 * Skill Manager — LLM tool-use system for the RTSP server.
 *
 * Hybrid dispatch strategy (same as ASR-LLM-TTS):
 *
 *   User text → SkillManager::detect_and_execute()
 *                │
 *                ├─ [Primary] Function Calling (LLM-driven)
 *                │    LLM receives JSON Schema for all tools →
 *                │    autonomously selects tool + extracts parameters
 *                │
 *                ├─ [Fallback] Keyword Match (downgrade)
 *                │    When LLM unavailable / model too small / JSON parse fails
 *                │
 *                └─ Miss → text passes through to main LLM for chat
 *
 * Ported from /eir/lixin/ASR-LLM-TTS/src/brain/skill_manager.h
 */

#include "brain/skill_base.h"
#include <string>
#include <vector>
#include <memory>
#include <ctime>

class FunctionCaller;
class ChatMemory;
class UserMemoryStore;

// ── Skill Manager ────────────────────────────────────────

class SkillManager {
public:
    SkillManager();

    /// Add a skill (takes ownership)
    void add_skill(std::unique_ptr<Skill> skill);

    /// Set the Function Calling engine (enables LLM-driven tool selection)
    void set_function_caller(std::shared_ptr<FunctionCaller> fc) {
        function_caller_ = std::move(fc);
    }

    /// Enable/disable Function Calling (does not affect keyword fallback)
    void set_function_calling_enabled(bool v) { fc_enabled_ = v; }

    /// Enable/disable a specific skill by name
    void set_enabled(const std::string& name, bool enabled);

    /// Set the memory store pointer (for MemorySkill)
    void set_memory_store(UserMemoryStore* store) { memory_store_ = store; }

    /// Set the chat memory pointer (for context injection)
    void set_chat_memory(ChatMemory* cm) { chat_memory_ = cm; }

    /// Detect intent + execute skill (hybrid dispatch)
    /// @return SkillResult — check .hit to see if a skill was triggered
    SkillResult detect_and_execute(const std::string& user_text);

    /// Get system context (current time, etc.)
    static std::string get_system_context();

    /// Collect function definitions from all enabled skills
    std::vector<FunctionDef> collect_function_defs() const;

    /// Execute a tool by name with structured arguments (for ReAct callback)
    std::string execute_tool(const std::string& name,
                             const nlohmann::json& args,
                             const std::string& user_text);

private:
    std::vector<std::unique_ptr<Skill>> skills_;
    std::shared_ptr<FunctionCaller> function_caller_;
    bool fc_enabled_ = true;

    // External references (not owned)
    UserMemoryStore* memory_store_ = nullptr;
    ChatMemory* chat_memory_ = nullptr;

    /// Find a skill by name
    Skill* find_skill(const std::string& name);
};
