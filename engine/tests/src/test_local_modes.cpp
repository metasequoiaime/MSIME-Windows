#include "../../local_modes/date_time_query.h"
#include "../../local_modes/emoji_query.h"
#include "../../local_modes/kaomoji_query.h"
#include "../../local_modes/number_query.h"
#include "../../local_modes/quick_phrase_query.h"
#include "../../core/data_path.h"

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
using metasequoia::local_modes::LocalDateTime;

class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the quick-phrase test database.");
        }
    }

    ~Database()
    {
        sqlite3_close(database_);
    }

    void execute(const char *sql)
    {
        char *error = nullptr;
        if (sqlite3_exec(database_, sql, nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error == nullptr ? "SQLite operation failed." : error;
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

  private:
    sqlite3 *database_ = nullptr;
};

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

LocalDateTime sample_time()
{
    return {2026, 8, 9, 0, 14, 30, 0};
}

template <std::size_t Size>
void require_words(const std::vector<WordItem> &actual, const std::array<const char *, Size> &expected,
                   const char *message)
{
    require(actual.size() == expected.size(), message);
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        require(actual[index].word == expected[index] && actual[index].source == CandidateSource::Generated &&
                    actual[index].weight == static_cast<std::int64_t>(expected.size() - index),
                message);
    }
}
} // namespace

