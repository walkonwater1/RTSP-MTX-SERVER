#include "utils/logger.h"
/**
 * 娱乐技能 — 实现
 */

#include "skill_entertainment.h"
#include "brain/skill_utils.h"

#include <iostream>
#include <sstream>
#include <cstdlib>
#include <ctime>

// ── 关键词匹配 ──────────────────────────────────────────

bool EntertainmentSkill::match(const std::string& text)
{
    static const std::vector<std::string> keywords = {
        "笑话", "讲个", "段子", "故事", "睡前",
        "有趣的事", "冷知识", "趣闻", "说个", "来一个",
        "说点什么", "好玩", "开心一下",
        "毒鸡汤", "反鸡汤", "心灵鸡汤", "丧文化"
    };

    // 需要明确的娱乐意图（避免"故事"误触 RAG）
    bool has_story = (text.find("讲个") != std::string::npos ||
                      text.find("说个") != std::string::npos ||
                      text.find("来一个") != std::string::npos ||
                      text.find("睡前") != std::string::npos ||
                      text.find("想听") != std::string::npos);

    if (text.find("故事") != std::string::npos && !has_story) return false;

    return contains_any(text, keywords);
}

// ── 随机选择 ────────────────────────────────────────────

std::string EntertainmentSkill::pick_random(const std::vector<std::string>& items)
{
    if (items.empty()) return "";
    int idx = rand() % items.size();
    return items[idx];
}

// ── 笑话库 ──────────────────────────────────────────────

std::string EntertainmentSkill::random_joke()
{
    static const std::vector<std::string> jokes = {
        // 程序员笑话
        "一个测试工程师走进一家酒吧，要了一杯啤酒。\n"
        "又走进一家酒吧，要了0杯啤酒。\n"
        "又走进一家酒吧，要了999999杯啤酒。\n"
        "又走进一家酒吧，要了一只蜥蜴。\n"
        "又走进一家酒吧，要了-1杯啤酒。\n"
        "又走进一家酒吧，要了一杯雪碧一杯啤酒一杯水一杯牛奶。\n"
        "用户走进一家酒吧，要了杯咖啡，然后酒吧炸了。",

        "程序员去面试，面试官问：\"你毕业才两年，这三年工作经验是怎么来的？！\"\n程序员答：\"加班。\"",

        "问：程序员最讨厌康熙的哪个儿子？\n答：胤禩，因为他是八阿哥（bug）。",

        "老婆给程序员打电话：\"下班顺路买一斤包子回来，如果看到卖西瓜的，买一个。\"\n"
        "晚上程序员回家，手里只拿了一个包子。\n"
        "老婆怒吼：\"你怎么就买了一个包子？！\"\n"
        "程序员：\"因为我看到卖西瓜的了。\"",

        "一群客人走进一家酒吧。\n"
        "第一个客人要了一杯啤酒。\n"
        "第二、三个客人也各要了一杯啤酒。\n"
        "第四个客人要了一杯水。\n"
        "酒保问：你呢？第五个客人说：我跟前面的一样。\n"
        "酒保给了他一杯啤酒、一杯啤酒、一杯啤酒、一杯水。",

        // 日常生活笑话
        "有一天小明去银行取钱，柜员问：\"取多少？\"\n"
        "小明说：\"取五万。\"柜员看了看说：\"卡里只有三千。\"\n"
        "小明说：\"那就取三千。\"柜员：\"请输密码。\"\n"
        "小明小声说：\"六个八。\"柜员：\"密码不对。\"\n"
        "小明说：\"那就六个六。\"柜员：\"还是不对。\"\n"
        "小明急了：\"那就六个零！\"柜员：\"密码正确，请问取多少？\"",

        "医生：\"你的检查结果出来了，有一个坏消息和一个更坏的消息。\"\n"
        "病人：\"先听坏消息吧。\"\n"
        "医生：\"你得了健忘症。\"\n"
        "病人：\"那更坏的消息呢？\"\n"
        "医生：\"你已经来过三次了。\"",

        "老师：\"小明，请用'一带一路'造句。\"\n"
        "小明：\"我家小区那一带，一路都是火锅店！\"",

        "上课的时候，老师问：\"有没有知道什么叫'先发制人'？\"\n"
        "小明举手站起来，用力推了旁边同学一下，说：\"老师，这就是先发制人！\"",

        "女朋友问程序员：\"我和你妈同时掉水里，你先救谁？\"\n"
        "程序员认真思考后说：\"我不会游泳，但我可以给你写个救生圈的程序。\"\n"
        "女朋友：\"滚！\"",
    };
    return pick_random(jokes);
}

// ── 故事库 ──────────────────────────────────────────────

