#pragma once
/**
 * 对话记忆管理 — Token 感知 + O(1) 截断 + Skill 事实持久化
 *
 * Python 对应: src/memory.py → ChatMemory
 *
 * 改进:
 *   1. TokenCounter 启发式估算（中文 ~1.8 token/字, 英文 ~1.3 token/词）
 *   2. total_tokens_ 运行计数 → trim() 从 O(n²) 降到 O(1)
 *   3. stats() 可视化上下文窗口使用率
 *   4. skill_fact 字段: 工具返回的事实数据持久化到记忆，后续轮次可见
 *   5. get_messages(): 返回结构化 user/assistant 交替消息（不再塞进单个 user 消息）
 */

#include <string>
#include <deque>
#include <vector>
#include <utility>

struct ChatMessage {
    std::string role;     // "user", "assistant", or "system" (skill facts)
    std::string content;
};

class ChatMemory {
public:
    /// @param max_rounds  最多保留 N 轮对话
    /// @param max_tokens  总 token 上限（估算值）
    explicit ChatMemory(int max_rounds = 10, int max_tokens = 1536);

    /// 记录一轮对话（不含 skill 事实）
    void add(const std::string& user_msg, const std::string& assistant_msg);

    /// 记录一轮对话 + skill 返回的事实数据（持久化到记忆）
    void add(const std::string& user_msg, const std::string& assistant_msg,
             const std::string& skill_fact);

    /// 获取对话上下文（格式化文本，向后兼容）
    std::string get_context() const;

    /// 获取结构化消息列表（user/assistant 交替，含 skill 事实）
    std::vector<ChatMessage> get_messages() const;

    /// 清空
    void clear();

    /// 运行时更新记忆限制（支持热配置重载 Layer 4.4）
    void set_limits(int max_rounds, int max_tokens);

    // ── 持久化 ─────────────────────────────────────

    /// 保存对话历史到 JSON 文件
    bool save_to_file(const std::string& path) const;

    /// 从 JSON 文件加载对话历史（追加到现有历史，不清空）
    bool load_from_file(const std::string& path);

    // ── Token 统计（可观测性）───────────────────────

    /// 当前估算 token 数
    int total_tokens() const { return total_tokens_; }

    /// token 上限
    int max_tokens() const { return max_tokens_; }

    /// 上下文窗口使用率 (0.0 ~ 1.0)
    float usage() const {
        return max_tokens_ > 0 ? (float)total_tokens_ / max_tokens_ : 0.0f;
    }

    /// 当前保留轮数
    int size() const { return (int)history_.size(); }

private:
    struct Entry {
        std::string user;
        std::string assistant;
        std::string skill_fact;  // 本轮 skill 返回的事实数据（可为空）
        int tokens;  // 这一轮的 token 数（user + assistant + skill_fact）
    };

    std::deque<Entry> history_;
    int max_rounds_;
    int max_tokens_;
    int total_tokens_ = 0;   // 运行计数器 → O(1) trim

    /// 从队头弹出最旧的一轮
    void pop_front();

    /// 截断超出限制的历史
    void trim();
};
