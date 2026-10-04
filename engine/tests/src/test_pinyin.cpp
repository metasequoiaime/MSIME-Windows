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

        // msime.db 是回归断言的主体，缺失即环境不完整。其余几个都不是硬依赖：
        // dict_pinyin.dat 随安装包发布，user_dict.dat 是可写用户文件、由引擎自建，
        // 两者缺失时 PinyinDecoder::sentence 直接返回空串；sc.lm 缺失则退回启发式
        // 打分，整句候选只是变差、不会消失。所以这里仅在源目录存在时才拷贝。
        for (const auto &file_name : {"msime.db", "dict_pinyin.dat", "user_dict.dat", "sc.lm"})
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

// 两条整句来源的相对次序：词格（Generated，kenlm 打分）必须排在 Google 解码器
// （Fallback）之前。merge_lattice_candidates 自己的插入点已经由上面的表驱动用例
// 覆盖，但词典层曾经在合并之后又把 Fallback 提回首位，那一步没有任何测试盯着，
// 换掉打分模型之后才发现次序是反的。这里断的是词典层的最终结果。
//
// 不写死具体句子：词格出什么取决于 msime.db 和 sc.lm，钉死了会随词库更新而碎。
// 断的是相对位置，且只在两条来源都真的出现时才断。
void test_quanpin_lattice_precedes_google_fallback()
{
    QuanpinDictionary dictionary;

    fmt::println("==== Quanpin Lattice Precedes Google Fallback ====");
    bool observed = false;
    for (const auto *query : {"nihaoshijie", "jintiantianqizhenhao", "womenyiqiquchifan"})
    {
        const auto result = dictionary.query(query);
        const auto generated = std::find_if(result.begin(), result.end(), [](const WordItem &item) {
            return item.source == CandidateSource::Generated;
        });
        const auto fallback = std::find_if(result.begin(), result.end(), [](const WordItem &item) {
            return item.source == CandidateSource::Fallback;
        });
        if (generated == result.end() || fallback == result.end())
            continue;
        observed = true;
        expect(generated < fallback,
               fmt::format("Expected the lattice sentence ahead of the Google fallback for '{}', got '{}' before '{}'",
                           query, fallback->word, generated->word));
    }
    if (!observed)
    {
        // sc.lm 或 dict_pinyin.dat 不在数据目录里时两条来源凑不齐，与其他用例一样优雅跳过。
        fmt::println("Skipped: no query produced both a lattice and a Google fallback sentence.");
    }
}

// 多音字的生僻读音不能被词格整句选中。词库里「卷」带一行 gun、「而」带一行
// neng，权重都是 0；kenlm 只看汉字，这两个字它见得多，于是 gun'qi 出过「卷七」、
// neng'fa'sheng 出过「而发生」。WordLatticeOptions::reading_prior 按读音占比补上
// 这一维。依赖真实 msime.db + sc.lm，凑不齐时与其他用例一样优雅跳过。
void test_quanpin_lattice_rejects_rare_readings()
{
    fmt::println("==== Quanpin Lattice Rejects Rare Readings ====");

    // 表驱动的词格用例跑的是无模型的启发式打分，那条路本来就按绝对权重压生僻行，
    // 出不了这个 bug。所以这里直接拿真实 msime.db + sc.lm 解一次词格。
    const auto paths = metasequoia::RuntimePaths::legacy();
    const auto &model = ngram::shared_language_model(paths.resource(metasequoia::assets::language_model));
    const std::string db_path = quanpin::get_default_db_path();
    sqlite3 *db = nullptr;
    if (!model.valid() || sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        if (db != nullptr)
            sqlite3_close(db);
        fmt::println("Skipped: sc.lm or msime.db is not available.");
        return;
    }

    std::unordered_map<std::string, sqlite3_stmt *> statement_cache;
    for (const auto &probe : {std::pair<const char *, const char *>{"gun'qi", "卷七"},
                              std::pair<const char *, const char *>{"neng'fa'sheng", "而发生"}})
    {
        quanpin::WordLatticeOptions options;
        options.nbest = 1;
        options.language_model = &model;
        const auto lattice_paths = quanpin::decode_word_lattice(
            quanpin::split_segments(probe.first),
            quanpin::make_lattice_db_lookup(db, statement_cache, options.span_limit), options);
        const std::string sentence = lattice_paths.empty() ? "" : lattice_paths.front().sentence;
        // 「卷」在词库里带一行 gun、「而」带一行 neng，权重都是 0；kenlm 只看汉字，
        // 这两个字它见得多，reading_prior 之前 gun'qi 出「卷七」、neng'fa'sheng
        // 出「而发生」。
        expect(sentence != probe.second,
               fmt::format("'{}' spells a rare reading and must not win the lattice for '{}'.", probe.second,
                           probe.first));
    }

    for (auto &entry : statement_cache)
        sqlite3_finalize(entry.second);
    sqlite3_close(db);
}

