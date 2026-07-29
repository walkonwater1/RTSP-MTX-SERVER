#pragma once
/**
 * 向量存储 — 内存版，余弦相似度检索
 *
 * 学习要点:
 *   1. 向量相似度 = 两个 embedding 的夹角余弦值
 *   2. embedding 已 L2 归一化 → 余弦相似度 = 向量点积 (dot product)
 *   3. 生产环境需替换为 FAISS / Chroma / Milvus 等向量数据库
 *
 * 使用方式:
 *   VectorStore store;
 *   store.add("今天天气很好", emb1);
 *   store.add("明天会下雨",   emb2);
 *   auto results = store.search(query_emb, 3);  // top-3
 *   // results = [{"今天天气很好", score:0.95}, ...]
 */

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>

struct SearchResult {
    std::string text;
    float       score;       // 余弦相似度 [0, 1]，越高越相关
};

class VectorStore {
public:
    /// 添加一条文档（文本 + embedding 向量）
    void add(const std::string& text, const std::vector<float>& embedding)
    {
        docs_.push_back({text, embedding});
    }

    /// 批量添加
    void add_batch(const std::vector<std::string>& texts,
                   const std::vector<std::vector<float>>& embeddings)
    {
        for (size_t i = 0; i < texts.size() && i < embeddings.size(); ++i) {
            add(texts[i], embeddings[i]);
        }
    }

    /// 语义搜索 — 返回 top-K 最相似文档
    /// @param query_emb  查询文本的 embedding（需要已 L2 归一化）
    /// @param k          返回前 K 条
    /// @param min_score  最低相似度阈值 (0~1)，低于此值的结果丢弃
    std::vector<SearchResult> search(const std::vector<float>& query_emb,
                                     int k = 3,
                                     float min_score = 0.3f) const
    {
        std::vector<SearchResult> results;
        results.reserve(docs_.size());

        for (const auto& doc : docs_) {
            float score = dot_product(query_emb, doc.embedding);
            if (score >= min_score) {
                results.push_back({doc.text, score});
            }
        }

        // 按相似度降序排列
        std::sort(results.begin(), results.end(),
                  [](const SearchResult& a, const SearchResult& b) {
                      return a.score > b.score;
                  });

        if ((int)results.size() > k) {
            results.resize(k);
        }

        return results;
    }

    /// 文档数量
    size_t size() const { return docs_.size(); }

    /// 清空
    void clear() { docs_.clear(); }

private:
    struct Document {
        std::string        text;
        std::vector<float> embedding;
    };
    std::vector<Document> docs_;

    /// 向量点积（embedding 已归一化 → 等价于余弦相似度）
    static float dot_product(const std::vector<float>& a,
                             const std::vector<float>& b)
    {
        if (a.size() != b.size()) return 0.0f;

        float sum = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) {
            sum += a[i] * b[i];
        }
        return sum;  // 已归一化 → 范围大致在 [-1, 1]
    }
};
