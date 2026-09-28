#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"
#include "engine/japanese/romaji_converter.h"
#include "engine/japanese/japanese_sentence_decoder.h"
#include "engine/japanese/japanese_matrix_search.h"
#include "engine/providers/japanese_candidate_provider.h"
#include "engine/schemes/japanese_romaji_scheme.h"
#include "src/session/engine_input_session.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>

namespace
{
UINT VirtualKeyForRomajiChar(char ch)
{
    if (ch == '-')
        return 0xBD; // VK_OEM_MINUS
    if (ch == '\'')
        return 0xDE; // VK_OEM_7
    return static_cast<UINT>(static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(ch))));
}

void InputRomaji(JapaneseRomajiScheme &scheme, const std::string &keys)
{
    for (const char ch : keys)
        scheme.handle_key(VirtualKeyForRomajiChar(ch), 0, static_cast<WCHAR>(ch));
}

bool ContainsWord(const std::vector<WordItem> &items, const std::string &word)
{
    return std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == word; });
}

size_t WordIndex(const std::vector<WordItem> &items, const std::string &word)
{
    const auto found =
        std::find_if(items.begin(), items.end(), [&](const WordItem &item) { return item.word == word; });
    return found == items.end() ? items.size() : static_cast<size_t>(found - items.begin());
}

std::filesystem::path CreateJapaneseDatabase()
{
    const auto path = std::filesystem::temp_directory_path() / "msime-japanese-provider-test.db";
    std::filesystem::remove(path);
    sqlite3 *db = nullptr;
    if (sqlite3_open(test::Utf8(path).c_str(), &db) != SQLITE_OK)
        throw std::runtime_error("Failed to create Japanese test database.");
    const char *sql = "CREATE TABLE japanese_lexicon(code TEXT,value TEXT,weight INTEGER,PRIMARY KEY(code,value));"
                      "INSERT INTO japanese_lexicon VALUES('qa','亜',20);"
                      "INSERT INTO japanese_lexicon VALUES('qa','会',10);"
                      "INSERT INTO japanese_lexicon VALUES('qwatashi','私',30);"
                      "INSERT INTO japanese_lexicon VALUES('kawaii','かわいい',40);"
                      "INSERT INTO japanese_lexicon VALUES('qkawaii','可愛い',50);";
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (result != SQLITE_OK)
        throw std::runtime_error("Failed to initialize Japanese test database.");
    return path;
}
} // namespace

TEST_CASE(JapaneseRomajiConvertsCommonImeSpellings)
{
    REQUIRE_EQ(japanese::ConvertRomaji("nihongo").hiragana, std::string("にほんご"));
    REQUIRE_EQ(japanese::ConvertRomaji("konnichiha").hiragana, std::string("こんにちは"));
    REQUIRE_EQ(japanese::ConvertRomaji("gakkou").hiragana, std::string("がっこう"));
    REQUIRE_EQ(japanese::ConvertRomaji("shin'you").hiragana, std::string("しんよう"));
    REQUIRE_EQ(japanese::HiraganaToKatakana("にほんご"), std::string("ニホンゴ"));
    REQUIRE_EQ(japanese::HiraganaToRomaji("かわいい"), std::string("kawaii"));
    REQUIRE_EQ(japanese::HiraganaToRomaji("にほんご"), std::string("nihongo"));
}

TEST_CASE(JapaneseRomajiLongVowelKeyTypesChounpu)
{
    // "コーヒー" 里的 "ー" 就是 P 右上角的 '-' 键。
    REQUIRE_EQ(japanese::ConvertRomaji("ko-hi-").hiragana, std::string("こーひー"));
    REQUIRE(japanese::ConvertRomaji("ko-hi-").complete);
    REQUIRE_EQ(japanese::HiraganaToKatakana("こーひー"), std::string("コーヒー"));
    REQUIRE_EQ(japanese::ConvertRomaji("ra-men").hiragana, std::string("らーめん"));
    // '-' 不是元音，前面的单个 n 要收成 ん。
    REQUIRE_EQ(japanese::ConvertRomaji("n-").hiragana, std::string("んー"));
    REQUIRE_EQ(japanese::HiraganaToRomaji("コーヒー"), std::string("ko-hi-"));
}