// 词格的跨度查询只认精确键，所以每个合法音节在词库里都必须至少查得到一行。少一个，
// 句子里一旦用到它，整张词格就断了——不是候选变差，是长句联想整个消失。切分器认
// jv / lue 这类 ü 的另一种拼法，而词库只存 ju / lve，曾经就是这么断的。这条用例把
// 「切分器认的音节集」和「词库存的键」钉在一起，任何一边漂了都会在这里先炸。
void test_quanpin_lattice_covers_every_syllable()
{
    fmt::println("==== Quanpin Lattice Covers Every Syllable ====");

    const std::string db_path = quanpin::get_default_db_path();
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        if (db != nullptr)
            sqlite3_close(db);
        fmt::println("Skipped: msime.db is not available.");
        return;
    }

    std::unordered_map<std::string, sqlite3_stmt *> statement_cache;
    const auto lookup = quanpin::make_lattice_db_lookup(db, statement_cache, 32);
    std::string uncovered;
    size_t uncovered_count = 0;
    for (const auto &syllable : quanpin::intact_pinyin_list())
    {
        if (!lookup({syllable}).empty())
            continue;
        if (uncovered_count++ > 0)
            uncovered += ' ';
        uncovered += syllable;
    }
    expect(uncovered_count == 0, fmt::format("Every syllable needs at least one exact-key row, but {} have none: {}.",
                                             uncovered_count, uncovered));

    for (auto &entry : statement_cache)
        sqlite3_finalize(entry.second);
    sqlite3_close(db);
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
        // 门槛是 2 个音节：两音节输入也出整句，但整句排在整串拼音精确命中的词库
        // 候选之后，不许抢首位。
        std::unordered_map<std::string, std::vector<LatticeLexeme>> table;
        table["nie"] = {{"nie", "捏", 8000}};
        table["zi"] = {{"zi", "子", 9000}};
        table["nie'zi"] = {{"nie'zi", "镊子", 18000}};
        std::vector<WordItem> candidates;
        candidates.emplace_back("nxzi", "捏子", 12000, CandidateSource::Database, "nie'zi");
        quanpin::merge_lattice_candidates(candidates, {"nie", "zi"}, make_table_lattice_lookup(table), "nxzi");
        expect(candidates.front().word == "捏子" && candidates.front().source == CandidateSource::Database,
               fmt::format("Expected the exact Database hit to stay first, got '{}'", candidates.front().word));
        expect(find_candidate(candidates, "镊子") != nullptr,
               "Expected the two-syllable lattice sentence 镊子 to be merged in.");
    }

    {
        // 单音节仍然不出整句：精确查表已经全覆盖。
        std::unordered_map<std::string, std::vector<LatticeLexeme>> table;
        table["zi"] = {{"zi", "子", 9000}, {"zi", "字", 8000}};
        std::vector<WordItem> candidates;
        candidates.emplace_back("zi", "子", 9000, CandidateSource::Database, "zi");
        quanpin::merge_lattice_candidates(candidates, {"zi"}, make_table_lattice_lookup(table), "zi");
        expect(candidates.size() == 1 && candidates.front().source == CandidateSource::Database,
               "Single-syllable merge is a no-op; exact SQLite already covers the key.");
    }

    {
        // generated_sentence_insert_position：跳过开头那串「键与输入完全相等」的
        // 词库候选，前缀候选和非词库来源都不算，遇到就停。全拼/双拼词典层的 Google
        // 整句用的是同一个位置，所以两条整句来源都落在词库短语之后。
        const quanpin::Segments ni_hao{"ni", "hao"};
        std::vector<WordItem> candidates;
        candidates.emplace_back("ni'hao", "你好", 30000, CandidateSource::Database, "ni'hao");
        candidates.emplace_back("ni'hao", "拟好", 9000, CandidateSource::UserDatabase, "ni'hao");
        candidates.emplace_back("ni", "你", 50000, CandidateSource::Database, "ni");
        expect(quanpin::generated_sentence_insert_position(candidates, ni_hao) == 2,
               "Expected the insert position to sit right after the two exact-key hits.");

        std::vector<WordItem> no_exact_hit;
        no_exact_hit.emplace_back("ni", "你", 50000, CandidateSource::Database, "ni");
        expect(quanpin::generated_sentence_insert_position(no_exact_hit, ni_hao) == 0,
               "Expected a prefix-only list to take the sentence at the head.");

        // 前缀区间扫描出来的行音节数、字数都和输入一样（gun'qi 捞出 gun'qiu 的
        // 滚球），但键不等于输入，不许挡在整句前面。截图里「滚其」排到末尾就是
        // 按音节数判断的后果。
        const quanpin::Segments gun_qi{"gun", "qi"};
        std::vector<WordItem> prefix_scan;
        prefix_scan.emplace_back("gyqi", "滚球", 30000, CandidateSource::Database, "gun'qiu");
        prefix_scan.emplace_back("gyqi", "滚起", 12000, CandidateSource::Database, "gun'qi");
        expect(quanpin::generated_sentence_insert_position(prefix_scan, gun_qi) == 0,
               "A prefix-range row with the same syllable count must not count as an exact hit.");

        std::vector<WordItem> exact_then_prefix;
        exact_then_prefix.emplace_back("gyqi", "滚起", 12000, CandidateSource::Database, "gun'qi");
        exact_then_prefix.emplace_back("gyqi", "滚球", 30000, CandidateSource::Database, "gun'qiu");
        expect(quanpin::generated_sentence_insert_position(exact_then_prefix, gun_qi) == 1,
               "A leading exact-key hit still outranks the sentence; the prefix row behind it does not.");
    }

    {
        // 整句只出各来源的首选，n-best 的其余部分是解码中间产物，不进候选区。
        // 没有重排器时就一条；重排器改了主意时，把它的首选和词格的首选各留一条。
        std::unordered_map<std::string, std::vector<LatticeLexeme>> table;
        table["ni"] = {{"ni", "你", 12000}, {"ni", "拟", 3000}, {"ni", "泥", 2000}};
        table["hao"] = {{"hao", "好", 12000}, {"hao", "号", 3000}, {"hao", "耗", 2000}};

        std::vector<WordItem> alone;
        quanpin::merge_lattice_candidates(alone, {"ni", "hao"}, make_table_lattice_lookup(table), "ni'hao");
        expect(alone.size() == 1, fmt::format("Expected a single lattice sentence, got {}", alone.size()));

        // 重排器把第二条顶到首位：两条首选都要留下，重排的在前。
        std::vector<WordItem> reranked;
        quanpin::merge_lattice_candidates(
            reranked, {"ni", "hao"}, make_table_lattice_lookup(table), "ni'hao", {},
            [](std::vector<quanpin::LatticePath> &paths) {
                if (paths.size() >= 2)
                    std::swap(paths[0], paths[1]);
                return true;
            },
            CandidateSource::NeuralDesktop);
        expect(reranked.size() == 2,
               fmt::format("Expected the reranker's pick and the lattice's, got {}", reranked.size()));
        if (reranked.size() == 2 && alone.size() == 1)
        {
            expect(reranked.front().word != alone.front().word && reranked[1].word == alone.front().word,
                   "Expected the reranked pick first and the lattice's own pick behind it.");
            // 来源要分得开：候选框据此标注是谁挑的。
            expect(reranked.front().source == CandidateSource::NeuralDesktop &&
                       reranked[1].source == CandidateSource::Generated,
                   "Expected the reranker's pick tagged with its own source and the lattice's with Generated.");
        }

        // 重排器同意词格的排序时不该凭空多出一行。行保持词格来源（Generated）：
        // include_lattice_best=true 时词格首选永远是 Generated，见 word_lattice.h 单重排器
        // 契约——被其他来源认可的三元首选只是提前，来源仍记在词格上。历史意图曾是
        // 「模型看过并认可了它，来源记在重排那边」，但 merge 的实现与头文件契约都是词格
        // 来源；以实现为准，若要恢复「认可即署名」需要先改 merge。
        std::vector<WordItem> agreed;
        quanpin::merge_lattice_candidates(
            agreed, {"ni", "hao"}, make_table_lattice_lookup(table), "ni'hao", {},
            [](std::vector<quanpin::LatticePath> &) { return true; }, CandidateSource::NeuralDesktop);
        expect(agreed.size() == 1,
               fmt::format("A reranker that agrees should add no extra row, got {}", agreed.size()));
        if (agreed.size() == 1 && alone.size() == 1)
        {
            expect(agreed.front().word == alone.front().word && agreed.front().source == CandidateSource::Generated,
                   "A trigram pick the reranker agreed with should keep the lattice's own source.");
        }

        // 弃权（模型没加载、后台还没算完）时重排器不参与共识，这一行同样只记在词格上。
        std::vector<WordItem> declined;
        quanpin::merge_lattice_candidates(
            declined, {"ni", "hao"}, make_table_lattice_lookup(table), "ni'hao", {},
            [](std::vector<quanpin::LatticePath> &) { return false; }, CandidateSource::NeuralDesktop);
        expect(declined.size() == 1 && declined.front().source == CandidateSource::Generated,
               "A reranker that declined must leave the row credited to the lattice.");
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
                      "CREATE TABLE tbl_2_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_4_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "INSERT INTO tbl_1_s VALUES('shang','s','上',100);"
                      "INSERT INTO tbl_2_s VALUES('shang''zhi','sz','上至',100);"
                      "INSERT INTO tbl_2_j VALUES('jian''du','jd','监督',100);"
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
    // fix (AC1-AC4). Since the generated correction space was added, the neighbour
    // bit also carries out-of-table substitutions, so a bit no longer maps onto
    // exactly one table and the 'must NOT correct' rows had to give up their empty
    // expectation for the input's widened reading. The bare bits used here
    // (neighbour alone, T|N) are unreachable in the product: engine.cpp always
    // pairs the deletion and insertion bits with the legacy switches, so only
    // 0xd/0xe/0xf ever occur. That does NOT make the generated space unreachable
    // -- 0xe/0xf still carry the neighbour bit and all three carry the insertion
    // bit -- it only means these particular one-bit rows cannot be triggered by a
    // user. The space itself is reachable and is covered by the session-level
    // cases below plus test_input_session.cpp.
    expect(quanpin::autocorrect_cut("sahng", none).empty(), "Both switches off must disable the correction cut.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("sahng", transposition_only)) == "shang",
           "Transposition-only must correct 'sahng'.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("sahng", neighbor_only)) == "sa'ang",
           "Neighbor-only reads 'sahng' as sa + ang through a generated substitution.");
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
    // A complete syllable plus a lone trailing VOWEL is not jianpin (no vowel is
    // a jianpin initial); it reads as a transposition typo, so the guard must
    // let it through to correction ("gau" = ga + u -> gua).
    expect(!quanpin::looks_like_syllable_with_jianpin_tail("gau"),
           "'gau' (ga + trailing vowel u) must stay correctable, not be read as jianpin.");
    expect(!quanpin::looks_like_syllable_with_jianpin_tail("hau"),
           "'hau' (ha + trailing vowel u) must stay correctable, not be read as jianpin.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("gau", transposition_only)) == "gua",
           "'gau' must correct to 'gua' via transposition once the guard lets it through.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("bqng", neighbor_only)) == "bang",
           "'bqng' -> bang must remain a valid neighbor correction.");
    // "zheg" is jianpin intent (zhe + g). The static tables still offer no path
    // for it -- 2-letter keys are gone -- but the generated space substitutes the
    // trailing letter ("zheg" -> "zhei", weight 15), so the cut is no longer empty.
    // Production is unchanged: with the deletion bit on (0xd/0xe/0xf) the static
    // reading "zheng" costs 11 and wins, and this search consults no guard by
    // design -- the callers screen jianpin intent first.
    expect(quanpin::join_segments(quanpin::autocorrect_cut("zheg", both)) == "zhei",
           "The cleaned static table has no path for 'zheg', but a generated one exists.");

    // Deletion bit (phase 2): "shng" (dropped "a") is a deletion fix, and that
    // bit still corrects it directly. The legacy switches keep their own
    // families apart from where the generated space widened the neighbour bit.
    const unsigned deletion_only = quanpin::kAutocorrectDeletion;
    const unsigned all = transposition_only | neighbor_only | deletion_only;
    expect(quanpin::join_segments(quanpin::autocorrect_cut("shng", deletion_only)) == "shang",
           "Deletion-only must correct 'shng'.");
    expect(quanpin::autocorrect_cut("shng", transposition_only).empty(),
           "Transposition-only must not correct the deletion case 'shng'.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("shng", neighbor_only)) == "sang",
           "Neighbor-only reads 'shng' as sang through a generated substitution.");
    expect(quanpin::autocorrect_cut("sahng", deletion_only).empty() == false &&
               quanpin::join_segments(quanpin::autocorrect_cut("sahng", deletion_only)) == "sa'hang",
           "Bits gate tables, not intents: deletion-only still explains 'sahng' via sa + hng -> hang.");
    expect(quanpin::autocorrect_cut("shabg", deletion_only).empty(),
           "Deletion-only must not correct the neighbor case 'shabg'.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("shngzhk", all)) == "shang'zhi",
           "A deletion edge followed by a neighbor edge must cut 'shngzhk'.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("shngzhk", both)) == "sang'zhi",
           "Without the deletion bit 'shngzhk' still cuts: generated sang + static zhk -> zhi.");

    // Insertion bit (fourth type): "shangg" (doubled g) and "sjhang" (j is a
    // QWERTY neighbor of h) are insertion fixes; only that bit may correct them.
    const unsigned insertion_only = quanpin::kAutocorrectInsertion;
    const unsigned all_four = all | insertion_only;
    expect(quanpin::join_segments(quanpin::autocorrect_cut("shangg", insertion_only)) == "shang",
           "Insertion-only must correct 'shangg'.");
    expect(quanpin::autocorrect_cut("shangg", transposition_only).empty(),
           "Transposition-only must not correct the insertion case 'shangg'.");
    expect(quanpin::autocorrect_cut("shangg", deletion_only).empty(),
           "Deletion-only must not correct the insertion case 'shangg'.");
    expect(quanpin::autocorrect_cut("shangg", neighbor_only).empty(),
           "Neighbor-only must not correct the insertion case 'shangg'.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("sjhang", insertion_only)) == "shang",
           "Insertion-only must correct the neighbor-key insertion 'sjhang'.");
    expect(quanpin::autocorrect_cut("sjhang", neighbor_only).empty(),
           "Neighbor-only must not correct the insertion case 'sjhang'.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("shanggzhi", all_four)) == "shang'zhi",
           "An insertion edge followed by a legal tail must cut 'shanggzhi'.");

    // Jianpin guard shapes sit INSIDE the insertion table (zher = zhe + r, and
    // r is a QWERTY neighbor of e): the BFS cut exists by design, the shape
    // guard must fire before it at the dictionary layer (end-to-end in the
    // display test below).
    expect(quanpin::looks_like_syllable_with_jianpin_tail("zher"),
           "'zher' (zhe + r) must be detected as jianpin intent.");
    expect(!quanpin::autocorrect_cut_detail("zher", insertion_only).empty(),
           "The insertion key 'zher' is in the BFS search space by design; the guard is the dictionary layer's job.");

    // k-best (phase 2): ambiguous keys keep parallel readings for query-time
    // disambiguation; k = 1 stays consistent with the single-cut projection.
    const auto cut_reading = [](const quanpin::AutocorrectCut &cut) {
        quanpin::Segments syllables;
        for (const auto &segment : cut.segments)
        {
            syllables.push_back(segment.syllable);
        }
        return quanpin::join_segments(syllables);
    };
    const auto ahan_cuts = quanpin::autocorrect_cut_kbest("ahan", both);
    expect(ahan_cuts.size() >= 2, "'ahan' must keep both the shan and zhan readings.");
    expect(cut_reading(ahan_cuts[0]) == "shan" && cut_reading(ahan_cuts[1]) == "zhan",
           "The first two 'ahan' cuts must be shan then zhan (edge count, weight, table order).");
    const auto shng_cuts = quanpin::autocorrect_cut_kbest("shng", all);
    expect(shng_cuts.size() >= 2 && cut_reading(shng_cuts[0]) == "shang",
           "'shng' must cut to shang first with sheng as a kept alternative.");
    bool has_sheng = false;
    for (const auto &cut : shng_cuts)
    {
        has_sheng = has_sheng || cut_reading(cut) == "sheng";
    }
    expect(has_sheng, "'shng' must keep the sheng reading alongside shang.");
    const auto sahng_cuts = quanpin::autocorrect_cut_kbest("sahng", all);
    expect(sahng_cuts.size() >= 2 && cut_reading(sahng_cuts[0]) == "shang" && cut_reading(sahng_cuts[1]) == "sa'hang",
           "Within one edge the transposition weight (10) must outrank the split deletion (11).");
    // Cost tiers must be reported on the cut so the query layer keeps frequency
    // disambiguation inside one tier: gau -> gua (transposition, weight 10) is
    // strictly cheaper than gau -> gai (neighbor, weight 13), so gua leads and
    // the two land in different tiers regardless of dictionary frequency.
    const auto gau_cuts = quanpin::autocorrect_cut_kbest("gau", both);
    expect(gau_cuts.size() >= 2 && cut_reading(gau_cuts[0]) == "gua",
           "'gau' must rank the transposition reading gua first.");
    const auto gau_gai = std::find_if(gau_cuts.begin(), gau_cuts.end(),
                                      [&](const quanpin::AutocorrectCut &cut) { return cut_reading(cut) == "gai"; });
    expect(gau_gai != gau_cuts.end(), "'gau' must keep the neighbor reading gai as an alternative.");
    expect(gau_gai != gau_cuts.end() && !gau_cuts.front().same_cost_as(*gau_gai) &&
               gau_cuts.front().weight < gau_gai->weight,
           "gua (weight 10) and gai (weight 13) must be reported as different cost tiers.");
    // Ambiguous insertion keys keep parallel readings (query-time
    // disambiguation): baio -> {biao via transposition, bai, bao via
    // insertion}. Same edge count, so weight orders them: 10 < 12.
    const auto baio_cuts = quanpin::autocorrect_cut_kbest("baio", all_four);
    expect(baio_cuts.size() >= 3 && cut_reading(baio_cuts[0]) == "biao" && cut_reading(baio_cuts[1]) == "bai" &&
               cut_reading(baio_cuts[2]) == "bao",
           "'baio' must rank the transposition reading (10) before the insertion readings (12) within one edge.");
    const std::pair<const char *, unsigned> projection_inputs[] = {
        {"sahng", both},      {"shabg", both}, {"sahnguai", both},
        {"ahan", both},       {"zheg", both},  {"keneng", both},
        {"shng", all},        {"zhngu", all},  {"shangg", insertion_only},
        {"sjhang", all_four},
    };
    for (const auto &[input, types] : projection_inputs)
    {
        const auto single = quanpin::autocorrect_cut(input, types);
        const auto top1 = quanpin::autocorrect_cut_kbest(input, types, 1);
        expect(top1.size() == (single.empty() ? 0u : 1u) &&
                   (single.empty() || cut_reading(top1.front()) == quanpin::join_segments(single)),
               std::string("k=1 must agree with autocorrect_cut for '") + input + "'.");
    }

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

        // AC2 gating is verified through the correction marker rather than through
        // 上's presence. Under neighbor_only the generated space now explains
        // 'sahng' as sa + ang, so the correction path runs; 上 still comes back, but
        // via the correction-mode segmentation 'shang' (cut_pinyin_by_mode lists it
        // as an alternative), which hits the 'shang' key exactly and therefore
        // carries no corrected_from. The transposition reading is not produced, so
        // nothing is marked. Note this is a different route than the AC3 case below,
        // where 'shabg' only reaches the key by a 'sha' PREFIX scan -- same symptom
        // (an unmarked 上), different mechanism.
        const auto neighbor_denied = dictionary.query("sahng", "sa'h'n'g", neighbor_only);
        expect(std::none_of(neighbor_denied.begin(), neighbor_denied.end(),
                            [](const WordItem &item) { return item.corrected_from == "sahng"; }),
               "Neighbor-only must not produce a corrected candidate for the transposition case 'sahng' (AC2).");

        const auto neighbor_corrected = dictionary.query("shabg", "sha'b'g", neighbor_only);
        expect(!neighbor_corrected.empty() && neighbor_corrected.front().word == "上",
               "Neighbor-only must correct 'shabg' at the dictionary layer (AC3).");

        // 门控语义按纠错标记验证：transposition-only 不产生针对 'shabg' 的纠错
        // 候选。不能断言"上"完全缺席——序列前缀查询会以 sha 前缀命中 'shang'
        // 键，那是与纠错无关的既有行为。
        const auto transposition_denied = dictionary.query("shabg", "sha'b'g", transposition_only);
        expect(std::none_of(transposition_denied.begin(), transposition_denied.end(),
                            [](const WordItem &item) { return item.corrected_from == "shabg"; }),
               "Transposition-only must not correct the neighbor case 'shabg' (AC3).");

        const auto multi = dictionary.query("sahngzhi", "sa'h'n'g'zhi", both);
        expect(!multi.empty() && multi.front().word == "上至",
               "Cross-syllable correction must survive the mask wiring.");

        // 空基础切分（丢声母：jian -> ian）也要进纠错：correction 模式对该类错拼
        // 切不出任何段，旧门 !segments.empty() 会整体跳过纠错。BFS 从原始字母
        // 纠正，备选切分 jian'du 经查询期词频合并成为首位。
        const auto dropped_initial = dictionary.query("iandu", "", all);
        expect(!dropped_initial.empty() && dropped_initial.front().word == "监督" &&
                   dropped_initial.front().corrected_from == "iandu",
               "An empty base segmentation (dropped initial) must still reach the correction path.");
        const auto dropped_initial_off = dictionary.query("iandu", "", none);
        expect(std::none_of(dropped_initial_off.begin(), dropped_initial_off.end(),
                            [](const WordItem &item) { return item.word == "监督"; }),
               "With autocorrection off the dropped-initial typo must not resolve through a corrected key.");

        // The guard fires before the BFS, so a jianpin-shaped input must resolve
        // through its raw segmentation instead of a corrected key such as 'zu'ge'.
        (void)dictionary.query("zheg", "", both);
        expect(dictionary.get_pinyin_segmentation() != "zu'ge",
               "A jianpin-shaped input must not resolve through a corrected key.");

        // Correction composes with a trailing jianpin tail: the k-best search
        // fails on the whole input (the tail is an incomplete syllable), so the
        // head is corrected and the tail carried through to the jianpin query.
        (void)dictionary.query("hauzh", "", both);
        expect(dictionary.get_pinyin_segmentation() == "hua'zh",
               "A correctable head plus a jianpin tail must compose: hauzh -> hua'zh.");
        (void)dictionary.query("hauz", "", both);
        expect(dictionary.get_pinyin_segmentation() == "hua'z",
               "The jianpin tail may be a single initial: hauz -> hua'z.");
        // A correctly spelled head with the same jianpin tail is untouched.
        (void)dictionary.query("huazh", "", both);
        expect(dictionary.get_pinyin_segmentation() == "hua'zh",
               "A correctly spelled jianpin input keeps its plain segmentation.");
        // With autocorrection off, the mistyped head must not compose either.
        (void)dictionary.query("hauzh", "", none);
        expect(dictionary.get_pinyin_segmentation() != "hua'zh",
               "With autocorrection off, hauzh must not resolve through a corrected key.");
        // Neighbor corrections are excluded from the composition head: without
        // that guard a deletion-shaped input becomes noise via a low-confidence
        // neighbor head plus a speculative jianpin tail (shng -> sun'g).
        (void)dictionary.query("shng", "sh'n'g", both);
        expect(dictionary.get_pinyin_segmentation() != "sun'g",
               "A neighbor head must not compose with a jianpin tail (shng must not become sun'g).");
    }
    std::error_code cleanup_ec;
    fs::remove(db_path, cleanup_ec);

    // Generated-table invariants shared by all three type tables (AC6).
    // Ambiguity is kept: one wrong key may map to several syllables, so
    // uniqueness is asserted per (wrong, correct) pair instead of per key.
    const auto &legal = quanpin::intact_pinyin_set();
    std::unordered_set<std::string> seen_pairs;
    size_t total_entries = 0;
    const std::pair<const quanpin::autocorrect::Entry *, std::size_t> tables[] = {
        {quanpin::autocorrect::kTranspositionEntries, quanpin::autocorrect::kTranspositionCount},
        {quanpin::autocorrect::kNeighborEntries, quanpin::autocorrect::kNeighborCount},
        {quanpin::autocorrect::kDeletionEntries, quanpin::autocorrect::kDeletionCount},
        {quanpin::autocorrect::kInsertionEntries, quanpin::autocorrect::kInsertionCount},
    };
    for (const auto &[entries, count] : tables)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::string wrong(entries[i].wrong);
            // correct 是生成音节表的 16-bit 下标，断言前先解引用。
            const std::string correct(quanpin::autocorrect::kCorrectSyllables[entries[i].correct]);
            expect(wrong.size() >= 3, "2-letter keys belong to the jianpin space and must not be generated.");
            expect(legal.find(wrong) == legal.end(), "A correction key must never shadow a legal syllable.");
            expect(legal.find(correct) != legal.end(), "A correction target must be a legal syllable.");
            expect(seen_pairs.insert(wrong + '>' + correct).second,
                   "(wrong, correct) pairs must be unique across all tables.");
            ++total_entries;
        }
    }
    expect(total_entries >= 4700, "The generated tables unexpectedly shrank.");
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
                      "CREATE TABLE tbl_2_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_2_k(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_4_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "INSERT INTO tbl_1_s VALUES('shang','s','上',100);"
                      "INSERT INTO tbl_1_n VALUES('nv','n','女',100);"
                      "INSERT INTO tbl_2_s VALUES('shang''hao','sh','上好',100);"
                      "INSERT INTO tbl_2_j VALUES('jian''du','jd','监督',100);"
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
    expect(sahnghao_cut.segments[1].syllable == "hao" && sahnghao_cut.segments[1].raw_text == "hao" &&
               sahnghao_cut.segments[1].start == 5 && !sahnghao_cut.segments[1].corrected,
           "The untouched tail of 'sahnghao' must keep its raw span.");

    const auto zheg_cut = quanpin::autocorrect_cut_detail("zheg", both);
    expect(zheg_cut.segments.size() == 1 && zheg_cut.segments[0].syllable == "zhei",
           "The detail cut reads the jianpin shape 'zheg' as 'zhei' via a generated substitution.");
    // "keneng" is ken + eng, both legal syllables. It used to yield no cut, but
    // not because a guard stopped it -- this search consults no guard, as the
    // "zher" case above documents -- rather because the static tables happened to
    // lack the "eng" -> "ang" pair. The generated space supplies it, so the input
    // now cuts. Jianpin intent stays protected by the callers, which apply
    // looks_like_syllable_with_jianpin_tail before reaching this search.
    expect(!quanpin::autocorrect_cut_detail("keneng", both).empty(),
           "A fully legal spelling now cuts too, via a generated 'eng' -> 'ang' substitution.");
    expect(quanpin::autocorrect_cut_detail("sahng", none).empty(),
           "Both switches off must disable the range-carrying cut too.");
    expect(quanpin::autocorrect_cut_detail("xi'an", both).empty(),
           "Manual delimiters must disable the range-carrying cut.");
    expect(quanpin::join_segments(quanpin::autocorrect_cut("sahng", both)) == "shang",
           "The Segments wrapper must stay a projection of the detail cut.");

    // 不等长边（漏字）：4 字母 raw "zhng" 映到 5 字母音节 zhang，第二段从 raw
    // 偏移 4 接 "gu"；位置推进跟随 raw_length 而非音节长度。
    const unsigned all = transposition_only | neighbor_only | quanpin::kAutocorrectDeletion;
    const auto zhnggu_cut = quanpin::autocorrect_cut_detail("zhnggu", all);
    expect(zhnggu_cut.segments.size() == 2, "'zhnggu' must cut into two segments via a deletion edge.");
    expect(zhnggu_cut.segments[0].syllable == "zhang" && zhnggu_cut.segments[0].raw_text == "zhng" &&
               zhnggu_cut.segments[0].start == 0 && zhnggu_cut.segments[0].corrected,
           "The deletion segment must map 4 raw letters onto the 5-letter syllable.");
    expect(zhnggu_cut.segments[1].syllable == "gu" && zhnggu_cut.segments[1].raw_text == "gu" &&
               zhnggu_cut.segments[1].start == 4 && !zhnggu_cut.segments[1].corrected,
           "The legal tail of 'zhnggu' must start at the raw offset the deletion edge left.");

    // "zhngu"（3+2）：更短的不等长拆分，第二段起点在 3。
    const auto zhngu_cut = quanpin::autocorrect_cut_detail("zhngu", all);
    expect(zhngu_cut.segments.size() == 2 && zhngu_cut.segments[0].raw_text == "zhn" &&
               zhngu_cut.segments[0].corrected && zhngu_cut.segments[1].raw_text == "gu" &&
               zhngu_cut.segments[1].start == 3 && !zhngu_cut.segments[1].corrected,
           "'zhngu' must split 3+2 with the legal tail starting at raw offset 3.");

    // 插入（第四类）：raw 比音节长 1（"shangg" -> shang）；位置推进同样跟随
    // raw_length 而非音节长度。
    const unsigned all_four = all | quanpin::kAutocorrectInsertion;
    const auto shangg_cut = quanpin::autocorrect_cut_detail("shangg", all_four);
    expect(shangg_cut.segments.size() == 1, "'shangg' must cut into a single corrected segment.");
    expect(shangg_cut.segments[0].syllable == "shang" && shangg_cut.segments[0].raw_text == "shangg" &&
               shangg_cut.segments[0].start == 0 && shangg_cut.segments[0].corrected,
           "The insertion segment must map 6 raw letters onto the 5-letter syllable.");
    const auto shangghao_cut = quanpin::autocorrect_cut_detail("shangghao", all_four);
    expect(shangghao_cut.segments.size() == 2, "'shangghao' must cut into two segments.");
    // 同为 1 条纠错边时权重定序：漏字解释（shang + ghao->gao，11）胜过插入
    // 解释（shangg->shang + hao，12），插入不劫持更便宜的解释。
    expect(shangghao_cut.segments[0].syllable == "shang" && shangghao_cut.segments[0].raw_text == "shang" &&
               shangghao_cut.segments[0].start == 0 && !shangghao_cut.segments[0].corrected,
           "The cheaper deletion reading (weight 11) must win over the insertion reading (12).");
    expect(shangghao_cut.segments[1].syllable == "gao" && shangghao_cut.segments[1].raw_text == "ghao" &&
               shangghao_cut.segments[1].start == 5 && shangghao_cut.segments[1].corrected,
           "The 'ghao' tail must be corrected through the deletion key.");
    // 纯插入解释的 raw 区间：无更便宜的捷径时插入边胜出，第二段起点跟随
    // raw_length（6 而非 5）。
    const auto shanggni_cut = quanpin::autocorrect_cut_detail("shanggni", all_four);
    expect(shanggni_cut.segments.size() == 2, "'shanggni' must cut into two segments via an insertion edge.");
    expect(shanggni_cut.segments[0].syllable == "shang" && shanggni_cut.segments[0].raw_text == "shangg" &&
               shanggni_cut.segments[0].start == 0 && shanggni_cut.segments[0].corrected,
           "The insertion segment must keep the raw letters of the corrected part.");
    expect(shanggni_cut.segments[1].syllable == "ni" && shanggni_cut.segments[1].raw_text == "ni" &&
               shanggni_cut.segments[1].start == 6 && !shanggni_cut.segments[1].corrected,
           "The legal tail of 'shanggni' must start at the raw offset the insertion edge left.");

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
        // 整句候选（词格 / Google 解码器）走的是同一条纠错读音，字母与该读音相同，
        // 因此同样带上 corrected_from。整句门槛降到 2 个音节之后 shang'hao 就够格，
        // 主切分和候选读音 shan'gua'o 各会多出整句，所以不再是「恰好一条」。这里断
        // 的仍是原来的意图：被标记的必须覆盖整串输入，前缀候选一律不标。
        const auto letter_count = [](const std::string &pinyin) {
            return static_cast<size_t>(std::count_if(pinyin.begin(), pinyin.end(), [](char c) { return c != '\''; }));
        };
        expect(count_marked(full) >= 1, "The full-length corrected candidate must be marked for 'sahnghao'.");
        const size_t typed_letters = letter_count("sahnghao");
        expect(std::all_of(full.begin(), full.end(),
                           [&](const WordItem &item) {
                               return item.corrected_from.empty() || letter_count(item.pinyin) == typed_letters;
                           }),
               "Only candidates covering the whole typed input may be marked for 'sahnghao'.");

        const auto keneng = dictionary.query("keneng", "ke'neng", both);
        expect(count_marked(keneng) == 0, "A legal spelling must produce no marks.");
        expect(count_marked(dictionary.query("wj", "w'j", both)) == 0, "Jianpin input must produce no marks (AC5).");
        const auto nv = dictionary.query("nv", "nv", both);
        expect(!nv.empty() && nv.front().word == "女" && nv.front().corrected_from.empty(),
               "The u-umlaut 'v' spelling must stay unmarked.");

        // 漏字（阶段 2）：deletion 位开时 "shng" 经纠错键命中目标词并标记；
        // 关掉则回到现状（无标记，乱码后备尾）。打标按字母比较，天然兼容不等长边。
        const auto deletion = dictionary.query("shng", "sh'n'g", all);
        expect(!deletion.empty() && deletion.front().word == "上" && deletion.front().corrected_from == "shng",
               "A deletion typo must resolve through the corrected key and carry corrected_from.");
        const auto deletion_off = dictionary.query("shng", "sh'n'g", both);
        expect(count_marked(deletion_off) == 0, "Without the deletion bit 'shng' must stay uncorrected and unmarked.");

        // 插入（阶段 4）：insertion 位开时 "sshang"（双打 s）经纠错键命中目标词
        // 并带 corrected_from（AC5）；关闭 insertion 位则无纠错标记。注：结尾单
        // 插入（如 shangg = shang + g）属简拼尾形状，被词典门按设计拦截，
        // 与 zher 同理 —— insertion 的可达面在中间/开头插入与多音节输入。
        const auto insertion = dictionary.query("sshang", "", all_four);
        expect(!insertion.empty() && insertion.front().word == "上" && insertion.front().corrected_from == "sshang",
               "An insertion typo must resolve through the corrected key and carry corrected_from.");
        const auto insertion_off = dictionary.query("sshang", "", all);
        expect(count_marked(insertion_off) == 0,
               "Without the insertion bit 'sshang' must stay uncorrected and unmarked.");
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
        // 空基础切分的丢声母错拼（jian -> ian）：会话层同样要走到纠错候选，
        // 预编辑按不等长切分的 raw 区间重绘分隔（ian'du）。
        metasequoia::InputSession session(SchemeType::Quanpin, all, true, true, true, paths);
        type_display_session(session, "iandu");
        expect(session.get_pinyin_segmentation_with_cases() == "ian'du",
               "A dropped initial must redraw separators from the raw spans (ian'du).");
        expect(!session.candidates().empty() && session.candidates().front().word == "监督" &&
                   session.candidates().front().corrected_from == "iandu",
               "An InputSession-level dropped initial must reach the corrected candidate.");
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
    {
        // 阶段 3：deletion 位随既有开关联动（design D2）后，显式三位 mask 下
        // zheg 的 preedit 必须仍走简拼守卫，不被漏字表重排为 zheng。
        const unsigned all_three = transposition_only | neighbor_only | quanpin::kAutocorrectDeletion;
        metasequoia::InputSession session(SchemeType::Quanpin, all_three, true, true, true, paths);
        type_display_session(session, "zheg");
        expect(session.get_pinyin_segmentation_with_cases() == "zhe'g",
               "The jianpin guard must hold with the deletion bit enabled (phase 3).");
        expect(count_marked(session.candidates()) == 0,
               "The jianpin shape must stay unmarked with the deletion bit enabled.");
    }
    {
        // 阶段 3：漏字输入的 display —— 切分段携带不等长 raw span，rebuild 后
        // preedit 显示原始字母（无分隔，单段纠错）。
        metasequoia::InputSession session(SchemeType::Quanpin, both, true, true, true, paths);
        type_display_session(session, "shng");
        expect(session.get_pinyin_segmentation_with_cases() == "shng",
               "A deletion-corrected input must show its typed letters in the preedit.");
        expect(!session.candidates().empty() && session.candidates().front().word == "上" &&
                   session.candidates().front().corrected_from == "shng",
               "The deletion reading must reach the candidates end to end with the linked bit.");
    }
    {
        // 阶段 3（design D2）：仅开 transposition 时请求布尔映射也带上 deletion
        // 位 —— "shng" 必须可纠（端到端行为断言：mask 是会话内部状态，以纠错
        // 生效为准）。
        metasequoia::InputSession session(SchemeType::Quanpin, transposition_only, true, true, true, paths);
        type_display_session(session, "shng");
        expect(std::any_of(session.candidates().begin(), session.candidates().end(),
                           [](const WordItem &item) { return item.word == "上"; }),
               "Transposition-only must still enable the deletion reading of 'shng' (D2 linkage).");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, none, true, true, true, paths);
        type_display_session(session, "shng");
        // 门控按纠错标记验证：前缀查询可能以 'sh' 命中「上」，那与纠错无关；
        // 开关全关时不得存在任何带 corrected_from 的候选。
        expect(std::none_of(session.candidates().begin(), session.candidates().end(),
                            [](const WordItem &item) { return !item.corrected_from.empty(); }),
               "Both switches off must keep the deletion reading disabled.");
    }
    {
        // 阶段 4：插入输入的 display —— raw span 比音节长 1，preedit 显示原始
        // 字母（无分隔，单段纠错）。用开头双打 "sshang"：结尾单插入属简拼尾
        // 形状，被词典门拦截（见 zher 用例）。
        const unsigned all_four =
            transposition_only | neighbor_only | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
        metasequoia::InputSession session(SchemeType::Quanpin, all_four, true, true, true, paths);
        type_display_session(session, "sshang");
        expect(session.get_pinyin_segmentation_with_cases() == "sshang",
               "An insertion-corrected input must show its typed letters in the preedit.");
        expect(!session.candidates().empty() && session.candidates().front().word == "上" &&
                   session.candidates().front().corrected_from == "sshang",
               "The insertion reading must reach the candidates end to end.");
    }
    {
        // 阶段 4（design D4）：仅开 transposition 时请求布尔映射也带上 insertion
        // 位 —— "sshang" 必须可纠（端到端行为断言）。
        metasequoia::InputSession session(SchemeType::Quanpin, transposition_only, true, true, true, paths);
        type_display_session(session, "sshang");
        expect(std::any_of(session.candidates().begin(), session.candidates().end(),
                           [](const WordItem &item) { return item.word == "上"; }),
               "Transposition-only must still enable the insertion reading of 'sshang' (D4 linkage).");
    }
    {
        // 阶段 4：'zher' 是 insertion 键（r 是 e 的邻键）但属简拼守卫形状，
        // 词典门在 BFS 之前拦截：preedit 保持 zhe'r，无任何纠错标记（AC2）。
        const unsigned all_four =
            transposition_only | neighbor_only | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
        metasequoia::InputSession session(SchemeType::Quanpin, all_four, true, true, true, paths);
        type_display_session(session, "zher");
        // 守卫拦下纠错后，preedit 保持方案层自身的贪心切分（zh + er），
        // 不重绘为 insertion 键的字母区间。
        expect(session.get_pinyin_segmentation_with_cases() == "zh'er",
               "The jianpin guard must hold for 'zher' with the insertion bit enabled.");
        expect(count_marked(session.candidates()) == 0,
               "The jianpin shape 'zher' must stay unmarked (insertion key inside the guard). ");
    }
    {
        metasequoia::InputSession session(SchemeType::Quanpin, none, true, true, true, paths);
        type_display_session(session, "sshang");
        expect(std::none_of(session.candidates().begin(), session.candidates().end(),
                            [](const WordItem &item) { return !item.corrected_from.empty(); }),
               "Both switches off must keep the insertion reading disabled.");
    }

    fs::remove_all(session_dir, cleanup_ec);
    fs::remove(db_path, cleanup_ec);
}

