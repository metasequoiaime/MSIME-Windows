//
// 测试拼音输入法的核心逻辑，包括双拼和全拼方案，以及动态切换输入方案的功能。
//
#include <Windows.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <fmt/core.h>
#include "fmt/base.h"
#include "core/ime_session.h"
#include "quanpin/autocorrect_table.h"
#include "quanpin/quanpin_dictionary.h"
#include "quanpin/quanpin_utils.h"
#include "quanpin/word_lattice.h"
#include "quanpin/quanpin_query.h"
#include "sqlite3.h"
#include "shuangpin/shuangpin_dictionary.h"
#include "shuangpin/shuangpin_query.h"
#include "shuangpin/shuangpin_utils.h"
#include "core/data_path.h"
#include "core/input_session.h"
#include "user_dictionary/user_dictionary_journal.h"
#include <algorithm>
#include <fstream>
#include <unordered_set>

using namespace std;

namespace
{
namespace fs = std::filesystem;

class ScopedLocalAppDataOverride
{
  public:
    explicit ScopedLocalAppDataOverride(const std::string &suffix)
    {
        const fs::path source_dir = metasequoia::data_directory();
        if (source_dir.empty())
        {
            throw std::runtime_error("A data directory should be available for regression tests.");
        }
        const auto current = metasequoia::detail::wide_environment_variable(kDataDirectoryVariable);
        original_ = current.value_or(L"");

        root_ = fs::temp_directory_path() / "msime-regression" / suffix;
        app_dir_ = root_ / "metasequoiaime";
        fs::remove_all(root_);
        fs::create_directories(app_dir_);

        // msime.db 是回归断言的主体，缺失即环境不完整；而当前产品布局已不再发布
        // 整句解码器的两个数据文件（dict_pinyin.dat/user_dict.dat），引擎对它们
        // 的缺失也是优雅降级（PinyinDecoder::sentence 直接返回空串），所以这里
        // 仅在源目录存在时才拷贝，不把它们当硬依赖。
        for (const auto &file_name : {"msime.db", "dict_pinyin.dat", "user_dict.dat"})
        {
            const fs::path source = source_dir / file_name;
            const fs::path target = app_dir_ / file_name;
            if (!fs::exists(source))
            {
                if (file_name == std::string_view("msime.db"))
                {
                    throw std::runtime_error(fmt::format("Expected test dependency '{}' to exist.", source.string()));
                }
                continue;
            }
            fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        }

        if (_wputenv_s(kDataDirectoryVariable, app_dir_.c_str()) != 0)
        {
            throw std::runtime_error("Failed to override the data directory for regression test.");
        }
    }

    ~ScopedLocalAppDataOverride()
    {
        (void)_wputenv_s(kDataDirectoryVariable, original_.c_str());
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    std::string local_appdata() const
    {
        return root_.string();
    }

  private:
    static constexpr const wchar_t *kDataDirectoryVariable = L"METASEQUOIA_IME_DATA_DIR";

    std::wstring original_;
    fs::path root_;
    fs::path app_dir_;
};

void expect(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void expect_session_state(const ImeSession &session, const std::string &expected_preedit)
{
    expect(session.get_preedit() == expected_preedit,
           fmt::format("Expected preedit '{}', got '{}'", expected_preedit, session.get_preedit()));
    expect(session.get_request().raw_input == expected_preedit,
           fmt::format("Expected raw_input '{}', got '{}'", expected_preedit, session.get_request().raw_input));
}

} // namespace

std::string hex_dump(const std::string &text)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (i > 0)
        {
            oss << ' ';
        }
        oss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(text[i]));
    }
    return oss.str();
}

void print_candidates(const std::vector<WordItem> &result)
{
    for (size_t index = 0; index < result.size(); ++index)
    {
        const auto &item = result[index];
        const auto &code = item.pinyin;
        const auto &word = item.word;
        const auto weight = item.weight;
        try
        {
            fmt::println("Candidate #{}: {} [{}] ({})", index, word, code, weight);
        }
        catch (const std::exception &ex)
        {
            throw std::runtime_error(
                fmt::format("Failed to print candidate #{}; word bytes=[{}], code bytes=[{}], error={}", index,
                            hex_dump(word), hex_dump(code), ex.what()));
        }
    }
}

const WordItem *find_candidate(const std::vector<WordItem> &result, const std::string &word)
{
    const auto found =
        std::find_if(result.begin(), result.end(), [&](const WordItem &item) { return item.word == word; });
    return found == result.end() ? nullptr : &(*found);
}

void run_quanpin_query_case(QuanpinDictionary &dictionary, const std::string &query)
{
    const auto start = std::chrono::high_resolution_clock::now();
    const auto result = dictionary.query(query);
    const auto end = std::chrono::high_resolution_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    fmt::println("Query: {}", query);
    fmt::println("Time: {} us", duration.count());
    print_candidates(result);
}

void feed_sequence(ImeSession &session, const vector<UINT> &sequence, const vector<WCHAR> &wch_sequence = {})
{
    for (int i = 0; i < sequence.size(); ++i)
    {
        std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
        session.handle_key(sequence[i], 0, i < wch_sequence.size() ? wch_sequence[i] : 0);
        std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        fmt::println("Preedit: {}", session.get_preedit());
        fmt::println("Time: {} us", duration.count());
    }
}

void test_shuangpin_session()
{
    ImeSession session(SchemeType::Shuangpin);
    const vector<UINT> sequence{'C', 'L', 'S'};      // 按键的 vk 码
    const vector<WCHAR> wch_sequence{'c', 'l', 's'}; // 实际的字符，区分大小写输入

    fmt::println("==== Shuangpin ====");
    feed_sequence(session, sequence, wch_sequence);
    print_candidates(session.get_candidates());
}

void test_shuangpin_session02()
{
    // ImeSession session(SchemeType::Quanpin);
    ImeSession session(SchemeType::Shuangpin);
    // const vector<UINT> sequence{'C', 'E', 'L', 'I', 'S', 'H', 'I'};
    const vector<UINT> sequence{'C', 'E', 'L', 'I', 'U', 'I'};
    const vector<WCHAR> wch_sequence{'c', 'e', 'l', 'i', 'u', 'i'};

    fmt::println("==== Shuangpin ====");
    feed_sequence(session, sequence, wch_sequence);
    print_candidates(session.get_candidates());
}

void test_quanpin_session()
{
    ImeSession session(SchemeType::Quanpin);
    const vector<UINT> sequence{'C', 'E', 'S', 'H', 'I'};
    const vector<WCHAR> wch_sequence{'c', 'e', 's', 'h', 'i'};

    fmt::println("==== Quanpin ====");
    feed_sequence(session, sequence, wch_sequence);
    print_candidates(session.get_candidates());
}

