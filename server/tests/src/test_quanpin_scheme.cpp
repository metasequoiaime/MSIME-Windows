#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "engine/common/helpcode_utils.h"
#include "engine/quanpin/autocorrect_table.h"
#include "engine/quanpin/quanpin_dictionary.h"
#include "engine/quanpin/quanpin_query.h"
#include "engine/quanpin/quanpin_utils.h"
#include "engine/schemes/quanpin_scheme.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
void InputKey(QuanpinScheme &scheme, UINT vk, WCHAR wch, UINT modifiers_down = 0)
{
    scheme.handle_key(vk, modifiers_down, wch);
}

std::filesystem::path CreatePinyinCacheDatabase()
{
    const auto path = std::filesystem::temp_directory_path() / "msime-pinyin-cache-refresh-test.db";
    std::filesystem::remove(path);
    sqlite3 *db = nullptr;
    if (sqlite3_open(test::Utf8(path).c_str(), &db) != SQLITE_OK)
    {
        throw std::runtime_error("Failed to create temporary pinyin database.");
    }
    const char *sql =
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "CREATE TABLE tbl_1_a(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "CREATE TABLE tbl_1_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "CREATE TABLE tbl_2_a(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "CREATE TABLE tbl_2_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "CREATE TABLE tbl_3_a(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "CREATE TABLE tbl_3_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "CREATE TABLE tbl_4_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "CREATE TABLE tbl_5_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "CREATE TABLE tbl_6_x(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
        "INSERT INTO tbl_3_a VALUES('ao''shi''ke','ask','奥湿克',1);"
        "INSERT INTO tbl_1_x VALUES('xian','x','__primary_xian_1__',1000);"
        "INSERT INTO tbl_1_x VALUES('xian','x','__primary_xian_2__',900);"
        "INSERT INTO tbl_1_x VALUES('xian','x','__primary_xian_3__',800);"
        "INSERT INTO tbl_2_x VALUES('xi''an','xa','__alternative_xi_an__',1);"
        "INSERT INTO tbl_4_x VALUES('xi''an''xian''xian','xaxx','__three_syllable_alternative__',100);"
        "INSERT INTO tbl_5_x VALUES('xi''an''xian''xian''xian','xaxxx','__four_syllable_alternative__',100);"
        "INSERT INTO tbl_6_x VALUES('xi''an''xian''xian''xian''xian','xaxxxx','__five_syllable_alternative__',100);";
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (result != SQLITE_OK)
    {
        std::filesystem::remove(path);
        throw std::runtime_error("Failed to initialize temporary pinyin database.");
    }
    return path;
}
} // namespace

TEST_CASE(QuanpinSchemeSpaceDoesNotResetComposition)
{
    QuanpinScheme scheme;
    InputKey(scheme, 'N', L'n');
    InputKey(scheme, 'I', L'i');
    InputKey(scheme, VK_SPACE, L' ');

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input, std::string("ni"));
    REQUIRE_EQ(request.normalized_input, std::string("ni"));
}

TEST_CASE(QuanpinSchemeApostropheIsPreservedInRawInput)
{
    QuanpinScheme scheme;
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, VK_OEM_7, L'\'');
    InputKey(scheme, 'I', L'i');

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input, std::string("x'i"));
    REQUIRE_EQ(request.normalized_input, std::string("xi"));
}

TEST_CASE(QuanpinSchemeTrailingApostropheIsPreservedInPreeditSegmentation)
{
    QuanpinScheme scheme;
    InputKey(scheme, 'F', L'f');
    InputKey(scheme, 'A', L'a');
    InputKey(scheme, 'N', L'n');
    InputKey(scheme, 'G', L'g');
    InputKey(scheme, VK_OEM_7, L'\'');
    InputKey(scheme, VK_OEM_7, L'\'');
    InputKey(scheme, VK_OEM_7, L'\'');

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input, std::string("fang'"));
    REQUIRE_EQ(request.normalized_segmentation, std::string("fang"));
    REQUIRE_EQ(request.raw_segmentation, std::string("fang'"));
    REQUIRE_EQ(request.key_strokes.size(), static_cast<size_t>(5));
}

TEST_CASE(HelpcodeSchemaSelectionLoadsAllSupportedSchemas)
{
    const std::vector<std::pair<std::string, std::string>> schemas{
        {"lantian", "(KK)"}, {"ziranma", "(KA)"}, {"shouyou2_0", "(KV)"}, {"shouyouplus", "(KE)"}, {"xiaohe", "(KK)"},
    };

    for (const auto &[schema, expected] : schemas)
    {
        REQUIRE(HelpcodeUtils::is_supported_helpcode_schema(schema));
        REQUIRE(HelpcodeUtils::select_helpcode_schema(schema));
        REQUIRE_EQ(HelpcodeUtils::compute_helpcodes("啊", true), expected);
    }

    REQUIRE(!HelpcodeUtils::is_supported_helpcode_schema("unknown"));
    REQUIRE(!HelpcodeUtils::select_helpcode_schema("unknown"));
    REQUIRE(HelpcodeUtils::select_helpcode_schema("lantian"));
}

TEST_CASE(QuanpinSchemeResegmentsEachManualApostrophePart)
{
    QuanpinScheme scheme;
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, 'S', L's');
    InputKey(scheme, VK_OEM_7, L'\'');
    InputKey(scheme, 'H', L'h');

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.normalized_segmentation, std::string("x's'h"));
    REQUIRE_EQ(request.raw_segmentation, std::string("x's'h"));
}

TEST_CASE(QuanpinCandidateCacheKeepsManualSegmentationBoundariesDistinct)
{
    QuanpinDictionary dictionary;
    const std::string automatic_only_candidate = "__automatic_fan_gan__";
    REQUIRE_EQ(
        dictionary.insert_word_to_series_cache("fangan", automatic_only_candidate, CandidateSource::CloudSuggestion),
        QuanpinDictionary::OK);

    const auto automatic_candidates = dictionary.query("fangan", "fan'gan");
    REQUIRE(std::any_of(automatic_candidates.begin(), automatic_candidates.end(),
                        [&](const WordItem &item) { return item.word == automatic_only_candidate; }));

    const auto manual_candidates = dictionary.query("fang'an", "fang'an");
    REQUIRE(std::none_of(manual_candidates.begin(), manual_candidates.end(),
                         [&](const WordItem &item) { return item.word == automatic_only_candidate; }));

    const auto same_segmentation_manual_candidates = dictionary.query("fan'gan", "fan'gan");
    REQUIRE(std::none_of(same_segmentation_manual_candidates.begin(), same_segmentation_manual_candidates.end(),
                         [&](const WordItem &item) { return item.word == automatic_only_candidate; }));
}

TEST_CASE(QuanpinSyllableGraphKeepsEveryCompleteSegmentation)
{
    REQUIRE(quanpin::has_only_complete_pinyin_segments(quanpin::Segments{"xi", "an"}));
    REQUIRE(!quanpin::has_only_complete_pinyin_segments(quanpin::Segments{"x", "ian"}));

    const auto graph = quanpin::build_syllable_graph("xian");
    const auto segmentations = quanpin::enumerate_complete_segmentations(graph);
    REQUIRE_EQ(segmentations.size(), static_cast<size_t>(2));
    REQUIRE(std::find(segmentations.begin(), segmentations.end(), quanpin::Segments{"xian"}) != segmentations.end());
    REQUIRE(std::find(segmentations.begin(), segmentations.end(), quanpin::Segments{"xi", "an"}) !=
            segmentations.end());
}