// 万象语法模型的会话链路冒烟：经 QuanpinDictionary::set_sentence_association 下发
// 模型路径，验证加性搭配项能改写整句首选——bu'zhe'ji 在三元模型下出「不这几」，
// 加上搭配后词格首选应为「不着急」（离线评测的典型翻盘案例）。模型路径由
// METASEQUOIA_IME_TEST_COLLOCATION_MODEL 环境变量给出，未设置或载入失败时优雅跳过：
// .gram 是按需下载的可选资产，测试数据目录默认没有。
void test_quanpin_collocation_session()
{
    fmt::println("==== Quanpin Collocation Session ====");
    const char *model_path = std::getenv("METASEQUOIA_IME_TEST_COLLOCATION_MODEL");
    if (model_path == nullptr)
    {
        fmt::println("Skipped: METASEQUOIA_IME_TEST_COLLOCATION_MODEL is not set.");
        return;
    }

    QuanpinDictionary dictionary;
    SentenceAssociationOptions association;
    association.word_lattice = true;
    association.collocation_model = model_path;
    association.collocation_weight = 0.25;
    dictionary.set_sentence_association(association);

    const auto result = dictionary.query("buzheji");
    const auto trigram = std::find_if(result.begin(), result.end(),
                                      [](const WordItem &item) { return item.source == CandidateSource::Generated; });
    if (trigram == result.end())
    {
        fmt::println("Skipped: no lattice sentence for 'buzheji' (dictionary or sc.lm incomplete?).");
        return;
    }
    expect(
        trigram->word == "不着急",
        fmt::format("Expected the collocation-enabled lattice to pick 不着急 for buzheji, got '{}'.", trigram->word));
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
        test_quanpin_lattice_precedes_google_fallback();
        test_quanpin_lattice_rejects_rare_readings();
        test_quanpin_collocation_session();
        test_quanpin_lattice_covers_every_syllable();
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
    catch (...)
    {
        fmt::println(stderr, "An exception escaped the test body.");
        return 1;
    }
}
