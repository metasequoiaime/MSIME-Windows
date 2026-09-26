#include "../../core/input_session.h"
#include "../../common/helpcode_utils.h"
#include "../../core/data_path.h"
#include "../../user_dictionary/user_dictionary_journal.h"
#include "test_directory_cleanup.h"
#include "../../contracts/dictionary/format.h"
#include "../../quanpin/quanpin_query.h"
#include "../../quanpin/quanpin_utils.h"
#include "../../shuangpin/shuangpin_profile.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the input-session test dictionary.");
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

    std::int64_t query_integer(const char *sql)
    {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW)
        {
            sqlite3_finalize(statement);
            throw std::runtime_error("Failed to query the input-session test dictionary.");
        }
        const std::int64_t value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

  private:
    sqlite3 *database_ = nullptr;
};

void type(metasequoia::InputSession &session, const std::string &text)
{
    for (const char character : text)
    {
        if (!session.handle_character(character).handled)
        {
            throw std::runtime_error("A pinyin character was not handled.");
        }
    }
}

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void write_file(const std::filesystem::path &path, const std::string &contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << contents;
    if (!stream)
    {
        throw std::runtime_error("Failed to prepare an input-session helpcode fixture.");
    }
}

void set_data_directory(const std::filesystem::path &directory)
{
#ifdef _WIN32
    if (_wputenv_s(L"METASEQUOIA_IME_DATA_DIR", directory.c_str()) != 0)
#else
    if (setenv("METASEQUOIA_IME_DATA_DIR", metasequoia::path_to_utf8(directory).c_str(), 1) != 0)
#endif
    {
        throw std::runtime_error("Failed to set the data directory override.");
    }
}

void prepare_frequency_fixture(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    Database database(directory / "msime.db");
    database.execute("BEGIN;"
                     "CREATE TABLE tbl_1_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '甲', 100);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '乙', 90);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '丙', 80);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '丁', 70);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '戊', 60);"
                     "INSERT INTO tbl_1_n VALUES('ni', 'n', '己', 50);"
                     "COMMIT;");
}

void prepare_shuangpin_frequency_fixture(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    Database database(directory / "msime.db");
    database.execute("BEGIN;"
                     "CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '你好', 100);"
                     "INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '拟好', 50);"
                     "COMMIT;");
}

void prepare_wubi_frequency_fixture(const std::filesystem::path &directory)
{
    std::filesystem::create_directories(directory);
    Database database(directory / "msime.db");
    database.execute("BEGIN;"
                     "CREATE TABLE wubi86(key TEXT, value TEXT, weight INTEGER);"
                     "INSERT INTO wubi86 VALUES('aaaa', '工', 100);"
                     "INSERT INTO wubi86 VALUES('aaaa', '或', 50);"
                     "COMMIT;");
}

std::size_t candidate_index(const metasequoia::InputSession &session, const std::string &word)
{
    const auto found = std::find_if(session.candidates().begin(), session.candidates().end(),
                                    [&](const WordItem &item) { return item.word == word; });
    if (found == session.candidates().end())
    {
        std::string message = "The expected edge-selection candidate was not produced: " + word + "; actual:";
        for (const auto &candidate : session.candidates())
        {
            message += " [" + candidate.word + "]";
        }
        throw std::runtime_error(message);
    }
    return static_cast<std::size_t>(std::distance(session.candidates().begin(), found));
}

bool same_candidate_words(const metasequoia::InputSession &left, const metasequoia::InputSession &right)
{
    if (left.candidates().size() != right.candidates().size())
    {
        return false;
    }
    return std::equal(left.candidates().begin(), left.candidates().end(), right.candidates().begin(),
                      [](const auto &left_item, const auto &right_item) { return left_item.word == right_item.word; });
}

// 同 candidate_index，但未命中返回 candidates().size() 而不是拖出：供「不得出现」断言用。
std::size_t find_candidate_index(const metasequoia::InputSession &session, const std::string &word)
{
    const auto found = std::find_if(session.candidates().begin(), session.candidates().end(),
                                    [&](const WordItem &item) { return item.word == word; });
    return static_cast<std::size_t>(std::distance(session.candidates().begin(), found));
}

// ü 系拼写别名归一与轻标记的会话级回归（AC1–AC6）。自建隔离词库，不依赖主 fixture：
// 词库正键全部用标准拼写，另放真实音节 nu/lu 供隔离断言。打标与开关无关，掩码取
// both 仅代表真实前端配置。
void run_umlaut_alias_session_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "umlaut-alias";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('nve', 'n', '虐', 100);"
                         "INSERT INTO tbl_1_n VALUES('nv', 'n', '女', 90);"
                         "INSERT INTO tbl_1_n VALUES('nu', 'n', '怒', 80);");
        database.execute("CREATE TABLE tbl_1_l(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_l VALUES('lve', 'l', '略', 100);"
                         "INSERT INTO tbl_1_l VALUES('lv', 'l', '绿', 90);"
                         "INSERT INTO tbl_1_l VALUES('lu', 'l', '路', 80);");
        database.execute("CREATE TABLE tbl_1_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_j VALUES('jue', 'j', '决', 100);");
        database.execute("CREATE TABLE tbl_1_e(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_e VALUES('e', 'e', '鹅', 50);");
        database.execute("CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_n VALUES('nu''e', 'ne', '怒鹅', 30);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;

    // AC1 + preedit 非回归：nue 命中 nve 行并带标记，preedit 仍画原样字母。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nue");
        require(session.get_pinyin_segmentation_with_cases() == "nue",
                "The preedit for 'nue' must keep the typed letters, not the alias rewrite.");
        require(session.preedit() == "nue", "The engine preedit for 'nue' must stay unrewritten.");
        const auto found = find_candidate_index(session, "虐");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from == "nue",
                "The 'nue' candidate for 虐 must carry corrected_from='nue'.");
        require(found < session.candidates().size() && session.candidates()[found].pinyin == "nve",
                "The 'nue' candidate must carry the canonical pinyin 'nve'.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nve");
        const auto found = find_candidate_index(session, "虐");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from.empty(),
                "The standard spelling 'nve' must stay unmarked.");
    }

    // AC2：lue/lve 同理。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "lue");
        const auto found = find_candidate_index(session, "略");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from == "lue",
                "The 'lue' candidate for 略 must carry corrected_from='lue'.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "lve");
        const auto found = find_candidate_index(session, "略");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from.empty(),
                "The standard spelling 'lve' must stay unmarked.");
    }

    // AC3：jqxy 系既有归一从无标变带标（行为变更），标准拼法无标。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "jve");
        require(session.get_pinyin_segmentation_with_cases() == "jve",
                "The preedit for 'jve' must keep the typed letters.");
        const auto found = find_candidate_index(session, "决");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from == "jve",
                "The 'jve' candidate for 决 must carry corrected_from='jve'.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "jue");
        const auto found = find_candidate_index(session, "决");
        require(found < session.candidates().size() && session.candidates()[found].corrected_from.empty(),
                "The standard spelling 'jue' must stay unmarked.");
    }

    // AC4：nu/nv/lu/lv 真实音节互不串，标准拼法均无标记。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nu");
        require(find_candidate_index(session, "怒") < session.candidates().size() &&
                    find_candidate_index(session, "女") == session.candidates().size() &&
                    find_candidate_index(session, "虐") == session.candidates().size(),
                "'nu' must only offer 怒, never 女/虐.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nv");
        const auto found = find_candidate_index(session, "女");
        require(found < session.candidates().size() &&
                    find_candidate_index(session, "怒") == session.candidates().size() &&
                    find_candidate_index(session, "虐") == session.candidates().size() &&
                    session.candidates()[found].corrected_from.empty(),
                "'nv' must only offer unmarked 女, never 怒/虐.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "lu");
        require(find_candidate_index(session, "路") < session.candidates().size() &&
                    find_candidate_index(session, "绿") == session.candidates().size() &&
                    find_candidate_index(session, "略") == session.candidates().size(),
                "'lu' must only offer 路, never 绿/略.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "lv");
        const auto found = find_candidate_index(session, "绿");
        require(found < session.candidates().size() &&
                    find_candidate_index(session, "路") == session.candidates().size() &&
                    find_candidate_index(session, "略") == session.candidates().size() &&
                    session.candidates()[found].corrected_from.empty(),
                "'lv' must only offer unmarked 绿, never 路/略.");
    }

    // AC5：手动分隔符 nu'e 切分为 怒+鹅，不触发别名（虐不可见），无标记。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nu'e");
        require(find_candidate_index(session, "怒鹅") < session.candidates().size() &&
                    find_candidate_index(session, "怒") < session.candidates().size() &&
                    find_candidate_index(session, "虐") == session.candidates().size(),
                "'nu'e' must split as 怒+鹅 and never trigger the nue alias.");
        require(std::none_of(session.candidates().begin(), session.candidates().end(),
                             [](const WordItem &item) { return !item.corrected_from.empty(); }),
                "'nu'e' must produce no marked candidates.");
    }

    // AC6：别名命中的候选上屏后，调频数据落在标准拼法键 nve 上。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type(session, "nue");
        (void)session.select_candidate(find_candidate_index(session, "虐"));
        require(!session.has_composition(), "Selecting the only candidate must finish the composition.");
        Database database(directory / "msime.db");
        require(database.query_integer("SELECT weight FROM tbl_1_n WHERE key='nve' AND value='虐'") == 101,
                "Selecting 虐 from 'nue' must update the canonical 'nve' row.");
        require(database.query_integer("SELECT COUNT(*) FROM tbl_1_n WHERE key='nue'") == 0,
                "No user data may accumulate under the alias key 'nue'.");
    }

    std::filesystem::remove_all(directory);
}

