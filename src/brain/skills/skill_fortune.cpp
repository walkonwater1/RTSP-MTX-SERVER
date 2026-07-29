#include "utils/logger.h"
#include "skill_fortune.h"
#include "brain/skill_utils.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <algorithm>

// ── 每日种子（同一天同一星座运势一致）────────────────

int FortuneSkill::today_seed()
{
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    return tm->tm_yday * 100 + tm->tm_year;  // day-of-year based
}

std::string FortuneSkill::pick_random(const std::vector<std::string>& items)
{
    if (items.empty()) return "";
    return items[rand() % items.size()];
}

// ── 关键词 ──────────────────────────────────────────

bool FortuneSkill::match(const std::string& text)
{
    static const std::vector<std::string> kw = {
        "星座", "运势", "占卜", "塔罗", "黄历",
        "宜", "忌", "算卦", "算一卦", "抽牌",
        "白羊", "金牛", "双子", "巨蟹", "狮子", "处女",
        "天秤", "天蝎", "射手", "摩羯", "水瓶", "双鱼",
        "幸运色", "桃花运", "财运", "今日运势"
    };

    // 特别匹配：X座 或 X星座
    for (size_t i = 0; i + 3 < text.size(); ++i) {
        std::string sub = text.substr(i, 3);
        if (sub == "座" || sub == "星座") return true;  // caught by keyword match anyway
    }

    return contains_any(text, kw);
}

// ── 提取星座 ──────────────────────────────────────

static std::string extract_sign(const std::string& text)
{
    static const std::vector<std::string> signs = {
        "白羊", "金牛", "双子", "巨蟹", "狮子", "处女",
        "天秤", "天蝎", "射手", "摩羯", "水瓶", "双鱼"
    };
    for (const auto& s : signs) {
        if (text.find(s) != std::string::npos) return s;
    }
    return "";
}

// ── 星座运势 ────────────────────────────────────────

std::string FortuneSkill::zodiac_fortune(const std::string& sign)
{
    // 种子 = 天序号 × 星座序号，每天每星座稳定
    static const std::vector<std::string> signs = {
        "白羊", "金牛", "双子", "巨蟹", "狮子", "处女",
        "天秤", "天蝎", "射手", "摩羯", "水瓶", "双鱼"
    };
    int sign_idx = 0;
    for (size_t i = 0; i < signs.size(); ++i) {
        if (signs[i] == sign) { sign_idx = (int)i; break; }
    }

    int day_seed = today_seed();
    srand(day_seed + sign_idx * 37);
    int r = rand();

    // 综合评分 1-5
    int overall    = (r % 3) + 2;        // 2-4
    int love       = ((r >> 3) % 3) + 2; // 2-4
    int career     = ((r >> 6) % 3) + 2;
    int wealth     = ((r >> 9) % 3) + 2;

    static const std::vector<std::string> colors = {
        "红色", "蓝色", "绿色", "黄色", "紫色", "粉色", "白色", "橙色", "灰色", "棕色"
    };
    static const std::vector<std::string> lucky_nums = {
        "3", "5", "7", "8", "11", "17", "21", "27", "33", "42"
    };
    std::string color = colors[rand() % colors.size()];
    std::string num   = lucky_nums[rand() % lucky_nums.size()];

    // Convert numeric rating to Chinese text (TTS-readable, no emoji)
    static const char* stars[] = {"", "一颗星", "两颗星", "三颗星", "四颗星", "五颗星"};
    auto star_text = [](int n) -> std::string {
      if (n >= 1 && n <= 5) return stars[n];
      return std::to_string(n) + "颗星";
    };

    std::ostringstream oss;
    oss << sign << "座今日运势\n\n"
        << "综合运势: " << star_text(overall)
        << "\n桃花运: " << star_text(love)
        << "\n事业运: " << star_text(career)
        << "\n财运: " << star_text(wealth)
        << "\n\n幸运色: " << color
        << "\n幸运数字: " << num;

    // 整体建议
    if (overall >= 4)
        oss << "\n\n今天运势不错，大胆去做想做的事吧！";
    else if (overall >= 3)
        oss << "\n\n平平淡淡才是真，保持好心情最重要。";
    else
        oss << "\n\n今天适合低调行事，多喝热水少做决定。";

    return oss.str();
}

// ── 塔罗占卜 ────────────────────────────────────────

