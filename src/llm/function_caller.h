#pragma once
/**
 * Function Calling — LLM 驱动的工具选择
 *
 * 核心思想（Agent 演进的关键一步）:
 *   之前: 关键字匹配 → "冷" 不匹配 "天气" → 漏判
 *   之后: LLM 理解语义 → "外面冷不冷" → 语义理解 → 选 weather 工具
 *
 * 工作流程:
 *   1. 收集所有 Skill 的 FunctionDef (name + description + JSON Schema)
 *   2. 构建 function calling prompt，发送给 LLM
 *   3. LLM 返回 {"tool": "xxx", "args": {...}} 或 {"tool": null}
 *   4. 解析 JSON → ToolDecision
 *
 * 注意:
 *   - 需要模型有一定的指令遵循能力（qwen2.5:1.5b+ 即可）
 *   - 0.5b 模型可能输出格式不稳定，SkillManager 会自动降级到关键字匹配
 *   - 每次工具选择都是一次独立的 LLM 调用（增加 ~1s 延迟）
 *   - 使用 LLMBackend 抽象接口，兼容 Ollama / TensorRT 等多种后端
 */

#include <memory>
#include <string>
#include <vector>

#include "brain/skill_base.h"  // FunctionDef
#include "llm/llm_backend.h"

// ── 工具选择结果 ──────────────────────────────────────

struct ToolDecision {
    bool        use_tool  = false;   // 是否需要调用工具
    std::string tool_name;           // 选中的工具名
    nlohmann::json arguments;        // LLM 提取的参数 (JSON)
};

// ── FunctionCaller ─────────────────────────────────────

class FunctionCaller {
public:
    /// @param backend  LLM backend instance (Ollama / TensorRT / ...).
    ///                 The caller chooses the model (fc_model) and creates
    ///                 the appropriate backend before passing it here.
    explicit FunctionCaller(std::shared_ptr<rtsp_server::LLMBackend> backend);

    /// 让 LLM 决定调用哪个工具
    /// @param user_message  用户输入的原始文本
    /// @param tools         所有可用工具的函数定义
    /// @return 工具选择决策（可能 use_tool=false 表示无需工具）
    ToolDecision decide(const std::string& user_message,
                        const std::vector<FunctionDef>& tools);

private:
    std::shared_ptr<rtsp_server::LLMBackend> backend_;

    /// 构建 function calling 的 system prompt
    static std::string build_system_prompt(
        const std::vector<FunctionDef>& tools);

    /// 构建用户消息（告诉 LLM 要做什么）
    static std::string build_user_message(const std::string& user_input);
};