void test_dynamic_switch()
{
    ImeSession session(SchemeType::Shuangpin);

    fmt::println("==== Switch Scheme ====");
    feed_sequence(session, {'N', 'I'}, {'n', 'i'});
    fmt::println("Before switch preedit: {}", session.get_preedit());

    session.switch_scheme(SchemeType::Quanpin);
    fmt::println("After switch preedit: {}", session.get_preedit());

    feed_sequence(session, {'N', 'I', 'H', 'A', 'O'}, {'n', 'i', 'h', 'a', 'o'});
    print_candidates(session.get_candidates());
}

void test_quanpin_session_backspace()
{
    ImeSession session(SchemeType::Quanpin);

    fmt::println("==== Quanpin Backspace ====");
    feed_sequence(session, {'C', 'E', 'S', 'H', 'I'}, {'c', 'e', 's', 'h', 'i'});
    expect_session_state(session, "ceshi");
    expect(!session.get_candidates().empty(), "Quanpin session should have candidates before backspace.");

    session.handle_key(VK_BACK);
    fmt::println("Preedit after backspace: {}", session.get_preedit());
    expect_session_state(session, "cesh");
    expect(session.get_request().valid, "Quanpin session request should stay valid after backspace.");
}

void test_shuangpin_session_backspace()
{
    ImeSession session(SchemeType::Shuangpin);

    fmt::println("==== Shuangpin Backspace ====");
    feed_sequence(session, {'C', 'E', 'L', 'I', 'U', 'I'}, {'c', 'e', 'l', 'i', 'u', 'i'});
    expect_session_state(session, "celiui");
    expect(!session.get_candidates().empty(), "Shuangpin session should have candidates before backspace.");

    session.handle_key(VK_BACK);
    fmt::println("Preedit after backspace: {}", session.get_preedit());
    expect_session_state(session, "celiu");
    expect(session.get_request().valid, "Shuangpin session request should stay valid after backspace.");
}

void test_shuangpin_manual_apostrophe()
{
    ImeSession session(SchemeType::Shuangpin);

    fmt::println("==== Shuangpin Manual Apostrophe ====");
    feed_sequence(session, {'J', 'W', VK_OEM_7, 'D'}, {'j', 'w', '\'', 'd'});
    expect_session_state(session, "jw'd");
    expect(session.get_request().raw_segmentation.find('\'') != std::string::npos,
           fmt::format("Expected raw segmentation to preserve apostrophes, got '{}'",
                       session.get_request().raw_segmentation));
    expect(session.get_request().normalized_segmentation.find('\'') != std::string::npos,
           fmt::format("Expected normalized segmentation to preserve apostrophes, got '{}'",
                       session.get_request().normalized_segmentation));
    expect(session.get_request().normalized_input.find('\'') == std::string::npos,
           fmt::format("Expected normalized input to strip apostrophes, got '{}'",
                       session.get_request().normalized_input));

    session.handle_key(VK_BACK);
    expect_session_state(session, "jw'");
    session.handle_key(VK_BACK);
    expect_session_state(session, "jw");
}

void test_shuangpin_query_manual_apostrophe()
{
    fmt::println("==== Shuangpin Query Manual Apostrophe ====");
    expect(shuangpin::segment_input("ce'ce") == "ce'ce",
           "Expected manual apostrophe to be preserved in raw segmentation for complete chunks.");
    expect(shuangpin::normalize_input_with_delimiters("ce'ce") == "ce'ce",
           "Expected manual apostrophe to be preserved in normalized segmentation for complete chunks.");
    expect(shuangpin::normalize_input("ce'ce") == "cece",
           "Expected normalized input to strip apostrophes for complete chunks.");
    expect(shuangpin::is_complete_input("ce'ce"), "Expected ce'ce to be recognized as complete shuangpin input.");
    expect(!shuangpin::is_complete_input("jw'"), "Expected trailing manual apostrophe to stay incomplete.");
}

void test_quanpin_dictionary_backspace()
{
    QuanpinDictionary dictionary;

    fmt::println("==== Quanpin Dictionary Backspace ====");
    dictionary.handleVkCode('C', 0, 'c');
    dictionary.handleVkCode('E', 0, 'e');
    dictionary.handleVkCode('S', 0, 's');
    expect(dictionary.get_pinyin_sequence() == "ces",
           fmt::format("Expected quanpin dictionary sequence 'ces', got '{}'", dictionary.get_pinyin_sequence()));

    dictionary.handleVkCode(VK_BACK, 0);
    expect(dictionary.get_pinyin_sequence() == "ce",
           fmt::format("Expected quanpin dictionary sequence 'ce' after backspace, got '{}'",
                       dictionary.get_pinyin_sequence()));
    expect(!dictionary.get_current_candidate_list().empty(),
           "Quanpin dictionary should still have candidates after backspace.");
}

void test_shuangpin_dictionary_backspace()
{
    ShuangpinDictionary dictionary;

    fmt::println("==== Shuangpin Dictionary Backspace ====");
    dictionary.handleVkCode('C', 0, 'c');
    dictionary.handleVkCode('E', 0, 'e');
    dictionary.handleVkCode('L', 0, 'l');
    expect(dictionary.get_pinyin_sequence() == "cel",
           fmt::format("Expected shuangpin dictionary sequence 'cel', got '{}'", dictionary.get_pinyin_sequence()));

    dictionary.handleVkCode(VK_BACK, 0);
    expect(dictionary.get_pinyin_sequence() == "ce",
           fmt::format("Expected shuangpin dictionary sequence 'ce' after backspace, got '{}'",
                       dictionary.get_pinyin_sequence()));
    expect(!dictionary.get_current_candidate_list().empty(),
           "Shuangpin dictionary should still have candidates after backspace.");
}

