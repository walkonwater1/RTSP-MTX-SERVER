#include "utils/logger.h"
#include "skill_games.h"
#include "brain/skill_utils.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <ctime>
#include <nlohmann/json.hpp>
#include <sys/stat.h>

// ── 状态文件 ──────────────────────────────────────

std::string GamesSkill::game_state_path()
{
    const char* home = getenv("HOME");
    std::string dir = home ? std::string(home) + "/.voice_notes" : ".";
    mkdir(dir.c_str(), 0755);
    return dir + "/game_state.json";
}

// ── 关键词 ──────────────────────────────────────

bool GamesSkill::match(const std::string& text)
{
    static const std::vector<std::string> kw = {
        "猜数字", "我想玩游戏", "玩个游戏",
        "成语接龙", "接龙", "你出一个成语"
    };
    return contains_any(text, kw);
}

// ── 猜数字 ──────────────────────────────────────

std::string GamesSkill::guess_number(const std::string& user_text,
                                      const nlohmann::json& args)
{
    std::string path = game_state_path();
    nlohmann::json state;

    // 加载或初始化
    {
        std::ifstream f(path);
        if (f.is_open()) {
            try { state = nlohmann::json::parse(f); } catch (...) {}
        }
    }

    // 检查是否是新的猜数字请求（重置游戏）
    bool is_new = (user_text.find("猜数字") != std::string::npos ||
                   user_text.find("玩个游戏") != std::string::npos);

    if (is_new || !state.contains("guess_target")) {
        srand(time(nullptr));
        int target = rand() % 100 + 1;
        state["game"] = "guess";
        state["guess_target"] = target;
        state["guess_count"] = 0;
        state["guess_min"] = 1;
        state["guess_max"] = 100;

        std::ofstream f(path);
        f << state.dump();

        std::cout << "   [Skill:猜数字] 新游戏, 目标=" << target << std::endl;
        return "猜数字游戏开始！\n\n"
               "我心里想了一个 1-100 之间的数字，你猜猜是多少？\n"
               "直接说数字就行，我会告诉你大了还是小了。";
    }

    // 检查是否是猜的数字
    int guess = -1;
    if (args.contains("guess_number") && args["guess_number"].is_number()) {
        guess = args["guess_number"].get<int>();
    } else {
        // 从文本中提取数字
        for (size_t i = 0; i < user_text.size(); ++i) {
            if (user_text[i] >= '0' && user_text[i] <= '9') {
                size_t end = i;
                while (end < user_text.size() && user_text[end] >= '0' && user_text[end] <= '9')
                    ++end;
                try { guess = std::stoi(user_text.substr(i, end - i)); } catch (...) {}
                break;
            }
        }
    }

    if (guess <= 0 || guess > 100) {
        return "请说一个1到100之间的数字哦。";
    }

    int target = state["guess_target"].get<int>();
    int count  = state["guess_count"].get<int>() + 1;
    int gmin   = state["guess_min"].get<int>();
    int gmax   = state["guess_max"].get<int>();

    state["guess_count"] = count;

    std::ostringstream oss;

    if (guess == target) {
        oss << "恭喜！你猜对了！就是 " << target << "！\n"
            << "你一共猜了 " << count << " 次。";
        if (count <= 5) oss << "\n太厉害了！简直是读心术！";
        else if (count <= 8) oss << "\n不错哦，运气挺好的！";
        else oss << "\n下次一定能猜得更快！";

        // 删除游戏状态
        std::remove(path.c_str());
        std::cout << "   [Skill:猜数字] 猜中! " << count << "次" << std::endl;
        return oss.str();
    }

    if (guess < target) {
        gmin = std::max(gmin, guess + 1);
        oss << "太小了！范围现在是 " << gmin << "到" << gmax << "。";
    } else {
        gmax = std::min(gmax, guess - 1);
        oss << "太大了！范围现在是 " << gmin << "到" << gmax << "。";
    }

    oss << " (第" << count << "次)";

    state["guess_min"] = gmin;
    state["guess_max"] = gmax;

    // 如果范围缩小到只有一个数
    if (gmin == gmax) {
        oss << "\n范围只剩下 " << gmin << " 了！";
    }

    {
        std::ofstream f(path);
        f << state.dump();
    }

    std::cout << "   [Skill:猜数字] 猜" << guess << " → " << (guess < target ? "太小" : "太大") << std::endl;
    return oss.str();
}

// ── 成语接龙（简化版：AI出题）───────────────────

std::string GamesSkill::idiom_chain()
{
    static const std::vector<std::string> idioms = {
        "一心一意", "马到成功", "龙马精神", "画龙点睛",
        "胸有成竹", "一帆风顺", "心想事成", "万事如意",
        "生机勃勃", "金玉满堂", "天长地久", "水落石出",
        "开门见山", "对牛弹琴", "守株待兔", "画蛇添足",
        "掩耳盗铃", "亡羊补牢", "狐假虎威", "叶公好龙",
        "自相矛盾", "刻舟求剑", "杯弓蛇影", "指鹿为马",
    };

    srand(time(nullptr));
    const std::string& idiom = idioms[rand() % idioms.size()];

    // 提取最后一个字（UTF-8）
    size_t len = idiom.size();
    std::string last_char;
    if (len >= 3) {
        // 最后一个 UTF-8 字符
        size_t i = len - 1;
        while (i > 0 && (idiom[i] & 0xC0) == 0x80) --i;
        last_char = idiom.substr(i);
    } else {
        last_char = idiom.substr(len - 1);
    }

    std::cout << "   [Skill:成语接龙] " << idiom << " → 接「" << last_char << "」" << std::endl;

    std::ostringstream oss;
    oss << "成语接龙！我先来：\n\n"
        << "「" << idiom << "」\n\n"
        << "请接一个以「" << last_char << "」开头的成语！";
    return oss.str();
}

// ── 执行入口 ────────────────────────────────────

std::string GamesSkill::execute(const std::string& text)
{
    return execute(text, nlohmann::json::object());
}

std::string GamesSkill::execute(const std::string& text,
                                 const nlohmann::json& args)
{
    // 判断游戏类型
    if (text.find("成语") != std::string::npos ||
        text.find("接龙") != std::string::npos) {
        return idiom_chain();
    }

    // 默认猜数字
    return guess_number(text, args);
}