int main()
{
    const LocalDateTime now = sample_time();
    const std::array<const char *, 17> expected_dates = {
        "2026年8月9日",
        "2026-08-09",
        "2026/08/09",
        "2026.08.09",
        "20260809",
        "26年8月9日",
        "8月9日",
        "08-09",
        "0809",
        "2026年8月9日 星期日",
        "8月9日 周日",
        "2026-08-09 Sun",
        "2026-08-09 14:30",
        "8月9日 14:30",
        "二〇二六年八月九日",
        "贰零贰陆年捌月零玖日",
        "丙午年六月二十七日",
    };
    for (const char *keyword : std::array<const char *, 3>{"rq", "riqi", "date"})
    {
        require_words(metasequoia::local_modes::query_date_time(keyword, &now), expected_dates,
                      "A date alias did not preserve the Windows candidate order.");
    }

    const std::array<const char *, 13> expected_times = {
        "14:30",
        "14:30:00",
        "1430",
        "143000",
        "下午2:30",
        "下午2点30分",
        "下午两点半",
        "2:30 PM",
        "2:30pm",
        "02:30:00 PM",
        "2026-08-09 14:30:00",
        "2026年8月9日 14:30",
        "8月9日 下午2:30",
    };
    for (const char *keyword : std::array<const char *, 3>{"sj", "shijian", "time"})
    {
        require_words(metasequoia::local_modes::query_date_time(keyword, &now), expected_times,
                      "A time alias did not preserve the Windows candidate order.");
    }

    const std::array<const char *, 4> expected_sunday = {"星期日", "星期天", "Sunday", "Sun"};
    for (const char *keyword : std::array<const char *, 3>{"xq", "xingqi", "week"})
    {
        require_words(metasequoia::local_modes::query_date_time(keyword, &now), expected_sunday,
                      "A weekday alias did not preserve the Windows candidate order.");
    }

    LocalDateTime monday = now;
    monday.weekday = 1;
    const std::array<const char *, 3> expected_monday = {"星期一", "Monday", "Mon"};
    require_words(metasequoia::local_modes::query_date_time("week", &monday), expected_monday,
                  "Monday candidates were formatted incorrectly.");

    require(!metasequoia::local_modes::is_date_time_keyword("today") &&
                metasequoia::local_modes::is_date_time_keyword("week"),
            "Date/time keyword recognition diverged from Windows.");
    require(metasequoia::local_modes::query_date_time("today", &now).empty() &&
                metasequoia::local_modes::query_date_time("rq", &now, 0).empty() &&
                metasequoia::local_modes::query_date_time("rq", &now, -1).empty() &&
                metasequoia::local_modes::query_date_time("rq", &now, 3).size() == 3,
            "Date/time query limit or unknown-keyword handling was incorrect.");

    using metasequoia::local_modes::is_number_text;
    using metasequoia::local_modes::query_number;
    require(is_number_text("123") && is_number_text("12.") && is_number_text("12.5") && is_number_text("0.05") &&
                !is_number_text("") && !is_number_text(".") && !is_number_text(".5") && !is_number_text("1.2.3") &&
                !is_number_text("a1") && !is_number_text("1a") && !is_number_text("1,000"),
            "Number text validation diverged from the composition rules.");
    require_words(query_number("123456"),
                  std::array<const char *, 7>{"壹拾贰万叁仟肆佰伍拾陆元整", "壹拾贰万叁仟肆佰伍拾陆",
                                              "十二万三千四百五十六", "壹贰叁肆伍陆", "一二三四五六", "123,456",
                                              "12,3456"},
                  "An integer amount was not rendered in every reading.");
    require_words(query_number("1234.5"),
                  std::array<const char *, 6>{"壹仟贰佰叁拾肆元伍角整", "壹仟贰佰叁拾肆点伍", "一千二百三十四点五",
                                              "壹贰叁肆点伍", "一二三四点五", "1,234.5"},
                  "An amount with 角 was not rendered in every reading.");
    require_words(
        query_number("16.05"),
        std::array<const char *, 5>{"壹拾陆元零伍分", "壹拾陆点零伍", "十六点零五", "壹陆点零伍", "一六点零五"},
        "A zero 角 before 分 was not written as 零.");
    require_words(query_number("12.50"),
                  std::array<const char *, 5>{"壹拾贰元伍角整", "壹拾贰点伍", "十二点五", "壹贰点伍零", "一二点五零"},
                  "A trailing fraction zero was not trimmed from the readings.");
    require_words(query_number("100001"),
                  std::array<const char *, 7>{"壹拾万零壹元整", "壹拾万零壹", "十万零一", "壹零零零零壹",
                                              "一零零零零一", "100,001", "10,0001"},
                  "Zeros across a 万 group were not collapsed into one 零.");
    require_words(query_number("1005000"),
                  std::array<const char *, 7>{"壹佰万零伍仟元整", "壹佰万零伍仟", "一百万零五千", "壹零零伍零零零",
                                              "一零零五零零零", "1,005,000", "100,5000"},
                  "A zero between 万 and 仟 was not written.");
    require_words(query_number("100000000"),
                  std::array<const char *, 7>{"壹亿元整", "壹亿", "一亿", "壹零零零零零零零零", "一零零零零零零零零",
                                              "100,000,000", "1,0000,0000"},
                  "亿 was not rendered without trailing zero groups.");
    require_words(query_number("20010"),
                  std::array<const char *, 7>{"贰万零壹拾元整", "贰万零壹拾", "二万零一十", "贰零零壹零", "二零零一零",
                                              "20,010", "2,0010"},
                  "壹拾 inside a number was reduced to 拾.");
    require_words(query_number("10"), std::array<const char *, 5>{"壹拾元整", "壹拾", "十", "壹零", "一零"},
                  "A leading 一十 was not read as 十.");
    require_words(query_number("0"), std::array<const char *, 2>{"零元整", "零"}, "Zero produced duplicate readings.");
    require_words(query_number("0.5"), std::array<const char *, 3>{"零元伍角整", "零点伍", "零点五"},
                  "A sub-yuan amount was not rendered as 零元.");
    require_words(query_number("12."), std::array<const char *, 5>{"壹拾贰元整", "壹拾贰", "十二", "壹贰", "一二"},
                  "A trailing decimal point was not treated as an integer.");
    require_words(query_number("0012"),
                  std::array<const char *, 6>{"壹拾贰元整", "壹拾贰", "十二", "零零壹贰", "零零一二", "12"},
                  "Leading zeros were not stripped from the positional readings.");
    require_words(query_number("1.234"), std::array<const char *, 2>{"壹点贰叁肆", "一点二三四"},
                  "A fraction longer than 分 still produced a financial amount.");
    require(query_number("1234").size() == 6 && query_number("1234").back().word == "1,234",
            "A four-digit number repeated itself as the 万-grouped form.");
    require(query_number("12345678901234567").empty() && query_number("").empty() && query_number("x").empty() &&
                query_number("123456", 0).empty() && query_number("123456", 2).size() == 2,
            "Number query limit or overflow handling was incorrect.");

    const auto suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::filesystem::path quick_phrase_directory =
        std::filesystem::temp_directory_path() / ("metasequoia-quick-phrase-" + suffix);
    std::filesystem::create_directories(quick_phrase_directory);
    const std::filesystem::path quick_phrase_database = quick_phrase_directory / "msime.db";
    {
        Database database(quick_phrase_database);
        database.execute("CREATE TABLE quick_parases(key TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO quick_parases VALUES('ab','highest weight',20)");
        database.execute("INSERT INTO quick_parases VALUES('aa','tie b',10)");
        database.execute("INSERT INTO quick_parases VALUES('aa','tie a',10)");
        database.execute("INSERT INTO quick_parases VALUES('b','outside prefix',100)");
    }

    const auto quick_phrases = metasequoia::local_modes::query_quick_phrases("a", quick_phrase_database, 10);
    require(!quick_phrases.diagnostic.has_value() && quick_phrases.candidates.size() == 3 &&
                quick_phrases.candidates[0].pinyin == "ab" && quick_phrases.candidates[0].word == "highest weight" &&
                quick_phrases.candidates[1].pinyin == "aa" && quick_phrases.candidates[1].word == "tie a" &&
                quick_phrases.candidates[2].word == "tie b" &&
                quick_phrases.candidates[0].source == CandidateSource::QuickPhrase,
            "Quick-phrase prefix ordering diverged from Windows.");
    const auto limited_quick_phrases = metasequoia::local_modes::query_quick_phrases("a", quick_phrase_database, 2);
    require(!limited_quick_phrases.diagnostic.has_value() && limited_quick_phrases.candidates.size() == 2,
            "Quick-phrase query did not honor its limit.");
    require(metasequoia::local_modes::query_quick_phrases("", quick_phrase_database, 10).candidates.empty() &&
                metasequoia::local_modes::query_quick_phrases("A", quick_phrase_database, 10).candidates.empty() &&
                metasequoia::local_modes::query_quick_phrases("a1", quick_phrase_database, 10).candidates.empty() &&
                metasequoia::local_modes::query_quick_phrases("a", quick_phrase_database, 0).candidates.empty(),
            "Quick-phrase query accepted an invalid prefix or limit.");

    const auto missing_quick_phrases =
        metasequoia::local_modes::query_quick_phrases("secret", quick_phrase_directory / "private-missing.db", 10);
    require(missing_quick_phrases.candidates.empty() && missing_quick_phrases.diagnostic.has_value() &&
                missing_quick_phrases.diagnostic->find("secret") == std::string::npos &&
                missing_quick_phrases.diagnostic->find("private-missing") == std::string::npos,
            "A missing quick-phrase database lacked a privacy-safe diagnostic.");

    const std::filesystem::path corrupt_database = quick_phrase_directory / "private-corrupt.db";
    {
        std::ofstream stream(corrupt_database, std::ios::binary);
        stream << "not a sqlite database";
    }
    const auto corrupt_quick_phrases = metasequoia::local_modes::query_quick_phrases("secret", corrupt_database, 10);
    require(corrupt_quick_phrases.candidates.empty() && corrupt_quick_phrases.diagnostic.has_value() &&
                corrupt_quick_phrases.diagnostic->find("secret") == std::string::npos &&
                corrupt_quick_phrases.diagnostic->find("private-corrupt") == std::string::npos,
            "A corrupt quick-phrase database lacked a privacy-safe diagnostic.");

    const std::filesystem::path others_database = quick_phrase_directory / "others.db";
    {
        Database database(others_database);
        database.execute("CREATE TABLE emoji_pinyin(key TEXT,emoji TEXT,sort_order INTEGER)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xiaolian','😀',10)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xiaolian','😄',20)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xiaolian','😀',30)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xl','😀',10)");
        database.execute("INSERT INTO emoji_pinyin VALUES('laugh','😀',10)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xnlm','raw shuangpin match',40)");
        database.execute("CREATE TABLE kaomoji(pinyin TEXT,jianpin TEXT,kaomoji TEXT,sort_order INTEGER)");
        database.execute("INSERT INTO kaomoji VALUES('haixiu','hx','(*/ω＼*)',10)");
        database.execute("INSERT INTO kaomoji VALUES('haixiu','hx','(^_^)',20)");
        database.execute("INSERT INTO kaomoji VALUES('haixiu','hx','(*/ω＼*)',30)");
        database.execute("INSERT INTO kaomoji VALUES('kiss','','( ˘ ³˘)♥',40)");
        database.execute("INSERT INTO kaomoji VALUES('kind','','single prefix',50)");
    }

    const auto emoji = metasequoia::local_modes::query_emoji("XIAOLIAN", SchemeType::Quanpin, others_database, 10);
    require(!emoji.diagnostic.has_value() && emoji.candidates.size() == 2 && emoji.candidates[0].word == "😀" &&
                emoji.candidates[1].word == "😄" && emoji.candidates[0].pinyin == "xiaolian" &&
                emoji.candidates[0].source == CandidateSource::Emoji,
            "Emoji lookup did not normalize, order, or deduplicate full-pinyin matches.");
    require(
        metasequoia::local_modes::query_emoji("xl", SchemeType::Quanpin, others_database, 10).candidates.front().word ==
                "😀" &&
            metasequoia::local_modes::query_emoji("laugh", SchemeType::Quanpin, others_database, 10)
                    .candidates.front()
                    .word == "😀",
        "Emoji lookup did not support jianpin and English keywords.");
    const auto shuangpin_emoji =
        metasequoia::local_modes::query_emoji("xnlm", SchemeType::Shuangpin, others_database, 10);
    require(shuangpin_emoji.candidates.size() == 3 && shuangpin_emoji.candidates[0].word == "😀" &&
                shuangpin_emoji.candidates[1].word == "😄" &&
                shuangpin_emoji.candidates[2].word == "raw shuangpin match",
            "Emoji lookup did not merge raw and Xiaohe-shuangpin prefixes in catalog order.");
    require(
        metasequoia::local_modes::query_emoji("xiaolian", SchemeType::Quanpin, others_database, 1).candidates.size() ==
                1 &&
            metasequoia::local_modes::query_emoji("", SchemeType::Quanpin, others_database, 10).candidates.empty() &&
            metasequoia::local_modes::query_emoji("bad1", SchemeType::Quanpin, others_database, 10)
                .candidates.empty() &&
            metasequoia::local_modes::query_emoji("x", SchemeType::Quanpin, others_database, 0).candidates.empty(),
        "Emoji lookup did not honor its validation and limit rules.");

    const auto kaomoji = metasequoia::local_modes::query_kaomoji("HAIXIU", SchemeType::Quanpin, others_database, 10);
    require(!kaomoji.diagnostic.has_value() && kaomoji.candidates.size() == 2 &&
                kaomoji.candidates[0].word == "(*/ω＼*)" && kaomoji.candidates[1].word == "(^_^)" &&
                kaomoji.candidates[0].pinyin == "haixiu" && kaomoji.candidates[0].source == CandidateSource::Kaomoji,
            "Kaomoji lookup did not normalize, order, or deduplicate full-pinyin matches.");
    require(
        metasequoia::local_modes::query_kaomoji("hx", SchemeType::Quanpin, others_database, 10)
                    .candidates.front()
                    .word == "(*/ω＼*)" &&
            metasequoia::local_modes::query_kaomoji("kiss", SchemeType::Quanpin, others_database, 10)
                    .candidates.front()
                    .word == "( ˘ ³˘)♥" &&
            !metasequoia::local_modes::query_kaomoji("k", SchemeType::Quanpin, others_database, 10).candidates.empty(),
        "Kaomoji lookup did not support jianpin, English, and single-letter prefixes.");
    const auto shuangpin_kaomoji =
        metasequoia::local_modes::query_kaomoji("hx", SchemeType::Shuangpin, others_database, 10);
    require(shuangpin_kaomoji.candidates.size() == 2 && shuangpin_kaomoji.candidates.front().word == "(*/ω＼*)",
            "Kaomoji lookup did not expand Xiaohe shuangpin or deduplicate merged matches.");

    const auto missing_emoji = metasequoia::local_modes::query_emoji(
        "privatecode", SchemeType::Quanpin, quick_phrase_directory / "private-others-missing.db", 10);
    require(missing_emoji.candidates.empty() && missing_emoji.diagnostic.has_value() &&
                missing_emoji.diagnostic->find("privatecode") == std::string::npos &&
                missing_emoji.diagnostic->find("private-others-missing") == std::string::npos,
            "A missing Emoji database lacked a privacy-safe diagnostic.");
    const auto corrupt_kaomoji =
        metasequoia::local_modes::query_kaomoji("privatecode", SchemeType::Quanpin, corrupt_database, 10);
    require(corrupt_kaomoji.candidates.empty() && corrupt_kaomoji.diagnostic.has_value() &&
                corrupt_kaomoji.diagnostic->find("privatecode") == std::string::npos &&
                corrupt_kaomoji.diagnostic->find("private-corrupt") == std::string::npos,
            "A corrupt kaomoji database lacked a privacy-safe diagnostic.");
    std::filesystem::remove_all(quick_phrase_directory);
    return 0;
}
