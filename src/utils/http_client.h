/**
 * HTTP 客户端 — 统一 curl 封装
 *
 * 消除 llm_engine.cpp 和 skill_manager.cpp 中重复的 curl GET/POST 代码。
 *
 * 用法：
 *   #include "http_client.h"
 *   std::string resp = HttpClient::get("http://...", 10);
 *   std::string resp = HttpClient::post(url, json_body, 60);
 */

#pragma once

#include <string>
#include <functional>
#include <stdexcept>
#include <curl/curl.h>

class HttpClient {
public:
    /// HTTP GET 请求
    /// @param url          请求地址
    /// @param timeout_sec  超时秒数
    /// @return 响应体字符串
    /// @throws std::runtime_error 网络错误
    static std::string get(const std::string& url, long timeout_sec = 10)
    {
        std::string response;
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init failed");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            throw std::runtime_error(std::string("curl GET error: ") + curl_easy_strerror(res));
        }
        return response;
    }

    /// HTTP POST 请求（JSON 请求体）
    /// @param url          请求地址
    /// @param request_json JSON 字符串
    /// @param timeout_sec  超时秒数
    /// @return 响应体字符串
    /// @throws std::runtime_error 网络错误
    static std::string post(const std::string& url,
                            const std::string& request_json,
                            long timeout_sec = 60)
    {
        std::string response;
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("curl_easy_init failed");

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_json.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            throw std::runtime_error(std::string("curl POST error: ") + curl_easy_strerror(res));
        }
        return response;
    }

    /// HTTP POST 流式请求（SSE）
    /// @param url          请求地址
    /// @param request_json JSON 字符串
    /// @param on_line      每行 SSE 数据的回调，返回 false 中断流
    /// @param timeout_sec  超时秒数
    /// @return true = 成功（或被回调中断），false = 网络错误
    static bool post_stream(const std::string& url,
                            const std::string& request_json,
                            std::function<bool(const std::string& line)> on_line,
                            long timeout_sec = 60)
    {
        StreamState state{std::move(on_line), ""};

        CURL* curl = curl_easy_init();
        if (!curl) return false;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_json.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);

        CURLcode res = curl_easy_perform(curl);

        // 刷新剩余行
        if (!state.line_buf.empty()) {
            state.on_line(state.line_buf);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return res == CURLE_OK || res == CURLE_WRITE_ERROR; // WRITE_ERROR = 回调中断
    }

private:
    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* str = static_cast<std::string*>(userdata);
        str->append(ptr, size * nmemb);
        return size * nmemb;
    }

    struct StreamState {
        std::function<bool(const std::string&)> on_line;
        std::string line_buf;
    };

    static size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* state = static_cast<StreamState*>(userdata);
        size_t total = size * nmemb;
        for (size_t i = 0; i < total; ++i) {
            char c = ptr[i];
            if (c == '\n') {
                if (!state->line_buf.empty()) {
                    bool ok = state->on_line(state->line_buf);
                    state->line_buf.clear();
                    if (!ok) return 0;
                }
            } else {
                state->line_buf += c;
            }
        }
        return total;
    }
};
