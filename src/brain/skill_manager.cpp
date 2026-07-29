/**
 * Skill Manager — implementation for RTSP server.
 *
 * Registers all available skills. To add a new skill:
 *   1. Create skill_xxx.h / skill_xxx.cpp in brain/skills/
 *   2. #include it here and add_skill() in the constructor
 *   3. Add source files to CMakeLists.txt
 *
 * Ported from /eir/lixin/ASR-LLM-TTS/src/brain/skill_manager.cpp
 */

#include "brain/skill_manager.h"

// ── All skill headers ────────────────────────────────────
#include "brain/skills/skill_weather.h"
#include "brain/skills/skill_time.h"
#include "brain/skills/skill_calculator.h"
#include "brain/skills/skill_entertainment.h"
#include "brain/skills/skill_riddle.h"
#include "brain/skills/skill_fortune.h"
#include "brain/skills/skill_poetry.h"
#include "brain/skills/skill_games.h"
#include "brain/skills/skill_memory.h"

#include "llm/function_caller.h"
#include "utils/logger.h"

#include <sstream>
#include <ctime>
#include <cstdio>

// ── Constructor: register all skills ─────────────────────

SkillManager::SkillManager()
{
    add_skill(std::make_unique<WeatherSkill>());
    add_skill(std::make_unique<TimeSkill>());
    add_skill(std::make_unique<CalculatorSkill>());
    add_skill(std::make_unique<EntertainmentSkill>());
    add_skill(std::make_unique<RiddleSkill>());
    add_skill(std::make_unique<FortuneSkill>());
    add_skill(std::make_unique<PoetrySkill>());
    add_skill(std::make_unique<GamesSkill>());
    // MemorySkill needs a UserMemoryStore — added later via set_memory_store()
}

void SkillManager::add_skill(std::unique_ptr<Skill> skill)
{
    skills_.push_back(std::move(skill));
}

void SkillManager::set_enabled(const std::string& name, bool enabled)
{
    for (auto& s : skills_) {
        if (s->name() == name) {
            s->set_enabled(enabled);
            return;
        }
    }
}

// ── Collect function definitions ─────────────────────────

std::vector<FunctionDef> SkillManager::collect_function_defs() const
{
    std::vector<FunctionDef> defs;
    for (auto& s : skills_) {
        if (!s->enabled()) continue;
        FunctionDef def = s->get_function_def();
        if (!def.name.empty()) {
            defs.push_back(std::move(def));
        }
    }
    return defs;
}

std::string SkillManager::execute_tool(const std::string& name,
                                       const nlohmann::json& args,
                                       const std::string& user_text)
{
    Skill* skill = find_skill(name);
    if (!skill || !skill->enabled()) {
        return "";
    }
    return skill->execute(user_text, args);
}

Skill* SkillManager::find_skill(const std::string& name)
{
    for (auto& s : skills_) {
        if (s->name() == name) return s.get();
    }
    return nullptr;
}

void SkillManager::set_current_user_memory(UserMemoryStore* store)
{
    for (auto& s : skills_) {
        if (s->name() == "memory") {
            auto* mem_skill = dynamic_cast<MemorySkill*>(s.get());
            if (mem_skill) {
                mem_skill->set_memory_store(store);
            }
            return;
        }
    }
}

// ── Core: hybrid dispatch ────────────────────────────────

SkillResult SkillManager::detect_and_execute(const std::string& user_text)
{
    // ── Strategy 1: Keyword Match (fast, <1ms) ─────────────
    // Run keyword match first — it's instant and covers most daily queries.
    // FC LLM is only used as fallback for ambiguous queries without clear keywords.
    for (auto& s : skills_) {
        if (!s->enabled()) continue;

        if (s->match(user_text)) {
            std::string result = s->execute(user_text);
            if (!result.empty()) {
                LOG_INFO("[Skill-KW] \"{}\" → keyword match {}", user_text, s->name());
                if (s->name() == "memory") memory_modified_ = true;
                return {true, s->is_direct_response(), s->name(), result};
            }
        }
    }

    // ── Strategy 2: Function Calling (LLM-driven, fallback) ─
    // Only call FC LLM if no keyword matched. This avoids the ~400ms FC overhead
    // for common queries like "你好", "几点了", "天气怎么样", etc.
    if (function_caller_ && fc_enabled_) {
        auto func_defs = collect_function_defs();
        if (!func_defs.empty()) {
            ToolDecision td = function_caller_->decide(user_text, func_defs);

            if (td.use_tool) {
                Skill* skill = find_skill(td.tool_name);
                if (skill && skill->enabled()) {
                    std::string result = skill->execute(user_text, td.arguments);
                    if (!result.empty()) {
                        LOG_INFO("[Skill-FC] \"{}\" → LLM selected {}", user_text, td.tool_name);
                        if (td.tool_name == "memory") memory_modified_ = true;
                        return {true, skill->is_direct_response(), td.tool_name, result};
                    }
                } else {
                    LOG_INFO("[Skill-FC] LLM selected unknown tool: {}, ignoring",
                             td.tool_name);
                }
            }
        }
    }

    return {false, "", ""};
}

std::string SkillManager::get_system_context()
{
    std::time_t now = std::time(nullptr);
    std::tm* local = std::localtime(&now);

    static const char* weekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday"
    };

    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "Current time: %d-%02d-%02d %02d:%02d (%s)",
        local->tm_year + 1900,
        local->tm_mon + 1,
        local->tm_mday,
        local->tm_hour,
        local->tm_min,
        weekdays[local->tm_wday]);

    return std::string(buf);
}
