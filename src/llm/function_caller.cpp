#include "utils/logger.h"
/**
 * Function Calling — LLM 驱动的工具选择（实现）
 *
 * 学习要点:
 *   1. Function Calling 是 Agent 的核心能力 — LLM 不再只是"聊天"，而是能"决策"
 *   2. Prompt Engineering 是关键 — 需要清晰告诉 LLM 输出格式
 *   3. 容错设计 — JSON 解析失败、LLM 返回非法格式时优雅降级
 *
 * Ollama API:
 *   POST /api/chat
 *   {
 *     "model": "qwen2.5:3b",
 *     "messages": [
 *       {"role": "system", "content": "..."},
 *       {"role": "user", "content": "..."}
 *     ],
 *     "stream": false,
 *     "options": {"temperature": 0}  // 低温 → 更确定性的输出
 *   }
 */

#include "llm/function_caller.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <sstream>

using json = nlohmann::json;

// ── curl 回调 ─────────────────────────────────────────

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* str = static_cast<std::string*>(userdata);
    str->append(ptr, size * nmemb);
    return size * nmemb;
}

// ── 构造 ──────────────────────────────────────────────

FunctionCaller::FunctionCaller(const std::string& ollama_host,
                               const std::string& model)
    : host_(ollama_host)
    , model_(model)
{}

// ── 核心：工具选择 ────────────────────────────────────

ToolDecision FunctionCaller::decide(const std::string& user_message,
                                    const std::vector<FunctionDef>& tools)
{
    if (tools.empty()) {
        return {false, "", {}};
    }

    // 1. 构建 messages
    json messages = json::array();

    messages.push_back({
        {"role", "system"},
        {"content", build_system_prompt(tools)}
    });

    messages.push_back({
        {"role", "user"},
        {"content", build_user_message(user_message)}
    });

    // 2. 构建请求体
    json body;
    body["model"] = model_;
    body["messages"] = messages;
    body["stream"] = false;
    body["options"] = {{"temperature", 0.0}};  // 低温 → 更稳定
    body["keep_alive"] = -1;  // keep model loaded

    // 3. 发送请求
    std::string response;
    try {
        response = http_post(body.dump());
    } catch (const std::exception& e) {
        std::cerr << "   [FunctionCaller] HTTP 错误: " << e.what() << std::endl;
        return {false, "", {}};
    }

    // 4. 解析响应
    try {
        json resp = json::parse(response);
        std::string content = resp.value("message", json::object())
                                   .value("content", "");

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

// ── HTTP POST ─────────────────────────────────────────

std::string FunctionCaller::http_post(const std::string& request_json) const
{
    std::string url = host_ + "/api/chat";
    std::string response;

    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init 失败");
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request_json.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec_);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("curl 错误: ") + curl_easy_strerror(res));
    }

    return response;
}