TEST_CASE(JapaneseRomajiSchemeAcceptsLongVowelKeyInsteadOfPaging)
{
    JapaneseRomajiScheme scheme;
    InputRomaji(scheme, "ko-hi-");
    const auto request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input, std::string("ko-hi-"));
    REQUIRE_EQ(request.segmentation, std::string("こーひー"));
    REQUIRE_EQ(request.key_strokes.size(), static_cast<size_t>(6));

    // 服务端回填编码（造词、光标编辑）时也必须保留 '-'；预编辑显示的是假名转换结果。
    scheme.set_raw_input("ko-hi-", "ko-hi-");
    REQUIRE_EQ(scheme.get_preedit(), std::string("こーひー"));
}

TEST_CASE(JapaneseRomajiSchemeStartsCompositionFromBareLongVowelKey)
{
    // 空编码下单独按 '-' 也要起头组合，好让候选框弹出来。
    JapaneseRomajiScheme scheme;
    InputRomaji(scheme, "-");
    const auto request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input, std::string("-"));
    REQUIRE_EQ(request.segmentation, std::string("ー"));
}

TEST_CASE(JapaneseProviderOffersChounpuThenPlainHyphenForBareMinus)
{
    const auto path = CreateJapaneseDatabase();
    {
        JapaneseCandidateProvider provider(test::Utf8(path));
        QueryRequest request;
        request.scheme = SchemeType::JapaneseRomaji;
        request.raw_input = "-";
        request.raw_input_with_cases = "-";
        request.valid = true;
        const auto candidates = provider.query(request);
        REQUIRE_EQ(candidates.size(), static_cast<size_t>(2));
        REQUIRE_EQ(candidates[0].word, std::string("ー"));
        REQUIRE_EQ(candidates[1].word, std::string("-"));
    }
    std::filesystem::remove(path);
}

TEST_CASE(JapaneseRomajiSchemePreservesTypedCodeAndShowsKanaSegmentation)
{
    JapaneseRomajiScheme scheme;
    InputRomaji(scheme, "nihongo");
    const auto request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.scheme, SchemeType::JapaneseRomaji);
    REQUIRE_EQ(request.raw_input, std::string("nihongo"));
    REQUIRE_EQ(request.segmentation, std::string("にほんご"));
}

TEST_CASE(JapanesePreeditPreservesTypedCasesAcrossEngineAndCandidateUi)
{
    EngineInputSession session(SchemeType::JapaneseRomaji);
    for (const char ch : std::string("NiHonGo"))
    {
        const char vk = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        session.handle_key(static_cast<UINT>(vk), 0, static_cast<WCHAR>(ch));
    }
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("nihongo"));
    REQUIRE_EQ(session.get_pinyin_sequence_with_cases(), std::string("NiHonGo"));
    // 内联组合（与回车提交）显示实时假名转换；罗马音仍保留在 raw input 中供查询与编辑。
    REQUIRE_EQ(session.get_pinyin_segmentation_with_cases(), std::string("にほんご"));
}

TEST_CASE(TemporaryJapaneseSessionDoesNotMutateChineseSession)
{
    EngineInputSession chinese(SchemeType::Quanpin);
    chinese.handle_key('N', 0, L'n');
    chinese.handle_key('I', 0, L'i');

    const auto japanese = std::make_shared<EngineInputSession>(SchemeType::JapaneseRomaji);
    for (const char ch : std::string("nihongo"))
    {
        const char vk = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        japanese->handle_key(static_cast<UINT>(vk), 0, static_cast<WCHAR>(ch));
    }

    REQUIRE_EQ(chinese.current_scheme_type(), SchemeType::Quanpin);
    REQUIRE_EQ(chinese.get_pinyin_sequence_with_cases(), std::string("ni"));
    REQUIRE_EQ(japanese->current_scheme_type(), SchemeType::JapaneseRomaji);
    REQUIRE_EQ(japanese->get_pinyin_sequence_with_cases(), std::string("nihongo"));
}