TEST_CASE(QuanpinDictionaryUsesSyllableGraphAlternativeSegmentations)
{
    QuanpinDictionary dictionary;

    const auto fangan_candidates = dictionary.query("fangan", "fan'gan");
    const auto fangan_solution = std::find_if(fangan_candidates.begin(), fangan_candidates.end(),
                                              [](const WordItem &item) { return item.word == "方案"; });
    REQUIRE(fangan_solution != fangan_candidates.end());
    REQUIRE_EQ(fangan_solution->canonical_pinyin, std::string("fang'an"));
    REQUIRE_EQ(fangan_candidates.front().word, std::string("方案"));

    const auto qinai_candidates = dictionary.query("qinai", "qi'nai");
    const auto qinai_solution = std::find_if(qinai_candidates.begin(), qinai_candidates.end(),
                                             [](const WordItem &item) { return item.word == "亲爱"; });
    REQUIRE(qinai_solution != qinai_candidates.end());
    REQUIRE_EQ(qinai_solution->canonical_pinyin, std::string("qin'ai"));
    REQUIRE_EQ(qinai_candidates.front().word, std::string("亲爱"));

    const auto xian_candidates = dictionary.query("xian", "xian");
    const auto xian_solution = std::find_if(xian_candidates.begin(), xian_candidates.end(),
                                            [](const WordItem &item) { return item.word == "西安"; });
    REQUIRE(xian_solution != xian_candidates.end());
    REQUIRE_EQ(xian_solution->canonical_pinyin, std::string("xi'an"));
    REQUIRE(static_cast<size_t>(std::distance(xian_candidates.begin(), xian_solution)) <= static_cast<size_t>(1));
}

TEST_CASE(QuanpinDictionaryRequiresCompletePrimarySegmentsBeforeTryingAlternatives)
{
    QuanpinDictionary dictionary;
    const auto candidates = dictionary.query("xian", "x'ian");
    REQUIRE(
        std::none_of(candidates.begin(), candidates.end(), [](const WordItem &item) { return item.word == "西安"; }));
}

TEST_CASE(QuanpinDictionaryKeepsBestAlternativeSegmentationNearTheFront)
{
    const auto db_path = CreatePinyinCacheDatabase();
    {
        QuanpinDictionary dictionary(test::Utf8(db_path));
        const auto candidates = dictionary.query("xian", "xian");
        const auto alternative = std::find_if(candidates.begin(), candidates.end(), [](const WordItem &item) {
            return item.word == "__alternative_xi_an__";
        });
        REQUIRE(alternative != candidates.end());
        REQUIRE(static_cast<size_t>(std::distance(candidates.begin(), alternative)) <= static_cast<size_t>(1));
    }
    std::filesystem::remove(db_path);
}

TEST_CASE(QuanpinDictionaryTriesFourSyllablesButBoundsFiveSyllableAmbiguity)
{
    const auto db_path = CreatePinyinCacheDatabase();
    {
        QuanpinDictionary dictionary(test::Utf8(db_path));

        const auto three_syllable_candidates = dictionary.query("xianxianxian", "xian'xian'xian");
        REQUIRE(std::any_of(three_syllable_candidates.begin(), three_syllable_candidates.end(),
                            [](const WordItem &item) { return item.word == "__three_syllable_alternative__"; }));

        const auto four_syllable_candidates = dictionary.query("xianxianxianxian", "xian'xian'xian'xian");
        REQUIRE(std::any_of(four_syllable_candidates.begin(), four_syllable_candidates.end(),
                            [](const WordItem &item) { return item.word == "__four_syllable_alternative__"; }));

        const auto five_syllable_candidates = dictionary.query("xianxianxianxianxian", "xian'xian'xian'xian'xian");
        REQUIRE(std::none_of(five_syllable_candidates.begin(), five_syllable_candidates.end(),
                             [](const WordItem &item) { return item.word == "__five_syllable_alternative__"; }));
    }
    std::filesystem::remove(db_path);
}

TEST_CASE(QuanpinCandidateCacheDetectsExternalDictionaryWrites)
{
    const auto db_path = CreatePinyinCacheDatabase();
    {
        QuanpinDictionary dictionary(test::Utf8(db_path));
        const auto before = dictionary.query("aoshike", "ao'shi'ke");
        REQUIRE(std::none_of(before.begin(), before.end(), [](const WordItem &item) { return item.word == "澳鳾科"; }));

        sqlite3 *writer = nullptr;
        REQUIRE_EQ(sqlite3_open(test::Utf8(db_path).c_str(), &writer), SQLITE_OK);
        REQUIRE_EQ(sqlite3_exec(writer, "INSERT INTO tbl_3_a VALUES('ao''shi''ke','ask','澳鳾科',1)", nullptr, nullptr,
                                nullptr),
                   SQLITE_OK);
        sqlite3_close(writer);

        const auto after = dictionary.query("aoshike", "ao'shi'ke");
        REQUIRE(std::any_of(after.begin(), after.end(), [](const WordItem &item) { return item.word == "澳鳾科"; }));
    }
    std::filesystem::remove(db_path);
}

TEST_CASE(QuanpinSchemePreservesUppercaseRawInputForHelpcodes)
{
    QuanpinScheme scheme;
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, 'I', L'i');
    InputKey(scheme, 'T', L't');
    InputKey(scheme, 'E', L'e');
    InputKey(scheme, 'L', L'l');
    InputKey(scheme, 'E', L'e');
    InputKey(scheme, 'A', L'A', 1);
    InputKey(scheme, 'A', L'A', 1);

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input_with_cases, std::string("xiteleAA"));
    REQUIRE_EQ(request.raw_input, std::string("xiteleaa"));
    REQUIRE_EQ(request.normalized_input, std::string("xitele"));
    REQUIRE_EQ(request.raw_segmentation, std::string("xi'te'le'AA"));
}

TEST_CASE(QuanpinDoubleHelpModeRecognizesTrailingUppercaseLetters)
{
    REQUIRE(HelpcodeUtils::is_quanpin_double_help_mode("xiteleAA"));
    REQUIRE(!HelpcodeUtils::is_quanpin_double_help_mode("xiteleaA"));
    REQUIRE(!HelpcodeUtils::is_quanpin_double_help_mode("xiteleaa"));
}

TEST_CASE(QuanpinSchemePreservesUppercaseRawInputForSingleHelpcode)
{
    QuanpinScheme scheme;
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, 'I', L'i');
    InputKey(scheme, 'T', L't');
    InputKey(scheme, 'E', L'e');
    InputKey(scheme, 'L', L'l');
    InputKey(scheme, 'E', L'e');
    InputKey(scheme, 'A', L'A', 1);

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input_with_cases, std::string("xiteleA"));
    REQUIRE_EQ(request.raw_input, std::string("xitelea"));
    REQUIRE_EQ(request.normalized_input, std::string("xitele"));
    REQUIRE_EQ(request.raw_segmentation, std::string("xi'te'le'A"));
}

TEST_CASE(QuanpinSingleHelpModeRecognizesTrailingUppercaseLetter)
{
    REQUIRE(HelpcodeUtils::is_quanpin_single_help_mode("xiteleA"));
    REQUIRE(!HelpcodeUtils::is_quanpin_single_help_mode("xiteleAA"));
    REQUIRE(!HelpcodeUtils::is_quanpin_single_help_mode("xitelea"));
}

TEST_CASE(QuanpinHelpcodeRequiresCompleteBasePinyin)
{
    QuanpinScheme scheme;
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, 'I', L'i');
    InputKey(scheme, 'T', L't');
    InputKey(scheme, 'E', L'e');
    InputKey(scheme, 'L', L'l');
    InputKey(scheme, 'A', L'A', 1);

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input_with_cases, std::string("xitelA"));
    REQUIRE_EQ(request.raw_input, std::string("xitela"));
    REQUIRE_EQ(request.normalized_input, std::string("xitela"));
    REQUIRE_EQ(request.raw_segmentation, std::string("xi'te'lA"));
}