std::string EntertainmentSkill::random_story()
{
    static const std::vector<std::string> stories = {
        "从前有座山，山里有座庙，庙里有个老和尚在给小和尚讲故事。\n"
        "老和尚说：\"从前有座山，山里有座庙……\"\n"
        "小和尚说：\"师父，这个故事你已经讲了八百遍了！\"\n"
        "老和尚微微一笑：\"那你来讲一个。\"\n"
        "小和尚清了清嗓子：\"从前有个老和尚，总喜欢让徒弟替他讲故事……\"",

        "一只小兔子去钓鱼。\n"
        "第一天，什么都没钓到。\n"
        "第二天，还是什么都没钓到。\n"
        "第三天，一条鱼从水里跳出来，冲着小兔子喊：\n"
        "\"你再用胡萝卜当鱼饵，我就揍你！\"",

        "小蚂蚁迷路了，找不到回家的路。\n"
        "它遇到了一只蜗牛，问：\"蜗牛大哥，你知道我家在哪儿吗？\"\n"
        "蜗牛说：\"就在前面那个小土堆后面。\"\n"
        "小蚂蚁道谢后，蜗牛又喊住它：\"等一下！\"\n"
        "小蚂蚁回头，蜗牛认真地说：\"刚才那句话，我是二十分钟前开始说的。\"",

        "森林里开运动会，乌龟和兔子又比赛跑。\n"
        "这次兔子没有睡觉，一路狂奔到了终点。\n"
        "却发现乌龟已经在那儿喝茶了。\n"
        "兔子惊呼：\"你怎么这么快？！\"\n"
        "乌龟慢悠悠地说：\"时代变了，我打车来的。\"",

        "有一粒沙子，它觉得自己很渺小。\n"
        "它问风：\"我怎样才能变得伟大？\"\n"
        "风把它吹进了蚌壳里。\n"
        "很多年后，沙子变成了一颗美丽的珍珠。\n"
        "这个故事告诉我们：有时候，换一个环境，你会发现自己不一样的价值。",
    };
    return pick_random(stories);
}

// ── 冷知识库 ────────────────────────────────────────────

std::string EntertainmentSkill::random_fact()
{
    static const std::vector<std::string> facts = {
        "你知道吗？打喷嚏时，心脏会暂停跳动一瞬间。\n"
        "而且你不可能睁着眼睛打喷嚏——不信你试试。",

        "你知道吗？树懒憋气的时间比海豚还长。\n"
        "树懒可以在水下憋气40分钟，海豚只有10分钟左右。",

        "你知道吗？考拉的指纹和人类的指纹几乎一模一样，\n"
        "连显微镜下都很难区分。犯罪现场如果留下考拉的指纹，法医可能会疯掉。",

        "你知道吗？香蕉其实有微量的放射性。\n"
        "因为香蕉富含钾元素，而自然界中有一小部分钾是有放射性的钾40。\n"
        "当然，你得一口气吃几百万根香蕉才会有什么事。",

        "你知道吗？章鱼有三个心脏，血液是蓝色的。\n"
        "而且它们的智商很高，能拧开瓶盖、走迷宫，\n"
        "还会跟水族馆的工作人员玩恶作剧。",

        "你知道吗？当手机掉地上时，猫总是会先看一下你的表情，\n"
        "再决定要不要表现出\"对不起\"的样子。\n"
        "科学家称之为\"社交参考行为\"——其实就是看你是不是生气了。",

        "你知道吗？水獭睡觉时会手拉手，怕被水流冲散。\n"
        "如果找不到同伴的手，它们会拉着海草。\n"
        "这大概是动物界最暖心的睡姿了。",

        "你知道吗？人的鼻子能够记住五万种不同的气味。\n"
        "相比之下，眼睛只能分辨几百万种颜色——\n"
        "等等，好像还是眼睛厉害。但鼻子也不差！",
    };
    return pick_random(facts);
}

// ── 毒鸡汤库 ────────────────────────────────────────────

std::string EntertainmentSkill::random_soup()
{
    static const std::vector<std::string> soups = {
        "努力不一定成功，但不努力一定很轻松。",

        "当你觉得自己又穷又丑的时候，不要绝望——至少你的判断是对的。",

        "世上无难事，只要肯放弃。",

        "今天不想做的事，明天也不会想做的。",

        "请相信，所有你花钱买来的教训，最后都会被证明：\n是你想多了。",

        "比你优秀的人都在努力，那你努力还有什么用？",

        "年轻人不要总想着躺平，你要相信——\n躺久了也会累的。",

        "世界上没有真正的感同身受，\n但「我早就说了吧」这句话，每个人都能深刻体会。",

        "你不是懒，你只是有一种\"把事情完美地拖到最后\"的天赋。",

        "失败是成功之母，可惜成功六亲不认。",

        "生活不止眼前的苟且，还有明天和后天的苟且。",

        "你之所以觉得累，是因为你总是想得太多，做的太少——\n不是的，是因为你真的累了，快去睡觉吧。",
    };
    return pick_random(soups);
}

// ── 执行入口 ────────────────────────────────────────────

std::string EntertainmentSkill::execute(const std::string& text)
{
    return execute(text, nlohmann::json::object());
}

std::string EntertainmentSkill::execute(const std::string& text,
                                         const nlohmann::json& args)
{
    std::string type = "joke";  // 默认讲笑话

    if (args.contains("type") && args["type"].is_string()) {
        type = args["type"].get<std::string>();
    } else {
        if (text.find("故事") != std::string::npos ||
            text.find("睡前") != std::string::npos) {
            type = "story";
        } else if (text.find("知识") != std::string::npos ||
                   text.find("趣闻") != std::string::npos ||
                   text.find("有趣") != std::string::npos) {
            type = "fact";
        } else if (text.find("毒鸡汤") != std::string::npos ||
                   text.find("反鸡汤") != std::string::npos ||
                   text.find("丧") != std::string::npos) {
            type = "soup";
        }
    }

    std::string result;
    if (type == "story") {
        std::cout << "   [Skill:娱乐] 讲故事" << std::endl;
        result = random_story();
    } else if (type == "fact") {
        std::cout << "   [Skill:娱乐] 冷知识" << std::endl;
        result = random_fact();
    } else if (type == "soup") {
        std::cout << "   [Skill:娱乐] 毒鸡汤" << std::endl;
        result = random_soup();
    } else {
        std::cout << "   [Skill:娱乐] 讲笑话" << std::endl;
        result = random_joke();
    }

    if (result.empty()) {
        return "让我想想……好像暂时想不出来了。";
    }
    return result;
}
