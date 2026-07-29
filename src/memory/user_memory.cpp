/**
 * 用户长期记忆存储 — 实现
 */

#include "memory/user_memory.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include "utils/logger.h"

// ── ID 生成 ────────────────────────────────────────

std::string UserMemoryStore::gen_id()
{
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return "mem_" + std::to_string(ms);
}

// ── 添加记忆 ───────────────────────────────────────

void UserMemoryStore::add_memory(const std::string& content)
{
    add_memory(content, {});
}

void UserMemoryStore::add_memory(const std::string& content,
                                  const std::vector<float>& embedding)
{
    // 去重: 检查是否已有相同内容
    for (const auto& e : entries_) {
        if (e.content == content) {
            LOG_INFO("[UserMemory] duplicate memory, skip: \"{}\"", content);
            return;
        }
    }

    MemoryEntry entry;
    entry.id         = gen_id();
    entry.content    = content;
    entry.embedding  = embedding;

    // ISO 时间戳
    auto t = std::time(nullptr);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
    entry.created_at = oss.str();

    entries_.push_back(entry);

    // 如果有 embedding，也加入向量存储
    if (!embedding.empty()) {
        store_.add(content, embedding);
    }

    LOG_INFO("[UserMemory] remembered: \"{}\" (total {})", content, entries_.size());
}

// ── 语义搜索 ───────────────────────────────────────

std::vector<SearchResult> UserMemoryStore::search(
    const std::vector<float>& query_emb, int k, float min_score) const
{
    if (query_emb.empty()) return {};
    return store_.search(query_emb, k, min_score);
}

// ── 关键字搜索 ─────────────────────────────────────

std::vector<std::string> UserMemoryStore::search_by_keyword(
    const std::string& keyword, int max_results) const
{
    std::vector<std::string> results;

    // 简单包含匹配（UTF-8 友好：直接字符串查找）
    std::string kw_lower = keyword;
    std::transform(kw_lower.begin(), kw_lower.end(), kw_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& e : entries_) {
        std::string content_lower = e.content;
        std::transform(content_lower.begin(), content_lower.end(),
                       content_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (content_lower.find(kw_lower) != std::string::npos) {
            results.push_back(e.content);
            if ((int)results.size() >= max_results) break;
        }
    }

    return results;
}

// ── 上下文格式化 ───────────────────────────────────

std::string UserMemoryStore::get_all_as_context() const
{
    if (entries_.empty()) return "";

    std::string result = "关于用户的信息（长期记忆）:\n";
    for (const auto& e : entries_) {
        result += "- " + e.content + "\n";
    }
    return result;
}

// ── 删除 / 清空 ────────────────────────────────────

bool UserMemoryStore::remove_by_content(const std::string& content)
{
    auto it = std::find_if(entries_.begin(), entries_.end(),
        [&content](const MemoryEntry& e) { return e.content == content; });

    if (it != entries_.end()) {
        entries_.erase(it);
        // 重建向量存储（简单方案：清空重建）
        store_.clear();
        for (const auto& e : entries_) {
            if (!e.embedding.empty()) {
                store_.add(e.content, e.embedding);
            }
        }
        LOG_INFO("[UserMemory] deleted: \"{}\"", content);
        return true;
    }
    return false;
}

void UserMemoryStore::clear()
{
    entries_.clear();
    store_.clear();
}

// ── 持久化 ────────────────────────────────────────

bool UserMemoryStore::save_to_file(const std::string& path) const
{
    nlohmann::json j;
    nlohmann::json facts = nlohmann::json::array();

    for (const auto& e : entries_) {
        nlohmann::json entry;
        entry["id"]         = e.id;
        entry["content"]    = e.content;
        entry["created_at"] = e.created_at;

        // embedding 序列化
        if (!e.embedding.empty()) {
            nlohmann::json emb = nlohmann::json::array();
            for (float v : e.embedding) {
                emb.push_back(v);
            }
            entry["embedding"] = emb;
        }
        facts.push_back(entry);
    }

    j["facts"] = facts;

    try {
        std::ofstream f(path);
        if (!f) return false;
        f << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

bool UserMemoryStore::load_from_file(const std::string& path)
{
    try {
        std::ifstream f(path);
        if (!f) return false;

        nlohmann::json j = nlohmann::json::parse(f);

        if (j.contains("facts") && j["facts"].is_array()) {
            for (const auto& entry : j["facts"]) {
                MemoryEntry e;
                e.id         = entry.value("id", gen_id());
                e.content    = entry.value("content", "");
                e.created_at = entry.value("created_at", "");

                // embedding 反序列化
                if (entry.contains("embedding") && entry["embedding"].is_array()) {
                    for (const auto& v : entry["embedding"]) {
                        e.embedding.push_back(v.get<float>());
                    }
                    store_.add(e.content, e.embedding);
                }

                if (!e.content.empty()) {
                    entries_.push_back(e);
                }
            }
        }

        LOG_INFO("[UserMemory] loaded {} long-term memories from file", entries_.size());
        return true;
    } catch (...) {
        LOG_WARN("[UserMemory] failed to load memory file: {}", path);
        return false;
    }
}
