#pragma once
/**
 * 用户长期记忆存储 — 持久化 + 语义检索
 *
 * 用途:
 *   1. 记住用户个人信息（名字、偏好、重要事实等）
 *   2. 显式记忆指令 ("记住XXX", "别忘了XXX")
 *   3. 跨会话持久化（JSON 文件）
 *
 * 内部结构:
 *   - entries_: 完整记忆列表（id, content, embedding, timestamp）
 *   - store_:   VectorStore 用于语义检索（需 EmbeddingEngine）
 *
 * 使用方式:
 *   UserMemoryStore mem;
 *   mem.add_memory("用户的名字是张三");
 *   mem.add_memory("用户喜欢吃川菜", embedding_vec);
 *   auto results = mem.search(query_emb, 3);
 *   std::string ctx = mem.get_all_as_context();
 */

#include <string>
#include <vector>
#include <ctime>
#include "memory/vector_store.h"

class UserMemoryStore {
public:
    struct MemoryEntry {
        std::string id;           // 唯一标识 (时间戳生成)
        std::string content;      // 记忆文本
        std::vector<float> embedding;  // 语义向量（可为空）
        std::string created_at;   // ISO 时间字符串
    };

    UserMemoryStore() = default;

    /// 添加一条记忆（不含 embedding，不支持语义搜索）
    void add_memory(const std::string& content);

    /// 添加一条记忆（含 embedding，支持语义搜索）
    void add_memory(const std::string& content, const std::vector<float>& embedding);

    /// 语义搜索 — 返回最相关的 top-K 记忆
    /// @param query_emb  查询文本的 embedding（需 L2 归一化）
    /// @param k          返回数量
    /// @param min_score  最低相似度阈值
    std::vector<SearchResult> search(const std::vector<float>& query_emb,
                                     int k = 5, float min_score = 0.3f) const;

    /// 关键字搜索 — 在记忆文本中搜索关键词
    /// @return 匹配的记忆内容列表
    std::vector<std::string> search_by_keyword(const std::string& keyword,
                                                int max_results = 5) const;

    /// 获取所有记忆，格式化为 LLM 上下文
    std::string get_all_as_context() const;

    /// 根据内容删除记忆（完全匹配）
    bool remove_by_content(const std::string& content);

    /// 清空所有记忆
    void clear();

    /// 记忆条数
    size_t size() const { return entries_.size(); }

    /// 是否为空
    bool empty() const { return entries_.empty(); }

    // ── 持久化 ─────────────────────────────────────

    /// 保存到 JSON 文件
    bool save_to_file(const std::string& path) const;

    /// 从 JSON 文件加载
    bool load_from_file(const std::string& path);

private:
    std::vector<MemoryEntry> entries_;
    VectorStore store_;   // 仅存有 embedding 的条目

    /// 生成唯一 ID
    static std::string gen_id();
};