TEST_CASE(QuanpinCompletePinyinInputDetectionMatchesHelpcodesRequirement)
{
    REQUIRE(quanpin::is_complete_pinyin_input("xitele"));
    REQUIRE(quanpin::is_complete_pinyin_input("xi'te'le"));
    REQUIRE(!quanpin::is_complete_pinyin_input("xitel"));
    REQUIRE(!quanpin::is_complete_pinyin_input("xi'tel"));
}

TEST_CASE(QuanpinHelpcodeDetectionUsesSharedUtilsRules)
{
    REQUIRE_EQ(quanpin::detect_active_helpcode_length("xitelea", "xiteleA"), static_cast<size_t>(1));
    REQUIRE_EQ(quanpin::detect_active_helpcode_length("xiteleaa", "xiteleAA"), static_cast<size_t>(2));
    REQUIRE_EQ(quanpin::detect_active_helpcode_length("xitelr", "xitelR"), static_cast<size_t>(0));
    REQUIRE_EQ(quanpin::strip_active_helpcodes("xitelea", "xiteleA"), std::string("xitele"));
    REQUIRE_EQ(quanpin::strip_active_helpcodes("xiteleaa", "xiteleAA"), std::string("xitele"));
    REQUIRE_EQ(quanpin::strip_active_helpcodes("xitelr", "xitelR"), std::string("xitelr"));
}

TEST_CASE(QuanpinCorrectionPrefersFewerSegments)
{
    const auto cuts = quanpin::cut_pinyin_by_mode("keneng", "correction");
    REQUIRE(!cuts.empty());
    REQUIRE_EQ(quanpin::join_segments(cuts.front()), std::string("ke'neng"));
}

TEST_CASE(QuanpinCorrectionNormalizesTransposedPinyinLetters)
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"laing", "liang"}, {"haung", "huang"}, {"bain", "bian"},   {"haun", "huan"},    {"laio", "liao"},
        {"ahng", "hang"},   {"cehng", "cheng"}, {"zehng", "zheng"}, {"zhehng", "zheng"}, {"mihng", "ming"},
        {"agn", "ang"},     {"bagn", "bang"},   {"chagn", "chang"}, {"zagn", "zang"},    {"egn", "eng"},
        {"begn", "beng"},   {"chegn", "cheng"}, {"zhegn", "zheng"}, {"jv", "ju"},
    };

    for (const auto &[typed, expected] : cases)
    {
        const auto cuts = quanpin::cut_pinyin_by_mode(typed, "correction");
        REQUIRE(!cuts.empty());
        const auto actual = quanpin::join_segments(cuts.front());
        if (actual != expected)
        {
            throw std::runtime_error("Expected '" + typed + "' to normalize to '" + expected + "', got '" + actual +
                                     "'.");
        }
    }
}

TEST_CASE(QuanpinCorrectionHandlesContinuousAndManuallySegmentedInput)
{
    const auto continuous = quanpin::cut_pinyin_by_mode("woxainxin", "correction");
    REQUIRE(!continuous.empty());
    REQUIRE_EQ(quanpin::join_segments(continuous.front()), std::string("wo'xian'xin"));

    const auto manual = quanpin::cut_pinyin_by_mode("wo'xain'xin", "correction");
    REQUIRE(!manual.empty());
    REQUIRE_EQ(quanpin::join_segments(manual.front()), std::string("wo'xian'xin"));

    const auto continuous_jv = quanpin::cut_pinyin_by_mode("wojv", "correction");
    REQUIRE(!continuous_jv.empty());
    REQUIRE_EQ(quanpin::join_segments(continuous_jv.front()), std::string("wo'ju"));

    const auto manual_jv = quanpin::cut_pinyin_by_mode("wo'jv", "correction");
    REQUIRE(!manual_jv.empty());
    REQUIRE_EQ(quanpin::join_segments(manual_jv.front()), std::string("wo'ju"));
}

TEST_CASE(QuanpinCorrectionAppliesSuffixRulesToEveryValidPinyinPrefix)
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"daun", "duan"}, {"jaun", "juan"}, {"laun", "luan"}, {"naun", "nuan"}, {"qaun", "quan"}, {"raun", "ruan"},
        {"saun", "suan"}, {"taun", "tuan"}, {"xaun", "xuan"}, {"yaun", "yuan"}, {"zaun", "zuan"},
    };

    for (const auto &[typed, expected] : cases)
    {
        const auto cuts = quanpin::cut_pinyin_by_mode(typed, "correction");
        REQUIRE(!cuts.empty());
        REQUIRE_EQ(quanpin::join_segments(cuts.front()), expected);
    }
}

TEST_CASE(QuanpinCorrectionRetainsAmbiguousEhngInterpretations)
{
    const auto cuts = quanpin::cut_pinyin_by_mode("cehng", "correction");
    REQUIRE_EQ(cuts.size(), static_cast<size_t>(2));
    REQUIRE_EQ(quanpin::join_segments(cuts[0]), std::string("cheng"));
    REQUIRE_EQ(quanpin::join_segments(cuts[1]), std::string("ceng"));

    const auto bare = quanpin::cut_pinyin_by_mode("ehng", "correction");
    REQUIRE_EQ(bare.size(), static_cast<size_t>(1));
    REQUIRE_EQ(quanpin::join_segments(bare[0]), std::string("heng"));

    const auto extra_h = quanpin::cut_pinyin_by_mode("behng", "correction");
    REQUIRE(!extra_h.empty());
    REQUIRE_EQ(quanpin::join_segments(extra_h.front()), std::string("beng"));
}

TEST_CASE(QuanpinCorrectionRetainsAmbiguousAhngInterpretations)
{
    const auto cuts = quanpin::cut_pinyin_by_mode("ahng", "correction");
    REQUIRE_EQ(cuts.size(), static_cast<size_t>(2));
    REQUIRE_EQ(quanpin::join_segments(cuts[0]), std::string("hang"));
    REQUIRE_EQ(quanpin::join_segments(cuts[1]), std::string("ang"));

    const auto prefixed = quanpin::cut_pinyin_by_mode("zahng", "correction");
    REQUIRE_EQ(prefixed.size(), static_cast<size_t>(2));
    REQUIRE_EQ(quanpin::join_segments(prefixed[0]), std::string("zhang"));
    REQUIRE_EQ(quanpin::join_segments(prefixed[1]), std::string("zang"));
}

TEST_CASE(QuanpinCorrectionCarriesAmbiguousPathsThroughContinuousInput)
{
    const auto cuts = quanpin::cut_pinyin_by_mode("wocehng", "correction");
    REQUIRE_EQ(cuts.size(), static_cast<size_t>(2));
    REQUIRE_EQ(quanpin::join_segments(cuts[0]), std::string("wo'cheng"));
    REQUIRE_EQ(quanpin::join_segments(cuts[1]), std::string("wo'ceng"));
}

TEST_CASE(QuanpinGreedyModeDoesNotApplyLetterCorrections)
{
    const auto cuts = quanpin::cut_pinyin_by_mode("haun", "greedy");
    REQUIRE(cuts.empty());
}

TEST_CASE(QuanpinPreeditPreservesTypedCaseEvenWhenHelpcodesDoNotApply)
{
    QuanpinScheme scheme;
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, 'I', L'i');
    InputKey(scheme, 'T', L't');
    InputKey(scheme, 'E', L'e');
    InputKey(scheme, 'L', L'l');
    InputKey(scheme, 'R', L'R', 1);

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input_with_cases, std::string("xitelR"));
    REQUIRE_EQ(request.raw_input, std::string("xitelr"));
    REQUIRE_EQ(request.raw_segmentation, std::string("xi'te'l'R"));
}