void test_shuangpin_dictionary_create_pin_delete()
{
    ScopedLocalAppDataOverride local_appdata("shuangpin-write-regression");
    expect(ShuangpinUtil::get_local_appdata_path() == local_appdata.local_appdata(),
           fmt::format("Expected shuangpin appdata path '{}', got '{}'.", local_appdata.local_appdata(),
                       ShuangpinUtil::get_local_appdata_path()));
    const std::string expected_user_db = local_appdata.local_appdata() + "\\metasequoiaime\\msime_user.db";
    expect(user_dictionary::default_user_db_path() == expected_user_db,
           fmt::format("Expected user db path '{}', got '{}'.", expected_user_db,
                       user_dictionary::default_user_db_path()));
    const char *process_appdata = std::getenv("LOCALAPPDATA");
    expect(process_appdata != nullptr && std::string(process_appdata) != local_appdata.local_appdata(),
           "Overriding the data root must not touch the process environment.");

    sqlite3 *probe_db = nullptr;
    const std::string probe_db_path = local_appdata.local_appdata() + "\\metasequoiaime\\msime.db";
    expect(sqlite3_open(probe_db_path.c_str(), &probe_db) == SQLITE_OK,
           fmt::format("Failed to open probe db '{}'.", probe_db_path));
    char *probe_error = nullptr;
    expect(sqlite3_exec(probe_db,
                        "insert into tbl_2_c (key, jp, value, weight) values ('ce''li', 'cl', '测棂', 10000);", nullptr,
                        nullptr, &probe_error) == SQLITE_OK,
           fmt::format("Expected probe insert to succeed, got '{}'.", probe_error == nullptr ? "" : probe_error));
    sqlite3_free(probe_error);
    probe_error = nullptr;
    expect(sqlite3_exec(probe_db, "delete from tbl_2_c where key = 'ce''li' and value = '测棂';", nullptr, nullptr,
                        &probe_error) == SQLITE_OK,
           fmt::format("Expected probe delete to succeed, got '{}'.", probe_error == nullptr ? "" : probe_error));
    sqlite3_free(probe_error);
    sqlite3_close(probe_db);

    ShuangpinDictionary dictionary;

    const std::string raw_shuangpin = "celi";
    const std::string segmented_shuangpin = shuangpin::segment_input(raw_shuangpin);
    const std::string test_word = "测棂";

    fmt::println("==== Shuangpin Dictionary Create/Pin/Delete ====");

    // Clean up any residue from prior runs so the assertions stay deterministic.
    dictionary.delete_by_pinyin_and_word(raw_shuangpin, test_word);

    const auto before_create = dictionary.generateSeries(raw_shuangpin, segmented_shuangpin);
    expect(find_candidate(before_create, test_word) == nullptr,
           fmt::format("Expected '{}' to be absent before create.", test_word));

    expect(dictionary.create_word(raw_shuangpin, test_word) == ShuangpinDictionary::OK,
           "Shuangpin create_word should succeed.");

    const auto after_create = dictionary.generateSeries(raw_shuangpin, segmented_shuangpin);
    const WordItem *created = find_candidate(after_create, test_word);
    expect(created != nullptr, fmt::format("Expected '{}' to appear after create.", test_word));
    const int created_weight = created->weight;
    const std::string canonical_pinyin = created->canonical_pinyin;
    expect(!canonical_pinyin.empty(), "Created candidate should expose its canonical pinyin.");

    expect(dictionary.update_weight_by_pinyin_and_word(raw_shuangpin, test_word) == ShuangpinDictionary::OK,
           "Shuangpin update_weight_by_pinyin_and_word should succeed.");

    const auto after_pin = dictionary.generateSeries(raw_shuangpin, segmented_shuangpin);
    const WordItem *pinned = find_candidate(after_pin, test_word);
    expect(pinned != nullptr, fmt::format("Expected '{}' to remain after pin.", test_word));
    expect(pinned->weight > created_weight,
           fmt::format("Expected pinned weight to increase from {}, got {}.", created_weight, pinned->weight));

    expect(dictionary.delete_by_pinyin_and_word(canonical_pinyin, test_word) == ShuangpinDictionary::OK,
           "Shuangpin delete_by_pinyin_and_word should accept a canonical pinyin key.");

    const auto after_delete = dictionary.generateSeries(raw_shuangpin, segmented_shuangpin);
    expect(find_candidate(after_delete, test_word) == nullptr,
           fmt::format("Expected '{}' to be absent after delete.", test_word));
}

void test_shuangpin_dictionary_create_pin_delete_three_syllables()
{
    ScopedLocalAppDataOverride local_appdata("shuangpin-write-three-syllables");
    ShuangpinDictionary dictionary;

    const std::string raw_shuangpin = "qbtmuo";
    const std::string segmented_shuangpin = shuangpin::segment_input(raw_shuangpin);
    const std::string test_word = "秦天朔";

    fmt::println("==== Shuangpin Dictionary Create/Pin/Delete Three Syllables ====");

    dictionary.delete_by_pinyin_and_word(raw_shuangpin, test_word);

    const auto before_create = dictionary.generateSeries(raw_shuangpin, segmented_shuangpin);
    expect(find_candidate(before_create, test_word) == nullptr,
           fmt::format("Expected '{}' to be absent before create.", test_word));

    expect(dictionary.create_word(raw_shuangpin, test_word) == ShuangpinDictionary::OK,
           "Three-syllable shuangpin create_word should succeed.");

    const auto after_create = dictionary.generateSeries(raw_shuangpin, segmented_shuangpin);
    const WordItem *created = find_candidate(after_create, test_word);
    expect(created != nullptr, fmt::format("Expected '{}' to appear after create.", test_word));
    const int created_weight = created->weight;

    expect(dictionary.update_weight_by_pinyin_and_word(raw_shuangpin, test_word) == ShuangpinDictionary::OK,
           "Three-syllable shuangpin update_weight_by_pinyin_and_word should succeed.");

    const auto after_pin = dictionary.generateSeries(raw_shuangpin, segmented_shuangpin);
    const WordItem *pinned = find_candidate(after_pin, test_word);
    expect(pinned != nullptr, fmt::format("Expected '{}' to remain after pin.", test_word));
    expect(pinned->weight > created_weight,
           fmt::format("Expected pinned weight to increase from {}, got {}.", created_weight, pinned->weight));

    expect(dictionary.delete_by_pinyin_and_word(raw_shuangpin, test_word) == ShuangpinDictionary::OK,
           "Three-syllable shuangpin delete_by_pinyin_and_word should succeed.");

    const auto after_delete = dictionary.generateSeries(raw_shuangpin, segmented_shuangpin);
    expect(find_candidate(after_delete, test_word) == nullptr,
           fmt::format("Expected '{}' to be absent after delete.", test_word));
}

void test_quanpin_four_syllable_alternative_segmentation()
{
    QuanpinDictionary dictionary;

    fmt::println("==== Quanpin Four Syllable Alternative Segmentation ====");
    // jianmingeyao is cut as jian'min'ge'yao, so the entry only shows up through
    // the alternative segmentation jian'ming'e'yao.
    const auto result = dictionary.query("jianmingeyao");
    const auto *found = find_candidate(result, "简明扼要");
    expect(found != nullptr, "Expected '简明扼要' among the candidates for 'jianmingeyao'.");
}

