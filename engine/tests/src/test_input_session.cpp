#include "../../core/input_session.h"
#include "../../core/data_path.h"
#include "../../user_dictionary/user_dictionary_journal.h"
#include "test_directory_cleanup.h"
#include "../../contracts/dictionary/format.h"
#include "../../quanpin/quanpin_query.h"
#include "../../quanpin/quanpin_utils.h"

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

        const std::vector<std::string> supported_helpcode_schemas{"lantian", "ziranma", "shouyou2_0", "shouyouplus",
                                                                  "xiaohe"};
        for (const std::string &schema : supported_helpcode_schemas)
        {
            require(metasequoia::InputSession::is_supported_helpcode_schema(schema) &&
                        metasequoia::InputSession::select_helpcode_schema(schema),
                    "A Windows-supported helpcode schema was rejected.");
        }
        require(!metasequoia::InputSession::is_supported_helpcode_schema("unknown") &&
                    !metasequoia::InputSession::select_helpcode_schema("unknown"),
                "An unknown helpcode schema was accepted.");

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

    metasequoia::InputSession number_session(SchemeType::Quanpin);
    require(number_session.handle_character('V', true).handled &&
                number_session.local_input_mode() == metasequoia::LocalInputMode::Number &&
                number_session.preedit() == "V" && number_session.candidates().empty(),
            "Shift+V did not enter an empty number composition.");
    require(number_session.handle_character('a').handled && number_session.preedit() == "V" &&
                number_session.candidates().empty(),
            "Number mode accepted or forwarded a letter.");
    for (const char character : std::string("1234.5"))
    {
        require(number_session.handle_character(character).handled, "Number mode rejected a digit or the point.");
    }
    require(number_session.handle_character('.').handled && number_session.preedit() == "V1234.5",
            "Number mode accepted a second decimal point.");
    require(number_session.candidates().size() == 6 && number_session.candidates().back().word == "1,234.5" &&
                number_session.candidates().front().word == "壹仟贰佰叁拾肆元伍角整" &&
                number_session.candidates().front().source == CandidateSource::Generated &&
                number_session.candidates()[2].word == "一千二百三十四点五",
            "Number mode did not produce the financial amount first.");
    const auto number_commit = number_session.select_candidate(0);
    require(number_commit.handled && number_commit.commit == "壹仟贰佰叁拾肆元伍角整" &&
                !number_session.has_composition() &&
                number_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a number candidate did not leave the local mode.");

    require(number_session.handle_character('V', true).handled && number_session.handle_character('1').handled &&
                number_session.handle_character('2').handled &&
                number_session.handle_command(metasequoia::Command::MoveLeft).handled &&
                number_session.handle_character('3').handled && number_session.preedit() == "V132" &&
                number_session.handle_character('.').handled && number_session.preedit() == "V13.2" &&
                !number_session.handle_character('.').handled && number_session.preedit() == "V13.2" &&
                !number_session.handle_character('x').handled && number_session.preedit() == "V13.2" &&
                number_session.candidates().front().word == "壹拾叁元贰角整",
            "Caret editing inside a number composition did not follow the digit rules.");
    require(number_session.handle_command(metasequoia::Command::Cancel).handled && !number_session.has_composition(),
            "Cancel did not leave number mode.");
    require(number_session.handle_character('V', true).handled &&
                number_session.handle_command(metasequoia::Command::Backspace).handled &&
                !number_session.has_composition() &&
                number_session.local_input_mode() == metasequoia::LocalInputMode::None,
            "Backspace on a bare number prefix did not leave the mode.");
    require(!plain_uppercase.handle_character('V').handled && !plain_uppercase.has_composition(),
            "An uppercase V without Shift-only was swallowed.");
    metasequoia::InputSession lowercase_v(SchemeType::Quanpin);
    require(lowercase_v.handle_character('l').handled && lowercase_v.handle_character('v').handled &&
                lowercase_v.has_composition() && lowercase_v.local_input_mode() == metasequoia::LocalInputMode::None,
            "A lowercase v after a letter stopped acting as pinyin ü.");
    require(lowercase_v.handle_command(metasequoia::Command::Cancel).handled &&
                lowercase_v.handle_character('v').handled &&
                lowercase_v.local_input_mode() == metasequoia::LocalInputMode::Number && lowercase_v.preedit() == "v",
            "A bare lowercase v did not start number mode in quanpin.");
    for (const char character : std::string("123456.78"))
    {
        require(lowercase_v.handle_character(character).handled, "Bare-v number mode rejected a digit.");
    }
    require(lowercase_v.preedit() == "v123456.78" &&
                lowercase_v.candidates().front().word == "壹拾贰万叁仟肆佰伍拾陆元柒角捌分",
            "Bare-v number mode did not render the financial amount.");
    const auto lowercase_commit = lowercase_v.select_candidate(0);
    require(lowercase_commit.handled && lowercase_commit.commit == "壹拾贰万叁仟肆佰伍拾陆元柒角捌分" &&
                lowercase_v.local_input_mode() == metasequoia::LocalInputMode::None,
            "Committing a bare-v number candidate did not leave the local mode.");
    metasequoia::InputSession shuangpin_v(SchemeType::Shuangpin);
    require(shuangpin_v.handle_character('v').handled && shuangpin_v.has_composition() &&
                shuangpin_v.local_input_mode() == metasequoia::LocalInputMode::None,
            "A lowercase v in shuangpin was hijacked by number mode.");
    require(shuangpin_v.handle_command(metasequoia::Command::Cancel).handled &&
                shuangpin_v.handle_character('V', true).handled &&
                shuangpin_v.local_input_mode() == metasequoia::LocalInputMode::Number,
            "Shift+V did not start number mode in shuangpin.");
    metasequoia::LocalModeOptions disabled_number_mode;
    disabled_number_mode.number = false;
    metasequoia::InputSession disabled_number(SchemeType::Quanpin);
    disabled_number.set_local_mode_options(disabled_number_mode);
    require(!disabled_number.handle_character('V', true).handled && !disabled_number.has_composition() &&
                disabled_number.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled number shortcut swallowed Shift+V.");
    require(disabled_number.handle_character('v').handled && disabled_number.has_composition() &&
                disabled_number.local_input_mode() == metasequoia::LocalInputMode::None,
            "A disabled number shortcut still hijacked a bare lowercase v.");
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