TEST_CASE(JapaneseProviderCombinesGeneratedKanaAndSqliteCandidates)
{
    const auto path = CreateJapaneseDatabase();
    {
        JapaneseCandidateProvider provider(test::Utf8(path));
        QueryRequest kana;
        kana.scheme = SchemeType::JapaneseRomaji;
        kana.raw_input = "nihongo";
        kana.raw_input_with_cases = "nihongo";
        kana.valid = true;
        const auto kana_candidates = provider.query(kana);
        REQUIRE(ContainsWord(kana_candidates, "にほんご"));
        // Auto + regular word: hiragana leads, katakana is hidden until F9 so the
        // kanji/word candidate keeps position 2.
        REQUIRE(!ContainsWord(kana_candidates, "ニホンゴ"));
        QueryRequest kata_form = kana;
        kata_form.japanese_kana_form = JapaneseKanaForm::Katakana;
        const auto kata_candidates = provider.query(kata_form);
        REQUIRE_EQ(kata_candidates[0].word, std::string("ニホンゴ"));
        REQUIRE_EQ(kata_candidates[1].word, std::string("にほんご"));

        QueryRequest single_kana;
        single_kana.scheme = SchemeType::JapaneseRomaji;
        single_kana.raw_input = "ka";
        single_kana.raw_input_with_cases = "Ka";
        single_kana.valid = true;
        const auto single_kana_candidates = provider.query(single_kana);
        REQUIRE(single_kana_candidates.size() >= 2);
        // Candidate 1 is the kana, candidate 2 is the matching word (not katakana).
        REQUIRE_EQ(single_kana_candidates[0].word, std::string("か"));
        REQUIRE_EQ(single_kana_candidates[1].word, std::string("かわいい"));
        REQUIRE_EQ(single_kana_candidates[0].pinyin, std::string("Ka"));

        QueryRequest direct;
        direct.scheme = SchemeType::JapaneseRomaji;
        direct.raw_input = "qa";
        direct.raw_input_with_cases = "qa";
        direct.valid = true;
        const auto direct_candidates = provider.query(direct);
        REQUIRE_EQ(direct_candidates.size(), static_cast<size_t>(2));
        REQUIRE_EQ(direct_candidates[0].word, std::string("亜"));
        REQUIRE_EQ(direct_candidates[1].word, std::string("会"));

        QueryRequest prefix;
        prefix.scheme = SchemeType::JapaneseRomaji;
        prefix.raw_input = "qwat";
        prefix.raw_input_with_cases = "qwat";
        prefix.valid = true;
        const auto prefix_candidates = provider.query(prefix);
        bool found_watashi = false;
        for (const auto &item : prefix_candidates)
            found_watashi = found_watashi || item.word == "私";
        REQUIRE(found_watashi);

        QueryRequest fuzzy;
        fuzzy.scheme = SchemeType::JapaneseRomaji;
        fuzzy.raw_input = "kaw";
        fuzzy.raw_input_with_cases = "kaw";
        fuzzy.valid = true;
        const auto fuzzy_candidates = provider.query(fuzzy);
        REQUIRE(ContainsWord(fuzzy_candidates, "か"));
        REQUIRE(ContainsWord(fuzzy_candidates, "かわいい") || ContainsWord(fuzzy_candidates, "可愛い"));
        const size_t phrase_index =
            (std::min)(WordIndex(fuzzy_candidates, "かわいい"), WordIndex(fuzzy_candidates, "可愛い"));
        // Kana is always candidate 1; the decoded phrase follows it.
        REQUIRE(WordIndex(fuzzy_candidates, "か") < phrase_index);
    }
    std::filesystem::remove(path);
}

