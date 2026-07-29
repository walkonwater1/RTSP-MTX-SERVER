/**
 * 记忆技能 — 实现
 */

#include "skill_memory.h"
#include "brain/skill_utils.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include "utils/logger.h"

MemorySkill::MemorySkill(UserMemoryStore* memory_store)
    : Skill("memory", true)
    , memory_(memory_store)
{}

bool MemorySkill::match(const std::string& text)
{
    static const std::vector<std::string> store_keywords = {
        "记住", "别忘了", "记一下", "记下来", "备忘",
        "我叫", "我是", "我的名字是", "我姓",
        "我喜欢", "我不喜欢", "我讨厌",
        "我家在", "我住在", "我在", "我的电话", "我的生日"
    };

    static const std::vector<std::string> query_keywords = {
        "你记得", "你还记得", "回忆", "我想起来",
        "我叫什么", "我是谁", "我的名字",
        "我喜欢什么", "我是什么", "我有哪些记忆", "你记住什么了"
    };

    static const std::vector<std::string> delete_keywords = {
        "忘记", "删掉记忆", "删除记忆", "清除记忆", "不要再记"
    };

    return contains_any(text, store_keywords)
        || contains_any(text, query_keywords)
        || contains_any(text, delete_keywords);
}

std::string MemorySkill::execute(const std::string& text)
{
    if (!memory_) {
        return "记忆系统未初始化";
    }

    // ── 查询类 ──────────────────────────────────────
    static const std::vector<std::string> query_kw = {
        "你记得", "你还记得", "回忆", "叫什么", "我是谁",
        "我的名字", "我喜欢什么", "我有哪些记忆", "你记住什么了"
    };

    if (contains_any(text, query_kw)) {
        // 特殊：问名字/身份
        if (contains_any(text, {"叫什么", "我是谁", "我的名字", "我的信息"})) {
            std::string ctx = memory_->get_all_as_context();
            if (ctx.empty()) {
                return "我目前还没有关于你的记忆呢。你可以告诉我一些关于你的事情，比如'我叫小明'或'记住我喜欢吃辣的'。";
            }
            return "根据我的记忆:\n" + ctx;
        }

        // 通用查询：提取关键词搜索
        std::string query = extract_query(text);
        if (!query.empty()) {
            auto results = memory_->search_by_keyword(query, 5);
            if (results.empty()) {
                return "我没有找到关于\"" + query + "\"的记忆。";
            }
            std::string reply = "关于\"" + query + "\"的记忆:\n";
            for (const auto& r : results) {
                reply += "- " + r + "\n";
            }
            return reply;
        }
        // 列出所有记忆
        std::string ctx = memory_->get_all_as_context();
        if (ctx.empty()) {
            return "我目前还没有任何关于你的长期记忆。";
        }
        return "根据我的记忆:\n" + ctx;
    }

    // ── 删除类 ──────────────────────────────────────
    static const std::vector<std::string> delete_kw = {
        "忘记", "删掉", "删除", "清除", "不要再记"
    };

    if (contains_any(text, delete_kw)) {
        // 尝试从文本中提取要删除的内容
        std::string to_delete = extract_fact(text);
        if (to_delete.empty()) {
            return "你想让我忘记什么呢？请说清楚要删除的记忆内容。";
        }
        if (memory_->remove_by_content(to_delete)) {
            return "好的，我已经忘掉了关于\"" + to_delete + "\"的记忆。";
        }
        // 模糊搜索
        auto results = memory_->search_by_keyword(to_delete, 1);
        if (!results.empty()) {
            memory_->remove_by_content(results[0]);
            return "好的，我已经忘掉了\"" + results[0] + "\"。";
        }
        return "我没有找到相关的记忆，无法忘记。";
    }

    // ── 存储类 ──────────────────────────────────────
    std::string fact = extract_fact(text);
    if (!fact.empty()) {
        memory_->add_memory(fact);
        return "好的，我记住了！";
    }

    // 兜底：直接存储原文
    memory_->add_memory(text);
    return "好的，我已经记下来了。";
}

FunctionDef MemorySkill::get_function_def() const
{
    FunctionDef def;
    def.name = "memory";
    def.description = "管理用户的长期记忆：存储个人信息、偏好、重要事实，或查询已有记忆";
    def.parameters = nlohmann::json::parse(R"({
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["store", "recall", "list", "forget"],
                "description": "操作类型: store=存储新记忆, recall=查询记忆, list=列出所有记忆, forget=删除记忆"
            },
            "content": {
                "type": "string",
                "description": "要存储的记忆内容（action=store时必填），或要搜索/删除的关键词"
            }
        },
        "required": ["action"]
    })");
    return def;
}

// ── 辅助：从用户输入提取事实 ────────────────────────

