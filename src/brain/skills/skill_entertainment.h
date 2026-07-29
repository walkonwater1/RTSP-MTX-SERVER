#pragma once
/**
 * 娱乐技能 — 内置笑话/故事/趣味知识库
 *
 * 用法:
 *   "讲个笑话" / "说个段子"
 *   "我要听个故事" / "有没有睡前故事"
 *   "说个有趣的事" / "来点冷知识"
 */

#include "brain/skill_base.h"

class EntertainmentSkill : public Skill {
public:
    EntertainmentSkill() : Skill("entertainment") {}

    bool match(const std::string& text) override;
    std::string execute(const std::string& text) override;
    std::string execute(const std::string& text,
                        const nlohmann::json& args) override;
    bool is_direct_response() const override { return true; }

    std::string describe() const override {
        return "你可以讲笑话、段子、小故事和趣味冷知识。";
    }

    FunctionDef get_function_def() const override {
        FunctionDef def;
        def.name = "entertainment";
        def.description = "讲笑话、段子、小故事、睡前故事、趣味冷知识";
        def.parameters = nlohmann::json::parse(R"({
            "type": "object",
            "properties": {
                "type": {
                    "type": "string",
                    "enum": ["joke", "story", "fact", "soup"],
                    "description": "类型: joke笑话/段子, story故事/睡前故事, fact冷知识/趣闻, soup毒鸡汤"
                }
            }
        })");
        return def;
    }

private:
    static std::string random_joke();
    static std::string random_story();
    static std::string random_fact();
    static std::string random_soup();
    static std::string pick_random(const std::vector<std::string>& items);
};