TEST_CASE(QuanpinSparsePinyinFallbackSegmentsAreGenerated)
{
    {
        const auto fallbacks = quanpin::sparse_pinyin_fallback_segments(quanpin::Segments{"dia"});
        REQUIRE_EQ(fallbacks.size(), static_cast<size_t>(2));
        REQUIRE_EQ(quanpin::join_segments(fallbacks[0]), std::string("di'a"));
        REQUIRE_EQ(quanpin::join_segments(fallbacks[1]), std::string("di"));
    }

    {
        const auto fallbacks = quanpin::sparse_pinyin_fallback_segments(quanpin::Segments{"biang"});
        REQUIRE_EQ(fallbacks.size(), static_cast<size_t>(2));
        REQUIRE_EQ(quanpin::join_segments(fallbacks[0]), std::string("bi'ang"));
        REQUIRE_EQ(quanpin::join_segments(fallbacks[1]), std::string("bi"));
    }

    {
        const auto fallbacks = quanpin::sparse_pinyin_fallback_segments(quanpin::Segments{"gei"});
        REQUIRE_EQ(fallbacks.size(), static_cast<size_t>(1));
        REQUIRE_EQ(quanpin::join_segments(fallbacks[0]), std::string("ge"));
    }

    {
        const auto fallbacks = quanpin::sparse_pinyin_fallback_segments(quanpin::Segments{"yo"});
        REQUIRE_EQ(fallbacks.size(), static_cast<size_t>(1));
        REQUIRE_EQ(quanpin::join_segments(fallbacks[0]), std::string("y"));
    }
}

TEST_CASE(QuanpinSparsePinyinFallbackSegmentsPreserveSuffixSegments)
{
    const auto fallbacks = quanpin::sparse_pinyin_fallback_segments(quanpin::Segments{"gei", "wo"});
    REQUIRE_EQ(fallbacks.size(), static_cast<size_t>(1));
    REQUIRE_EQ(quanpin::join_segments(fallbacks[0]), std::string("ge"));

    const auto dia_fallbacks = quanpin::sparse_pinyin_fallback_segments(quanpin::Segments{"dia", "wo"});
    REQUIRE_EQ(dia_fallbacks.size(), static_cast<size_t>(2));
    REQUIRE_EQ(quanpin::join_segments(dia_fallbacks[0]), std::string("di'a'wo"));
    REQUIRE_EQ(quanpin::join_segments(dia_fallbacks[1]), std::string("di"));
}

TEST_CASE(QuanpinAutocorrectTableHasNoCollisionsWithLegalPinyin)
{
    const auto &legal = quanpin::intact_pinyin_set();
    // Ambiguity is kept: one wrong key may map to several syllables and the
    // runtime disambiguates with k-best cuts + word frequency (CN 101133411 B),
    // so uniqueness is asserted per (wrong, correct) pair instead of per key.
    std::unordered_set<std::string> seen_pairs;
    size_t total_entries = 0;
    // 三张表的数组长度不同，初始化列表推导不出共同的指针类型（VS2022 严格报错）；
    // 用边界对遍历代替指针到数组的推导。
    const auto require_valid_entries = [&](const quanpin::autocorrect::Entry *entries, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::string wrong(entries[i].wrong);
            // correct 是生成音节表的 16-bit 下标，断言前先解引用。
            const std::string correct(quanpin::autocorrect::kCorrectSyllables[entries[i].correct]);
            REQUIRE(!wrong.empty());
            // 2-letter strings belong to the jianpin space: a correction key there
            // would shadow abbreviations such as wj -> 文件.
            REQUIRE(wrong.size() >= 3);
            REQUIRE(!legal.count(wrong));                             // a key must never shadow a legal syllable
            REQUIRE(legal.count(correct));                            // the correction must be a legal syllable
            REQUIRE(seen_pairs.insert(wrong + '>' + correct).second); // pairs unique across all tables
            ++total_entries;
        }
    };
    require_valid_entries(quanpin::autocorrect::kTranspositionEntries, quanpin::autocorrect::kTranspositionCount);
    require_valid_entries(quanpin::autocorrect::kNeighborEntries, quanpin::autocorrect::kNeighborCount);
    require_valid_entries(quanpin::autocorrect::kDeletionEntries, quanpin::autocorrect::kDeletionCount);
    require_valid_entries(quanpin::autocorrect::kInsertionEntries, quanpin::autocorrect::kInsertionCount);
    // Coverage gate (task AC1): transposition + neighbor + deletion entries together.
    REQUIRE(total_entries >= 4700);
    REQUIRE(seen_pairs.size() == total_entries);
}

TEST_CASE(QuanpinAutocorrectTableKeepsAmbiguousAndDeletionVariants)
{
    std::unordered_map<std::string, std::unordered_set<std::string>> targets;
    const auto collect = [&](const quanpin::autocorrect::Entry *entries, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
        {
            targets[entries[i].wrong].insert(std::string(quanpin::autocorrect::kCorrectSyllables[entries[i].correct]));
        }
    };
    collect(quanpin::autocorrect::kTranspositionEntries, quanpin::autocorrect::kTranspositionCount);
    collect(quanpin::autocorrect::kNeighborEntries, quanpin::autocorrect::kNeighborCount);
    collect(quanpin::autocorrect::kDeletionEntries, quanpin::autocorrect::kDeletionCount);
    collect(quanpin::autocorrect::kInsertionEntries, quanpin::autocorrect::kInsertionCount);

    const auto maps_to = [&](const char *wrong, const char *correct) {
        const auto found = targets.find(wrong);
        return found != targets.end() && found->second.count(correct) != 0;
    };

    // Ambiguous neighbor keys in the z/zh confusion zone used to be dropped at
    // generation time; they must now survive with every candidate syllable.
    REQUIRE(maps_to("ahan", "shan"));
    REQUIRE(maps_to("ahan", "zhan"));
    REQUIRE(maps_to("aang", "sang"));
    REQUIRE(maps_to("aang", "wang"));
    // Deletion variants: one dropped letter, ambiguous targets kept together.
    REQUIRE(maps_to("shng", "shang"));
    REQUIRE(maps_to("shng", "sheng"));
    REQUIRE(maps_to("chn", "chan"));
    REQUIRE(maps_to("chn", "chen"));
    REQUIRE(maps_to("bng", "bang"));
    REQUIRE(maps_to("bng", "beng"));
    REQUIRE(maps_to("zhng", "zhang"));
    REQUIRE(maps_to("zhng", "zheng"));
    // Insertion variants keep ambiguous targets too: baio = bai/bao + one
    // inserted letter inside the ai->ao confusion zone.
    REQUIRE(maps_to("baio", "bai"));
    REQUIRE(maps_to("baio", "bao"));
}

TEST_CASE(QuanpinAutocorrectDeletionTableEntriesAreLegal)
{
    // Shape pin for the deletion table: exactly one dropped letter per key,
    // never a legal syllable, always a legal target. The runtime builds its
    // multi-value index from this table (quanpin_utils.cpp correction_index).
    const auto &legal = quanpin::intact_pinyin_set();
    REQUIRE(quanpin::autocorrect::kDeletionCount > 0);
    for (std::size_t i = 0; i < quanpin::autocorrect::kDeletionCount; ++i)
    {
        const auto &entry = quanpin::autocorrect::kDeletionEntries[i];
        const std::string wrong(entry.wrong);
        const std::string correct(quanpin::autocorrect::kCorrectSyllables[entry.correct]);
        // A deletion key is exactly one letter shorter than its target.
        REQUIRE_EQ(wrong.size() + 1, correct.size());
        REQUIRE(wrong.size() >= 3);
        REQUIRE(!legal.count(wrong));
        REQUIRE(legal.count(correct));
    }
}