std::string MemorySkill::extract_fact(const std::string& text)
{
    // "记住XXX" → "XXX"
    // "别忘了XXX" → "XXX"
    // "我叫XXX" → "用户的名字是XXX"
    // "我是XXX" → "用户的身份是XXX"
    // "我喜欢XXX" → "用户喜欢XXX"
    // "我住在XXX" → "用户住在XXX"
    // "我的电话是XXX" → "用户的电话是XXX"
    // "我的生日是XXX" → "用户的生日是XXX"

    // 模式匹配表: {关键词, 前置描述}
    struct Pattern {
        std::string keyword;
        std::string prefix;   // 记忆描述前缀
    };

    static const std::vector<Pattern> patterns = {
        {"记住",    ""},
        {"别忘了",  ""},
        {"记一下",  ""},
        {"记下来",  ""},
        {"备忘",    ""},
        {"我叫",    "用户的名字是"},
        {"我的名字是", "用户的名字是"},
        {"我是",    "用户的身份是"},
        {"我姓",    "用户姓"},
        {"我喜欢",  "用户喜欢"},
        {"我不喜欢", "用户不喜欢"},
        {"我讨厌",  "用户讨厌"},
        {"我家在",  "用户家在"},
        {"我住在",  "用户住在"},
        {"我在",    "用户在"},
        {"我的电话是", "用户的电话是"},
        {"我的手机是", "用户的手机号是"},
        {"我的生日是", "用户的生日是"},
        {"我的工作是", "用户的工作是"},
        {"我的爱好是", "用户的爱好是"},
        {"我的职业是", "用户的职业是"},
    };

    // 辅助：判断是否是标点/空白字符（UTF-8 安全）
    auto is_punct_or_space = [](const std::string& s, size_t pos) -> int {
        if (pos >= s.size()) return 0;
        unsigned char c = (unsigned char)s[pos];
        // ASCII 标点和空白
        if (c == ' ' || c == ',' || c == '?' || c == '!') return 1;
        // UTF-8 中文标点（3字节序列: E3 80 82 = 。, EF BC 8C = ，, EF BC 81 = ！, EF BC 9F = ？）
        if (c == 0xE3 && pos + 2 < s.size() &&
            (unsigned char)s[pos+1] == 0x80 && (unsigned char)s[pos+2] == 0x82) return 3;
        if (c == 0xEF && pos + 2 < s.size()) {
            unsigned char c1 = (unsigned char)s[pos+1];
            unsigned char c2 = (unsigned char)s[pos+2];
            if (c1 == 0xBC && (c2 == 0x8C || c2 == 0x81 || c2 == 0x9F)) return 3;  // ，！？
        }
        return 0;
    };

    for (const auto& p : patterns) {
        size_t pos = text.find(p.keyword);
        if (pos != std::string::npos) {
            std::string content = text.substr(pos + p.keyword.size());

            // 去除前导标点和空白
            while (!content.empty()) {
                int skip = is_punct_or_space(content, 0);
                if (skip > 0) content.erase(0, skip);
                else break;
            }
            // 去除尾部标点和空白
            while (!content.empty()) {
                int skip = is_punct_or_space(content, content.size() - 1);
                if (skip == 1) content.pop_back();
                else if (skip == 3 && content.size() >= 3) content.erase(content.size() - 3);
                else break;
            }

            if (!content.empty()) {
                if (p.prefix.empty()) {
                    return content;
                } else {
                    return p.prefix + content;
                }
            }
        }
    }

    return "";
}

std::string MemorySkill::extract_query(const std::string& text)
{
    // "你记得XXX吗" → "XXX"
    // "你还记得XXX吗" → "XXX"
    // "回忆一下XXX" → "XXX"
    // "我喜欢什么" → "喜欢"
    // "我的XXX是什么" → "XXX"

    static const std::vector<std::string> prefixes = {
        "你记得", "你还记得", "回忆一下", "回忆"
    };

    for (const auto& p : prefixes) {
        size_t pos = text.find(p);
        if (pos != std::string::npos) {
            std::string query = text.substr(pos + p.size());

            // 去除尾部语气词和标点
            static const std::vector<std::string> suffixes = {
                "吗", "呢", "啊", "吧", "么", "嘛",
                "？", "?", "。", "！", "!", "是什么", "是谁"
            };
            for (const auto& s : suffixes) {
                if (query.size() >= s.size() &&
                    query.substr(query.size() - s.size()) == s) {
                    query = query.substr(0, query.size() - s.size());
                }
            }

            // 去除前后空白
            while (!query.empty() && query.front() == ' ') query.erase(0, 1);
            while (!query.empty() && query.back() == ' ') query.pop_back();

            if (!query.empty()) return query;
        }
    }

    // "我的XXX是什么" 模式
    size_t my_pos = text.find("我的");
    if (my_pos != std::string::npos) {
        size_t is_pos = text.find("是什么", my_pos);
        if (is_pos != std::string::npos) {
            return text.substr(my_pos + 6, is_pos - my_pos - 6);  // 跳过"我的"（UTF-8 6字节）
        }
    }

    return "";
}
