#pragma once
/**
 * 计算器技能 — 中文数学表达式解析
 *
 * 用法:
 *   "25乘以38等于多少"
 *   "100除以3"
 *   "256加128再减50"
 */

#include "brain/skill_base.h"

class CalculatorSkill : public Skill {
public:
    CalculatorSkill() : Skill("calculator") {}

    bool match(const std::string& text) override;
    std::string execute(const std::string& text) override;
    std::string execute(const std::string& text,
                        const nlohmann::json& args) override;
    std::string describe() const override {
        return "你可以做数学计算。例如用户问「25乘以38是多少」时调用。";
    }

    FunctionDef get_function_def() const override {
        FunctionDef def;
        def.name = "calculator";
        def.description = "执行数学计算（加减乘除、平方、开方等）";
        def.parameters = nlohmann::json::parse(R"({
            "type": "object",
            "properties": {
                "expression": {
                    "type": "string",
                    "description": "数学表达式，如 25*38+100"
                }
            },
            "required": ["expression"]
        })");
        return def;
    }

private:
    /// 将中文数学表达式转为算术表达式
    /// "25乘以38" → "25*38", "100除以3" → "100/3"
    static std::string chinese_to_expr(const std::string& text);

    /// 安全求值（仅限数字和 +-*/%().^ 运算符）
    static double safe_eval(const std::string& expr, bool& ok);
};
