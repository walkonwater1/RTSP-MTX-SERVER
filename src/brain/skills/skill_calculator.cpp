#include "utils/logger.h"
/**
 * 计算器技能 — 实现
 *
 * 中文数学表达式 → 算术求值
 */

#include "skill_calculator.h"
#include "brain/skill_utils.h"

#include <iostream>
#include <sstream>
#include <cmath>
#include <cctype>
#include <cstdlib>

// ── 关键词匹配 ──────────────────────────────────────────

bool CalculatorSkill::match(const std::string& text)
{
    static const std::vector<std::string> keywords = {
        "计算", "等于多少", "是多少",
        "乘以", "除以", "加上", "减去",
        "加", "减", "乘", "除",
        "平方", "开方", "几次方", "百分之",
        "×", "÷", "＋", "－",
        "+", "-", "*", "/"
    };

    // 额外检查：包含至少一个数字 + 运算符模式
    bool has_digit = false;
    for (char c : text) {
        if (c >= '0' && c <= '9') { has_digit = true; break; }
    }
    if (!has_digit) return false;

    return contains_any(text, keywords);
}

// ── 中文表达式转换 ──────────────────────────────────────

std::string CalculatorSkill::chinese_to_expr(const std::string& text)
{
    std::string out;
    out.reserve(text.size());

    // 先按字符扫描，替换中文运算符为符号
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);

        if (c <= 0x7F) {
            // ASCII：保留数字、运算符、小数点、括号、空格
            if (std::isdigit(c) || c == '+' || c == '-' || c == '*' || c == '/' ||
                c == '.' || c == '(' || c == ')' || c == '^' || c == '%' ||
                c == ' ' || c == '×' || c == '÷') {
                if (c == '×') out += '*';
                else if (c == '÷') out += '/';
                else out += (char)c;
            }
            ++i;
        } else {
            // 多字节字符（中文等）：检查是否为运算符
            int clen = 1;
            if ((c & 0xE0) == 0xC0) clen = 2;
            else if ((c & 0xF0) == 0xE0) clen = 3;
            else if ((c & 0xF8) == 0xF0) clen = 4;

            if (i + clen <= text.size()) {
                std::string ch = text.substr(i, clen);

                // 数字转换（中文数字）
                if (ch == "一") out += "1";
                else if (ch == "二" || ch == "两") out += "2";
                else if (ch == "三") out += "3";
                else if (ch == "四") out += "4";
                else if (ch == "五") out += "5";
                else if (ch == "六") out += "6";
                else if (ch == "七") out += "7";
                else if (ch == "八") out += "8";
                else if (ch == "九") out += "9";
                else if (ch == "零") out += "0";
                else if (ch == "十") out += "10";
                else if (ch == "百") out += "100";

                // 运算符
                else if (ch == "加" || ch == "加上") { out += '+'; if (clen==2) i = i; }
                else if (ch == "减" || ch == "减去") out += '-';
                else if (ch == "乘" || ch == "乘以") out += '*';
                else if (ch == "除" || ch == "除以") out += '/';
            }
            i += clen;
        }
    }

    // 二次处理: 替换文本级中文运算符（可能在第一步被跳过）
    struct Repl { const char* from; const char* to; };
    static const Repl repls[] = {
        {"乘以", "*"}, {"除以", "/"}, {"加上", "+"}, {"减去", "-"},
        {"加", "+"}, {"减", "-"}, {"乘", "*"}, {"除", "/"},
        {"×", "*"}, {"÷", "/"},
        {nullptr, nullptr}
    };
    for (const Repl* r = repls; r->from; ++r) {
        size_t pos = 0;
        while ((pos = out.find(r->from, pos)) != std::string::npos) {
            out.replace(pos, strlen(r->from), r->to);
            pos += strlen(r->to);
        }
    }

    return out;
}

// ── 安全表达式求值 ──────────────────────────────────────

double CalculatorSkill::safe_eval(const std::string& expr, bool& ok)
{
    ok = false;

    // 安全检查：只允许数字、运算符、括号、空格、点
    for (char c : expr) {
        if (std::isdigit(static_cast<unsigned char>(c))) continue;
        if (c == '+' || c == '-' || c == '*' || c == '/' ||
            c == '(' || c == ')' || c == '.' || c == '^' ||
            c == '%' || c == ' ') continue;
        return 0;  // 非法字符
    }

    // 简单表达式求值器（两遍：先乘除，后加减）
    // 支持: + - * / % ^ ( )
    std::string e = expr;
    // 去掉所有空格
    e.erase(std::remove(e.begin(), e.end(), ' '), e.end());
    if (e.empty()) return 0;

    // 防止除零
    if (e.find("/0") != std::string::npos) return 0;

    // 使用 popen 调用 python3 做安全求值（最简单可靠）
    std::string cmd = "python3 -c \"print(" + e + ")\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return 0;

    char buf[128];
    std::string result;
    if (fgets(buf, sizeof(buf), fp)) {
        result = buf;
    }
    pclose(fp);

    if (result.empty()) return 0;

    try {
        double val = std::stod(result);
        ok = true;
        return val;
    } catch (...) {
        return 0;
    }
}

// ── 格式化输出 ──────────────────────────────────────────

static std::string format_number(double d)
{
    if (d == std::floor(d) && std::abs(d) < 1e15) {
        // 整数，避免科学计数法
        return std::to_string((long long)d);
    }
    // 保留合理精度
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6g", d);
    return buf;
}

// ── 执行入口 ────────────────────────────────────────────

std::string CalculatorSkill::execute(const std::string& text)
{
    return execute(text, nlohmann::json::object());
}

std::string CalculatorSkill::execute(const std::string& text,
                                      const nlohmann::json& args)
{
    std::string expression;

    if (args.contains("expression") && args["expression"].is_string()) {
        expression = args["expression"].get<std::string>();
    } else {
        expression = chinese_to_expr(text);
    }

    std::cout << "   [Skill:计算] 表达式: " << expression << std::endl;

    // 如果表达式里只有中文（没有数字），降级处理
    bool has_digit = false;
    for (char c : expression) {
        if (std::isdigit(static_cast<unsigned char>(c))) { has_digit = true; break; }
    }
    if (!has_digit) {
        return "我没看懂要算什么，能再说一遍吗？比如「25乘以38」。";
    }

    bool ok = false;
    double result = safe_eval(expression, ok);

    if (!ok) {
        return "这道题太难了，我算不出来。";
    }

    if (std::isnan(result) || std::isinf(result)) {
        return "计算结果无效，可能除零了。";
    }

    // Build a TTS-friendly Chinese sentence instead of raw symbols
    // e.g. 25*38=950 → "25乘以38等于950"
    std::string readable;
    readable.reserve(expression.size() * 3);
    for (char c : expression) {
      switch (c) {
        case '+': readable += "加"; break;
        case '-': readable += "减"; break;
        case '*': readable += "乘以"; break;
        case '/': readable += "除以"; break;
        case '%': readable += "百分之"; break;
        case '^': readable += "的"; break;   // "2的3次方" — 实际由表达式决定
        default:  readable += c; break;
      }
    }
    readable += "等于";
    readable += format_number(result);
    return readable;
}