TEST_CASE(JapaneseRomajiPrefixMapsToKanaLikeHalfSpellingIds)
{
    const auto kana = japanese::KanaForRomajiPrefix("k");
    bool has_ka = false;
    bool has_kyo = false;
    for (const auto &item : kana)
    {
        has_ka = has_ka || item == "か";
        has_kyo = has_kyo || item == "きょ";
    }
    REQUIRE(has_ka);
    REQUIRE(has_kyo);
    REQUIRE(japanese::KanaForRomajiPrefix("").empty());
}

TEST_CASE(JapaneseProviderShowsPhrasesBeforeConvertedKana)
{
    const auto path = CreateJapaneseDatabase();
    {
        JapaneseCandidateProvider provider(test::Utf8(path));
        QueryRequest request;
        request.scheme = SchemeType::JapaneseRomaji;
        request.raw_input = "nihong";
        request.raw_input_with_cases = "nihong";
        request.valid = true;
        const auto candidates = provider.query(request);
        REQUIRE(ContainsWord(candidates, "にほん"));
        // Auto keeps katakana hidden for a regular (non-loanword) reading.
        REQUIRE(!ContainsWord(candidates, "ニホン"));
        QueryRequest kata_request = request;
        kata_request.japanese_kana_form = JapaneseKanaForm::Katakana;
        const auto kata_candidates = provider.query(kata_request);
        REQUIRE(ContainsWord(kata_candidates, "ニホン"));
    }
    std::filesystem::remove(path);
}

TEST_CASE(JapaneseMatrixSearchDecodesWholeSentenceWhenModelAvailable)
{
    const auto workspace = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const auto model = workspace / "MetasequoiaImeDict" / "out" / "dict_japanese.dat";
    if (!std::filesystem::is_regular_file(model))
        return;

    japanese::JapaneseSentenceDecoder decoder(test::Utf8(model));
    REQUIRE(decoder.ready());
    japanese::JapaneseMatrixSearch search(decoder);
    const auto complete = search.Search("nihongo", 12);
    REQUIRE(!complete.empty());
    bool found = false;
    for (const auto &candidate : complete)
        found = found || candidate.text == "日本語";
    REQUIRE(found);

    const auto prefix = search.Search("nihong", 12);
    bool found_prefix = false;
    for (const auto &candidate : prefix)
        found_prefix = found_prefix || candidate.text == "日本語";
    REQUIRE(found_prefix);

    const auto fuzzy = decoder.PrefixLemmas("かわ", 32);
    bool found_kawaii = false;
    for (const auto &lemma : fuzzy)
        found_kawaii =
            found_kawaii || lemma.surface == "可愛い" || lemma.reading == "かわいい" || lemma.surface == "かわいい";
    REQUIRE(found_kawaii);

    const auto exact = decoder.ExactLemmas("にほん", 64);
    const auto best_exact = decoder.ExactLemmas("にほん", 1);
    REQUIRE(!exact.empty());
    REQUIRE(best_exact.size() == 1);
    REQUIRE(best_exact.front().word_cost == exact.front().word_cost);
    for (size_t index = 1; index < exact.size(); ++index)
        REQUIRE(exact[index - 1].word_cost <= exact[index].word_cost);

    const auto short_prefix = decoder.PrefixLemmas("か", 32);
    REQUIRE(short_prefix.size() == 32);
    for (size_t index = 1; index < short_prefix.size(); ++index)
        REQUIRE(short_prefix[index - 1].word_cost <= short_prefix[index].word_cost);

    const auto pending_consonant = search.Search("k", 12);
    REQUIRE(!pending_consonant.empty());
}

TEST_CASE(JapaneseSentenceDecoderReadsGeneratedMozcModelWhenAvailable)
{
    const auto workspace = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const auto model = workspace / "MetasequoiaImeDict" / "out" / "dict_japanese.dat";
    if (!std::filesystem::is_regular_file(model))
        return;

    japanese::JapaneseSentenceDecoder decoder(test::Utf8(model));
    REQUIRE(decoder.ready());
    const auto candidates = decoder.Decode("にほんご", 12);
    REQUIRE(!candidates.empty());
    bool found = false;
    for (const auto &candidate : candidates)
        found = found || candidate.text == "日本語";
    REQUIRE(found);
}