void test_quanpin_single_letter_jianpin_ranking()
{
    ScopedLocalAppDataOverride local_appdata("single-letter-jianpin-ranking");
    QuanpinDictionary dictionary;

    fmt::println("==== Quanpin Single Letter Jianpin Ranking ====");
    // A single-letter context mixes entry keys: 一 comes from yi, 有 from you.
    const auto before = dictionary.query("y");
    const WordItem *selected = find_candidate(before, "有");
    const WordItem *rival = find_candidate(before, "一");
    expect(selected != nullptr, "Expected '有' among the candidates for 'y'.");
    if (rival == nullptr || rival->weight <= selected->weight)
    {
        fmt::println("Skipped: '一' does not outweigh '有' in this dictionary.");
        return;
    }

    // The server keys a selection by its canonical pinyin, so entry_key is 'you'
    // while context_key stays 'y'.
    const std::string entry_key = selected->canonical_pinyin.empty() ? selected->pinyin : selected->canonical_pinyin;
    bool ranking_changed = false;
    expect(user_dictionary::adjust_candidate_ranking(local_appdata.local_appdata() + "\\metasequoiaime\\msime.db",
                                                     user_dictionary::default_user_db_path(), "y", before, entry_key,
                                                     "有", "promote", 1, 1, true, &ranking_changed),
           "Expected the single-letter ranking adjustment to succeed.");
    expect(ranking_changed, "Expected the single-letter ranking adjustment to write a weight.");

    QuanpinDictionary reloaded;
    const auto after = reloaded.query("y");
    const WordItem *promoted = find_candidate(after, "有");
    const WordItem *demoted = find_candidate(after, "一");
    expect(promoted != nullptr, "Expected '有' to survive the ranking adjustment.");
    expect(demoted == nullptr || promoted->weight > demoted->weight,
           fmt::format("Expected '有' to outweigh '一' under context 'y', got {} and {}.", promoted->weight,
                       demoted == nullptr ? 0 : demoted->weight));
}

void test_quanpin_query_timings()
{
    QuanpinDictionary dictionary;

    fmt::println("==== Quanpin Query Timings ====");
    run_quanpin_query_case(dictionary, "nih");
    run_quanpin_query_case(dictionary, "niha");
    run_quanpin_query_case(dictionary, "nihao");
    run_quanpin_query_case(dictionary, "ni");
    run_quanpin_query_case(dictionary, "n");
    run_quanpin_query_case(dictionary, "shen");
    run_quanpin_query_case(dictionary, "shenme");
    run_quanpin_query_case(dictionary, "shenmeshi");
    run_quanpin_query_case(dictionary, "shenmeshi");
    run_quanpin_query_case(dictionary, "shenmeshui");
    run_quanpin_query_case(dictionary, "shenmesh");
    run_quanpin_query_case(dictionary, "shenmes");
    run_quanpin_query_case(dictionary, "n");
    run_quanpin_query_case(dictionary, "ni");
    run_quanpin_query_case(dictionary, "nis");
    run_quanpin_query_case(dictionary, "nish");
    run_quanpin_query_case(dictionary, "nishu");
    run_quanpin_query_case(dictionary, "nishuo");
    run_quanpin_query_case(dictionary, "nishuon");
    run_quanpin_query_case(dictionary, "nishuone");
    run_quanpin_query_case(dictionary, "keneng");
}

quanpin::WordLatticeLookup make_table_lattice_lookup(
    const std::unordered_map<std::string, std::vector<quanpin::LatticeLexeme>> &table)
{
    return [&table](const quanpin::Segments &span) {
        const auto it = table.find(quanpin::join_segments(span));
        return it == table.end() ? std::vector<quanpin::LatticeLexeme>{} : it->second;
    };
}

void test_word_lattice()
{
    using quanpin::LatticeLexeme;

    fmt::println("==== Word Lattice ====");

    {
        std::unordered_map<std::string, std::vector<LatticeLexeme>> table;
        table["nie"] = {{"nie", "捏", 8000}, {"nie", "聂", 4000}, {"nie", "镊", 3000}};
        table["zi"] = {{"zi", "子", 9000}};
        table["nie'zi"] = {{"nie'zi", "镊子", 18000}, {"nie'zi", "孽子", 2000}};
        const auto paths = quanpin::decode_word_lattice({"nie", "zi"}, make_table_lattice_lookup(table));
        expect(!paths.empty() && paths.front().sentence == "镊子",
               fmt::format("Expected 镊子 for nie zi, got '{}'", paths.empty() ? "" : paths.front().sentence));
    }

    {
        std::unordered_map<std::string, std::vector<LatticeLexeme>> table;
        table["gao"] = {{"gao", "高", 12000}, {"gao", "搞", 11000}};
        table["tan"] = {{"tan", "谈", 9000}, {"tan", "碳", 4000}, {"tan", "摊", 3500}};
        table["gang"] = {{"gang", "刚", 9000}, {"gang", "钢", 5000}, {"gang", "岗", 4000}};
        table["nie"] = {{"nie", "捏", 8000}, {"nie", "镊", 3000}};
        table["zi"] = {{"zi", "子", 9000}};
        table["gao'tan"] = {{"gao'tan", "高谈", 20000}};
        table["tan'gang"] = {{"tan'gang", "碳钢", 16000}};
        table["nie'zi"] = {{"nie'zi", "镊子", 18000}};
        const auto paths =
            quanpin::decode_word_lattice({"gao", "tan", "gang", "nie", "zi"}, make_table_lattice_lookup(table));
        expect(!paths.empty() && paths.front().sentence == "高碳钢镊子",
               fmt::format("Expected 高碳钢镊子, got '{}'", paths.empty() ? "" : paths.front().sentence));
    }

    {
        std::unordered_map<std::string, std::vector<LatticeLexeme>> table;
        table["gao"] = {{"gao", "高", 12000}};
        table["tan"] = {{"tan", "碳", 4000}};
        table["gang"] = {{"gang", "钢", 5000}};
        table["tan'gang"] = {{"tan'gang", "碳钢", 16000}};
        std::vector<WordItem> candidates;
        candidates.emplace_back("gktjgh", "高碳钢", 50000, CandidateSource::Database, "gao'tan'gang");
        quanpin::merge_lattice_candidates(candidates, {"gao", "tan", "gang"}, make_table_lattice_lookup(table),
                                          "gktjgh");
        expect(candidates.front().word == "高碳钢" && candidates.front().source == CandidateSource::Database,
               fmt::format("Expected Database 高碳钢 first, got '{}'", candidates.front().word));
    }

    {
        std::unordered_map<std::string, std::vector<LatticeLexeme>> table;
        table["gao"] = {{"gao", "高", 12000}};
        table["tan"] = {{"tan", "碳", 4000}, {"tan", "谈", 9000}};
        table["gang"] = {{"gang", "钢", 5000}, {"gang", "刚", 9000}};
        table["nie"] = {{"nie", "镊", 3000}};
        table["zi"] = {{"zi", "子", 9000}};
        table["tan'gang"] = {{"tan'gang", "碳钢", 16000}};
        table["nie'zi"] = {{"nie'zi", "镊子", 18000}};
        std::vector<WordItem> candidates;
        candidates.emplace_back("gktjghnxzi", "高谈刚捏子", 1, CandidateSource::Fallback);
        quanpin::merge_lattice_candidates(candidates, {"gao", "tan", "gang", "nie", "zi"},
                                          make_table_lattice_lookup(table), "gktjghnxzi");
        expect(candidates.front().word == "高碳钢镊子",
               fmt::format("Expected lattice 高碳钢镊子 ahead of Fallback, got '{}'", candidates.front().word));
        expect(find_candidate(candidates, "高谈刚捏子") != nullptr, "Expected Fallback 高谈刚捏子 to remain.");
    }

    {
        std::unordered_map<std::string, std::vector<LatticeLexeme>> table;
        table["xing"] = {{"xing", "性", 8000}};
        table["neng"] = {{"neng", "能", 8000}};
        table["hen"] = {{"hen", "很", 20000}, {"hen", "狠", 3000}};
        table["la"] = {{"la", "拉", 5000}};
        table["ji"] = {{"ji", "圾", 4000}};
        table["xing'neng"] = {{"xing'neng", "性能", 15000}};
        table["la'ji"] = {{"la'ji", "垃圾", 14000}};
        const auto paths =
            quanpin::decode_word_lattice({"xing", "neng", "hen", "la", "ji"}, make_table_lattice_lookup(table));
        expect(!paths.empty() && paths.front().sentence.find("很垃圾") != std::string::npos &&
                   paths.front().sentence.find("狠垃圾") == std::string::npos,
               fmt::format("Expected 很垃圾 over 狠垃圾, got '{}'", paths.empty() ? "" : paths.front().sentence));
    }

    {
        std::unordered_map<std::string, std::vector<LatticeLexeme>> table;
        table["nie"] = {{"nie", "捏", 8000}};
        table["zi"] = {{"zi", "子", 9000}};
        table["nie'zi"] = {{"nie'zi", "镊子", 18000}};
        std::vector<WordItem> candidates;
        candidates.emplace_back("nxzi", "镊子", 18000, CandidateSource::Database, "nie'zi");
        quanpin::merge_lattice_candidates(candidates, {"nie", "zi"}, make_table_lattice_lookup(table), "nxzi");
        expect(candidates.size() == 1 && candidates.front().source == CandidateSource::Database,
               "Two-syllable merge is a no-op; exact SQLite already covers the key.");
    }

    {
        std::unordered_map<std::string, std::vector<LatticeLexeme>> table;
        table["g"] = {{"g", "个", 100}};
        table["k"] = {{"k", "可", 100}};
        table["t"] = {{"t", "他", 100}};
        table["g'k't"] = {{"g'k't", "个可他", 50}};
        std::vector<WordItem> candidates;
        quanpin::merge_lattice_candidates(candidates, {"g", "k", "t"}, make_table_lattice_lookup(table), "gkt");
        expect(candidates.empty(), "Abbreviated quanpin segments must not produce lattice candidates.");
    }
}