// 光标驱动的前缀解码（PRD R2–R7，Stage 1）：候选与量化边界按「光标之前的完整音节
// 单元前缀」重算。自建隔离词库，不与主 fixture 互相污染；Server（Stage 2）将以
// set_caret + recompute_candidates 的同一方式消费这些入口。
void run_caret_prefix_session_tests(const std::filesystem::path &data_directory)
{
    const std::filesystem::path directory = data_directory / "caret-prefix";
    std::filesystem::create_directories(directory);
    {
        Database database(directory / "msime.db");
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_n VALUES('ni','n','你',100);"
                         "INSERT INTO tbl_1_n VALUES('ni','n','拟',90);");
        database.execute("CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_2_n VALUES('ni''hao','nh','你好',200);"
                         "INSERT INTO tbl_2_n VALUES('ni''hao','nh','拟好',100);");
        database.execute("CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_s VALUES('shi','sh','是',100);");
        database.execute("CREATE TABLE tbl_1_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO tbl_1_j VALUES('jie','j','接',100);");
        database.execute("CREATE TABLE wubi86(key TEXT,value TEXT,weight INTEGER);"
                         "INSERT INTO wubi86 VALUES('aaaa','工',100);"
                         "INSERT INTO wubi86 VALUES('aaaa','或',50);");
    }

    metasequoia::RuntimePaths paths;
    paths.resources = directory;
    paths.user_data = directory;
    paths.cache = directory;
    paths.dictionaries = directory;

    const auto words_of = [](const metasequoia::InputSession &session) {
        std::vector<std::string> words;
        words.reserve(session.candidates().size());
        for (const WordItem &item : session.candidates())
        {
            words.push_back(item.word);
        }
        return words;
    };
    const auto same_word_list = [](const metasequoia::InputSession &session, const std::vector<std::string> &expected) {
        if (session.candidates().size() != expected.size())
        {
            return false;
        }
        return std::equal(session.candidates().begin(), session.candidates().end(), expected.begin(),
                          [](const WordItem &left, const std::string &right) { return left.word == right; });
    };

    // R2/R3/R7：音节内 caret 向下取整到最后一个完整单元边界，候选等于前缀的候选；
    // caret 未设置或落在末尾时与现状整串解码零差异。
    const std::string sentence = "ni'hao'shi'jie";
    const auto quanpin_words = [&](const std::string &typed) {
        metasequoia::InputSession other(SchemeType::Quanpin, 0, true, true, false, paths);
        type(other, typed);
        return words_of(other);
    };
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        type(session, sentence);
        require(session.has_composition() && session.prefix_end() == sentence.size() &&
                    session.pending_suffix().empty(),
                "An unset caret must decode the whole string");
        const auto full_words = words_of(session);
        require(same_word_list(session, full_words),
                "The unset-caret baseline diverged from a plain typed composition");

        for (const std::size_t caret : {std::size_t(5), std::size_t(6)})
        {
            session.set_caret(caret);
            session.recompute_candidates();
            require(session.prefix_end() == 3, "An intra-syllable caret must floor to the last complete unit boundary");
            require(session.pending_suffix() == "hao'shi'jie",
                    "The pending suffix must keep the raw spelling the decode did not consume");
            require(candidate_index(session, "你") < session.candidates().size(),
                    "The 'ni' prefix lost its dictionary candidates");
            require(same_word_list(session, quanpin_words("ni")),
                    "The floored prefix must decode exactly like the typed prefix spelling");
        }

        session.set_caret(7);
        session.recompute_candidates();
        require(session.prefix_end() == 7 && session.pending_suffix() == "shi'jie",
                "A caret on the hao boundary must consume ni'hao");
        require(candidate_index(session, "你好") < session.candidates().size(),
                "The ni'hao prefix lost its phrase candidates");
        require(same_word_list(session, quanpin_words("ni'hao")),
                "The ni'hao prefix must decode exactly like a typed ni'hao");
        const auto at_hao = words_of(session);
        session.set_caret(9);
        session.recompute_candidates();
        require(session.prefix_end() == 7 && same_word_list(session, at_hao),
                "A caret inside 'shi' must floor back to the hao boundary");

        session.set_caret(13);
        session.recompute_candidates();
        require(session.prefix_end() == 11 && session.pending_suffix() == "jie",
                "A caret inside 'jie' must floor to the shi boundary");
        require(!session.candidates().empty() && same_word_list(session, quanpin_words("ni'hao'shi")),
                "The ni'hao'shi prefix must decode exactly like the typed spelling");

        // R4：量化后前缀为空 → 无候选，raw/preedit/caret 原样。
        session.set_caret(0);
        session.recompute_candidates();
        require(session.candidates().empty(), "A caret before the first unit must offer no candidate");
        require(session.prefix_end() == 0 && session.pending_suffix() == sentence,
                "An empty prefix must leave the whole string pending");
        require(session.editing_text() == sentence && session.caret_position() == 0 && session.preedit() == sentence,
                "An empty prefix must not disturb the composition or the preedit");

        // 越界 caret 被夹到串尾 → 退化为整串解码（R7）。
        session.set_caret(sentence.size() + 10);
        session.recompute_candidates();
        require(session.caret_position() == sentence.size() && session.prefix_end() == sentence.size(),
                "An out-of-range caret must clamp to the end");
        require(same_word_list(session, full_words), "A caret clamped to the end must restore the full-string decode");
        session.set_caret(std::nullopt);
        session.recompute_candidates();
        require(same_word_list(session, full_words), "Unsetting the caret must restore the full-string decode");
    }

    // pending_suffix 保留原始大小写：大写字母从光标处插入后原样留在后缀里。
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, true, true, false, paths);
        type(session, "ni'hao");
        session.handle_command(metasequoia::Command::MoveHome);
        require(session.handle_character('H').handled, "The uppercase insert at the caret was rejected");
        require(session.editing_text() == "Hni'hao" && session.caret_position() == 1,
                "The uppercase insert lost its case or position");
        session.set_caret(0);
        session.recompute_candidates();
        require(session.prefix_end() == 0 && session.pending_suffix() == "Hni'hao",
                "The pending suffix must preserve the typed casing");
        session.set_caret(std::nullopt);
        session.recompute_candidates();
        require(session.editing_text() == "Hni'hao", "Restoring the end caret altered the raw text");
    }

    // 无单元模型（五笔）：segment_raw_boundaries 为空 → caret 移动不量化，候选零变化。
    {
        metasequoia::InputSession session(SchemeType::Wubi, 0, true, true, false, paths);
        type(session, "aaaa");
        require(session.has_composition() && candidate_index(session, "工") < session.candidates().size(),
                "The wubi fixture lost its candidates");
        const auto native = words_of(session);
        for (const std::size_t caret : {std::size_t(0), std::size_t(2)})
        {
            session.set_caret(caret);
            session.recompute_candidates();
            require(same_word_list(session, native), "A scheme without the unit model must not re-decode by caret");
            require(session.prefix_end() == 4 && session.pending_suffix().empty(),
                    "Without a unit model the caret never shortens the decode");
        }
    }

    // 双拼贪心配对（engine spec #187）：nihkb; → {0,2,4,6}、nihcb; → {0,2,3,5,6}，
    // caret 落在段中间时同样 floor 到完整段边界。
    const auto shuangpin_words = [&](const std::string &typed) {
        metasequoia::InputSession other(SchemeType::Shuangpin, GetMicrosoftShuangpinProfile(), paths);
        type(other, typed);
        return words_of(other);
    };
    {
        metasequoia::InputSession session(SchemeType::Shuangpin, GetMicrosoftShuangpinProfile(), paths);
        type(session, "nihkb;");
        require(candidate_index(session, "你好") < session.candidates().size(),
                "The shuangpin baseline lost its phrase candidates");
        require(session.prefix_end() == 6 && session.pending_suffix().empty(),
                "The unset shuangpin caret must decode the whole string");

        session.set_caret(1);
        session.recompute_candidates();
        require(session.candidates().empty() && session.prefix_end() == 0 && session.pending_suffix() == "nihkb;",
                "A caret inside the first shuangpin unit must quantize to an empty prefix");

        session.set_caret(3);
        session.recompute_candidates();
        require(session.prefix_end() == 2 && session.pending_suffix() == "hkb;",
                "A caret inside the hk unit must floor to the ni boundary");
        require(same_word_list(session, shuangpin_words("ni")),
                "The shuangpin 'ni' prefix must decode exactly like the typed spelling");

        session.set_caret(5);
        session.recompute_candidates();
        require(session.prefix_end() == 4 && session.pending_suffix() == "b;",
                "A caret inside the b; unit must floor to the hk boundary");
        require(candidate_index(session, "你好") < session.candidates().size(),
                "The shuangpin nihk prefix lost its phrase candidates");
        require(same_word_list(session, shuangpin_words("nihk")),
                "The shuangpin nihk prefix must decode exactly like the typed spelling");

        session.set_caret(6);
        session.recompute_candidates();
        require(session.prefix_end() == 6 && !session.candidates().empty(),
                "A caret on the final shuangpin boundary must restore the full decode");

        session.handle_command(metasequoia::Command::Cancel);
        type(session, "nihcb;");
        session.set_caret(4);
        session.recompute_candidates();
        require(session.prefix_end() == 3 && session.pending_suffix() == "cb;",
                "The greedy pairing boundary must floor the caret to where cb starts");
        require(same_word_list(session, shuangpin_words("nih")),
                "The shuangpin nih prefix must decode exactly like the typed spelling");
    }

    std::filesystem::remove_all(directory);
}
} // namespace

