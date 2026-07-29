#pragma once
/**
 * 脑筋急转弯 — 随机出题+揭晓答案
 */

#include "brain/skill_base.h"

class RiddleSkill : public Skill {
public:
    RiddleSkill() : Skill("riddle") {}

    bool match(const std::string& text) override;
    std::string execute(const std::string& text) override;
    bool is_direct_response() const override { return true; }

    std::string describe() const override {
        return "你可以出脑筋急转弯题目。";
    }

    FunctionDef get_function_def() const override {
        FunctionDef def;
        def.name = "riddle";
        def.description = "出一个脑筋急转弯题目";
        def.parameters = nlohmann::json::parse(R"({"type":"object","properties":{}})");
        return def;
    }
};