void test_quanpin_order_corrections()
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"laing", "liang"}, {"haung", "huang"}, {"bain", "bian"},   {"daun", "duan"},
        {"laio", "liao"},   {"mihng", "ming"},  {"ahng", "hang"},   {"behng", "beng"},
        {"agn", "ang"},     {"zagn", "zang"},   {"egn", "eng"},     {"zhegn", "zheng"},
        {"jv", "ju"},       {"wojv", "wo'ju"},  {"wo'jv", "wo'ju"}, {"woxainxin", "wo'xian'xin"},
    };

    for (const auto &[typed, expected] : cases)
    {
        const auto cuts = quanpin::cut_pinyin_by_mode(typed, "correction");
        expect(!cuts.empty(), fmt::format("Expected '{}' to produce a corrected path.", typed));
        expect(quanpin::join_segments(cuts.front()) == expected,
               fmt::format("Expected '{}' to normalize to '{}', got '{}'.", typed, expected,
                           quanpin::join_segments(cuts.front())));
    }

    const auto ambiguous = quanpin::cut_pinyin_by_mode("cehng", "correction");
    expect(ambiguous.size() >= 2, "Expected cehng to retain both valid interpretations.");
    expect(quanpin::join_segments(ambiguous.front()) == "cheng", "Expected cheng to be the primary interpretation.");
    expect(std::any_of(ambiguous.begin(), ambiguous.end(),
                       [](const quanpin::Segments &segments) { return quanpin::join_segments(segments) == "ceng"; }),
           "Expected ceng to remain available as an alternative interpretation.");

    const auto ambiguous_ahng = quanpin::cut_pinyin_by_mode("ahng", "correction");
    expect(ambiguous_ahng.size() >= 2, "Expected ahng to retain both valid interpretations.");
    expect(quanpin::join_segments(ambiguous_ahng.front()) == "hang", "Expected hang to be the primary interpretation.");
    expect(std::any_of(ambiguous_ahng.begin(), ambiguous_ahng.end(),
                       [](const quanpin::Segments &segments) { return quanpin::join_segments(segments) == "ang"; }),
           "Expected ang to remain available as an alternative interpretation.");
}

namespace
{ // Minimal deterministic dictionary for the mask matrix: '上' exists only under the
// corrected key 'shang', so a leading 上 proves the corrected path actually ran.
std::filesystem::path create_autocorrect_probe_database()
{
    const fs::path path = fs::temp_directory_path() / "msime-quanpin-autocorrect-mask-test.db";
    std::error_code ec;
    fs::remove(path, ec);
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK)
    {
        throw std::runtime_error("Failed to create the autocorrect probe database.");
    }
    const char *sql = "CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_2_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_4_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "INSERT INTO tbl_1_s VALUES('shang','s','上',100);"
                      "INSERT INTO tbl_2_s VALUES('shang''zhi','sz','上至',100);"
                      "INSERT INTO tbl_4_s VALUES('sa''huang''na''ge','shng','撒谎那个',1000);";
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (result != SQLITE_OK)
    {
        fs::remove(path, ec);
        throw std::runtime_error("Failed to initialize the autocorrect probe database.");
    }
    return path;
}
} // namespace

