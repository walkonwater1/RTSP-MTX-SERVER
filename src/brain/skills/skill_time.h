#pragma once
/**
 * 时间/日期查询技能 — 本地 localtime 计算
 */

#include "brain/skill_base.h"

class TimeSkill : public Skill {
public:
    TimeSkill() : Skill("time") {}

    bool match(const std::string& text) override;
    std::string execute(const std::string& text) override;
    std::string describe() const override {
        return "你可以获取当前时间和日期。";
    }

    FunctionDef get_function_def() const override {
        FunctionDef def;
        def.name = "time";
        def.description = "获取当前日期、时间和星期";
        // 无需参数
        def.parameters = nlohmann::json::parse(R"({
            "type": "object",
            "properties": {}
        })");
        return def;
    }
};
