#pragma once
/**
 * 唐诗宋词 — 内置诗词库随机推荐
 */

#include "brain/skill_base.h"

class PoetrySkill : public Skill {
public:
    PoetrySkill() : Skill("poetry") {}

    bool match(const std::string& text) override;
    std::string execute(const std::string& text) override;
    std::string execute(const std::string& text,
                        const nlohmann::json& args) override;
    bool is_direct_response() const override { return true; }

    std::string describe() const override {
        return "你可以背诵唐诗宋词。";
    }

    FunctionDef get_function_def() const override {
        FunctionDef def;
        def.name = "poetry";
        def.description = "背诵或推荐一首唐诗宋词";
        def.parameters = nlohmann::json::parse(R"({
            "type": "object",
            "properties": {
                "poet": {
                    "type": "string",
                    "description": "诗人名: 李白/杜甫/苏轼/王维/白居易等"
                }
            }
        })");
        return def;
    }
};