TEST_CASE(QuanpinAutocorrectInsertionTableEntriesAreLegal)
{
    // Shape pin for the insertion table: exactly one inserted letter per key,
    // never a legal syllable, always a legal target. The 6-letter cap means
    // 6-letter syllables (zhuang family) have no insertion coverage on
    // purpose -- their variants would be 7 letters.
    const auto &legal = quanpin::intact_pinyin_set();
    REQUIRE(quanpin::autocorrect::kInsertionCount > 0);
    for (std::size_t i = 0; i < quanpin::autocorrect::kInsertionCount; ++i)
    {
        const auto &entry = quanpin::autocorrect::kInsertionEntries[i];
        const std::string wrong(entry.wrong);
        const std::string correct(quanpin::autocorrect::kCorrectSyllables[entry.correct]);
        // An insertion key is exactly one letter longer than its target.
        REQUIRE_EQ(wrong.size(), correct.size() + 1);
        REQUIRE(wrong.size() >= 3);
        REQUIRE(!legal.count(wrong));
        REQUIRE(legal.count(correct));
    }
}

TEST_CASE(QuanpinAutocorrectCutGatesEachTypeIndependently)
{
    const unsigned none = 0;
    const unsigned transposition_only = quanpin::kAutocorrectTransposition;
    const unsigned neighbor_only = quanpin::kAutocorrectNeighbor;
    const unsigned both = transposition_only | neighbor_only;

    // "sahng" is a transposition fix; only that bit may correct it.
    REQUIRE(quanpin::autocorrect_cut("sahng", none).empty());
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("sahng", transposition_only)), std::string("shang"));
    REQUIRE(quanpin::autocorrect_cut("sahng", neighbor_only).empty());
    // "shabg" is a neighbor fix; only that bit may correct it.
    REQUIRE(quanpin::autocorrect_cut("shabg", none).empty());
    REQUIRE(quanpin::autocorrect_cut("shabg", transposition_only).empty());
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("shabg", neighbor_only)), std::string("shang"));
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("shabg", both)), std::string("shang"));

    // "sshang" (doubled s) is an insertion fix; only that bit may correct it.
    REQUIRE(quanpin::autocorrect_cut("sshang", none).empty());
    REQUIRE(quanpin::autocorrect_cut("sshang", transposition_only).empty());
    REQUIRE(quanpin::autocorrect_cut("sshang", neighbor_only).empty());
    REQUIRE(quanpin::autocorrect_cut("sshang", quanpin::kAutocorrectDeletion).empty());
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("sshang", quanpin::kAutocorrectInsertion)),
               std::string("shang"));
}

TEST_CASE(QuanpinJianpinShapeGuardBlocksCorrection)
{
    // One or more legal syllables plus at most one trailing letter is jianpin
    // intent (zheg = zhe + g), never a typo the tables may rewrite.
    REQUIRE(quanpin::looks_like_syllable_with_jianpin_tail("zheg"));
    REQUIRE(quanpin::looks_like_syllable_with_jianpin_tail("keneng"));
    REQUIRE(quanpin::looks_like_syllable_with_jianpin_tail("shang"));
    REQUIRE(!quanpin::looks_like_syllable_with_jianpin_tail("sahng"));
    REQUIRE(!quanpin::looks_like_syllable_with_jianpin_tail("shabg"));
    REQUIRE(!quanpin::looks_like_syllable_with_jianpin_tail("xi'an"));
    // All-consonant strings stay correctable: the engine has no multi-letter
    // jianpin, so correction is the only useful reading of e.g. bqng -> bang.
    REQUIRE(!quanpin::looks_like_syllable_with_jianpin_tail("bqng"));
    REQUIRE(!quanpin::looks_like_syllable_with_jianpin_tail("wj"));
}

TEST_CASE(QuanpinAutocorrectCutSingleSyllableTranspositions)
{
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("sahng", both)), std::string("shang"));
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("hsang", both)), std::string("shang"));
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("shagn", both)), std::string("shang"));
}

TEST_CASE(QuanpinAutocorrectCutNeighborKeySubstitutions)
{
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("shabg", both)), std::string("shang")); // n -> b
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("sgang", both)), std::string("shang")); // h -> g
}

TEST_CASE(QuanpinAutocorrectCutMixedMultisyllableInput)
{
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("sahngzhi", both)), std::string("shang'zhi"));
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("sahngzhk", both)), std::string("shang'zhi")); // i -> k
}

TEST_CASE(QuanpinAutocorrectCutPrefersFewestCorrections)
{
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    // sahnguai has two plausible readings; the one with fewer corrected edges
    // (shan + guai, one mistake) wins over shang + hai (two mistakes).
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("sahnguai", both)), std::string("shan'guai"));
}

TEST_CASE(QuanpinAutocorrectCutRejectsInputsOutOfScope)
{
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    // The mask short-circuits before any table lookup.
    REQUIRE(quanpin::autocorrect_cut("shang", 0u).empty());
    // Legal input: the caller must only invoke autocorrect when the correction
    // cut already failed, so a fully legal spelling yields no correction.
    REQUIRE(quanpin::autocorrect_cut("shang", both).empty());
    REQUIRE(quanpin::autocorrect_cut("keneng", both).empty());
    // Manual delimiter: user-intended boundary, never rewritten.
    REQUIRE(quanpin::autocorrect_cut("xi'an", both).empty());
    // No correction path: unresolvable garbage.
    REQUIRE(quanpin::autocorrect_cut("xxxxx", both).empty());
    REQUIRE(quanpin::autocorrect_cut("qzzvv", both).empty());
    // Beyond the per-input correction edge budget.
    REQUIRE(quanpin::autocorrect_cut("sahngsahngsahngsahng", both).empty());
}

TEST_CASE(QuanpinAutocorrectCutRespectsEdgeBudget)
{
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("sahngsahngsahng", both)),
               std::string("shang'shang'shang"));
}

namespace
{
std::string JoinCut(const quanpin::AutocorrectCut &cut)
{
    quanpin::Segments syllables;
    syllables.reserve(cut.segments.size());
    for (const auto &segment : cut.segments)
    {
        syllables.push_back(segment.syllable);
    }
    return quanpin::join_segments(syllables);
}
} // namespace

TEST_CASE(QuanpinAutocorrectCutDeletionVariants)
{
    const unsigned all =
        quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor | quanpin::kAutocorrectDeletion;

    // "shng" drops the "a" of shang/sheng: an unequal-length deletion edge.
    // Both targets weigh kAutocorrectDeletionWeight, so the generated table
    // order (shang before sheng) fixes the first cut deterministically.
    const auto cuts = quanpin::autocorrect_cut_kbest("shng", all);
    REQUIRE(cuts.size() >= 2);
    REQUIRE_EQ(JoinCut(cuts[0]), std::string("shang"));
    std::unordered_set<std::string> readings;
    for (const auto &cut : cuts)
    {
        readings.insert(JoinCut(cut));
    }
    REQUIRE(readings.count("sheng") != 0);

    // The detail cut is the k = 1 projection; its raw span still covers the
    // four typed letters even though the syllable is five letters long.
    const auto detail = quanpin::autocorrect_cut_detail("shng", all);
    REQUIRE_EQ(detail.segments.size(), static_cast<size_t>(1));
    REQUIRE_EQ(detail.segments[0].syllable, std::string("shang"));
    REQUIRE_EQ(detail.segments[0].raw_text, std::string("shng"));
    REQUIRE_EQ(detail.segments[0].start, static_cast<size_t>(0));
    REQUIRE(detail.segments[0].corrected);
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("shng", all)), std::string("shang"));
}

