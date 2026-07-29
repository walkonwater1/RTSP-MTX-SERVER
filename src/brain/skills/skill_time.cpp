#include "utils/logger.h"
/**
 * 时间/日期查询技能 — 实现
 */

#include "skill_time.h"
#include "brain/skill_utils.h"
#include <ctime>
#include <cstdio>
#include <iostream>

bool TimeSkill::match(const std::string& text)
{
    static const std::vector<std::string> keywords = {
        "几点", "时间", "日期", "几号", "星期几",
        "今天几", "现在", "当前时间", "日历", "年月日"
    };
    return contains_any(text, keywords);
}

std::string TimeSkill::execute(const std::string& /*text*/)
{
    std::time_t now = std::time(nullptr);
    std::tm* local = std::localtime(&now);

    static const char* weekdays[] = {
        "星期日", "星期一", "星期二", "星期三",
        "星期四", "星期五", "星期六"
    };

    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "现在是 %d年%d月%d日 %s %02d:%02d",
        local->tm_year + 1900,
        local->tm_mon + 1,
        local->tm_mday,
        weekdays[local->tm_wday],
        local->tm_hour,
        local->tm_min);

    std::cout << "   [Skill:时间] " << buf << std::endl;
    return std::string(buf);
}
