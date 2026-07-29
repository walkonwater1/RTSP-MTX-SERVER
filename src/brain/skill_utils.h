#pragma once
/**
 * 技能工具函数 — 所有 Skill 和 SkillManager 共享
 */

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

inline std::string to_lower(const std::string& s)
{
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return r;
}

inline bool contains_any(const std::string& text,
                         const std::vector<std::string>& keywords)
{
    std::string lower = to_lower(text);
    for (auto& kw : keywords) {
        if (lower.find(to_lower(kw)) != std::string::npos)
            return true;
    }
    return false;
}
