/**
 * 对话记忆管理 — Token 感知 + O(1) 截断 + Skill 事实持久化
 */

#include "memory/chat_memory.h"
#include "memory/token_counter.h"
#include "utils/logger.h"
#include <algorithm>

ChatMemory::ChatMemory(int max_rounds, int max_tokens)
    : max_rounds_(max_rounds)
    , max_tokens_(max_tokens)
{}

void ChatMemory::add(const std::string& user_msg, const std::string& assistant_msg)
{
    add(user_msg, assistant_msg, "");
}

void ChatMemory::add(const std::string& user_msg, const std::string& assistant_msg,
                     const std::string& skill_fact)
{
    int tokens = TokenCounter::estimate(user_msg)
               + TokenCounter::estimate(assistant_msg)
               + TokenCounter::estimate(skill_fact);

    history_.push_back({user_msg, assistant_msg, skill_fact, tokens});
    total_tokens_ += tokens;

    trim();

    // 可观测: 显示上下文窗口使用率
    LOG_DEBUG("[Memory] {} rounds, ~{}/{} tokens ({:.0f}%)",
              size(), total_tokens_, max_tokens_, usage() * 100);
}

std::string ChatMemory::get_context() const
{
    if (history_.empty()) return "";

    std::string result;
    for (const auto& e : history_) {
        if (!e.skill_fact.empty()) {
            result += "[已知信息] " + e.skill_fact + "\n";
        }
        result += "User: " + e.user + "\n";
        result += "Assistant: " + e.assistant + "\n";
    }
    return result;
}

std::vector<ChatMessage> ChatMemory::get_messages() const
{
    std::vector<ChatMessage> msgs;
    if (history_.empty()) return msgs;

    for (const auto& e : history_) {
        // 如果本轮有 skill 事实，先作为一个 system 消息注入
        if (!e.skill_fact.empty()) {
            msgs.push_back({"system",
                "[已知信息] " + e.skill_fact});
        }
        msgs.push_back({"user", e.user});
        msgs.push_back({"assistant", e.assistant});
    }
    return msgs;
}

void ChatMemory::clear()
{
    history_.clear();
    total_tokens_ = 0;
}

void ChatMemory::set_limits(int max_rounds, int max_tokens)
{
    max_rounds_ = max_rounds;
    max_tokens_ = max_tokens;
    trim();
}

void ChatMemory::pop_front()
{
    if (history_.empty()) return;
    total_tokens_ -= history_.front().tokens;
    history_.pop_front();
}

void ChatMemory::trim()
{
    // 1) 限制轮数
    while ((int)history_.size() > max_rounds_) {
        pop_front();
    }

    // 2) 限制 token 数 — O(1) 判断 + 从队头逐轮弹出
    while (total_tokens_ > max_tokens_ && !history_.empty()) {
        pop_front();
    }
}

// ── 持久化 ────────────────────────────────────────

#include <fstream>
#include <nlohmann/json.hpp>

bool ChatMemory::save_to_file(const std::string& path) const
{
    nlohmann::json j;
    nlohmann::json rounds = nlohmann::json::array();

    for (const auto& e : history_) {
        nlohmann::json entry;
        entry["user"]       = e.user;
        entry["assistant"]  = e.assistant;
        entry["skill_fact"] = e.skill_fact;
        entry["tokens"]     = e.tokens;
        rounds.push_back(entry);
    }

    j["max_rounds"] = max_rounds_;
    j["max_tokens"] = max_tokens_;
    j["rounds"]     = rounds;

    try {
        std::ofstream f(path);
        if (!f) return false;
        f << j.dump(2);  // 缩进 2 空格，方便人工查看
        return true;
    } catch (...) {
        return false;
    }
}

bool ChatMemory::load_from_file(const std::string& path)
{
    try {
        std::ifstream f(path);
        if (!f) return false;

        nlohmann::json j = nlohmann::json::parse(f);

        // 恢复限制参数
        if (j.contains("max_rounds")) max_rounds_ = j["max_rounds"].get<int>();
        if (j.contains("max_tokens")) max_tokens_ = j["max_tokens"].get<int>();

        // 加载对话轮次
        if (j.contains("rounds") && j["rounds"].is_array()) {
            for (const auto& entry : j["rounds"]) {
                Entry e;
                e.user       = entry.value("user", "");
                e.assistant  = entry.value("assistant", "");
                e.skill_fact = entry.value("skill_fact", "");
                e.tokens     = entry.value("tokens", 0);
                history_.push_back(e);
                total_tokens_ += e.tokens;
            }
        }

        // 加载后裁剪（如果限制变了）
        trim();

        LOG_DEBUG("[Memory] loaded {} rounds (~{} tokens) from file",
                  size(), total_tokens_);
        return true;
    } catch (...) {
        LOG_WARN("[Memory] failed to load memory file: {}", path);
        return false;
    }
}
