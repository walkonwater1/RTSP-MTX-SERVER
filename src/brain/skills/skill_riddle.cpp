#include "utils/logger.h"
#include "skill_riddle.h"
#include "brain/skill_utils.h"
#include <iostream>
#include <cstdlib>

bool RiddleSkill::match(const std::string& text)
{
    static const std::vector<std::string> kw = {
        "脑筋急转弯", "急转弯", "猜谜", "谜语", "猜一猜",
        "出个题", "考考你", "智力题", "你猜"
    };
    return contains_any(text, kw);
}

std::string RiddleSkill::execute(const std::string& text)
{
    static const std::vector<std::string> riddles = {
        "问：什么东西越洗越脏？\n答：水！洗别的东西时它自己就脏了。",

        "问：什么东西是你的，但别人用的比你多？\n答：你的名字。",

        "问：什么东西天气越热它爬得越高？\n答：温度计。",

        "问：什么东西越用越有钱？\n答：存钱罐。",

        "问：小明从不读书却当上了科学家，为什么？\n答：因为他是实验员——被实验的「小白鼠」。",

        "问：什么东西打破了大家都叫好？\n答：世界纪录。",

        "问：一只鸡和一只鹅同时放进冰箱，为什么鸡冻死了鹅没死？\n答：因为那是企鹅。",

        "问：为什么鱼不能住在陆地上？\n答：因为陆地上有猫。",

        "问：什么东西不在天上飞，却叫「飞机」？\n答：纸飞机。",

        "问：什么书在书店买不到？\n答：秘书。",

        "问：世界上什么床最贵？\n答：病床。",

        "问：什么时候四减一等于五？\n答：一个四边形切掉一个角。",

        "问：什么东西往上升了就不会掉下来？\n答：年龄。",

        "问：为什么小明考试得零分还能笑出来？\n答：因为他是体育老师。",

        "问：哪个月有28天？\n答：每个月都有28天。",
    };

    int idx = rand() % riddles.size();
    std::cout << "   [Skill:谜语] #" << idx << std::endl;
    return "脑筋急转弯：\n\n" + riddles[idx];
}