TEST_CASE(QuanpinAutocorrectCutDeletionKeepsRawSpans)
{
    const unsigned all =
        quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor | quanpin::kAutocorrectDeletion;

    // "zhngu": the deletion key "zhn" consumes three letters, the untouched
    // "gu" starts at raw offset 3 -- position advance follows raw_length,
    // not syllable.size().
    const auto cuts = quanpin::autocorrect_cut_kbest("zhngu", all);
    REQUIRE(!cuts.empty());
    REQUIRE_EQ(cuts[0].segments.size(), static_cast<size_t>(2));
    REQUIRE_EQ(cuts[0].segments[0].syllable, std::string("zhan"));
    REQUIRE_EQ(cuts[0].segments[0].raw_text, std::string("zhn"));
    REQUIRE_EQ(cuts[0].segments[0].start, static_cast<size_t>(0));
    REQUIRE(cuts[0].segments[0].corrected);
    REQUIRE_EQ(cuts[0].segments[1].syllable, std::string("gu"));
    REQUIRE_EQ(cuts[0].segments[1].raw_text, std::string("gu"));
    REQUIRE_EQ(cuts[0].segments[1].start, static_cast<size_t>(3));
    REQUIRE(!cuts[0].segments[1].corrected);

    // "zhnggu": the 4-letter deletion key "zhng" -> zhang followed by a legal
    // "gu" at raw offset 4.
    const auto zhang_gu = quanpin::autocorrect_cut_detail("zhnggu", all);
    REQUIRE_EQ(zhang_gu.segments.size(), static_cast<size_t>(2));
    REQUIRE_EQ(zhang_gu.segments[0].syllable, std::string("zhang"));
    REQUIRE_EQ(zhang_gu.segments[0].raw_text, std::string("zhng"));
    REQUIRE_EQ(zhang_gu.segments[0].start, static_cast<size_t>(0));
    REQUIRE(zhang_gu.segments[0].corrected);
    REQUIRE_EQ(zhang_gu.segments[1].syllable, std::string("gu"));
    REQUIRE_EQ(zhang_gu.segments[1].raw_text, std::string("gu"));
    REQUIRE_EQ(zhang_gu.segments[1].start, static_cast<size_t>(4));
    REQUIRE(!zhang_gu.segments[1].corrected);
}

TEST_CASE(QuanpinAutocorrectCutMixedDeletionAndNeighbor)
{
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    const unsigned all = both | quanpin::kAutocorrectDeletion;

    // "shngzhk" mixes a deletion edge (shng -> shang) with a neighbor edge
    // (zhk -> zhi): two corrected edges, weights 11 + 13.
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("shngzhk", all)), std::string("shang'zhi"));
    // Without the deletion bit the same input stays unexplained.
    REQUIRE(quanpin::autocorrect_cut("shngzhk", both).empty());

    // A two-letter jianpin leftover ("zh") stays out of correction scope even
    // with deletion on: the tables hold no 2-letter keys.
    REQUIRE(quanpin::autocorrect_cut("sahngzh", all).empty());
}

TEST_CASE(QuanpinAutocorrectCutGatesDeletionIndependently)
{
    const unsigned none = 0;
    const unsigned transposition_only = quanpin::kAutocorrectTransposition;
    const unsigned neighbor_only = quanpin::kAutocorrectNeighbor;
    const unsigned deletion_only = quanpin::kAutocorrectDeletion;
    const unsigned all = transposition_only | neighbor_only | deletion_only;

    // "shng" is a deletion fix; only that bit may correct it.
    REQUIRE(quanpin::autocorrect_cut("shng", none).empty());
    REQUIRE(quanpin::autocorrect_cut("shng", transposition_only).empty());
    REQUIRE(quanpin::autocorrect_cut("shng", neighbor_only).empty());
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("shng", deletion_only)), std::string("shang"));
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("shng", all)), std::string("shang"));

    // The legacy switches keep their own families for the direct reading; note
    // that bits gate tables, not typo intents: with deletion only, "sahng" is
    // still explained through a DIFFERENT split (sa + hng -> hang), which is
    // exactly the added coverage the deletion table provides.
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("sahng", deletion_only)), std::string("sa'hang"));
    REQUIRE(quanpin::autocorrect_cut("shabg", deletion_only).empty());
}

TEST_CASE(QuanpinAutocorrectCutKbestRanksAmbiguousKeys)
{
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    const unsigned all = both | quanpin::kAutocorrectDeletion;

    // "ahan" maps to both shan and zhan in the neighbor table. Both readings
    // survive as parallel hypotheses (query-time disambiguation per CN
    // 101133411 B), ordered by (edge count, weight, table order).
    const auto cuts = quanpin::autocorrect_cut_kbest("ahan", both);
    REQUIRE(cuts.size() >= 2);
    REQUIRE_EQ(JoinCut(cuts[0]), std::string("shan"));
    REQUIRE_EQ(JoinCut(cuts[1]), std::string("zhan"));

    // Same edge count, different weights: the direct transposition (weight
    // 10) outranks the split deletion reading (weight 11) within one edge.
    const auto sahng = quanpin::autocorrect_cut_kbest("sahng", all);
    REQUIRE(sahng.size() >= 2);
    REQUIRE_EQ(JoinCut(sahng[0]), std::string("shang"));
    REQUIRE_EQ(JoinCut(sahng[1]), std::string("sa'hang"));

    // Edge count stays the primary key: one transposition (weight 10) beats
    // transposition + deletion (21) for the same input.
    REQUIRE_EQ(quanpin::join_segments(quanpin::autocorrect_cut("sahnguai", all)), std::string("shan'guai"));

    // k = 1 agrees with the single-cut projection on every input, including
    // the ones with no correction reading at all.
    const std::pair<const char *, unsigned> inputs[] = {
        {"sahng", both},  {"shabg", both}, {"sahnguai", both}, {"ahan", both},    {"zheg", both},
        {"keneng", both}, {"shng", all},   {"zhngu", all},     {"sahngzhk", all},
    };
    for (const auto &entry : inputs)
    {
        const auto single = quanpin::autocorrect_cut(entry.first, entry.second);
        const auto top1 = quanpin::autocorrect_cut_kbest(entry.first, entry.second, 1);
        REQUIRE_EQ(top1.size(), single.empty() ? static_cast<size_t>(0) : static_cast<size_t>(1));
        if (!single.empty())
        {
            REQUIRE_EQ(JoinCut(top1.front()), quanpin::join_segments(single));
        }
    }
}

