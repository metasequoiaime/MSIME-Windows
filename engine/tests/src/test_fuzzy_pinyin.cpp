#include <metasequoia/session.h>
#include "quanpin/fuzzy_pinyin.h"
#include "quanpin/quanpin_dictionary.h"
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>

using namespace metasequoia;
void require(bool value, const std::string &message)
{
    if (!value)
        throw std::runtime_error(message);
}
void type(Session &session, const std::string &text)
{
    for (char c : text)
        require(session.character(c).handled, "character rejected");
}
std::size_t index(Session &session, const std::string &word)
{
    const auto snapshot = session.snapshot();
    for (std::size_t i = 0; i < snapshot.candidates.size(); ++i)
        if (snapshot.candidates[i].word == word)
            return i;
    throw std::runtime_error("Missing " + word + " for " + snapshot.preedit);
}
bool contains(const std::vector<WordItem> &items, const std::string &word)
{
    return std::any_of(items.begin(), items.end(), [&](const auto &item) { return item.word == word; });
}
std::size_t position(const std::vector<WordItem> &items, const std::string &word)
{
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i].word == word)
            return i;
    throw std::runtime_error("Missing " + word);
}
int main()
{
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("msime-fuzzy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{directory};
    try
    {
        sqlite3 *db = nullptr;
        require(sqlite3_open((directory / "msime.db").u8string().c_str(), &db) == SQLITE_OK, "open fixture");
        const auto insert_weighted = [&](const std::string &key, const std::string &word, std::int64_t weight) {
            const auto segments = quanpin::split_segments(key);
            const auto table = quanpin::build_table_name(segments);
            std::string escaped;
            for (char c : key)
            {
                escaped += c;
                if (c == '\'')
                    escaped += c;
            }
            const auto sql = "CREATE TABLE IF NOT EXISTS " + table +
                             "(key TEXT,jp TEXT,value TEXT,weight INTEGER);INSERT INTO " + table + " VALUES('" +
                             escaped + "','','" + word + "'," + std::to_string(weight) + ");";
            require(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
                    "fixture insert " + key + ": " + sqlite3_errmsg(db));
        };
        const auto insert = [&](const std::string &key, const std::string &word) { insert_weighted(key, word, 100); };
        const std::vector<std::pair<std::string, std::string>> pairs = {
            {"zan", "zhan"}, {"can", "chan"}, {"san", "shan"}, {"na", "la"},      {"fa", "ha"},     {"ran", "lan"},
            {"ban", "bang"}, {"ben", "beng"}, {"bin", "bing"}, {"lian", "liang"}, {"guan", "guang"}};
        for (std::size_t i = 0; i < pairs.size(); ++i)
        {
            insert(pairs[i].first, "原" + std::to_string(i));
            insert(pairs[i].second, "糊" + std::to_string(i));
        }
        insert("zong", "宗");
        insert("zhong", "中");
        insert("guo", "国");
        insert("zhong'guo", "中国");
        // 回归夹具：权重取自出货词库的真实量级。xian->西安对先是真歧义（比值 3.3% 过门槛），
        // xie->西鄂、jiang->激昂被门槛拒收；youdian->尤迪安的键更长（you'di'an），旧排序按
        // 原始键长比较时，即使模糊音没有产出任何变体也会把它顶到第一。
        insert_weighted("xian", "先", 1662684);
        insert_weighted("xi'an", "西安", 55003);
        // 填充到首页之外：西安 的自然位置必须真的掉出前 6，保护位才有事可做。
        insert_weighted("xian", "现", 1500000);
        insert_weighted("xian", "线", 1400000);
        insert_weighted("xian", "县", 1300000);
        insert_weighted("xian", "限", 1200000);
        insert_weighted("xian", "显", 1100000);
        insert_weighted("xian", "险", 1000000);
        insert_weighted("xie", "些", 3752167);
        insert_weighted("xie", "蟹", 10000);
        insert_weighted("xi'e", "西鄂", 6);
        insert_weighted("jiang", "将", 2629219);
        insert_weighted("jiang", "僵", 94955);
        insert_weighted("ji'ang", "激昂", 23740);
        insert_weighted("you'dian", "邮电", 999);
        insert_weighted("you'di'an", "尤迪安", 7);
        // 调频之后的备选切分：吉安 的权重（766925）是用户按 promote 调出来的，它已经把自己
        // 排到自然第 5 位。保护位不能再把它拽到第 2 位——那等于用一个固定槽位覆盖掉调频的
        // 结果，用户看到的就是「选一次就跳到第二，再怎么调也只能是第二」。
        insert_weighted("jian", "见", 3460998);
        insert_weighted("jian", "间", 3067939);
        insert_weighted("jian", "剑", 1151704);
        insert_weighted("jian", "件", 935235);
        insert_weighted("jian", "建", 598616);
        insert_weighted("jian", "检", 500000);
        insert_weighted("ji'an", "吉安", 766925);
        insert_weighted("ji'an", "积案", 9420);
        sqlite3_close(db);
        std::filesystem::create_directories(directory / "helpcodes");
        std::ofstream(directory / "helpcodes" / "helpcode.txt") << "中=ab\n宗=cd\n国=ef\n";
        RuntimePaths paths{directory, directory, directory, directory};
        QuanpinDictionary dictionary({}, paths);
        for (std::size_t i = 0; i < pairs.size(); ++i)
        {
            FuzzyPinyinOptions fuzzy{1u << i};
            const auto forward = dictionary.query(pairs[i].first, pairs[i].first, false, fuzzy);
            require(contains(forward, "糊" + std::to_string(i)), "missing forward rule " + std::to_string(i));
            require(
                contains(dictionary.query(pairs[i].second, pairs[i].second, false, fuzzy), "原" + std::to_string(i)),
                "missing reverse rule");
            require(!contains(dictionary.query(pairs[i].first, pairs[i].first, 0u), "糊" + std::to_string(i)),
                    "fuzzy polluted exact cache");
            require(!contains(dictionary.query(pairs[i].first, pairs[i].first, false,
                                               FuzzyPinyinOptions{1u << ((i + 1) % pairs.size())}),
                              "糊" + std::to_string(i)),
                    "unselected rule expanded");
        }
        // 回归断言：备选切分的保护位与模糊音分支的排序不得把罕见重码顶到主读音前面。
        const FuzzyPinyinOptions fuzzy_on{1u};
        const auto xian_list = dictionary.query("xian", "xian", 0u, fuzzy_on);
        require(xian_list.at(0).word == "先" && xian_list.at(1).word == "西安",
                "real ambiguity lost its protected slot");
        const auto xie_list = dictionary.query("xie", "xie", 0u, fuzzy_on);
        require(xie_list.at(0).word == "些" && xie_list.at(1).word == "蟹" && position(xie_list, "西鄂") > 1,
                "rare re-segmentation outranked the exact reading");
        const auto jiang_list = dictionary.query("jiang", "jiang", 0u, fuzzy_on);
        require(jiang_list.at(0).word == "将" && jiang_list.at(1).word == "僵" && position(jiang_list, "激昂") > 1,
                "rare homophone word outranked the exact reading");
        const auto jian_list = dictionary.query("jian", "jian", 0u, fuzzy_on);
        require(position(jian_list, "吉安") == 4, "protected slot overrode a tuned candidate's earned rank");
        const auto youdian_list = dictionary.query("youdian", "you'dian", 0u, fuzzy_on);
        require(youdian_list.at(0).word == "邮电" && position(youdian_list, "尤迪安") > 0,
                "longer alternative key outranked the exact reading");
        require(quanpin::fuzzy_syllables("zh", {0x7ff}) == std::vector<std::string>{"zh"},
                "incomplete initial changed");
        require(quanpin::fuzzy_syllables("bian", {1u << 6}) == std::vector<std::string>{"bian"},
                "an rule changed ian final");
        require(quanpin::fuzzy_segmentations(quanpin::Segments(20, "lan"), {0x7ff}).size() <= 63,
                "unbounded fuzzy beam");
        SessionOptions options;
        options.paths = paths;
        options.helpcode = false;
        options.autocorrect_types = 0;
        options.learning = false;
        options.fuzzy_pinyin.rules = 1;
        Session session(options);
        type(session, "zongguo");
        auto view = session.snapshot();
        const auto selected = view.candidates[index(session, "中国")];
        require(selected.pinyin == "zong'guo" && selected.canonical_pinyin == "zhong'guo",
                "typed/canonical identity lost");
        require(session.select(index(session, "中")).commit == "中" && session.snapshot().editing_text == "guo",
                "partial selection consumed canonical length");
        require(session.finish().commit == "国", "remaining composition failed");
        type(session, "zhong");
        require(session.select(index(session, "宗")).commit == "宗" && session.snapshot().preedit.empty(),
                "reverse selection left input behind");
        session.set_nine_key_enabled(true);
        type(session, "9664");
        require(session.select(index(session, "中")).commit == "中" && session.snapshot().preedit.empty(),
                "nine-key fuzzy consumed wrong digit count");
        options.scheme = SchemeType::Shuangpin;
        Session doublePinyin(options);
        type(doublePinyin, "zsgo");
        require(doublePinyin.select(index(doublePinyin, "中")).commit == "中" &&
                    doublePinyin.snapshot().editing_text == "go",
                "shuangpin fuzzy consumption");
        require(doublePinyin.finish().commit == "国", "shuangpin suffix failed");
        options.helpcode = true;
        Session helped(options);
        type(helped, "zsaB");
        require(helped.select(index(helped, "中")).commit == "中" && helped.snapshot().preedit.empty(),
                "shuangpin helpcode fuzzy selection");
        options.scheme = SchemeType::Quanpin;
        Session helpedFull(options);
        type(helpedFull, "zongAB");
        require(helpedFull.select(index(helpedFull, "中")).commit == "中" && helpedFull.snapshot().preedit.empty(),
                "quanpin helpcode fuzzy selection");
        options.helpcode = false;
        options.learning = true;
        Session learned(options);
        type(learned, "zongguo");
        require(learned.select(index(learned, "中")).commit == "中", "learn prefix");
        require(learned.select(index(learned, "国")).commit == "国", "learn suffix");
        require(dictionary.find_candidate("zhong'guo", "中国").has_value(), "canonical phrase lost");
        require(!dictionary.find_candidate("zong'guo", "中国").has_value(), "learned mistyped pronunciation");
        options.scheme = SchemeType::Shuangpin;
        options.fuzzy_pinyin.rules = 0;
        Session exact(options);
        type(exact, "zsgo");
        require(!contains(exact.snapshot().candidates, "中国"), "session configuration leaked");
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 200; ++i)
            dictionary.query("zongguo", "zong'guo", false, {0x7ff});
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        require(elapsed < 5000, "warm fuzzy queries exceeded 25 ms per query");
        std::cout << "200 warm fuzzy queries: " << elapsed << " ms\n";
        std::cout << "Fuzzy pinyin rules, cache isolation and session selection passed\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
