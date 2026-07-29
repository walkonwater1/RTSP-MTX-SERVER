#pragma once
/**
 * Token 计数器 — 估算文本的 token 数量
 *
 * 学习要点:
 *   1. LLM 不是按「字」计费，而是按「token」
 *   2. 中文 1 个字 ≈ 1~2 个 token（取决于是否为常见字）
 *   3. 英文 1 个单词 ≈ 1~3 个 token（BPE 子词切分）
 *   4. 上下文窗口 (context window) 有限，需要精确管理
 *
 * 方法:
 *   - estimate(): 本地启发式估算（快速，无需网络）
 *   - Ollama API /api/tokens 可获取精确值（HuggingFace tokenizers 离线方案未引入）
 *
 * Qwen2.5 tokenizer 特征（BPE, vocab ~152k）:
 *   - 中文常见字: 1 token/字
 *   - 中文生僻字: 2+ token/字
 *   - 英文短词:    1 token/词
 *   - 英文长词:    2-3 token/词
 *   - 数字/符号:   1 token/连续段
 *
 * 本估算器的规则（偏保守，高估 10-20%，避免超限）:
 *   - CJK 字符: 1.8 token/字
 *   - 英文单词: 1.3 token/词
 *   - 其他 ASCII: 1 token/4 字符
 */

#include <string>
#include <cstdint>
#include <cctype>

class TokenCounter {
public:
    /// 估算文本的 token 数
    static int estimate(const std::string& text);

    /// 估算并返回详细统计
    struct Stats {
        int estimated_tokens;
        int cjk_chars;       // 中文字符数
        int ascii_words;      // 英文单词数
        int other_chars;      // 其他字符数
    };
    static Stats estimate_detailed(const std::string& text);

private:
    /// 判断是否为 CJK 统一表意文字 (U+4E00-U+9FFF)
    static bool is_cjk(unsigned char a, unsigned char b, unsigned char c);
};