void test_quanpin_autocorrect_switches_and_guard()
{
    fmt::println("==== Quanpin Autocorrect Switch Matrix And Jianpin Guard ====");

    const unsigned none = 0;
    const unsigned transposition_only = quanpin::kAutocorrectTransposition;
    const unsigned neighbor_only = quanpin::kAutocorrectNeighbor;
    const unsigned both = transposition_only | neighbor_only;

    // Cut-level switch matrix: 'sahng' is a transposition fix, 'shabg' a neighbor
    // fix, and each type bit must enable exactly its own family (AC1-AC4).
    expect(quanpin::autocorrect_cut("sahng", none).empty(), "Both switches off must disable the correction cut.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("sahng", transposition_only)) == "shang",
           "Transposition-only must correct 'sahng'.");
    expect(quanpin::autocorrect_cut("sahng", neighbor_only).empty(),
           "Neighbor-only must not correct the transposition case 'sahng'.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("sahng", both)) == "shang",
           "Both switches on must correct 'sahng'.");

    expect(quanpin::autocorrect_cut("shabg", none).empty(), "Both switches off must disable the correction cut.");
    expect(quanpin::autocorrect_cut("shabg", transposition_only).empty(),
           "Transposition-only must not correct the neighbor case 'shabg'.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("shabg", neighbor_only)) == "shang",
           "Neighbor-only must correct 'shabg'.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("shabg", both)) == "shang",
           "Both switches on must correct 'shabg'.");

    // Jianpin-shape guard: one or more legal syllables plus at most one trailing
    // letter is user intent, never a typo (AC5).
    expect(quanpin::looks_like_syllable_with_jianpin_tail("zheg"),
           "'zheg' (zhe + g) must be detected as jianpin intent.");
    expect(quanpin::looks_like_syllable_with_jianpin_tail("keneng"),
           "A fully legal spelling also satisfies the shape predicate.");
    expect(!quanpin::looks_like_syllable_with_jianpin_tail("sahng"),
           "'sahng' leaves a 3-letter tail and must stay correctable.");
    expect(!quanpin::looks_like_syllable_with_jianpin_tail("shabg"),
           "'shabg' leaves a 2-letter tail and must stay correctable.");
    expect(!quanpin::looks_like_syllable_with_jianpin_tail("xi'an"), "Manual delimiters never take part in the guard.");
    expect(!quanpin::looks_like_syllable_with_jianpin_tail("wj"),
           "Pure-consonant jianpin must stay correctable at the predicate level.");
    expect(!quanpin::looks_like_syllable_with_jianpin_tail("bqng"),
           "3+ letter all-consonant strings stay correctable by design (no multi-letter jianpin).");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("bqng", neighbor_only)) == "bang",
           "'bqng' -> bang must remain a valid neighbor correction.");
    expect(quanpin::autocorrect_cut("zheg", both).empty(),
           "The cleaned table must offer no correction path for 'zheg' (2-letter keys are gone).");

    // Dictionary-level matrix against a deterministic probe database.
    const auto db_path = create_autocorrect_probe_database();
    const auto is_shang = [](const WordItem &item) { return item.word == "上"; };
    {
        QuanpinDictionary dictionary(db_path.string());

        const auto corrected = dictionary.query("sahng", "sa'h'n'g", both);
        expect(!corrected.empty() && corrected.front().word == "上",
               "Both switches on must put the corrected candidate first.");
        expect(corrected.front().canonical_pinyin == "shang",
               "The corrected candidate must keep the corrected key as canonical pinyin.");

        const auto off = dictionary.query("sahng", "sa'h'n'g", none);
        expect(std::none_of(off.begin(), off.end(), is_shang),
               "Both switches off must keep the corrected candidate out (AC1).");
        expect(std::any_of(off.begin(), off.end(), [](const WordItem &item) { return item.word == "撒谎那个"; }),
               "The legacy fallback candidates must survive with both switches off.");

        const auto transposed = dictionary.query("sahng", "sa'h'n'g", transposition_only);
        expect(!transposed.empty() && transposed.front().word == "上",
               "Transposition-only must correct 'sahng' at the dictionary layer (AC2).");

        const auto neighbor_denied = dictionary.query("sahng", "sa'h'n'g", neighbor_only);
        expect(std::none_of(neighbor_denied.begin(), neighbor_denied.end(), is_shang),
               "Neighbor-only must not correct the transposition case 'sahng' (AC2).");

        const auto neighbor_corrected = dictionary.query("shabg", "sha'b'g", neighbor_only);
        expect(!neighbor_corrected.empty() && neighbor_corrected.front().word == "上",
               "Neighbor-only must correct 'shabg' at the dictionary layer (AC3).");

        const auto transposition_denied = dictionary.query("shabg", "sha'b'g", transposition_only);
        expect(std::none_of(transposition_denied.begin(), transposition_denied.end(), is_shang),
               "Transposition-only must not correct the neighbor case 'shabg' (AC3).");

        const auto multi = dictionary.query("sahngzhi", "sa'h'n'g'zhi", both);
        expect(!multi.empty() && multi.front().word == "上至",
               "Cross-syllable correction must survive the mask wiring.");

        // The guard fires before the BFS, so a jianpin-shaped input must resolve
        // through its raw segmentation instead of a corrected key such as 'zu'ge'.
        (void)dictionary.query("zheg", "", both);
        expect(dictionary.get_pinyin_segmentation() != "zu'ge",
               "A jianpin-shaped input must not resolve through a corrected key.");
    }
    std::error_code cleanup_ec;
    fs::remove(db_path, cleanup_ec);

    // Generated-table invariants shared by both type tables (AC6).
    const auto &legal = quanpin::intact_pinyin_set();
    std::unordered_set<std::string> seen_keys;
    size_t total_entries = 0;
    for (const auto *table : {&quanpin::autocorrect::kTranspositionEntries, &quanpin::autocorrect::kNeighborEntries})
    {
        for (const auto &entry : *table)
        {
            const std::string wrong(entry.wrong);
            const std::string correct(entry.correct);
            expect(wrong.size() >= 3, "2-letter keys belong to the jianpin space and must not be generated.");
            expect(legal.find(wrong) == legal.end(), "A correction key must never shadow a legal syllable.");
            expect(legal.find(correct) != legal.end(), "A correction target must be a legal syllable.");
            expect(seen_keys.insert(wrong).second, "Correction keys must be unique across both tables.");
            ++total_entries;
        }
    }
    expect(total_entries > 1000, "The generated tables unexpectedly shrank.");
}

namespace
{ // 显示/标记用例的独立探针库：shang、shang'hao、ke'neng、nv、sa'huang'na'ge 最小键集。
std::filesystem::path create_autocorrect_display_probe_database()
{
    const fs::path path = fs::temp_directory_path() / "msime-quanpin-autocorrect-display-test.db";
    std::error_code ec;
    fs::remove(path, ec);
    sqlite3 *db = nullptr;
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK)
    {
        throw std::runtime_error("Failed to create the autocorrect display probe database.");
    }
    const char *sql = "CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_2_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_2_k(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_4_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "INSERT INTO tbl_1_s VALUES('shang','s','上',100);"
                      "INSERT INTO tbl_1_n VALUES('nv','n','女',100);"
                      "INSERT INTO tbl_2_s VALUES('shang''hao','sh','上好',100);"
                      "INSERT INTO tbl_2_k VALUES('ke''neng','kn','可能',100);"
                      "INSERT INTO tbl_4_s VALUES('sa''huang''na''ge','shng','撒谎那个',1000);";
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (result != SQLITE_OK)
    {
        fs::remove(path, ec);
        throw std::runtime_error("Failed to initialize the autocorrect display probe database.");
    }
    return path;
}

