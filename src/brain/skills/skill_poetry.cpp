#include "utils/logger.h"
#include "skill_poetry.h"
#include "brain/skill_utils.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <ctime>

struct Poem {
    const char* title;
    const char* poet;
    const char* dynasty;
    const char* lines;
};

static const std::vector<Poem> poems = {
    // 李白
    {"静夜思", "李白", "唐",
     "床前明月光，疑是地上霜。\n举头望明月，低头思故乡。"},
    {"望庐山瀑布", "李白", "唐",
     "日照香炉生紫烟，遥看瀑布挂前川。\n飞流直下三千尺，疑是银河落九天。"},
    {"早发白帝城", "李白", "唐",
     "朝辞白帝彩云间，千里江陵一日还。\n两岸猿声啼不住，轻舟已过万重山。"},
    {"赠汪伦", "李白", "唐",
     "李白乘舟将欲行，忽闻岸上踏歌声。\n桃花潭水深千尺，不及汪伦送我情。"},

    // 杜甫
    {"春望", "杜甫", "唐",
     "国破山河在，城春草木深。\n感时花溅泪，恨别鸟惊心。\n烽火连三月，家书抵万金。\n白头搔更短，浑欲不胜簪。"},
    {"绝句", "杜甫", "唐",
     "两个黄鹂鸣翠柳，一行白鹭上青天。\n窗含西岭千秋雪，门泊东吴万里船。"},
    {"春夜喜雨", "杜甫", "唐",
     "好雨知时节，当春乃发生。\n随风潜入夜，润物细无声。"},

    // 苏轼
    {"水调歌头", "苏轼", "宋",
     "明月几时有，把酒问青天。\n不知天上宫阙，今夕是何年。\n我欲乘风归去，又恐琼楼玉宇，高处不胜寒。\n起舞弄清影，何似在人间。\n\n转朱阁，低绮户，照无眠。\n不应有恨，何事长向别时圆。\n人有悲欢离合，月有阴晴圆缺，此事古难全。\n但愿人长久，千里共婵娟。"},
    {"题西林壁", "苏轼", "宋",
     "横看成岭侧成峰，远近高低各不同。\n不识庐山真面目，只缘身在此山中。"},

    // 王维
    {"相思", "王维", "唐",
     "红豆生南国，春来发几枝。\n愿君多采撷，此物最相思。"},
    {"山居秋暝", "王维", "唐",
     "空山新雨后，天气晚来秋。\n明月松间照，清泉石上流。"},

    // 白居易
    {"赋得古原草送别", "白居易", "唐",
     "离离原上草，一岁一枯荣。\n野火烧不尽，春风吹又生。"},
    {"忆江南", "白居易", "唐",
     "江南好，风景旧曾谙。\n日出江花红胜火，春来江水绿如蓝。\n能不忆江南？"},

    // 孟浩然
    {"春晓", "孟浩然", "唐",
     "春眠不觉晓，处处闻啼鸟。\n夜来风雨声，花落知多少。"},

    // 王之涣
    {"登鹳雀楼", "王之涣", "唐",
     "白日依山尽，黄河入海流。\n欲穷千里目，更上一层楼。"},

    // 李商隐
    {"无题", "李商隐", "唐",
     "相见时难别亦难，东风无力百花残。\n春蚕到死丝方尽，蜡炬成灰泪始干。"},

    // 李煜
    {"虞美人", "李煜", "南唐",
     "春花秋月何时了，往事知多少。\n小楼昨夜又东风，故国不堪回首月明中。\n\n问君能有几多愁，恰似一江春水向东流。"},

    // 辛弃疾
    {"青玉案·元夕", "辛弃疾", "宋",
     "东风夜放花千树，更吹落、星如雨。\n宝马雕车香满路。\n凤箫声动，玉壶光转，一夜鱼龙舞。\n\n蓦然回首，那人却在，灯火阑珊处。"},
};

// ── 关键词 ──────────────────────────────────────────

bool PoetrySkill::match(const std::string& text)
{
    static const std::vector<std::string> kw = {
        "唐诗", "宋词", "诗词", "古诗", "背一首",
        "来首诗", "念首诗", "读首诗", "推荐一首",
        "李白", "杜甫", "苏轼", "王维", "白居易",
        "孟浩然", "王之涣", "李商隐", "辛弃疾", "李煜"
    };
    return contains_any(text, kw);
}

// ── 执行 ────────────────────────────────────────────

std::string PoetrySkill::execute(const std::string& text)
{
    return execute(text, nlohmann::json::object());
}

std::string PoetrySkill::execute(const std::string& text,
                                  const nlohmann::json& args)
{
    std::string poet;
    if (args.contains("poet") && args["poet"].is_string())
        poet = args["poet"].get<std::string>();

    // 从文本提取诗人
    if (poet.empty()) {
        static const char* poets[] = {"李白","杜甫","苏轼","王维","白居易",
                                       "孟浩然","王之涣","辛弃疾","李煜",nullptr};
        for (int i = 0; poets[i]; ++i) {
            if (text.find(poets[i]) != std::string::npos) {
                poet = poets[i];
                break;
            }
        }
    }

    // 按诗人筛选
    std::vector<const Poem*> filtered;
    for (const auto& p : poems) {
        if (poet.empty() || p.poet == poet)
            filtered.push_back(&p);
    }

    if (filtered.empty()) {
        return "没找到这位诗人的作品，试试李白、杜甫或苏轼吧。";
    }

    srand(time(nullptr));
    const Poem* p = filtered[rand() % filtered.size()];

    std::cout << "   [Skill:诗词] " << p->poet << " · " << p->title << std::endl;

    std::ostringstream oss;
    oss << p->dynasty << " · " << p->poet << " — 《" << p->title << "》\n\n"
        << p->lines;
    return oss.str();
}
