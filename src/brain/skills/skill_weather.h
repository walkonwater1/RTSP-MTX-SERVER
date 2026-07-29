#pragma once
/**
 * 天气查询技能 — 通过 wttr.in API（免费，无需注册）
 */

#include "brain/skill_base.h"

class WeatherSkill : public Skill {
public:
    WeatherSkill() : Skill("weather") {}

    bool match(const std::string& text) override;
    std::string execute(const std::string& text) override;
    std::string execute(const std::string& text,
                        const nlohmann::json& args) override;

    std::string describe() const override {
        return "你可以查询天气，当用户问到天气相关问题时直接回答，不要说你不能查。";
    }

    FunctionDef get_function_def() const override {
        FunctionDef def;
        def.name = "weather";
        def.description = "查询指定城市的实时天气信息（温度、湿度、风力等）";
        def.parameters = nlohmann::json::parse(R"({
            "type": "object",
            "properties": {
                "city": {
                    "type": "string",
                    "description": "城市名称（中文），如北京、上海、广州"
                }
            },
            "required": ["city"]
        })");
        return def;
    }

private:
    /// 从文本中提取城市名
    static std::string extract_city(const std::string& text);
};