std::size_t count_marked(const std::vector<WordItem> &items)
{
    return static_cast<std::size_t>(
        std::count_if(items.begin(), items.end(), [](const WordItem &item) { return !item.corrected_from.empty(); }));
}

void type_display_session(metasequoia::InputSession &session, const std::string &text)
{
    for (const char character : text)
    {
        if (!session.handle_character(character).handled)
        {
            throw std::runtime_error("A display-test pinyin character was not handled.");
        }
    }
}
} // namespace

void test_quanpin_autocorrect_display()
{
    fmt::println("==== Quanpin Autocorrect Display (raw-letter preedit + candidate marking) ====");

    const unsigned none = 0;
    const unsigned transposition_only = quanpin::kAutocorrectTransposition;
    const unsigned neighbor_only = quanpin::kAutocorrectNeighbor;
    const unsigned both = transposition_only | neighbor_only;

    // utils 级：纠错切分携带原始区间，raw span 从回溯位置直接推出。
    const auto sahng_cut = quanpin::autocorrect_cut_detail("sahng", both);
    expect(sahng_cut.segments.size() == 1, "'sahng' must cut into a single corrected segment.");
    expect(sahng_cut.segments[0].syllable == "shang" && sahng_cut.segments[0].raw_text == "sahng" &&
               sahng_cut.segments[0].start == 0 && sahng_cut.segments[0].corrected,
           "The 'sahng' segment must keep the raw letters alongside the corrected syllable.");

    const auto shabg_cut = quanpin::autocorrect_cut_detail("shabg", both);
    expect(shabg_cut.segments.size() == 1 && shabg_cut.segments[0].syllable == "shang" &&
               shabg_cut.segments[0].raw_text == "shabg" && shabg_cut.segments[0].start == 0 &&
               shabg_cut.segments[0].corrected,
           "The 'shabg' segment must keep the raw letters alongside the corrected syllable.");

    const auto sahnghao_cut = quanpin::autocorrect_cut_detail("sahnghao", both);
    expect(sahnghao_cut.segments.size() == 2, "'sahnghao' must cut into two segments.");
    expect(sahnghao_cut.segments[0].syllable == "shang" && sahng_cut.segments[0].raw_text == "sahng" &&
               sahnghao_cut.segments[0].start == 0 && sahnghao_cut.segments[0].corrected,
           "The first 'sahnghao' segment must keep the raw letters of the corrected part.");
    expect(sahnghao_cut.segments[1].syllable == "hao" && sahng_cut.segments[1].raw_text == "hao" &&
               sahng_cut.segments[1].start == 5 && !sahnghao_cut.segments[1].corrected,
           "The untouched tail of 'sahnghao' must keep its raw span.");

    expect(quanpin::autocorrect_cut_detail("zheg", both).empty(),
           "The cleaned table must leave the jianpin shape 'zheg' unexplained by the BFS.");
    expect(quanpin::autocorrect_cut_detail("keneng", both).empty(),
           "A fully legal spelling must produce no correction cut.");
    expect(quanpin::autocorrect_cut_detail("sahng", none).empty(),
           "Both switches off must disable the range-carrying cut too.");
    expect(quanpin::autocorrect_cut_detail("xi'an", both).empty(),
           "Manual delimiters must disable the range-carrying cut.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("sahng", both)) == "shang",
           "The Segments wrapper must stay a projection of the detail cut.");

    // 字典级标记：候选字母 == 主切分字母 且 主切分字母 != 原始字母 才标记。
    const auto db_path = create_autocorrect_display_probe_database();
    {
        QuanpinDictionary dictionary(db_path.string());

        const auto corrected = dictionary.query("sahng", "sa'h'n'g", both);
        expect(!corrected.empty() && corrected.front().word == "上" && corrected.front().corrected_from == "sahng",
               "The corrected first candidate must carry the typed input as corrected_from (AC7).");
        const auto legacy = std::find_if(corrected.begin(), corrected.end(),
                                         [](const WordItem &item) { return item.word == "撒谎那个"; });
        expect(legacy != corrected.end() && legacy->corrected_from.empty(),
               "The legacy fallback tail must stay unmarked.");

        const auto off = dictionary.query("sahng", "sa'h'n'g", none);
        expect(count_marked(off) == 0,
               "No candidate may be marked while the primary segmentation keeps the typed letters.");

        // 别名层改写（真实方案层会把校正后的 segmentation 传进来）与开关无关，照样标记。
        const auto alias_like = dictionary.query("sahng", "shang", none);
        expect(!alias_like.empty() && alias_like.front().word == "上" && alias_like.front().corrected_from == "sahng",
               "Alias-layer corrected candidates must be labelled regardless of the switches.");

        // 前缀候选不标记：只有字母等于主切分的整词候选才带 corrected_from。
        const auto full = dictionary.query("sahnghao", "sa'h'n'g'hao", both);
        expect(!full.empty() && full.front().word == "上好" && full.front().corrected_from == "sahnghao",
               "The full-length corrected candidate must be labelled with the typed input.");
        const auto prefix =
            std::find_if(full.begin(), full.end(), [](const WordItem &item) { return item.word == "上"; });
        expect(prefix != full.end() && prefix->corrected_from.empty(), "Partial prefix candidates must stay unmarked.");
        expect(count_marked(full) == 1, "Exactly the full-length corrected candidate may be marked for 'sahnghao'.");

        const auto keneng = dictionary.query("keneng", "ke'neng", both);
        expect(count_marked(keneng) == 0, "A legal spelling must produce no marks.");
        expect(count_marked(dictionary.query("wj", "w'j", both)) == 0, "Jianpin input must produce no marks (AC5).");
        const auto nv = dictionary.query("nv", "nv", both);
        expect(!nv.empty() && nv.front().word == "女" && nv.front().corrected_from.empty(),
               "The u-umlaut 'v' spelling must stay unmarked.");
    }

    // 会话级 preedit：get_pinyin_segmentation_with_cases 必须画原始字母。
    const fs::path session_dir = fs::temp_directory_path() / "msime-quanpin-autocorrect-display-session";
    std::error_code cleanup_ec;
    fs::remove_all(session_dir, cleanup_ec);
    fs::create_directories(session_dir / "helpcodes");
    fs::copy_file(db_path, session_dir / "msime.db", fs::copy_options::overwrite_existing);
    {
        std::ofstream helpcodes(session_dir / "helpcodes" / "helpcode.txt");
        helpcodes << "你=ab\n";
    }
    metasequoia::RuntimePaths paths;
    paths.resources = session_dir;
    paths.user_data = session_dir;
    paths.cache = session_dir;
    paths.dictionaries = session_dir;

    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type_display_session(session, "sahng");
        expect(session.get_pinyin_segmentation_with_cases() == "sahng",
               "The preedit must show the typed letters, not the alias rewrite (AC7).");
        expect(!session.candidates().empty() && session.candidates().front().word == "上" &&
                   session.candidates().front().corrected_from == "sahng",
               "The first candidate for 'sahng' must be the corrected 上 with corrected_from.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type_display_session(session, "sahnghao");
        expect(session.get_pinyin_segmentation_with_cases() == "sahng'hao",
               "The preedit must keep the typed letters and re-separate at the cut positions.");
        expect(!session.candidates().empty() && session.candidates().front().word == "上好" &&
                   session.candidates().front().corrected_from == "sahnghao",
               "The full-length candidate for 'sahnghao' must be marked.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        session.set_pinyin_sequence("sahng");
        session.set_pinyin_sequence_with_cases("saHng");
        session.recompute_candidates();
        expect(session.get_pinyin_segmentation_with_cases() == "saHng",
               "Uppercase input letters must survive the display rebuild.");
        expect(!session.candidates().empty() && session.candidates().front().corrected_from == "sahng",
               "Marking for cased input must carry the folded typed letters.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type_display_session(session, "shabg");
        expect(session.get_pinyin_segmentation_with_cases() == "shabg",
               "A BFS-corrected input with untouched letters must redraw separators from the raw spans.");
        expect(!session.candidates().empty() && session.candidates().front().word == "上" &&
                   session.candidates().front().corrected_from == "shabg",
               "The BFS-corrected candidate for 'shabg' must be marked.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, none, true, true, true, paths);
        type_display_session(session, "shabg");
        expect(session.get_pinyin_segmentation_with_cases() == "sha'b'g",
               "With both switches off the legacy greedy separators must stay.");
        expect(count_marked(session.candidates()) == 0, "No corrected candidate exists with both switches off.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, none, true, true, true, paths);
        type_display_session(session, "sahng");
        expect(session.get_pinyin_segmentation_with_cases() == "sahng",
               "Even with both switches off the alias rewrite must be undone in the preedit.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, transposition_only, true, true, true, paths);
        type_display_session(session, "sahng");
        expect(!session.candidates().empty() && session.candidates().front().word == "上" &&
                   session.candidates().front().corrected_from == "sahng",
               "Transposition-only must correct 'sahng' end to end (AC2, mask must survive the session).");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, neighbor_only, true, true, true, paths);
        type_display_session(session, "shabg");
        expect(!session.candidates().empty() && session.candidates().front().word == "上" &&
                   session.candidates().front().corrected_from == "shabg",
               "Neighbor-only must correct 'shabg' end to end (AC3, mask must survive the session).");
        expect(session.get_pinyin_segmentation_with_cases() == "shabg",
               "Neighbor-only 'shabg' preedit shows the typed letters.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, transposition_only, true, true, true, paths);
        type_display_session(session, "shabg");
        expect(count_marked(session.candidates()) == 0,
               "Transposition-only must not correct the neighbor case 'shabg' (AC3).");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, neighbor_only, true, true, true, paths);
        type_display_session(session, "sahng");
        // 邻键开关关不掉别名层（现状基线），sahng 仍由别名层给出「上」并标记。
        expect(session.get_pinyin_segmentation_with_cases() == "sahng",
               "Neighbor-only 'sahng' preedit still shows the typed letters.");
        expect(!session.candidates().empty() && session.candidates().front().word == "上" &&
                   session.candidates().front().corrected_from == "sahng",
               "Neighbor-only keeps the alias-layer baseline for 'sahng'.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type_display_session(session, "keneng");
        expect(session.get_pinyin_segmentation_with_cases() == "ke'neng",
               "A legal spelling must keep its exact preedit.");
        expect(count_marked(session.candidates()) == 0, "A legal spelling must produce no marks.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type_display_session(session, "xi'an");
        expect(session.get_pinyin_segmentation_with_cases() == "xi'an",
               "A manual delimiter input must keep its exact preedit.");
        expect(count_marked(session.candidates()) == 0, "A manual delimiter input must produce no marks.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type_display_session(session, "zheg");
        expect(session.get_pinyin_segmentation_with_cases() == "zhe'g",
               "The jianpin shape 'zheg' must keep its raw letters and separator.");
        expect(count_marked(session.candidates()) == 0, "The jianpin shape 'zheg' must produce no marks.");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type_display_session(session, "wj");
        expect(session.get_pinyin_segmentation_with_cases() == "w'j", "Pure jianpin must keep its greedy preedit.");
        expect(count_marked(session.candidates()) == 0, "Pure jianpin must produce no marks (AC5).");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type_display_session(session, "nv");
        expect(session.get_pinyin_segmentation_with_cases() == "nv",
               "The u-umlaut spelling must keep its exact preedit.");
        expect(!session.candidates().empty() && session.candidates().front().word == "女" &&
                   session.candidates().front().corrected_from.empty(),
               "The u-umlaut candidate must stay unmarked.");
    }

    fs::remove_all(session_dir, cleanup_ec);
    fs::remove(db_path, cleanup_ec);
}

int main(int argc, char *argv[])
{
    try
    {
        test_word_lattice();
        test_shuangpin_session();
        test_shuangpin_session02();
        test_quanpin_session();
        test_dynamic_switch();
        test_quanpin_session_backspace();
        test_shuangpin_session_backspace();
        test_shuangpin_manual_apostrophe();
        test_quanpin_dictionary_backspace();
        test_shuangpin_dictionary_backspace();
        test_shuangpin_dictionary_create_pin_delete();
        test_shuangpin_dictionary_create_pin_delete_three_syllables();
        test_shuangpin_query_manual_apostrophe();
        test_quanpin_order_corrections();
        test_quanpin_four_syllable_alternative_segmentation();
        test_quanpin_single_letter_jianpin_ranking();
        test_quanpin_query_timings();
        test_quanpin_autocorrect_switches_and_guard();
        test_quanpin_autocorrect_display();
        fmt::println("All tests passed.");
        return 0;
    }
    catch (const std::exception &ex)
    {
        fmt::println(stderr, "Test failure: {}", ex.what());
        return 1;
    }
}