std::string FortuneSkill::tarot_reading()
{
    static const std::vector<std::string> cards = {
        "愚者 — 新的开始，保持好奇心，但也要注意脚下。",
        "魔术师 — 你拥有所需的一切资源，行动起来吧。",
        "女祭司 — 相信直觉，有些答案在心里。",
        "女皇 — 创造力爆发，适合做些美好的事。",
        "皇帝 — 秩序和自律会带来稳定。",
        "教皇 — 有人在默默帮助着你。",
        "恋人 — 面临重要选择，听从内心。",
        "战车 — 战胜困难的力量就在你手中。",
        "力量 — 温柔比暴力更有力量。",
        "隐士 — 需要独处思考，答案自会浮现。",
        "命运之轮 — 好运即将来临，保持期待。",
        "正义 — 诚实和公平会带来最好的结果。",
        "倒吊人 — 换个角度看问题，会豁然开朗。",
        "死神 — 结束意味着新开始，不必恐惧改变。",
        "节制 — 平衡是当下的关键词。",
        "月亮 — 有些事看不清，暂时按兵不动。",
        "太阳 — 一切顺利，尽情享受美好时光。",
        "审判 — 过去的努力即将得到认可。",
        "世界 — 一个阶段圆满完成，新的旅程开始。",
    };

    srand(time(nullptr));
    int idx = rand() % cards.size();

    std::ostringstream oss;
    oss << "塔罗占卜 — 命运之轮转动中...\n\n"
        << "你抽到的牌是:\n\n"
        << cards[idx] << "\n\n"
        << "（仅供娱乐，请勿过于认真哦～）";
    return oss.str();
}

// ── 今日黄历 ────────────────────────────────────────

std::string FortuneSkill::daily_almanac()
{
    srand(today_seed());

    static const std::vector<std::string> yi_list = {
        "嫁娶", "出行", "开业", "搬家", "签约", "装修",
        "理发", "洗澡", "吃火锅", "写代码", "睡懒觉",
        "刷视频", "点外卖", "喝奶茶", "打游戏", "看书",
        "听音乐", "逛淘宝", "种花", "做手工"
    };
    static const std::vector<std::string> ji_list = {
        "熬夜", "冲动消费", "摸鱼被抓", "跟领导顶嘴",
        "吃太多", "忘记充电", "穿白衣服吃火锅",
        "走路看手机", "跟对象吵架", "拖延症",
        "随便立flag", "多管闲事", "借别人钱"
    };

    int yi_count = 2 + (rand() % 3);
    int ji_count = 1 + (rand() % 2);

    std::ostringstream oss;
    oss << "今日黄历（仅供参考，切勿当真）\n\n";

    oss << "宜: ";
    for (int i = 0; i < yi_count; i++) {
        if (i > 0) oss << "、";
        oss << yi_list[rand() % yi_list.size()];
    }

    oss << "\n忌: ";
    for (int i = 0; i < ji_count; i++) {
        if (i > 0) oss << "、";
        oss << ji_list[rand() % ji_list.size()];
    }

    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    char date_buf[32];
    strftime(date_buf, sizeof(date_buf), "%Y年%m月%d日", tm);
    oss << "\n\n" << date_buf << "  农历 "
        << ((tm->tm_mon + 1)) << "月"
        << (tm->tm_mday > 15 ? "十" : "初");

    int day = tm->tm_mday % 15;
    if (day == 0) day = 15;
    static const char* nums[] = {"","一","二","三","四","五","六","七","八","九","十"};
    if (day <= 10) oss << nums[day];
    else oss << "十" << std::string(nums[day-10]);
    oss << "\n\n今日吉神: 天德，冲煞: 岁煞南";

    return oss.str();
}

// ── 执行入口 ────────────────────────────────────────────

std::string FortuneSkill::execute(const std::string& text)
{
    return execute(text, nlohmann::json::object());
}

std::string FortuneSkill::execute(const std::string& text,
                                   const nlohmann::json& args)
{
    std::string type;
    std::string sign;

    if (args.contains("type") && args["type"].is_string())
        type = args["type"].get<std::string>();
    if (args.contains("sign") && args["sign"].is_string())
        sign = args["sign"].get<std::string>();

    // 从文本推断
    if (type.empty()) {
        if (text.find("塔罗") != std::string::npos ||
            text.find("占卜") != std::string::npos ||
            text.find("抽牌") != std::string::npos ||
            text.find("算一卦") != std::string::npos ||
            text.find("算卦") != std::string::npos) {
            type = "tarot";
        } else if (text.find("黄历") != std::string::npos ||
                   text.find("宜") != std::string::npos ||
                   text.find("忌") != std::string::npos) {
            type = "almanac";
        } else {
            type = "zodiac";  // 默认星座
        }
    }

    if (sign.empty() && type == "zodiac") {
        sign = extract_sign(text);
    }

    std::cout << "   [Skill:运势] type=" << type << " sign=" << sign << std::endl;

    if (type == "tarot") return tarot_reading();
    if (type == "almanac") return daily_almanac();

    // zodiac
    if (sign.empty()) {
        return "你想看哪个星座的运势呢？比如「天秤座今日运势」。\n"
               "十二星座：白羊、金牛、双子、巨蟹、狮子、处女、"
               "天秤、天蝎、射手、摩羯、水瓶、双鱼。";
    }
    return zodiac_fortune(sign);
}