namespace
{
std::filesystem::path CreateAutocorrectDatabase()
{
    const auto path = std::filesystem::temp_directory_path() / "msime-quanpin-autocorrect-test.db";
    std::filesystem::remove(path);
    sqlite3 *db = nullptr;
    if (sqlite3_open(test::Utf8(path).c_str(), &db) != SQLITE_OK)
    {
        throw std::runtime_error("Failed to create temporary autocorrect database.");
    }
    const char *sql = "CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_2_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_2_g(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_2_j(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_2_q(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "CREATE TABLE tbl_4_s(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                      "INSERT INTO tbl_1_s VALUES('shang','s','上',100);"
                      "INSERT INTO tbl_2_s VALUES('shang''zhi','sz','上至',100);"
                      "INSERT INTO tbl_2_g VALUES('guan''li','gl','管理',100);"
                      "INSERT INTO tbl_2_j VALUES('jian''du','jd','监督',100);"
                      "INSERT INTO tbl_2_q VALUES('quan''li','ql','权利',100);"
                      "INSERT INTO tbl_4_s VALUES('sa''huang''na''ge','shng','撒谎那个',1000);";
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (result != SQLITE_OK)
    {
        std::filesystem::remove(path);
        throw std::runtime_error("Failed to initialize temporary autocorrect database.");
    }
    return path;
}
} // namespace

TEST_CASE(QuanpinDictionaryAutocorrectPutsCorrectedCandidateFirst)
{
    const auto db_path = CreateAutocorrectDatabase();
    QuanpinDictionary dictionary(test::Utf8(db_path));
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;

    const auto candidates = dictionary.query("sahng", "sa'h'n'g", both);
    REQUIRE(!candidates.empty());
    // The corrected candidate leads, and its pinyin is the corrected key so
    // that selection/weight updates land on the right dictionary entry.
    REQUIRE_EQ(candidates.front().word, std::string("上"));
    REQUIRE_EQ(candidates.front().pinyin, std::string("shang"));
    REQUIRE_EQ(candidates.front().canonical_pinyin, std::string("shang"));
    // The original greedy candidates stay behind as a fallback tail.
    REQUIRE(std::any_of(candidates.begin(), candidates.end(),
                        [](const WordItem &item) { return item.word == "撒谎那个"; }));
    REQUIRE(candidates.front().word != "撒谎那个");
}

TEST_CASE(QuanpinDictionaryAutocorrectNeighborKeySubstitution)
{
    const auto db_path = CreateAutocorrectDatabase();
    QuanpinDictionary dictionary(test::Utf8(db_path));

    const auto candidates = dictionary.query("shabg", "sha'b'g", quanpin::kAutocorrectNeighbor);
    REQUIRE(!candidates.empty());
    REQUIRE_EQ(candidates.front().word, std::string("上"));
}

TEST_CASE(QuanpinDictionaryAutocorrectDeletionTypo)
{
    const auto db_path = CreateAutocorrectDatabase();
    QuanpinDictionary dictionary(test::Utf8(db_path));
    const unsigned all =
        quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor | quanpin::kAutocorrectDeletion;

    // 阶段 2 生效信号：漏字输入经纠错键命中目标词，标记 corrected_from，
    // 关掉 deletion 位则回到现状（乱码后备尾）。缓存隔离：纠错结果不污染
    // 后续对同键的其他查询。
    const auto candidates = dictionary.query("shng", "sh'n'g", all);
    REQUIRE(!candidates.empty());
    REQUIRE_EQ(candidates.front().word, std::string("上"));
    REQUIRE_EQ(candidates.front().pinyin, std::string("shang"));
    REQUIRE_EQ(candidates.front().corrected_from, std::string("shng"));

    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;
    const auto legacy = dictionary.query("shng", "sh'n'g", both);
    REQUIRE(
        std::none_of(legacy.begin(), legacy.end(), [](const WordItem &item) { return item.corrected_from == "shng"; }));
}

TEST_CASE(QuanpinDictionaryAutocorrectInsertionTypo)
{
    const auto db_path = CreateAutocorrectDatabase();
    QuanpinDictionary dictionary(test::Utf8(db_path));
    const unsigned all = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor |
                         quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;

    // 阶段 4 生效信号：开头双打（sshang）经插入纠错键命中目标词，标记
    // corrected_from，canonical 读音落在纠错后的 shang 上（造词/调频不学习
    // 错拼）。关掉 insertion 位则回到无纠错标记的现状。
    const auto candidates = dictionary.query("sshang", "", all);
    REQUIRE(!candidates.empty());
    REQUIRE_EQ(candidates.front().word, std::string("上"));
    REQUIRE_EQ(candidates.front().pinyin, std::string("shang"));
    REQUIRE_EQ(candidates.front().corrected_from, std::string("sshang"));

    const unsigned legacy_bits =
        quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor | quanpin::kAutocorrectDeletion;
    const auto legacy = dictionary.query("sshang", "", legacy_bits);
    REQUIRE(std::none_of(legacy.begin(), legacy.end(),
                         [](const WordItem &item) { return item.corrected_from == "sshang"; }));
}

TEST_CASE(QuanpinDictionaryAutocorrectAmbiguousDisambiguation)
{
    // k-best 进查询管线（阶段 3）："chn" 同时映射 chan/chen，主切分查 chan，
    // 备选切分经 merge_alternative_segmentations 与主切分同台竞争，词频
    // 高者胜出（专利 M3 查询期消歧）。分两个权重布局验证顺序跟随词频。
    // 不用派发指令中的 ahan：ahan = a + han 是全合法拼读，被简拼形状守卫
    // （AC3 既有语义）挡在纠错门外；chn 无任何合法切分，是真实可达的歧义键。
    const auto create_database = [](long chan_weight, long chen_weight) {
        const auto path = std::filesystem::temp_directory_path() / "msime-quanpin-autocorrect-ambiguous-test.db";
        std::filesystem::remove(path);
        sqlite3 *db = nullptr;
        if (sqlite3_open(test::Utf8(path).c_str(), &db) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the ambiguous autocorrect database.");
        }
        const std::string sql = std::string("CREATE TABLE tbl_1_c(key TEXT,jp TEXT,value TEXT,weight INTEGER);") +
                                "INSERT INTO tbl_1_c VALUES('chan','c','产'," + std::to_string(chan_weight) + ");" +
                                "INSERT INTO tbl_1_c VALUES('chen','c','陈'," + std::to_string(chen_weight) + ");";
        const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
        sqlite3_close(db);
        if (result != SQLITE_OK)
        {
            std::filesystem::remove(path);
            throw std::runtime_error("Failed to initialize the ambiguous autocorrect database.");
        }
        return path;
    };
    const unsigned all =
        quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor | quanpin::kAutocorrectDeletion;

    {
        const auto db_path = create_database(100000, 1000);
        QuanpinDictionary dictionary(test::Utf8(db_path));
        const auto candidates = dictionary.query("chn", "", all);
        const auto chan =
            std::find_if(candidates.begin(), candidates.end(), [](const WordItem &item) { return item.word == "产"; });
        const auto chen =
            std::find_if(candidates.begin(), candidates.end(), [](const WordItem &item) { return item.word == "陈"; });
        REQUIRE(chan != candidates.end());
        REQUIRE(chen != candidates.end());
        // Both readings survive (query-time disambiguation) and the higher
        // weight wins the order.
        REQUIRE(chan < chen);
        // AC5: candidates from the alternative cut carry corrected_from too.
        REQUIRE_EQ(chan->corrected_from, std::string("chn"));
        REQUIRE_EQ(chen->corrected_from, std::string("chn"));
    }
    {
        // Weight layout reversed: the order must follow the dictionary
        // frequency, not the k-best ranking of the cuts.
        const auto db_path = create_database(1000, 100000);
        QuanpinDictionary dictionary(test::Utf8(db_path));
        const auto candidates = dictionary.query("chn", "", all);
        const auto chan =
            std::find_if(candidates.begin(), candidates.end(), [](const WordItem &item) { return item.word == "产"; });
        const auto chen =
            std::find_if(candidates.begin(), candidates.end(), [](const WordItem &item) { return item.word == "陈"; });
        REQUIRE(chan != candidates.end());
        REQUIRE(chen != candidates.end());
        REQUIRE(chen < chan);
    }
}

TEST_CASE(QuanpinDictionaryAutocorrectEmptyBaseSegmentationReachesCorrection)
{
    // 丢声母类错拼（quan -> uan、jian -> ian）在 correction 模式下完全切不出
    // 段：prefix 切词器连前缀都匹配不上，segments 为空。旧 eligible 门的
    // !segments.empty() 会把这类输入整体挡在纠错之外（候选为空）；BFS 工作在
    // 原始字母上，不依赖基础切分，空切分必须进入纠错路径。
    const auto db_path = CreateAutocorrectDatabase();
    QuanpinDictionary dictionary(test::Utf8(db_path));
    const unsigned all =
        quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor | quanpin::kAutocorrectDeletion;

    // ian 的漏字目标里 jian 排在 k-best 前 3（bian/dian/jian），备选切分
    // jian'du 经查询期词频合并成为首位。
    const auto iandu = dictionary.query("iandu", "", all);
    REQUIRE(!iandu.empty());
    REQUIRE_EQ(iandu.front().word, std::string("监督"));
    REQUIRE_EQ(iandu.front().corrected_from, std::string("iandu"));

    // 关掉纠错（types=0）回到不纠错的现状：纠错词不出现。
    const auto off = dictionary.query("iandu", "", 0u);
    REQUIRE(std::none_of(off.begin(), off.end(), [](const WordItem &item) { return item.word == "监督"; }));

    // uan 键有 15 个等权漏字目标（cuan/duan/guan/.../zuan），quan 按表序排
    // 第 9。k-best 的 k=9 是评测定的截断线：quan'li 恰好进入本轮查询，权利
    // 可达并带 corrected_from；k=3 时 quan 超出截断线，权利不可达（评测阶段
    // 4 的边界结论）。本 fixture 里管理/权利同权重 100，quan'li 表序靠后，
    // 因此权利排第二而非首位（阶段 4 验证时真实 msime.db 上权利进入首选）。
    const auto uanli = dictionary.query("uanli", "", all);
    REQUIRE(!uanli.empty());
    REQUIRE_EQ(uanli.front().word, std::string("管理"));
    const auto quanli =
        std::find_if(uanli.begin(), uanli.end(), [](const WordItem &item) { return item.word == "权利"; });
    REQUIRE(quanli != uanli.end());
    REQUIRE_EQ(quanli->corrected_from, std::string("uanli"));

    // 无可纠错解读的垃圾串：不产生任何纠错标记（BFS 无路径，走既有后备尾）。
    const auto garbage = dictionary.query("qqqq", "", all);
    REQUIRE(std::none_of(garbage.begin(), garbage.end(),
                         [](const WordItem &item) { return !item.corrected_from.empty(); }));
}

TEST_CASE(QuanpinDictionaryAutocorrectMultisyllableInput)
{
    const auto db_path = CreateAutocorrectDatabase();
    QuanpinDictionary dictionary(test::Utf8(db_path));

    const auto candidates = dictionary.query("sahngzhi", "sa'h'n'g'zhi",
                                             quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor);
    REQUIRE(!candidates.empty());
    REQUIRE_EQ(candidates.front().word, std::string("上至"));
}

TEST_CASE(QuanpinDictionaryAutocorrectDisabledKeepsLegacyBehavior)
{
    const auto db_path = CreateAutocorrectDatabase();
    QuanpinDictionary dictionary(test::Utf8(db_path));

    const auto candidates = dictionary.query("sahng", "sa'h'n'g", 0u);
    REQUIRE(!candidates.empty());
    REQUIRE(std::none_of(candidates.begin(), candidates.end(), [](const WordItem &item) { return item.word == "上"; }));
    REQUIRE(std::any_of(candidates.begin(), candidates.end(),
                        [](const WordItem &item) { return item.word == "撒谎那个"; }));
}

TEST_CASE(QuanpinDictionaryAutocorrectLeavesLegalInputsUntouched)
{
    const auto db_path = CreateAutocorrectDatabase();
    QuanpinDictionary dictionary(test::Utf8(db_path));
    const unsigned both = quanpin::kAutocorrectTransposition | quanpin::kAutocorrectNeighbor;

    // Corrected results live in their own cache slot; a plain spelling never
    // sees the fallback tail produced for the corrected spelling.
    (void)dictionary.query("sahng", "sa'h'n'g", both);
    const auto plain = dictionary.query("shang", "shang", both);
    REQUIRE(!plain.empty());
    REQUIRE_EQ(plain.front().word, std::string("上"));
    REQUIRE(std::none_of(plain.begin(), plain.end(), [](const WordItem &item) { return item.word == "撒谎那个"; }));

    const auto manual = dictionary.query("xi'an", "xi'an", both);
    const auto manual_off = dictionary.query("xi'an", "xi'an", 0u);
    REQUIRE_EQ(manual.size(), manual_off.size());
}

namespace
{
struct HelpcodeSample
{
    char help_code = 0;
    std::string first_matched_a;
    std::string first_matched_b;
    std::string unmatched;
};

// Picks a helpcode letter that has at least two candidates matching on the
// leading part plus one candidate that does not match at all, which is enough
// to observe how the single-helpcode reordering treats the buckets.
HelpcodeSample FindSplitBucketHelpcode()
{
    const auto &keymap = HelpcodeUtils::helpcode_keymap();
    for (char letter = 'a'; letter <= 'z'; ++letter)
    {
        std::vector<std::string> firsts;
        std::vector<std::string> unmatched;
        for (const auto &entry : keymap)
        {
            if (entry.second.size() < 2 || HelpcodeUtils::count_han_chars(entry.first) != 1)
            {
                continue;
            }
            if (entry.second[0] == letter)
            {
                firsts.push_back(entry.first);
            }
            else if (entry.second[1] != letter)
            {
                unmatched.push_back(entry.first);
            }
        }
        if (firsts.size() < 2 || unmatched.empty())
        {
            continue;
        }
        std::sort(firsts.begin(), firsts.end());
        std::sort(unmatched.begin(), unmatched.end());
        return {letter, firsts[0], firsts[1], unmatched[0]};
    }
    return {};
}
} // namespace

// Mirrors typing a helpcode that filters out the candidates above the AI
// suggestion: the suggestion has to move up with them instead of staying pinned
// to its old slot.
TEST_CASE(SingleHelpcodeReorderKeepsAiSuggestionAheadOfLaterCandidates)
{
    const HelpcodeSample sample = FindSplitBucketHelpcode();
    REQUIRE(sample.help_code != 0);

    const std::vector<WordItem> base{
        WordItem("py", sample.unmatched, 10, CandidateSource::Database),
        WordItem("py", sample.first_matched_a, 1, CandidateSource::AiSuggestion),
        WordItem("py", sample.first_matched_b, 8, CandidateSource::Database),
    };

    const auto result = HelpcodeUtils::reorder_candidates_with_single_helpcode(base, std::string(1, sample.help_code));

    REQUIRE_EQ(result.size(), base.size());
    REQUIRE(result[0].source == CandidateSource::AiSuggestion);
    REQUIRE_EQ(result[1].word, sample.first_matched_b);
    REQUIRE_EQ(result[2].word, sample.unmatched);
}

TEST_CASE(DoubleHelpcodeFilterPreservesCloudAndAiRelativeOrder)
{
    const auto &keymap = HelpcodeUtils::helpcode_keymap();
    std::unordered_map<std::string, std::vector<std::string>> words_by_helpcode;
    for (const auto &entry : keymap)
    {
        if (entry.second.size() < 2 || HelpcodeUtils::count_han_chars(entry.first) != 1)
        {
            continue;
        }
        words_by_helpcode[entry.second].push_back(entry.first);
    }

    std::string help_codes;
    std::vector<std::string> words;
    for (const auto &entry : words_by_helpcode)
    {
        if (entry.second.size() >= 3 && (help_codes.empty() || entry.first < help_codes))
        {
            help_codes = entry.first;
            words = entry.second;
        }
    }
    REQUIRE(!help_codes.empty());
    std::sort(words.begin(), words.end());
    words.resize(3);

    const std::vector<WordItem> base{
        WordItem("py", words[0], 10, CandidateSource::Database),
        WordItem("py", words[1], 1, CandidateSource::CloudSuggestion),
        WordItem("py", words[2], 1, CandidateSource::AiSuggestion),
    };

    const auto result = HelpcodeUtils::filter_candidates_with_double_helpcodes(base, help_codes);

    REQUIRE_EQ(result.size(), base.size());
    REQUIRE_EQ(result[0].word, words[0]);
    REQUIRE(result[1].source == CandidateSource::CloudSuggestion);
    REQUIRE(result[2].source == CandidateSource::AiSuggestion);
}
