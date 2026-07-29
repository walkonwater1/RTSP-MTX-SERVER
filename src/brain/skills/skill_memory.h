#pragma once
/**
 * 记忆技能 — 处理用户的长期记忆指令
 *
 * 支持:
 *   1. 存储: "记住XXX", "我叫XXX", "别忘了XXX"
 *   2. 查询: "你记得XXX吗?", "回忆一下XXX", "我的XXX是什么?"
 *   3. 列出: "我有哪些记忆?", "你记住什么了?"
 *   4. 删除: "忘记XXX", "删除记忆XXX"
 *
 * 依赖: UserMemoryStore（长期记忆存储）
 */

#include "brain/skill_base.h"
#include "memory/user_memory.h"
#include <string>

class MemorySkill : public Skill {
public:
    /// @param memory_store 长期记忆存储指针（由 VoicePipeline 持有）
    explicit MemorySkill(UserMemoryStore* memory_store);

    bool match(const std::string& text) override;
    std::string execute(const std::string& text) override;

    std::string describe() const override {
        return "你可以帮助用户管理长期记忆。当用户说'记住XXX'、'我叫XXX'时，存储信息。"
               "当用户问'你记得XXX吗'时，帮助查找相关记忆。";
    }

    FunctionDef get_function_def() const override;

private:
    UserMemoryStore* memory_;  // 非拥有，由 VoicePipeline 管理生命周期

    /// 从文本中提取要记住的内容
    /// "记住我喜欢吃川菜" → "用户喜欢吃川菜"
    /// "我叫张三" → "用户的名字是张三"
    static std::string extract_fact(const std::string& text);

    /// 从文本中提取查询关键词
    /// "你记得我喜欢吃什么吗" → "喜欢吃什么"
    static std::string extract_query(const std::string& text);
};
