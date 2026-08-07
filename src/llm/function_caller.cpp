/**
 * Function Calling — LLM 驱动的工具选择（实现）
 *
 * 重构说明:
 *   原先直接使用 curl HTTP POST 调用 Ollama /api/chat。
 *   现在通过 LLMBackend 抽象接口，可以无缝切换后端。
 *
 *   注意: Function Calling 场景需要 temperature=0（确定性输出），
 *   当前 LLMBackend 接口暂未暴露 temperature 参数（使用后端默认值）。
 *   若后端默认 temperature 偏高导致 JSON 输出不稳定，可考虑:
 *     1. 在 LLMBackend 接口增加 ChatParams 结构体
 *     2. 或使用专门的 fc_model（小参数 + 低温度配置）
 */

#include "llm/function_caller.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <sstream>

#include "utils/logger.h"

using json = nlohmann::json;

// ── 构造 ──────────────────────────────────────────────

FunctionCaller::FunctionCaller(std::shared_ptr<rtsp_server::LLMBackend> backend)
    : backend_(std::move(backend))
{}

// ── 核心：工具选择 ────────────────────────────────────

ToolDecision FunctionCaller::decide(const std::string& user_message,
                                    const std::vector<FunctionDef>& tools)
{
    if (tools.empty()) {
        return {false, "", {}};
    }

    if (!backend_ || !backend_->IsAvailable()) {
        std::cerr << "   [FunctionCaller] LLM backend not available" << std::endl;
        return {false, "", {}};
    }

    // 1. 构建 system prompt + user message
    std::string system_prompt = build_system_prompt(tools);
    std::string user_prompt   = build_user_message(user_message);

    // 2. 通过 LLMBackend 发送 Chat 请求
    //    (无对话历史，无 skill context，无取消)
    std::string content;
    try {
        content = backend_->Chat(system_prompt, user_prompt,
                                  {} /* no history */,
                                  "" /* no skill context */,
                                  nullptr /* no cancel */);
    } catch (const std::exception& e) {
        std::cerr << "   [FunctionCaller] LLM 调用异常: " << e.what() << std::endl;
        return {false, "", {}};
    }

    if (content.empty()) {
        std::cerr << "   [FunctionCaller] LLM 返回空响应" << std::endl;
        return {false, "", {}};
    }

    // 3. 解析响应（与旧实现相同逻辑）
    try {
        // LLM 输出可能包含 markdown 代码块，先清洗
        // 尝试提取 JSON 部分
        std::string json_str;
        size_t brace_start = content.find('{');
        size_t brace_end   = content.rfind('}');
        if (brace_start != std::string::npos &&
            brace_end   != std::string::npos &&
            brace_end   > brace_start) {
            json_str = content.substr(brace_start, brace_end - brace_start + 1);
        } else {
            json_str = content;
        }

        json decision = json::parse(json_str);

        // 处理 tool 字段：可能是 null（无需工具）或 string（工具名）
        if (!decision.contains("tool") || decision["tool"].is_null()) {
            return {false, "", {}};
        }

        std::string tool;
        try {
            tool = decision["tool"].get<std::string>();
        } catch (...) {
            return {false, "", {}};
        }

        if (tool.empty()) {
            return {false, "", {}};
        }

        ToolDecision result;
        result.use_tool  = true;
        result.tool_name = tool;
        result.arguments = decision.value("args", json::object());

        std::cout << "   [FunctionCaller] ✅ LLM 选择了工具: " << tool
                  << " args=" << result.arguments.dump() << std::endl;

        return result;

    } catch (const std::exception& e) {
        std::cerr << "   [FunctionCaller] ⚠️ JSON 解析失败: " << e.what() << std::endl;
        return {false, "", {}};
    }
}

// ── Prompt 构建 ───────────────────────────────────────

std::string FunctionCaller::build_system_prompt(
    const std::vector<FunctionDef>& tools)
{
    std::ostringstream ss;

    ss << "你是一个工具选择助手。你的任务是判断用户输入是否需要调用外部工具。\n\n";

    // 列出所有可用工具
    ss << "可用工具列表:\n";
    for (size_t i = 0; i < tools.size(); ++i) {
        auto& t = tools[i];
        ss << "\n--- 工具 #" << (i + 1) << " ---\n";
        ss << "函数名: " << t.name << "\n";
        ss << "描述: " << t.description << "\n";
        if (!t.parameters.empty()) {
            ss << "参数定义:\n";
            ss << t.parameters.dump(2) << "\n";
        }
    }

    ss << "\n--- 输出规则 ---\n";
    ss << "1. 如果用户输入需要调用某个工具，输出 JSON:\n";
    ss << "   {\"tool\": \"函数名\", \"args\": {\"参数名\": \"参数值\"}}\n";
    ss << "2. 如果用户输入只是普通聊天，不需要工具，输出:\n";
    ss << "   {\"tool\": null}\n";
    ss << "3. 只输出 JSON，不要加任何解释、注释或 markdown 代码块标记。\n";
    ss << "4. 从用户输入中提取参数的具体值，不要猜测。如果用户没说，用默认值。\n";

    return ss.str();
}

std::string FunctionCaller::build_user_message(const std::string& user_input)
{
    return "用户说: \"" + user_input + "\"\n\n请判断需要调用哪个工具，并输出 JSON。";
}
