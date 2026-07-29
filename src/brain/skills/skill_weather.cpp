#include "utils/logger.h"
/**
 * 天气查询技能 — 实现
 *
 * 使用 wttr.in API，免费无需注册。
 */

#include "skill_weather.h"
#include "brain/skill_utils.h"
#include "utils/http_client.h"
#include <iostream>

bool WeatherSkill::match(const std::string& text)
{
    static const std::vector<std::string> keywords = {
        "天气", "气温", "温度", "下雨", "下雪", "刮风",
        "多少度", "冷不冷", "热不热", "晴", "阴天",
        "多云", "暴雨", "台风", "雾霾", "空气质量",
        "湿度", "风力", "紫外线", "天气预报"
    };
    return contains_any(text, keywords);
}

std::string WeatherSkill::execute(const std::string& text)
{
    return execute(text, nlohmann::json::object());
}

std::string WeatherSkill::execute(const std::string& text,
                                   const nlohmann::json& args)
{
    std::string city;
    // 优先使用 LLM 提取的结构化参数
    if (args.contains("city") && args["city"].is_string()) {
        city = extract_city(args["city"].get<std::string>());
    } else {
        city = extract_city(text);
    }

    std::string url = "https://wttr.in/" + city + "?format=%C+%t+%h+%w&lang=zh";

    std::cout << "   [Skill:天气] 查询 " << city << " ..." << std::endl;

    std::string result;
    try {
        result = HttpClient::get(url);
    } catch (const std::exception& e) {
        std::cerr << "   [Skill] HTTP 请求失败: " << e.what() << std::endl;
        return "天气查询失败，请稍后再试。";
    }

    if (result.empty()) {
        return "天气查询失败，请稍后再试。";
    }

    // 清洗结果（去除换行、多余空格）
    std::string clean;
    for (char c : result) {
        if (c == '\n' || c == '\r') clean += ' ';
        else clean += c;
    }

    return city + " 天气: " + clean;
}

std::string WeatherSkill::extract_city(const std::string& text)
{
    static const std::vector<std::pair<std::string, std::string>> cities = {
        {"北京", "Beijing"}, {"上海", "Shanghai"}, {"广州", "Guangzhou"},
        {"深圳", "Shenzhen"}, {"杭州", "Hangzhou"}, {"成都", "Chengdu"},
        {"武汉", "Wuhan"},   {"南京", "Nanjing"},  {"西安", "Xi'an"},
        {"重庆", "Chongqing"}, {"天津", "Tianjin"}, {"苏州", "Suzhou"},
        {"长沙", "Changsha"}, {"郑州", "Zhengzhou"}, {"东莞", "Dongguan"},
        {"青岛", "Qingdao"}, {"厦门", "Xiamen"}, {"大连", "Dalian"},
        {"济南", "Jinan"}, {"合肥", "Hefei"}, {"福州", "Fuzhou"},
        {"昆明", "Kunming"}, {"哈尔滨", "Harbin"}, {"沈阳", "Shenyang"},
        {"台北", "Taipei"}, {"香港", "HongKong"},
    };

    for (auto& [cn, en] : cities) {
        if (text.find(cn) != std::string::npos)
            return en;
    }
    return "Beijing";
}
