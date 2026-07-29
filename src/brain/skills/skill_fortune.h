#pragma once
/**
 * 运势占卜 — 星座运势 / 塔罗占卜 / 今日黄历
 */

#include "brain/skill_base.h"

class FortuneSkill : public Skill {
public:
    FortuneSkill() : Skill("fortune") {}

    bool match(const std::string& text) override;
    std::string execute(const std::string& text) override;
    std::string execute(const std::string& text,
                        const nlohmann::json& args) override;
    bool is_direct_response() const override { return true; }

    std::string describe() const override {
        return "你可以查星座运势、塔罗占卜、今日黄历。";
    }

    FunctionDef get_function_def() const override {
        FunctionDef def;
        def.name = "fortune";
        def.description = "星座运势、塔罗占卜、今日黄历";
        def.parameters = nlohmann::json::parse(R"({
            "type": "object",
            "properties": {
                "type": {
                    "type": "string",
                    "enum": ["zodiac", "tarot", "almanac"],
                    "description": "zodiac星座运势, tarot塔罗占卜, almanac黄历"
                },
                "sign": {
                    "type": "string",
                    "description": "星座名（zodiac时）: 白羊/金牛/双子/巨蟹/狮子/处女/天秤/天蝎/射手/摩羯/水瓶/双鱼"
                }
            }
        })");
        return def;
    }

private:
    static std::string zodiac_fortune(const std::string& sign);
    static std::string tarot_reading();
    static std::string daily_almanac();
    static std::string pick_random(const std::vector<std::string>& items);
    static int today_seed();
};
