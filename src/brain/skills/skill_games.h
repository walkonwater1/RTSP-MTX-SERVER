#pragma once
/**
 * 小游戏 — 猜数字 / 成语接龙
 *
 * 游戏状态存储在 ~/.voice_notes/game_state.json
 */

#include "brain/skill_base.h"

class GamesSkill : public Skill {
public:
    GamesSkill() : Skill("games") {}

    bool match(const std::string& text) override;
    std::string execute(const std::string& text) override;
    std::string execute(const std::string& text,
                        const nlohmann::json& args) override;
    bool is_direct_response() const override { return true; }

    std::string describe() const override {
        return "你可以和用户玩小游戏：猜数字、成语接龙。";
    }

    FunctionDef get_function_def() const override {
        FunctionDef def;
        def.name = "games";
        def.description = "猜数字游戏或成语接龙";
        def.parameters = nlohmann::json::parse(R"({
            "type": "object",
            "properties": {
                "game": {
                    "type": "string",
                    "enum": ["guess", "idiom"],
                    "description": "guess猜数字, idiom成语接龙"
                },
                "guess_number": {
                    "type": "integer",
                    "description": "猜数字时用户猜的数"
                }
            }
        })");
        return def;
    }

private:
    static std::string game_state_path();

    // 猜数字
    std::string guess_number(const std::string& user_text,
                             const nlohmann::json& args);

    // 成语接龙 (简化版：AI先出一个成语)
    std::string idiom_chain();
};