int run_test()
{
    const auto unique_suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::filesystem::path data_directory =
        std::filesystem::temp_directory_path() / std::filesystem::u8path("metasequoia-session-词库-" + unique_suffix);
    metasequoia::test::ScopedDataDirectoryCleanup cleanup(data_directory);
    std::filesystem::create_directories(data_directory);
    set_data_directory(data_directory);

#ifndef METASEQUOIA_FREQUENCY_TESTS_ONLY
    {
        const std::filesystem::path helpcode_directory = data_directory / "helpcodes";
        write_file(helpcode_directory / "helpcode.txt", "你=ab\n拟=cd\n好=ef\n");
        write_file(helpcode_directory / "zrm_helpcode_big_unique.txt", "你=cb\n拟=ad\n好=ef\n");
        write_file(helpcode_directory / "shouyou2_0_helpcode.txt", "你=ab\n拟=cd\n好=ef\n");
        write_file(helpcode_directory / "shouyouplus_helpcode.txt", "你=ab\n拟=cd\n好=ef\n");
        write_file(helpcode_directory / "xiaohe_helpcode.txt", "你=ab\n拟=cd\n好=ef\n");

        Database database(data_directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '你好', 200)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '拟好', 100)");
        database.execute("CREATE TABLE tbl_2_z(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_2_z VALUES('zhong''guo', 'zg', '中国', 200)");
        database.execute("CREATE TABLE tbl_2_d(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_2_d VALUES('dong''gua', 'dg', '冬瓜', 200)");
        database.execute("INSERT INTO tbl_2_d VALUES('dong''an', 'da', '东安', 200)");
        database.execute("CREATE TABLE tbl_2_b(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_2_b VALUES('bu''hao', 'bh', '不好', 200)");
        database.execute("INSERT INTO tbl_2_b VALUES('bu''hao', 'bh', '补好', 100)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '𠀀方案𠮷', 90)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', 'C语言 2', 80)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', 'GitHub', 70)");
        database.execute("CREATE TABLE tbl_1_j(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_1_j VALUES('ju', 'j', '居', 100)");
        database.execute("CREATE TABLE tbl_1_q(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_1_q VALUES('qu', 'q', '去', 100)");
        database.execute("CREATE TABLE tbl_1_x(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_1_x VALUES('xu', 'x', '需', 100)");
        database.execute("CREATE TABLE tbl_1_y(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_1_y VALUES('yu', 'y', '与', 100)");

        database.execute("INSERT INTO tbl_1_x VALUES('xi', 'x', '西', 100)");
        database.execute("CREATE TABLE tbl_2_t(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO tbl_2_t VALUES('te''le','tl','特乐',100)");
        database.execute("CREATE TABLE tbl_3_x(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        {
            metasequoia::InputSession portable(SchemeType::Quanpin, true, false);
            type(portable, "xi'te'le");
            const auto first = portable.select_candidate(candidate_index(portable, "西"));
            require(first.commit == "西" && portable.has_composition() && portable.preedit() == "te'le",
                    "Portable selection discarded the unconsumed pinyin suffix.");
            const auto last = portable.select_candidate(candidate_index(portable, "特乐"));
            require(last.commit == "特乐" && !portable.has_composition(),
                    "Portable continuation duplicated the committed prefix or retained completed input.");
            require(database.query_integer("SELECT COUNT(*) FROM tbl_3_x WHERE key='xi''te''le' AND value='西特乐'") ==
                        1,
                    "Portable composition did not learn the canonical phrase across selections.");
            type(portable, "xi'te'le");
            (void)portable.select_candidate(candidate_index(portable, "西"));
            require(portable.handle_command(metasequoia::Command::Cancel).handled && !portable.has_composition(),
                    "Cancel did not clear an incomplete portable phrase.");

            type(portable, "xi'te'le");
            (void)portable.select_candidate(candidate_index(portable, "西"));
            while (portable.has_composition())
            {
                require(portable.handle_command(metasequoia::Command::Backspace).handled,
                        "Backspace did not consume the abandoned portable composition.");
            }
            type(portable, "nihao");
            require(portable.select_candidate(candidate_index(portable, "你好")).commit == "你好",
                    "The composition following an abandoned phrase could not be committed.");
            require(database.query_integer("SELECT COUNT(*) FROM tbl_3_x WHERE value='西你好'") == 0,
                    "A phrase abandoned by Backspace was learned together with the next composition.");
        }
        {
            metasequoia::InputSession unlearned(SchemeType::Quanpin, true, false, true, false);
            type(unlearned, "xi'te'le");
            (void)unlearned.select_candidate(candidate_index(unlearned, "西"));
            const auto punctuation = unlearned.handle_punctuation(',');
            require(punctuation.commit == "特乐，" && !unlearned.has_composition(),
                    "Punctuation failed to finish the remaining portable composition atomically.");
        }

        // 整句候选（词格 / Google 解码器）在词库里没有对应的行，调频无处落笔：用户
        // 选中一条整句多少次，它下次仍然要靠猜，排序也跟着重算。选中即落成用户词组，
        // 之后同样的输入就由词库那一行来回答。
        database.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO tbl_1_n VALUES('na','n','那',100)");
        database.execute("INSERT INTO tbl_1_y VALUES('yi','y','一',100)");
        database.execute("CREATE TABLE tbl_1_t(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO tbl_1_t VALUES('tiao','t','条',100)");
        database.execute("CREATE TABLE tbl_3_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        {
            metasequoia::InputSession sentence(SchemeType::Quanpin);
            // 整句联想默认全关，这一段考的就是整句候选，得先把造句那两条打开。
            SentenceAssociationOptions association;
            association.word_lattice = true;
            association.google = true;
            sentence.set_sentence_association(association);
            type(sentence, "na'yi'tiao");
            const auto guessed = candidate_index(sentence, "那一条");
            const auto guessed_source = sentence.candidates()[guessed].source;
            require(guessed_source == CandidateSource::Generated || guessed_source == CandidateSource::Fallback,
                    "The sentence under test must be a guess, not a dictionary row.");
            require(sentence.select_candidate(guessed).commit == "那一条" && !sentence.has_composition(),
                    "Selecting a whole-sentence candidate did not commit it.");
            require(
                database.query_integer("SELECT COUNT(*) FROM tbl_3_n WHERE key='na''yi''tiao' AND value='那一条'") == 1,
                "A selected whole-sentence candidate was not learned as a user phrase.");

            type(sentence, "na'yi'tiao");
            const auto learned = candidate_index(sentence, "那一条");
            require(sentence.candidates()[learned].source != CandidateSource::Generated &&
                        sentence.candidates()[learned].source != CandidateSource::Fallback,
                    "The learned sentence did not come back as a dictionary row.");
            require(learned == 0, "The learned sentence did not outrank the guessed ones.");
            (void)sentence.select_candidate(learned);
        }
        {
            // 学习整句是造词，不是调频：关掉候选学习的会话一条也不该落库。
            metasequoia::InputSession unlearned(SchemeType::Quanpin, 0, true, true, false);
            SentenceAssociationOptions association;
            association.word_lattice = true;
            association.google = true;
            unlearned.set_sentence_association(association);
            type(unlearned, "na'yi'na");
            const auto guessed = candidate_index(unlearned, "那一那");
            (void)unlearned.select_candidate(guessed);
            require(database.query_integer("SELECT COUNT(*) FROM tbl_3_n WHERE value='那一那'") == 0,
                    "A session with candidate learning disabled still stored a whole-sentence candidate.");
        }

        // A seven-syllable key and eight/nine-syllable keys cross the shipping
        // table boundary. Creation, normal query and upgrade replay must agree.
        database.execute("CREATE TABLE tbl_7_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("CREATE TABLE tbl_others_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        const auto replay_path = data_directory / "replay.db";
        const auto journal_path = metasequoia::path_to_utf8(data_directory / "format-journal.db");
        Database replay_database(replay_path);
        replay_database.execute("CREATE TABLE tbl_7_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        replay_database.execute("CREATE TABLE tbl_others_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        for (const int count : {7, 8, 9})
        {
            std::string key, word;
            for (int i = 0; i < count; ++i)
            {
                if (i)
                    key += "'";
                key += "ni";
                word += "你";
            }
            const std::string expected_table = count == 7 ? "tbl_7_n" : "tbl_others_n";
            require(quanpin::build_table_name(std::vector<std::string>(count, "ni")) == expected_table,
                    "Runtime lookup selected the wrong long-phrase table.");
            metasequoia::InputSession writer(SchemeType::Quanpin);
            require(writer.store_user_phrase_from_canonical_pinyin(key, word) == 0,
                    "Canonical phrase creation could not write the public format.");
            type(writer, key);
            (void)candidate_index(writer, word);
            require(user_dictionary::record_user_insert(journal_path, user_dictionary::DictionaryKind::Pinyin, key,
                                                        word, 10000),
                    "The long-phrase journal operation failed.");
        }
        const auto replay = user_dictionary::replay(journal_path, metasequoia::path_to_utf8(replay_path),
                                                    metasequoia::path_to_utf8(data_directory / "replay-english.db"));
        require(replay.failed == 0 && replay.applied == 3 &&
                    replay_database.query_integer("SELECT COUNT(*) FROM tbl_7_n") == 1 &&
                    replay_database.query_integer("SELECT COUNT(*) FROM tbl_others_n") == 2,
                "Upgrade replay disagreed with the public long-phrase table format.");

        // Hosts that insert text asynchronously use the same composition owner as
        // portable character/command clients. Exercise their sequence boundary here.
        for (const bool raw_dispatch : {false, true})
        {
            metasequoia::InputSession session(SchemeType::Quanpin);
            const std::string input = "xi'te'le";
            if (raw_dispatch)
            {
                session.set_pinyin_sequence(input);
                session.set_pinyin_sequence_with_cases(input);
                session.recompute_candidates();
            }
            else
            {
                type(session, input);
            }
            const auto query_before_selection = session.online_query();
            const auto first = session.advance_composition_after_selection("xi", "西", "xi");
            require(first.continues_composition && session.get_pinyin_sequence() == "te'le",
                    "Partial selection lost the remaining manually delimited input.");
            require(first.consumed_raw_input_with_cases == "xi",
                    "Partial selection did not report the raw input it consumed.");
            const auto progress = session.update_creating_word_progress("", "", "西", first);
            require(!progress.completed && progress.pinyin == "xi" && progress.preedit == "西te'le",
                    "Partial selection produced the wrong phrase progress.");
            require(query_before_selection && !session.apply_online_candidate(*query_before_selection, "旧",
                                                                              CandidateSource::CloudSuggestion),
                    "Partial selection accepted a result for the previous composition.");
            const auto modern_query = session.online_query();
            const auto host_query = session.get_cloud_query_state();
            require(modern_query && modern_query->query_text == host_query.query_text &&
                        modern_query->cache_key == host_query.cache_key,
                    "Portable and asynchronous hosts produced different online queries.");
            const auto last = session.advance_composition_after_selection("te'le", "特乐", "te'le");
            require(!last.continues_composition && last.consumed_raw_input_with_cases == "te'le",
                    "The final selection did not report the raw input it consumed.");
            const auto complete = session.update_creating_word_progress(progress.pinyin, progress.word, "特乐", last);
            require(complete.completed && complete.can_store && complete.pinyin == "xi'te'le" &&
                        complete.word == "西特乐",
                    "The completed phrase did not retain canonical pinyin across selections.");
            const auto invalid = session.update_creating_word_progress("", "西", "特乐", last);
            require(invalid.completed && !invalid.can_store && invalid.pinyin.empty(),
                    "A phrase with an earlier unknown canonical reading became storeable.");
            session.set_pinyin_sequence("pending");
            session.reset_state();
            session.recompute_candidates();
            require(!session.has_composition(), "Reset left a pending host composition alive.");
        }

        // A host that retracts a selected segment replays the reported spelling,
        // so it must keep the casing the user typed rather than reuse the
        // normalized pre-selection raw.
        {
            metasequoia::InputSession session(SchemeType::Quanpin);
            session.set_pinyin_sequence("xi'te'le");
            session.set_pinyin_sequence_with_cases("Xi'Te'Le");
            session.recompute_candidates();
            const auto transition = session.advance_composition_after_selection("xi", "西", "xi");
            require(transition.continues_composition && transition.consumed_raw_input_with_cases == "Xi",
                    "Consumed input lost the user's original casing.");
            // Retracting that selection replays the reported spelling. The
            // restored raw must survive the host-side round trip unchanged.
            session.set_pinyin_sequence(transition.consumed_raw_input_with_cases);
            session.set_pinyin_sequence_with_cases(transition.consumed_raw_input_with_cases);
            session.recompute_candidates();
            require(session.get_pinyin_sequence_with_cases() == "Xi" && session.get_pinyin_sequence() == "xi",
                    "Restoring the consumed spelling did not round-trip through the session.");
        }

        metasequoia::InputSession default_session;
        require(default_session.scheme_type() == SchemeType::Quanpin,
                "The default input scheme should be full pinyin.");
        require(default_session.quanpin_autocorrect_types() == 0,
                "Pinyin autocorrection should default to off (no type bits set).");
        require(default_session.helpcode_enabled(), "Helpcode should be enabled by default.");
        require(default_session.chinese_punctuation_enabled(), "Chinese punctuation should be enabled by default.");
        require(default_session.candidate_learning_enabled(), "Candidate learning should be enabled by default.");

        struct UmlautAliasCase
        {
            const char *pinyin;
            const char *candidate;
        };
        const std::array<UmlautAliasCase, 4> umlaut_alias_cases = {
            {{"jv", "居"}, {"qv", "去"}, {"xv", "需"}, {"yv", "与"}}};
        for (const auto &test_case : umlaut_alias_cases)
        {
            metasequoia::InputSession alias_session;
            type(alias_session, test_case.pinyin);
            require(candidate_index(alias_session, test_case.candidate) == 0,
                    "A v-form umlaut syllable did not query its canonical dictionary key.");
        }

        const std::array<UmlautAliasCase, 6> missing_final_g_cases = {{{"zhonguo", "中国"},
                                                                       {"zhon'guo", "中国"},
                                                                       {"zhongguo", "中国"},
                                                                       {"dongua", "冬瓜"},
                                                                       {"donggua", "冬瓜"},
                                                                       {"dongan", "东安"}}};
        for (const auto &test_case : missing_final_g_cases)
        {
            metasequoia::InputSession corrected;
            type(corrected, test_case.pinyin);
            require(candidate_index(corrected, test_case.candidate) == 0,
                    "A missing final g did not resolve to the complete dictionary phrase.");
            require(corrected.select_candidate(0).commit == test_case.candidate && !corrected.has_composition(),
                    "Selecting a corrected phrase left an unconsumed input suffix.");
        }

        metasequoia::InputSession no_autocorrect_session(SchemeType::Quanpin, 0u);
        require(no_autocorrect_session.quanpin_autocorrect_types() == 0,
                "The requested pinyin autocorrect setting was not retained.");
        const unsigned both_types = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
        no_autocorrect_session.set_quanpin_autocorrect_types(both_types);
        require(no_autocorrect_session.quanpin_autocorrect_types() == both_types,
                "The requested pinyin autocorrect mask was not retained.");
        metasequoia::InputSession no_helpcode_session(SchemeType::Quanpin, true, false);
        require(!no_helpcode_session.helpcode_enabled(), "The requested helpcode setting was not retained.");
        type(no_helpcode_session, "ni");
        require(!no_helpcode_session.handle_character('H').handled && no_helpcode_session.preedit() == "ni",
                "A constructor-disabled Quanpin helpcode key was swallowed.");

        metasequoia::InputSession shuangpin_session(SchemeType::Shuangpin);
        require(shuangpin_session.scheme_type() == SchemeType::Shuangpin,
                "The requested double-pinyin scheme was not retained.");
        require(shuangpin_session.handle_character('n').handled && shuangpin_session.preedit() == "n",
                "A valid Shuangpin letter was rejected.");
        metasequoia::InputSession japanese_session(SchemeType::JapaneseRomaji);
        require(japanese_session.handle_character('k').handled && japanese_session.preedit() == "k",
                "A valid Japanese romaji letter was rejected.");
        metasequoia::InputSession wubi_session(SchemeType::Wubi);
        require(!wubi_session.handle_character('z').handled && wubi_session.preedit().empty(),
                "An unsupported Wubi letter was swallowed.");
        type(wubi_session, "abcd");
        require(!wubi_session.handle_character('e').handled && wubi_session.preedit() == "abcd",
                "A Wubi letter beyond the four-code limit was swallowed.");
        require(!wubi_session.handle_character('\'').handled && wubi_session.preedit() == "abcd",
                "An unsupported Wubi apostrophe was swallowed.");

        metasequoia::InputSession ascii_punctuation_session(SchemeType::Quanpin, true, true, false);
        require(!ascii_punctuation_session.chinese_punctuation_enabled(),
                "The requested punctuation setting was not retained.");
        require(!ascii_punctuation_session.handle_punctuation('.').handled,
                "Disabled Chinese punctuation swallowed ASCII punctuation.");

        metasequoia::InputSession no_learning_session(SchemeType::Quanpin, true, true, true, false);
        require(!no_learning_session.candidate_learning_enabled(),
                "The requested candidate-learning setting was not retained.");
        type(no_learning_session, "buhao");
        require(no_learning_session.candidates().size() >= 2 && no_learning_session.candidates().front().word == "不好",
                "The learning test dictionary did not preserve its initial order.");
        const auto unlearned_selection = no_learning_session.select_candidate(1);
        require(unlearned_selection.handled && unlearned_selection.commit == "补好",
                "Candidate selection failed while learning was disabled.");
        metasequoia::InputSession verify_unlearned_session;
        type(verify_unlearned_session, "buhao");
        require(!verify_unlearned_session.candidates().empty() &&
                    verify_unlearned_session.candidates().front().word == "不好",
                "A selected candidate was learned while candidate learning was disabled.");

        metasequoia::InputSession uppercase_session;
        require(!uppercase_session.handle_character('N').handled,
                "An uppercase letter was swallowed while no composition was active.");
        type(uppercase_session, "ni");
        require(uppercase_session.handle_character('H').handled && uppercase_session.preedit() == "niH",
                "An uppercase helpcode was rejected from an active pinyin composition.");
        uppercase_session.handle_command(metasequoia::Command::Cancel);

        metasequoia::InputSession duplicate_apostrophe_session;
        type(duplicate_apostrophe_session, "ni");
        require(duplicate_apostrophe_session.handle_character('\'').handled,
                "The first Pinyin apostrophe was rejected.");
        require(!duplicate_apostrophe_session.handle_character('\'').handled &&
                    duplicate_apostrophe_session.preedit() == "ni'",
                "A duplicate Pinyin apostrophe was swallowed.");

        metasequoia::InputSession session(SchemeType::Quanpin, true, true, true, false);
        type(session, "nihao");
        require(session.preedit() == "nihao", "The preedit did not mirror the raw pinyin.");
        require(session.raw_segmentation() == "ni'hao" && session.normalized_segmentation() == "ni'hao",
                "Quanpin segmentation was not exposed through the native session API.");
        require(session.has_composition(), "Typing pinyin did not start a composition.");
        require(session.candidates().size() >= 2, "The engine did not return both dictionary candidates.");

        const auto selected = session.select_candidate(static_cast<std::size_t>(1));
        require(selected.handled && selected.commit == "拟好", "Selecting the second candidate committed wrong text.");
        require(!session.has_composition(), "Selecting a candidate did not end the composition.");

        type(session, "nihao");
        const auto by_word = session.select_candidate(std::string("拟好"));
        require(by_word.handled && by_word.commit == "拟好", "Selecting a candidate by word committed the wrong text.");

        type(session, "nihao");
        const auto out_of_range = session.select_candidate(session.candidates().size());
        require(!out_of_range.handled, "An out-of-range candidate index was accepted.");
        const auto unknown_word = session.select_candidate(std::string("没有这个词"));
        require(!unknown_word.handled, "An unknown candidate word was accepted.");

        const auto leading = session.handle_command(metasequoia::Command::CommitCandidate);
        require(leading.handled && leading.commit == "你好", "CommitCandidate did not commit the leading candidate.");

        type(session, "nihao");
        const auto composed_punctuation = session.handle_punctuation(',');
        require(composed_punctuation.handled && composed_punctuation.commit == "你好，",
                "Punctuation did not commit the candidate atomically.");
        require(session.handle_punctuation('.').commit == "。", "Idle Chinese punctuation was not converted.");
        require(session.handle_punctuation('"').commit == "“" && session.handle_punctuation('"').commit == "”",
                "Double quotes did not alternate between opening and closing Chinese quotes.");
        require(session.handle_punctuation('\'').commit == "‘" && session.handle_punctuation('\'').commit == "’",
                "Single quotes did not alternate between opening and closing Chinese quotes.");
        require(session.handle_punctuation('(').commit == "（" && session.handle_punctuation(')').commit == "）",
                "Parentheses were not converted to Chinese punctuation.");
        require(session.handle_punctuation('[').commit == "【" && session.handle_punctuation(']').commit == "】",
                "Square brackets were not converted to Chinese punctuation.");
        require(session.handle_punctuation('`').commit == "·" && session.handle_punctuation('$').commit == "￥" &&
                    session.handle_punctuation('^').commit == "……" && session.handle_punctuation('_').commit == "——",
                "The keys whose Chinese form differs from ASCII were not converted.");

        // The outer pair is 《》 and anything inside it uses 〈〉, so depth decides the mark.
        require(session.handle_punctuation('<').commit == "《" && session.handle_punctuation('<').commit == "〈" &&
                    session.handle_punctuation('>').commit == "〉" && session.handle_punctuation('>').commit == "》",
                "Book title marks did not nest.");
        // An unmatched '>' must not drive the depth below zero, or the next '<' would open with 〈.
        require(session.handle_punctuation('>').commit == "》", "An unmatched closing book title mark was not 》.");
        require(session.handle_punctuation('<').commit == "《",
                "An unmatched closing book title mark left the nesting depth negative.");
        require(session.handle_punctuation('>').commit == "》", "Book title nesting did not return to depth zero.");
        require(session.handle_punctuation('<').commit == "《" && session.handle_punctuation('>').commit == "》",
                "Book-title brackets were not converted to Chinese punctuation.");
        require(session.handle_punctuation('\\').commit == "、", "The enumeration comma was not converted.");

        type(session, "nihao");
        const auto digit = session.handle_candidate_key('2');
        require(digit.handled && digit.commit == "拟好", "The 2 key did not commit the second candidate.");

        metasequoia::InputSession learning_source_session;
        type(learning_source_session, "nihao");
        const auto learned_selection = learning_source_session.select_candidate(1);
        require(learned_selection.handled && learned_selection.commit == "拟好",
                "The learning source candidate was not selected.");
        metasequoia::InputSession learned_session;
        type(learned_session, "nihao");
        require(!learned_session.candidates().empty() && learned_session.candidates().front().word == "拟好",
                "Selecting a candidate did not promote it for the next matching input.");

        type(session, "nihao");
        const auto first_bmp =
            session.select_candidate_edge(candidate_index(session, "拟好"), metasequoia::CandidateEdge::FirstHan);
        require(first_bmp.handled && first_bmp.commit == "拟" && !session.has_composition(),
                "FirstHan did not commit the first BMP Han character and reset the composition.");

        type(session, "nihao");
        const auto last_bmp =
            session.select_candidate_edge(candidate_index(session, "拟好"), metasequoia::CandidateEdge::LastHan);
        require(last_bmp.handled && last_bmp.commit == "好" && !session.has_composition(),
                "LastHan did not commit the last BMP Han character and reset the composition.");

        type(session, "nihao");
        const auto first_supplementary =
            session.select_candidate_edge(candidate_index(session, "𠀀方案𠮷"), metasequoia::CandidateEdge::FirstHan);
        require(first_supplementary.handled && first_supplementary.commit == "𠀀" && !session.has_composition(),
                "FirstHan split a supplementary-plane Han character.");

        type(session, "nihao");
        const auto last_supplementary =
            session.select_candidate_edge(candidate_index(session, "𠀀方案𠮷"), metasequoia::CandidateEdge::LastHan);
        require(last_supplementary.handled && last_supplementary.commit == "𠮷" && !session.has_composition(),
                "LastHan split a supplementary-plane Han character.");

        type(session, "nihao");
        const auto first_mixed =
            session.select_candidate_edge(candidate_index(session, "C语言 2"), metasequoia::CandidateEdge::FirstHan);
        require(first_mixed.handled && first_mixed.commit == "语", "FirstHan did not skip a non-Han candidate prefix.");

        type(session, "nihao");
        const auto last_mixed =
            session.select_candidate_edge(candidate_index(session, "C语言 2"), metasequoia::CandidateEdge::LastHan);
        require(last_mixed.handled && last_mixed.commit == "言", "LastHan did not skip a non-Han candidate suffix.");

        type(session, "nihao");
        const auto no_han =
            session.select_candidate_edge(candidate_index(session, "GitHub"), metasequoia::CandidateEdge::FirstHan);
        require(!no_han.handled && session.has_composition(),
                "A candidate without Han characters was consumed by edge selection.");
        session.handle_command(metasequoia::Command::Cancel);

        type(session, "nihao");
        session.handle_command(metasequoia::Command::Backspace);
        require(session.preedit() == "niha", "Backspace did not remove the last pinyin character.");
        const auto raw = session.handle_command(metasequoia::Command::CommitRaw);
        require(raw.handled && raw.commit == "niha", "CommitRaw did not commit the typed input.");

        type(session, "nihao");
        const auto cancel = session.handle_command(metasequoia::Command::Cancel);
        require(cancel.handled && !cancel.commit.has_value() && !session.has_composition(),
                "Cancel did not discard the composition.");

        type(session, "nihao");
        session.switch_scheme(SchemeType::Wubi);
        require(session.scheme() == SchemeType::Wubi, "Switching to Wubi did not update the active scheme.");
        require(!session.has_composition() && session.candidates().empty(),
                "Switching schemes did not discard the old composition.");

        session.switch_scheme(SchemeType::JapaneseRomaji);
        require(session.scheme() == SchemeType::JapaneseRomaji,
                "Switching without a composition did not update the active scheme.");
        session.switch_scheme(SchemeType::Quanpin);

        const std::vector<std::string> supported_helpcode_schemas{"lantian",     "ziranma", "shouyou2_0",
                                                                  "shouyouplus", "xiaohe",  "jiajia"};
        for (const std::string &schema : supported_helpcode_schemas)
        {
            require(metasequoia::InputSession::is_supported_helpcode_schema(schema) &&
                        metasequoia::InputSession::select_helpcode_schema(schema),
                    "A Windows-supported helpcode schema was rejected.");
        }
        require(!metasequoia::InputSession::is_supported_helpcode_schema("unknown") &&
                    !metasequoia::InputSession::select_helpcode_schema("unknown"),
                "An unknown helpcode schema was accepted.");
        // User tables in helpcodes/custom: UTF-8 file names, a BOM, CRLF and a full-width colon in the header.
        const std::filesystem::path custom_directory = data_directory / "helpcodes" / "custom";
        write_file(custom_directory / std::filesystem::u8path("我的码.txt"),
                   "\xEF\xBB\xBF# name\xEF\xBC\x9A 我的辅助码\n# name_en: Mine\n你=cb\n拟=ab\n好=ef\n");
        write_file(custom_directory / "plain.txt", "你=ab\n");
        write_file(custom_directory / "README.md", "# name: not a schema\n");
        const auto custom_schemas = HelpcodeUtils::list_custom_helpcode_schemas(data_directory);
        require(custom_schemas.size() == 2 && custom_schemas[0].schema == "custom/plain" &&
                    custom_schemas[0].name.empty() && custom_schemas[0].name_en.empty() &&
                    custom_schemas[1].schema == "custom/我的码" && custom_schemas[1].name == "我的辅助码" &&
                    custom_schemas[1].name_en == "Mine",
                "Custom helpcode schemas were not discovered with their header names.");
        require(HelpcodeUtils::is_helpcode_schema_available(data_directory, "custom/我的码") &&
                    HelpcodeUtils::is_helpcode_schema_available(data_directory, "lantian") &&
                    !HelpcodeUtils::is_helpcode_schema_available(data_directory, "custom/missing"),
                "Custom helpcode availability did not follow the files on disk.");
        for (const std::string schema : {"custom/", "custom/../helpcode", "custom/a\\b", "custom/.hidden"})
        {
            require(!metasequoia::InputSession::is_supported_helpcode_schema(schema),
                    "A custom helpcode schema escaping its directory was accepted.");
        }
        const auto custom_keymap = HelpcodeUtils::load_helpcode_keymap(data_directory, "custom/我的码");
        require(custom_keymap->size() == 3 && custom_keymap->at("你") == "cb",
                "A custom helpcode table was not loaded past its header and BOM.");
        {
            metasequoia::InputSession custom_helpcode(SchemeType::Quanpin);
            custom_helpcode.set_quanpin_helpcode_enabled(true);
            require(custom_helpcode.set_helpcode_schema("custom/我的码"), "A custom helpcode schema was not selected.");
            type(custom_helpcode, "nihaoC");
            require(!custom_helpcode.candidates().empty() && custom_helpcode.candidates().front().word == "你好",
                    "A custom helpcode table did not drive candidate reordering.");
        }

        metasequoia::InputSession quanpin_helpcode(SchemeType::Quanpin);
        quanpin_helpcode.set_quanpin_helpcode_enabled(true);
        require(quanpin_helpcode.set_helpcode_schema("lantian"), "The Lantian helpcode fixture was not selected.");
        type(quanpin_helpcode, "nihaoC");
        require(!quanpin_helpcode.candidates().empty() && quanpin_helpcode.candidates().front().word == "拟好",
                "Quanpin helpcode did not reorder candidates after a complete spelling.");
        quanpin_helpcode.handle_command(metasequoia::Command::Cancel);
        type(quanpin_helpcode, "nihC");
        metasequoia::InputSession quanpin_without_helpcode(SchemeType::Quanpin);
        quanpin_without_helpcode.set_quanpin_helpcode_enabled(false);
        require(!quanpin_without_helpcode.helpcode_enabled(),
                "Disabling Quanpin helpcode was not reflected by the session.");
        type(quanpin_without_helpcode, "nih");
        require(!quanpin_without_helpcode.handle_character('C').handled && quanpin_without_helpcode.preedit() == "nih",
                "A setter-disabled Quanpin helpcode key was swallowed.");
        require(same_candidate_words(quanpin_helpcode, quanpin_without_helpcode),
                "Quanpin helpcode changed candidates after an incomplete base spelling.");

        metasequoia::InputSession shuangpin_helpcode(SchemeType::Shuangpin);
        shuangpin_helpcode.set_shuangpin_helpcode_enabled(true);
        type(shuangpin_helpcode, "nihcc");
        require(shuangpin_helpcode.raw_segmentation() == "ni'hc'c" &&
                    shuangpin_helpcode.normalized_segmentation() == "ni'hao'c" &&
                    !shuangpin_helpcode.candidates().empty() && shuangpin_helpcode.candidates().front().word == "拟好",
                "Shuangpin helpcode or exposed segmentation did not match the complete base spelling.");

        metasequoia::InputSession shuangpin_delimited_helpcode(SchemeType::Shuangpin);
        shuangpin_delimited_helpcode.set_shuangpin_helpcode_enabled(true);
        type(shuangpin_delimited_helpcode, "nihcAB'");
        require(shuangpin_delimited_helpcode.raw_segmentation() == "ni'hc'AB" &&
                    shuangpin_delimited_helpcode.normalized_segmentation() == "ni'hao'AB",
                "A manual delimiter next to the Shuangpin help codes leaked into the exposed segmentation.");

        metasequoia::InputSession shuangpin_without_helpcode(SchemeType::Shuangpin);
        shuangpin_without_helpcode.set_shuangpin_helpcode_enabled(false);
        require(!shuangpin_without_helpcode.helpcode_enabled(),
                "Disabling Shuangpin helpcode was not reflected by the session.");
        type(shuangpin_without_helpcode, "ni");
        require(!shuangpin_without_helpcode.handle_character('H').handled &&
                    shuangpin_without_helpcode.preedit() == "ni",
                "A setter-disabled Shuangpin helpcode key was swallowed.");

        require(!session.handle_character('1').handled, "A digit was swallowed instead of passed through.");
        require(!session.handle_command(metasequoia::Command::Backspace).handled,
                "Backspace was swallowed while no composition was active.");
        require(!session.handle_command(metasequoia::Command::CommitRaw).handled,
                "CommitRaw was swallowed while no composition was active.");
        require(!session.select_candidate(static_cast<std::size_t>(0)).handled,
                "A candidate was selected while no composition was active.");

        require(!session.handle_character('\'').handled, "An idle apostrophe was swallowed.");
    }

    run_umlaut_alias_session_tests(data_directory);
    run_caret_prefix_session_tests(data_directory);
#endif

#ifndef METASEQUOIA_SKIP_FREQUENCY_TESTS
    struct FrequencyCase
    {
        metasequoia::FrequencyAdjustmentMode mode;
        const char *name;
        std::size_t expected_index;
        int linear_step;
    };
    const std::array frequency_cases{
        FrequencyCase{metasequoia::FrequencyAdjustmentMode::Disabled, "disabled", 5, 1},
        FrequencyCase{metasequoia::FrequencyAdjustmentMode::Pin, "pin", 0, 1},
        FrequencyCase{metasequoia::FrequencyAdjustmentMode::Halve, "halve", 2, 1},
        FrequencyCase{metasequoia::FrequencyAdjustmentMode::Linear, "linear", 3, 2},
        FrequencyCase{metasequoia::FrequencyAdjustmentMode::Promote, "promote", 4, 1},
    };
    for (const FrequencyCase &frequency_case : frequency_cases)
    {
        user_dictionary::close_default_user_database();
        const std::filesystem::path directory = data_directory / (std::string("frequency-") + frequency_case.name);
        prepare_frequency_fixture(directory);
        set_data_directory(directory);

        metasequoia::InputSession learning_session(SchemeType::Quanpin);
        require(learning_session.set_frequency_adjustment({frequency_case.mode, 1, frequency_case.linear_step}),
                "A supported frequency adjustment configuration was rejected.");
        type(learning_session, "ni");
        const auto learned = learning_session.select_candidate(std::string("己"));
        require(learned.handled && learned.commit == "己" && !learned.diagnostic.has_value(),
                "Frequency learning changed or diagnosed a successful candidate commit.");

        metasequoia::InputSession reopened(SchemeType::Quanpin);
        type(reopened, "ni");
        require(candidate_index(reopened, "己") == frequency_case.expected_index,
                "A frequency mode did not persist the Windows-compatible ranking transition.");
        const bool journal_exists = std::filesystem::exists(directory / "msime_user.db");
        require(journal_exists == (frequency_case.mode != metasequoia::FrequencyAdjustmentMode::Disabled),
                "Frequency learning wrote an unexpected user journal state.");
    }

    user_dictionary::close_default_user_database();
    const std::filesystem::path trigger_directory = data_directory / "frequency-trigger";
    prepare_frequency_fixture(trigger_directory);
    set_data_directory(trigger_directory);
    for (int selection = 0; selection < 2; ++selection)
    {
        metasequoia::InputSession triggered(SchemeType::Quanpin);
        require(triggered.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 2, 1}),
                "A valid trigger-count configuration was rejected.");
        type(triggered, "ni");
        require(triggered.select_candidate(std::string("己")).commit == "己",
                "A deferred frequency adjustment blocked candidate commit.");

        metasequoia::InputSession observed(SchemeType::Quanpin);
        type(observed, "ni");
        require(candidate_index(observed, "己") == (selection == 0 ? 5U : 0U),
                "Frequency trigger_count did not defer exactly the configured number of selections.");
    }

    user_dictionary::close_default_user_database();
    const std::filesystem::path first_candidate_directory = data_directory / "frequency-first-candidate";
    prepare_frequency_fixture(first_candidate_directory);
    set_data_directory(first_candidate_directory);
    metasequoia::InputSession first_candidate_session(SchemeType::Quanpin);
    require(first_candidate_session.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid first-candidate learning configuration was rejected.");
    type(first_candidate_session, "ni");
    require(first_candidate_session.select_candidate(static_cast<std::size_t>(0)).commit == "甲" &&
                !std::filesystem::exists(first_candidate_directory / "msime_user.db"),
            "Selecting the already-leading candidate created frequency state.");

    user_dictionary::close_default_user_database();
    const std::filesystem::path shuangpin_directory = data_directory / "frequency-shuangpin";
    prepare_shuangpin_frequency_fixture(shuangpin_directory);
    set_data_directory(shuangpin_directory);
    metasequoia::InputSession shuangpin_learning(SchemeType::Shuangpin);
    require(shuangpin_learning.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid Shuangpin frequency configuration was rejected.");
    type(shuangpin_learning, "nihc");
    require(shuangpin_learning.select_candidate(std::string("拟好")).commit == "拟好",
            "Shuangpin frequency learning blocked candidate commit.");
    metasequoia::InputSession reopened_shuangpin(SchemeType::Shuangpin);
    type(reopened_shuangpin, "nihc");
    require(!reopened_shuangpin.candidates().empty() && reopened_shuangpin.candidates().front().word == "拟好",
            "Shuangpin frequency learning did not persist through the canonical pinyin key.");

    user_dictionary::close_default_user_database();
    const std::filesystem::path wubi_directory = data_directory / "frequency-wubi";
    prepare_wubi_frequency_fixture(wubi_directory);
    set_data_directory(wubi_directory);
    metasequoia::InputSession wubi_learning(SchemeType::Wubi);
    require(wubi_learning.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid Wubi frequency configuration was rejected.");
    type(wubi_learning, "aaaa");
    require(wubi_learning.select_candidate(std::string("或")).commit == "或",
            "Wubi frequency learning blocked candidate commit.");
    metasequoia::InputSession reopened_wubi(SchemeType::Wubi);
    type(reopened_wubi, "aaaa");
    require(!reopened_wubi.candidates().empty() && reopened_wubi.candidates().front().word == "或",
            "Wubi frequency learning did not persist through the Wubi table.");

    user_dictionary::close_default_user_database();
    const std::filesystem::path failure_directory = data_directory / "frequency-write-failure";
    prepare_frequency_fixture(failure_directory);
    std::filesystem::create_directory(failure_directory / "msime_user.db");
    set_data_directory(failure_directory);
    metasequoia::InputSession failing_learning_session(SchemeType::Quanpin);
    require(failing_learning_session.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid write-failure learning configuration was rejected.");
    type(failing_learning_session, "ni");
    const auto failure_commit = failing_learning_session.select_candidate(std::string("己"));
    require(failure_commit.handled && failure_commit.commit == "己" && failure_commit.diagnostic.has_value() &&
                failure_commit.diagnostic->find("己") == std::string::npos &&
                failure_commit.diagnostic->find("ni") == std::string::npos,
            "A frequency write failure blocked commit or exposed input text in its diagnostic.");

    user_dictionary::close_default_user_database();
    const std::filesystem::path partial_write_directory = data_directory / "frequency-partial-write";
    prepare_frequency_fixture(partial_write_directory);
    set_data_directory(partial_write_directory);
    require(user_dictionary::ensure_user_database(user_dictionary::default_user_db_path()),
            "The partial-write fixture could not create the user dictionary schema.");
    user_dictionary::close_default_user_database();
    {
        Database user_database(partial_write_directory / "msime_user.db");
        user_database.execute("CREATE TRIGGER reject_frequency_journal BEFORE INSERT ON user_dictionary_operations "
                              "BEGIN SELECT RAISE(FAIL, 'injected journal failure'); END");
    }
    metasequoia::InputSession partial_write_session(SchemeType::Quanpin);
    require(partial_write_session.set_frequency_adjustment({metasequoia::FrequencyAdjustmentMode::Pin, 1, 1}),
            "A valid partial-write learning configuration was rejected.");
    type(partial_write_session, "ni");
    const auto partial_write_commit = partial_write_session.select_candidate(std::string("己"));
    require(partial_write_commit.handled && partial_write_commit.commit == "己" &&
                partial_write_commit.diagnostic.has_value() &&
                partial_write_commit.diagnostic->find("己") == std::string::npos &&
                partial_write_commit.diagnostic->find("ni") == std::string::npos,
            "A partial frequency write blocked commit or exposed input text in its diagnostic.");
    {
        Database main_database(partial_write_directory / "msime.db");
        require(main_database.query_integer("SELECT weight FROM tbl_1_n WHERE key='ni' AND value='己'") == 50,
                "A failed journal write left a partial frequency update in the main dictionary.");
    }

    metasequoia::FrequencyAdjustmentOptions invalid_frequency;
    invalid_frequency.mode = static_cast<metasequoia::FrequencyAdjustmentMode>(99);
    require(!failing_learning_session.set_frequency_adjustment(invalid_frequency),
            "An unknown frequency mode was accepted.");
    invalid_frequency = {};
    invalid_frequency.trigger_count = 0;
    require(!failing_learning_session.set_frequency_adjustment(invalid_frequency),
            "An out-of-range frequency trigger count was accepted.");
    invalid_frequency = {};
    invalid_frequency.linear_step = 11;
    require(!failing_learning_session.set_frequency_adjustment(invalid_frequency),
            "An out-of-range frequency linear step was accepted.");
    user_dictionary::close_default_user_database();
#endif

#ifndef METASEQUOIA_FREQUENCY_TESTS_ONLY
    metasequoia::InputSession unicode_session(SchemeType::Quanpin);
    require(unicode_session.handle_character('U', true).handled &&
                unicode_session.local_input_mode() == metasequoia::LocalInputMode::Unicode &&
                unicode_session.preedit() == "U" && unicode_session.candidates().empty(),
            "Shift+U did not enter an empty Unicode composition.");
    require(unicode_session.handle_character('g').handled && unicode_session.preedit() == "U" &&
                unicode_session.candidates().empty(),
            "Unicode mode accepted or forwarded a non-hexadecimal character.");
    for (const char character : std::string("4e00"))
    {
        require(unicode_session.handle_character(character).handled, "Unicode mode rejected a hexadecimal character.");
    }
    require(unicode_session.preedit() == "U4e00" && unicode_session.candidates().size() == 1 &&
                unicode_session.candidates().front().word == "一" &&
                unicode_session.candidates().front().pinyin == "U+4E00" &&
                unicode_session.candidates().front().source == CandidateSource::Generated,
            "Unicode mode did not produce the Windows-compatible BMP candidate.");
    const auto unicode_commit = unicode_session.select_candidate(0);
    require(unicode_commit.handled && unicode_commit.commit == "一" && !unicode_session.has_composition() &&
                unicode_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a Unicode candidate did not leave the local mode.");

    // Segment boundaries are the engine's unit model: quanpin reports one offset
    // per displayed syllable, and a local mode reports none so the host keeps
    // single-character editing (PRD R4).
    {
        require(unicode_session.handle_character('U', true).handled,
                "Unicode mode could not be re-entered for the boundary check.");
        require(unicode_session.segment_raw_boundaries().empty(),
                "A local mode must report no pinyin segment boundaries.");
        require(unicode_session.handle_command(metasequoia::Command::Cancel).handled,
                "Cancel did not leave Unicode mode after the boundary check.");

        metasequoia::InputSession boundary_session(SchemeType::Quanpin);
        for (const char character : std::string("nihaoma"))
        {
            require(boundary_session.handle_character(character).handled, "Quanpin rejected a letter.");
        }
        require(boundary_session.segment_raw_boundaries() == std::vector<std::size_t>({0, 2, 5, 7}),
                "Quanpin unit boundaries did not follow the displayed syllables.");
    }

    require(unicode_session.handle_character('U', true).handled && unicode_session.handle_character('+').handled,
            "Unicode mode rejected its optional plus prefix.");
    for (const char character : std::string("1f600"))
    {
        require(unicode_session.handle_character(character).handled,
                "Unicode mode rejected a supplementary-plane hexadecimal character.");
    }
    require(unicode_session.preedit() == "U+1f600" && unicode_session.candidates().size() == 1 &&
                unicode_session.candidates().front().word == "😀",
            "Unicode mode did not produce a supplementary-plane scalar.");
    require(unicode_session.handle_command(metasequoia::Command::Cancel).handled && !unicode_session.has_composition(),
            "Cancel did not leave Unicode mode.");

    const auto require_invalid_unicode = [&](const std::string &hex) {
        require(unicode_session.handle_character('U', true).handled,
                "Unicode mode could not be re-entered for invalid-scalar coverage.");
        for (const char character : hex)
        {
            require(unicode_session.handle_character(character).handled,
                    "Unicode mode rejected an invalid scalar's hexadecimal spelling.");
        }
        require(unicode_session.candidates().empty(), "Unicode mode produced an invalid scalar candidate.");
        require(unicode_session.handle_command(metasequoia::Command::Cancel).handled,
                "Unicode invalid-scalar fixture could not be cancelled.");
    };
    require_invalid_unicode("d800");
    require_invalid_unicode("110000");
    require_invalid_unicode("0000001");

    require(unicode_session.handle_character('U', true).handled,
            "Unicode prefix was not handled before Backspace coverage.");
    require(unicode_session.handle_command(metasequoia::Command::Backspace).handled &&
                !unicode_session.has_composition() &&
                unicode_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Backspace on a bare Unicode prefix did not leave the mode.");

    metasequoia::InputSession plain_uppercase(SchemeType::Quanpin);
    require(!plain_uppercase.handle_character('U').handled && !plain_uppercase.has_composition() &&
                plain_uppercase.local_input_mode() == metasequoia::LocalInputMode::None,
            "An uppercase character without Shift-only was swallowed.");
    metasequoia::LocalModeOptions disabled_local_modes;
    disabled_local_modes.unicode = false;
    metasequoia::InputSession disabled_unicode(SchemeType::Quanpin);
    disabled_unicode.set_local_mode_options(disabled_local_modes);
    require(!disabled_unicode.handle_character('U', true).handled && !disabled_unicode.has_composition() &&
                disabled_unicode.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled Unicode shortcut swallowed Shift+U.");

    metasequoia::InputSession wubi_unicode(SchemeType::Wubi);
    require(!wubi_unicode.handle_character('U', true).handled && !wubi_unicode.has_composition() &&
                wubi_unicode.local_input_mode() == metasequoia::LocalInputMode::None,
            "Shift+U was swallowed outside a pinyin scheme.");
    metasequoia::InputSession switch_clears_unicode(SchemeType::Shuangpin);
    require(switch_clears_unicode.handle_character('U', true).handled &&
                switch_clears_unicode.local_input_mode() == metasequoia::LocalInputMode::Unicode,
            "Shuangpin could not enter Unicode mode.");
    switch_clears_unicode.switch_scheme(SchemeType::Quanpin);
    require(!switch_clears_unicode.has_composition() &&
                switch_clears_unicode.local_input_mode() == metasequoia::LocalInputMode::None,
            "Switching schemes did not clear Unicode mode.");

    metasequoia::InputSession date_time_session(SchemeType::Quanpin);
    date_time_session.set_local_date_time_provider(
        [] { return metasequoia::local_modes::LocalDateTime{2026, 8, 9, 0, 14, 30, 0}; });
    require(date_time_session.handle_character('T', true).handled &&
                date_time_session.local_input_mode() == metasequoia::LocalInputMode::DateTime &&
                date_time_session.preedit() == "T" && date_time_session.candidates().empty(),
            "Shift+T did not enter an empty date/time composition.");
    type(date_time_session, "rq");
    require(date_time_session.preedit() == "Trq" && date_time_session.candidates().size() == 17 &&
                date_time_session.candidates().front().word == "2026年8月9日" &&
                date_time_session.candidates().back().word == "丙午年六月二十七日",
            "Date/time mode did not expose deterministic date candidates.");
    const auto date_commit = date_time_session.select_candidate(0);
    require(date_commit.handled && date_commit.commit == "2026年8月9日" &&
                date_time_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a date candidate did not leave date/time mode.");

    require(date_time_session.handle_character('T', true).handled,
            "Date/time mode could not be re-entered for incomplete-input coverage.");
    type(date_time_session, "r");
    require(date_time_session.candidates().empty(), "An incomplete date keyword produced candidates.");
    const std::string incomplete_date_preedit = date_time_session.preedit();
    require(date_time_session.handle_character('1').handled && date_time_session.preedit() == incomplete_date_preedit,
            "Invalid date/time input leaked into normal composition or changed preedit.");
    require(date_time_session.handle_command(metasequoia::Command::Cancel).handled &&
                date_time_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Cancel did not leave date/time mode.");

    require(date_time_session.handle_character('T', true).handled &&
                date_time_session.handle_command(metasequoia::Command::Backspace).handled &&
                date_time_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Backspace on a bare date/time prefix did not leave the mode.");

    metasequoia::LocalModeOptions disabled_date_time_options;
    disabled_date_time_options.date_time = false;
    metasequoia::InputSession disabled_date_time(SchemeType::Quanpin);
    disabled_date_time.set_local_mode_options(disabled_date_time_options);
    require(!disabled_date_time.handle_character('T', true).handled && !disabled_date_time.has_composition() &&
                disabled_date_time.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled date/time shortcut swallowed Shift+T.");

    metasequoia::InputSession wubi_date_time(SchemeType::Wubi);
    require(!wubi_date_time.handle_character('T', true).handled && !wubi_date_time.has_composition() &&
                wubi_date_time.local_input_mode() == metasequoia::LocalInputMode::None,
            "Shift+T was swallowed outside a pinyin scheme.");

    const std::filesystem::path quick_phrase_directory = data_directory / "quick-phrase";
    std::filesystem::create_directories(quick_phrase_directory);
    {
        Database database(quick_phrase_directory / "msime.db");
        database.execute("CREATE TABLE quick_parases(key TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO quick_parases VALUES('ab','快捷短语一',20)");
        database.execute("INSERT INTO quick_parases VALUES('aa','快捷短语二',10)");
    }
    set_data_directory(quick_phrase_directory);
    metasequoia::InputSession quick_phrase_session(SchemeType::Quanpin);
    require(quick_phrase_session.handle_character('K', true).handled &&
                quick_phrase_session.local_input_mode() == metasequoia::LocalInputMode::QuickPhrase &&
                quick_phrase_session.preedit() == "K" && quick_phrase_session.candidates().empty(),
            "Shift+K did not enter an empty quick-phrase composition.");
    const auto quick_phrase_query = quick_phrase_session.handle_character('a');
    require(quick_phrase_query.handled && !quick_phrase_query.diagnostic.has_value() &&
                quick_phrase_session.preedit() == "Ka" && quick_phrase_session.candidates().size() == 2 &&
                quick_phrase_session.candidates().front().word == "快捷短语一" &&
                quick_phrase_session.candidates().front().source == CandidateSource::QuickPhrase,
            "Quick-phrase mode did not expose prefix candidates.");
    const auto quick_phrase_commit = quick_phrase_session.select_candidate(1);
    require(quick_phrase_commit.handled && quick_phrase_commit.commit == "快捷短语二" &&
                quick_phrase_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a quick phrase did not leave the local mode.");

    require(quick_phrase_session.handle_character('K', true).handled,
            "Quick-phrase mode could not be re-entered for invalid-input coverage.");
    const std::string quick_phrase_prefix = quick_phrase_session.preedit();
    require(quick_phrase_session.handle_character('1').handled && quick_phrase_session.preedit() == quick_phrase_prefix,
            "Invalid quick-phrase input leaked into normal composition or changed preedit.");
    require(quick_phrase_session.handle_command(metasequoia::Command::Backspace).handled &&
                quick_phrase_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Backspace on a bare quick-phrase prefix did not leave the mode.");

    metasequoia::LocalModeOptions disabled_quick_phrase_options;
    disabled_quick_phrase_options.quick_phrase = false;
    metasequoia::InputSession disabled_quick_phrase(SchemeType::Quanpin);
    disabled_quick_phrase.set_local_mode_options(disabled_quick_phrase_options);
    require(!disabled_quick_phrase.handle_character('K', true).handled && !disabled_quick_phrase.has_composition() &&
                disabled_quick_phrase.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled quick-phrase shortcut swallowed Shift+K.");

    const std::filesystem::path missing_quick_phrase_directory = data_directory / "quick-phrase-missing";
    std::filesystem::create_directories(missing_quick_phrase_directory);
    set_data_directory(missing_quick_phrase_directory);
    metasequoia::InputSession missing_quick_phrase(SchemeType::Quanpin);
    require(missing_quick_phrase.handle_character('K', true).handled,
            "Quick-phrase mode could not start with a missing database.");
    const auto missing_quick_phrase_result = missing_quick_phrase.handle_character('a');
    require(missing_quick_phrase_result.handled && missing_quick_phrase_result.diagnostic.has_value() &&
                missing_quick_phrase.candidates().empty(),
            "A missing quick-phrase database did not report a non-blocking diagnostic.");

    const std::filesystem::path corrupt_quick_phrase_directory = data_directory / "quick-phrase-corrupt";
    std::filesystem::create_directories(corrupt_quick_phrase_directory);
    write_file(corrupt_quick_phrase_directory / "msime.db", "not a sqlite database");
    set_data_directory(corrupt_quick_phrase_directory);
    metasequoia::InputSession corrupt_quick_phrase(SchemeType::Quanpin);
    require(corrupt_quick_phrase.handle_character('K', true).handled,
            "Quick-phrase mode could not start with a corrupt database.");
    const auto corrupt_quick_phrase_result = corrupt_quick_phrase.handle_character('a');
    require(corrupt_quick_phrase_result.handled && corrupt_quick_phrase_result.diagnostic.has_value() &&
                corrupt_quick_phrase.candidates().empty(),
            "A corrupt quick-phrase database did not report a non-blocking diagnostic.");

    const std::filesystem::path expressive_directory = data_directory / "expressive-modes";
    std::filesystem::create_directories(expressive_directory);
    {
        Database database(expressive_directory / "others.db");
        database.execute("CREATE TABLE emoji_pinyin(key TEXT,emoji TEXT,sort_order INTEGER)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xiaolian','😀',10)");
        database.execute("INSERT INTO emoji_pinyin VALUES('xiao''lian','😄',20)");
        database.execute("CREATE TABLE kaomoji(pinyin TEXT,jianpin TEXT,kaomoji TEXT,sort_order INTEGER)");
        database.execute("INSERT INTO kaomoji VALUES('haixiu','hx','(*/ω＼*)',10)");
    }
    set_data_directory(expressive_directory);

    metasequoia::InputSession emoji_session(SchemeType::Quanpin);
    require(emoji_session.handle_character('E', true).handled &&
                emoji_session.local_input_mode() == metasequoia::LocalInputMode::Emoji &&
                emoji_session.preedit() == "E" && emoji_session.candidates().empty(),
            "Shift+E did not enter an empty Emoji composition.");
    type(emoji_session, "XIAOLIAN");
    require(emoji_session.preedit() == "EXIAOLIAN" && emoji_session.candidates().size() == 1 &&
                emoji_session.candidates().front().word == "😀" &&
                emoji_session.candidates().front().source == CandidateSource::Emoji,
            "Emoji mode did not accept uppercase input or expose its candidate.");
    const auto emoji_commit = emoji_session.handle_command(metasequoia::Command::CommitCandidate);
    require(emoji_commit.handled && emoji_commit.commit == "😀" &&
                emoji_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing an Emoji candidate did not leave Emoji mode.");

    require(emoji_session.handle_character('E', true).handled,
            "Emoji mode could not be re-entered for apostrophe coverage.");
    type(emoji_session, "xiao'lian");
    require(emoji_session.preedit() == "Exiao'lian" && emoji_session.candidates().size() == 1 &&
                emoji_session.candidates().front().word == "😄",
            "Emoji mode did not retain and query an apostrophe.");
    const std::string emoji_preedit = emoji_session.preedit();
    require(emoji_session.handle_character('1').handled && emoji_session.preedit() == emoji_preedit,
            "Invalid Emoji input changed the local composition.");
    require(emoji_session.handle_command(metasequoia::Command::Backspace).handled &&
                emoji_session.preedit() == "Exiao'lia",
            "Emoji-mode Backspace did not edit and refresh the composition.");
    require(emoji_session.handle_command(metasequoia::Command::Cancel).handled &&
                emoji_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Cancel did not leave Emoji mode.");

    metasequoia::InputSession kaomoji_session(SchemeType::Shuangpin);
    require(kaomoji_session.handle_character('M', true).handled &&
                kaomoji_session.local_input_mode() == metasequoia::LocalInputMode::Kaomoji,
            "Shift+M did not enter kaomoji mode in Shuangpin.");
    type(kaomoji_session, "hx");
    require(kaomoji_session.candidates().size() == 1 && kaomoji_session.candidates().front().word == "(*/ω＼*)" &&
                kaomoji_session.candidates().front().source == CandidateSource::Kaomoji,
            "Kaomoji mode did not expose its Shuangpin-expanded candidate.");
    const auto kaomoji_commit = kaomoji_session.select_candidate(0);
    require(kaomoji_commit.handled && kaomoji_commit.commit == "(*/ω＼*)" &&
                kaomoji_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a kaomoji did not leave kaomoji mode.");

    metasequoia::LocalModeOptions disabled_expressive_options;
    disabled_expressive_options.emoji = false;
    disabled_expressive_options.kaomoji = false;
    metasequoia::InputSession disabled_expressive(SchemeType::Quanpin);
    disabled_expressive.set_local_mode_options(disabled_expressive_options);
    require(!disabled_expressive.handle_character('E', true).handled && !disabled_expressive.has_composition() &&
                disabled_expressive.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled Emoji shortcut swallowed Shift+E.");
    require(!disabled_expressive.handle_character('M', true).handled && !disabled_expressive.has_composition() &&
                disabled_expressive.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled kaomoji shortcut swallowed Shift+M.");

    metasequoia::InputSession disabling_active_expressive(SchemeType::Quanpin);
    require(disabling_active_expressive.handle_character('E', true).handled &&
                disabling_active_expressive.local_input_mode() == metasequoia::LocalInputMode::Emoji,
            "Emoji mode could not start before option-reset coverage.");
    auto disable_active_options = disabling_active_expressive.local_mode_options();
    disable_active_options.emoji = false;
    disabling_active_expressive.set_local_mode_options(disable_active_options);
    require(!disabling_active_expressive.has_composition() &&
                disabling_active_expressive.local_input_mode() == metasequoia::LocalInputMode::None,
            "Disabling an active Emoji mode did not reset it.");

    metasequoia::InputSession wubi_expressive(SchemeType::Wubi);
    require(!wubi_expressive.handle_character('E', true).handled && !wubi_expressive.has_composition() &&
                wubi_expressive.local_input_mode() == metasequoia::LocalInputMode::None,
            "Shift+E was swallowed outside a pinyin scheme.");
    require(!wubi_expressive.handle_character('M', true).handled && !wubi_expressive.has_composition() &&
                wubi_expressive.local_input_mode() == metasequoia::LocalInputMode::None,
            "Shift+M was swallowed outside a pinyin scheme.");

    const std::filesystem::path missing_expressive_directory = data_directory / "expressive-missing";
    std::filesystem::create_directories(missing_expressive_directory);
    set_data_directory(missing_expressive_directory);
    metasequoia::InputSession missing_emoji_session(SchemeType::Quanpin);
    require(missing_emoji_session.handle_character('E', true).handled,
            "Emoji mode could not start with a missing database.");
    const auto missing_emoji_result = missing_emoji_session.handle_character('x');
    require(missing_emoji_result.handled && missing_emoji_result.diagnostic.has_value() &&
                missing_emoji_session.candidates().empty(),
            "A missing Emoji database did not report a non-blocking diagnostic.");
#endif

    return 0;
}

int main()
{
    try
    {
        return run_test();
    }
    catch (const std::exception &exception)
    {
        std::fprintf(stderr, "%s\n", exception.what());
        return 1;
    }
}
