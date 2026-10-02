#include "tests/includes/test_framework.h"
#include "src/session/engine_input_session.h"
#include "src/config/ime_config.h"
#include "engine/common/helpcode_utils.h"
#include "engine/quanpin/quanpin_query.h"
#include "engine/shuangpin/shuangpin_dictionary.h"
#include "src/ipc/candidate_selection_policy.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <system_error>

namespace
{
void InputLetters(EngineInputSession &session, const std::string &keys)
{
    for (const char ch : keys)
    {
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const char upper = is_upper ? ch : static_cast<char>(ch - ('a' - 'A'));
        session.handle_key(static_cast<UINT>(upper), 0, static_cast<WCHAR>(ch));
    }
}

void InputSequence(EngineInputSession &session, const std::string &keys)
{
    for (const char ch : keys)
    {
        if (ch == '\'')
        {
            session.handle_key(VK_OEM_7, 0, L'\'');
            continue;
        }
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const char upper = is_upper ? ch : static_cast<char>(ch - ('a' - 'A'));
        session.handle_key(static_cast<UINT>(upper), 0, static_cast<WCHAR>(ch));
    }
}

// 把 ime 配置重定向到一次性目录：SetConfiguredFuzzyPinyinRule 会真实落盘，
// 不能写进开发机的用户配置。用 METASEQUOIA_IME_CONFIG_DIR 而不是改 LOCALAPPDATA：
// 后者已经不是配置位置的权威（安装器会把数据目录写进 HKLM），而且它会把引擎词库
// 一起带走——本用例的候选断言正需要真实词库。
class ScopedConfigRoot
{
  public:
    ScopedConfigRoot()
    {
        wchar_t buffer[32768];
        const DWORD length = GetEnvironmentVariableW(L"METASEQUOIA_IME_CONFIG_DIR", buffer, 32768);
        had_previous_ = length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        previous_.assign(buffer, length);
        root_ = std::filesystem::temp_directory_path() /
                (L"msime-模糊音注入测试-" + std::to_wstring(GetCurrentProcessId()));
        const auto data_dir = root_ / L"metasequoiaime";
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(data_dir, ec);
        std::filesystem::copy_file(MSIME_DEFAULT_CONFIG_PATH, data_dir / L"config.default.toml",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        // 拷贝失败（如仓库路径含非 ASCII 字符被 ACP 转换破坏）必须在源头报出来，
        // 否则后面的 SetConfiguredFuzzyPinyinRule 会以无关断言的形式失败。
        REQUIRE(!ec);
        SetEnvironmentVariableW(L"METASEQUOIA_IME_CONFIG_DIR", data_dir.c_str());
    }
    ~ScopedConfigRoot()
    {
        SetEnvironmentVariableW(L"METASEQUOIA_IME_CONFIG_DIR", had_previous_ ? previous_.c_str() : nullptr);
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    ScopedConfigRoot(const ScopedConfigRoot &) = delete;
    ScopedConfigRoot &operator=(const ScopedConfigRoot &) = delete;

  private:
    std::wstring previous_;
    bool had_previous_ = false;
    std::filesystem::path root_;
};

bool HasCandidateWithCanonicalPrefix(const EngineInputSession &session, const char *prefix)
{
    for (const auto &item : session.get_candidates())
    {
        if (item.canonical_pinyin.rfind(prefix, 0) == 0)
            return true;
    }
    return false;
}
} // namespace

TEST_CASE(EngineSessionRejectsOnlineResponsesFromOtherSessionsAndPriorCompositions)
{
    EngineInputSession first(SchemeType::Quanpin), second(SchemeType::Quanpin);
    InputLetters(first, "nihao");
    InputLetters(second, "nihao");
    const auto query = first.online_query();
    REQUIRE(query.has_value());
    REQUIRE(!second.apply_online_candidate(*query, "跨会话测试候选", CandidateSource::CloudSuggestion));
    first.reset_state();
    InputLetters(first, "nihao");
    REQUIRE(!first.apply_online_candidate(*query, "旧组合测试候选", CandidateSource::CloudSuggestion));
    const auto current = first.online_query();
    REQUIRE(current.has_value());
    REQUIRE(first.apply_online_candidate(*current, "本次测试候选", CandidateSource::CloudSuggestion));
    const auto &candidates = first.get_candidates();
    const auto accepted = std::find_if(candidates.begin(), candidates.end(), [](const auto &item) {
        return item.word == "本次测试候选" && item.source == CandidateSource::CloudSuggestion;
    });
    REQUIRE(accepted != candidates.end());
    REQUIRE(!first.apply_online_candidate(*current, "本次测试候选", CandidateSource::CloudSuggestion));
}

TEST_CASE(EngineShuangpinSessionYoFindsDictionaryCandidate)
{
    EngineInputSession session(SchemeType::Shuangpin, GetXiaoheShuangpinProfile());
    InputLetters(session, "yo");

    const auto &candidates = session.get_candidates();
    REQUIRE(std::any_of(candidates.begin(), candidates.end(),
                        [](const auto &item) { return item.word == "哟" && item.canonical_pinyin == "yo"; }));
    REQUIRE_EQ(session.get_quanpin(), std::string("yo"));
}

TEST_CASE(EngineShuangpinSessionContinuesCompositionWithoutHelpcode)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputLetters(session, "xitele");

    const auto transition = session.advance_composition_after_selection("xi", "西", "xi");
    REQUIRE(transition.continues_composition);
    REQUIRE_EQ(transition.consumed_raw_input_with_cases, std::string("xi"));
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("tele"));
}

TEST_CASE(EngineShuangpinMicrosoftSemicolonFinalStaysInConsumedInput)
{
    EngineInputSession session(SchemeType::Shuangpin, GetMicrosoftShuangpinProfile());
    // In the Microsoft profile ';' is the "ing" final: "b;" is bing, "ni" is ni.
    session.handle_key('B', 0, L'b');
    session.handle_key(VK_OEM_1, 0, L';');
    session.handle_key('N', 0, L'n');
    session.handle_key('I', 0, L'i');

    const auto transition = session.advance_composition_after_selection("b;", "冰", "bing");
    REQUIRE(transition.continues_composition);
    REQUIRE_EQ(transition.consumed_raw_input_with_cases, std::string("b;"));
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("ni"));
}

// 模糊音是会话级注入：总开关 + 规则位两层。总开关关 → 聚合 getter 全零，候选不出现；
// 开规则 + 开总开关 → zang 候选出现（全拼 zhang / 双拼 vh，AC1/AC2）；仅关总开关（规则保留）→
// 候选消失；重开总开关 → 恢复（AC2b）。
TEST_CASE(EngineSessionAppliesFuzzyPinyinRulesForQuanpinAndShuangpin)
{
    ScopedConfigRoot config_root;
    InitImeConfig();
    REQUIRE(!GetConfiguredFuzzyPinyinEnabled());
    REQUIRE_EQ(GetConfiguredFuzzyPinyinOptions().rules, 0u);

    // A：默认全关，zhang 只命中 zhang 读音。
    {
        EngineInputSession quanpin(SchemeType::Quanpin);
        InputLetters(quanpin, "zhang");
        REQUIRE(!HasCandidateWithCanonicalPrefix(quanpin, "zang"));
    }

    // 开规则、总开关仍关：getter 全零，行为不变。
    REQUIRE(SetConfiguredFuzzyPinyinRule("fuzzy_z_zh", true));
    REQUIRE_EQ(GetConfiguredFuzzyPinyinOptions().rules, 0u);
    {
        EngineInputSession quanpin(SchemeType::Quanpin);
        InputLetters(quanpin, "zhang");
        REQUIRE(!HasCandidateWithCanonicalPrefix(quanpin, "zang"));
    }

    // B：开总开关 —— 首次启用会播种全部规则（产品语义）；本用例要的是「只有 z/zh 一条
    // 规则」的纯净场景，播种后把其余规则关回去，此后总开关翻动不再碰规则位。
    REQUIRE(SetConfiguredFuzzyPinyinEnabled(true));
    for (const char *key : {"fuzzy_c_ch", "fuzzy_s_sh", "fuzzy_n_l", "fuzzy_f_h", "fuzzy_r_l", "fuzzy_an_ang",
                            "fuzzy_en_eng", "fuzzy_in_ing", "fuzzy_ian_iang", "fuzzy_uan_uang"})
        REQUIRE(SetConfiguredFuzzyPinyinRule(key, false));
    REQUIRE_EQ(GetConfiguredFuzzyPinyinRuleStates().rules,
               static_cast<std::uint32_t>(metasequoia::FuzzyPinyinRule::Z_ZH));
    {
        EngineInputSession quanpin(SchemeType::Quanpin);
        InputLetters(quanpin, "zhang");
        REQUIRE(HasCandidateWithCanonicalPrefix(quanpin, "zang"));
    }
    {
        EngineInputSession shuangpin(SchemeType::Shuangpin);
        InputLetters(shuangpin, "vh");
        REQUIRE(HasCandidateWithCanonicalPrefix(shuangpin, "zang"));
    }

    // A：仅关总开关（规则位保留）—— 候选消失。
    REQUIRE(SetConfiguredFuzzyPinyinEnabled(false));
    REQUIRE_EQ(GetConfiguredFuzzyPinyinRuleStates().rules,
               static_cast<std::uint32_t>(metasequoia::FuzzyPinyinRule::Z_ZH));
    {
        EngineInputSession quanpin(SchemeType::Quanpin);
        InputLetters(quanpin, "zhang");
        REQUIRE(!HasCandidateWithCanonicalPrefix(quanpin, "zang"));
    }

    // B：重开总开关 —— 恢复。
    REQUIRE(SetConfiguredFuzzyPinyinEnabled(true));
    {
        EngineInputSession quanpin(SchemeType::Quanpin);
        InputLetters(quanpin, "zhang");
        REQUIRE(HasCandidateWithCanonicalPrefix(quanpin, "zang"));
    }

    // 收尾：总开关与规则位归零，不留脏状态给进程内后续用例。
    REQUIRE(SetConfiguredFuzzyPinyinEnabled(false));
    REQUIRE(SetConfiguredFuzzyPinyinRule("fuzzy_z_zh", false));
    REQUIRE_EQ(GetConfiguredFuzzyPinyinOptions().rules, 0u);
    REQUIRE(!GetConfiguredFuzzyPinyinEnabled());
}

TEST_CASE(CloudCandidateNeverEntersCreatingWordMode)
{
    REQUIRE(!FanyImeIpc::ShouldEnterCreatingWord(CandidateSource::CloudSuggestion, true));
    REQUIRE(!FanyImeIpc::ShouldEnterCreatingWord(CandidateSource::CloudSuggestion, false));
    REQUIRE(FanyImeIpc::ShouldEnterCreatingWord(CandidateSource::Database, true));
    REQUIRE(!FanyImeIpc::ShouldEnterCreatingWord(CandidateSource::Database, false));
}

TEST_CASE(OnlyGeneratedSentencesWithBothCanonicalHalvesStoreAtTheEarlyReturn)
{
    // Generated is the only special source that can end a creating-word session
    // with a real reading; the other early-return sources are self-contained.
    REQUIRE(FanyImeIpc::ShouldStoreEarlyReturnPhrase(CandidateSource::Generated, true, "xi", "ni'hao'zhong'guo"));
    REQUIRE(!FanyImeIpc::ShouldStoreEarlyReturnPhrase(CandidateSource::Emoji, true, "xi", "ni'hao'zhong'guo"));
    REQUIRE(!FanyImeIpc::ShouldStoreEarlyReturnPhrase(CandidateSource::QuickPhrase, true, "xi", "ni'hao'zhong'guo"));
    // Fallback whole-sentence candidates take the normal path, where the
    // creating-word completion block persists them.
    REQUIRE(!FanyImeIpc::ShouldStoreEarlyReturnPhrase(CandidateSource::Fallback, true, "xi", "ni'hao'zhong'guo"));
    // 没有造词前缀的整句也要落库：它自己就是完整的一条，而词库里没有它那一行，
    // 调频改不到它。前缀为空时不看前缀读音。
    REQUIRE(FanyImeIpc::ShouldStoreEarlyReturnPhrase(CandidateSource::Generated, false, "", "ni'hao'zhong'guo"));
    REQUIRE(FanyImeIpc::ShouldStoreEarlyReturnPhrase(CandidateSource::Generated, false, "xi", "ni'hao'zhong'guo"));
    // 造词中而前缀没有读音，或整句自己没有读音，都拼不出完整读音，不能落库。
    REQUIRE(!FanyImeIpc::ShouldStoreEarlyReturnPhrase(CandidateSource::Generated, true, "", "ni'hao'zhong'guo"));
    REQUIRE(!FanyImeIpc::ShouldStoreEarlyReturnPhrase(CandidateSource::Generated, true, "xi", ""));
    REQUIRE(!FanyImeIpc::ShouldStoreEarlyReturnPhrase(CandidateSource::Generated, false, "", ""));
}

TEST_CASE(StandaloneWholeSentenceCandidatesAreLearnedWithinThePhraseLengthCap)
{
    // 两条整句来源都要落库：词格走提前返回，Google 解码器走普通路径，同一条规则。
    REQUIRE(FanyImeIpc::ShouldStoreStandaloneSentence(CandidateSource::Generated, "na'yi'tiao"));
    REQUIRE(FanyImeIpc::ShouldStoreStandaloneSentence(CandidateSource::Fallback, "na'yi'tiao"));
    REQUIRE(FanyImeIpc::ShouldStoreStandaloneSentence(CandidateSource::Generated, "na"));
    // 词库里本来就有的候选不用落库，调频改得到它自己那一行。
    REQUIRE(!FanyImeIpc::ShouldStoreStandaloneSentence(CandidateSource::Database, "na'yi'tiao"));
    REQUIRE(!FanyImeIpc::ShouldStoreStandaloneSentence(CandidateSource::UserDatabase, "na'yi'tiao"));
    REQUIRE(!FanyImeIpc::ShouldStoreStandaloneSentence(CandidateSource::CloudSuggestion, "na'yi'tiao"));
    // 没有读音就拼不出词条。
    REQUIRE(!FanyImeIpc::ShouldStoreStandaloneSentence(CandidateSource::Generated, ""));
    // 长度上限：7 音节落库，8 音节不落。
    REQUIRE(FanyImeIpc::CountCanonicalSyllables("a'a'a'a'a'a'a") == 7);
    REQUIRE(FanyImeIpc::ShouldStoreStandaloneSentence(CandidateSource::Generated, "a'a'a'a'a'a'a"));
    REQUIRE(!FanyImeIpc::ShouldStoreStandaloneSentence(CandidateSource::Generated, "a'a'a'a'a'a'a'a"));
}

TEST_CASE(MixedAsyncCandidatesKeepReservedSlotsForEveryArrivalOrder)
{
    const auto local = [](std::string word) { return WordItem("ni", std::move(word), 100); };
    const auto english = [] { return WordItem("ni", "nice", 1, CandidateSource::EnglishDictionary); };
    const auto emoji = [] { return WordItem("ni", "\xF0\x9F\x98\x80", 1, CandidateSource::Emoji); };
    const auto cloud = [] { return WordItem("ni", "云候选", 1, CandidateSource::CloudSuggestion); };
    const auto ai = [] { return WordItem("ni", "AI联想", 1, CandidateSource::AiSuggestion); };

    std::vector<WordItem> items = {local("你"), english(), local("呢"), ai(), cloud()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[0].word, std::string("你"));
    REQUIRE_EQ(items[1].source, CandidateSource::CloudSuggestion);
    REQUIRE_EQ(items[2].source, CandidateSource::AiSuggestion);
    REQUIRE_EQ(items[3].source, CandidateSource::EnglishDictionary);

    items = {local("你"), ai(), local("呢"), english()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[1].source, CandidateSource::EnglishDictionary);
    REQUIRE_EQ(items[2].source, CandidateSource::AiSuggestion);

    items = {local("你"), cloud(), local("呢"), english()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[1].source, CandidateSource::CloudSuggestion);
    REQUIRE_EQ(items[2].source, CandidateSource::EnglishDictionary);
}

TEST_CASE(EnglishWeightNeverOutranksChineseAcrossDictionaries)
{
    // 用户反馈：zai 下 zaire 选过一次后永远排第一。英文权重和中文权重不是一个量纲，再大也不能
    // 让英文越过默认位置。
    std::vector<WordItem> items = {
        WordItem("zai", "在", 19195987),
        WordItem("zai", "再", 2681503),
        WordItem("zaire", "zaire", 19196987, CandidateSource::EnglishDictionary),
    };
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[0].word, std::string("在"));
    REQUIRE_EQ(items[1].word, std::string("zaire"));
}

TEST_CASE(LearnedEnglishSlotCanMakeEnglishTheFirstMixedCandidate)
{
    const auto local = [](std::string word) { return WordItem("github", std::move(word), 100); };
    const auto cloud = [] { return WordItem("github", "云候选", 1, CandidateSource::CloudSuggestion); };
    const auto ai = [] { return WordItem("github", "AI联想", 1, CandidateSource::AiSuggestion); };

    std::vector<WordItem> items = {
        local("个"), cloud(), ai(), WordItem("github", "GitHub", 1, CandidateSource::EnglishDictionary), local("给"),
    };
    FanyImeIpc::EnglishPlacement placement;
    placement.slot = 0;
    placement.input = "github";
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);

    REQUIRE_EQ(items[0].word, std::string("GitHub"));
    REQUIRE_EQ(items[1].word, std::string("个"));
    REQUIRE_EQ(items[2].source, CandidateSource::CloudSuggestion);
    REQUIRE_EQ(items[3].source, CandidateSource::AiSuggestion);
    REQUIRE_EQ(*FanyImeIpc::SlottedEnglishIndex(items), size_t{0});
}

TEST_CASE(LearnedEnglishSlotMovesEnglishBehindItsDefaultPosition)
{
    const auto local = [](std::string word) { return WordItem("ni", std::move(word), 100); };
    const auto english = [] { return WordItem("ni", "nice", 1, CandidateSource::EnglishDictionary); };
    const auto emoji = [] { return WordItem("ni", "\xF0\x9F\x98\x80", 1, CandidateSource::Emoji); };

    std::vector<WordItem> items = {local("你"), local("呢"), local("泥"), local("尼"), english(), emoji()};
    FanyImeIpc::EnglishPlacement placement;
    placement.slot = 3;
    placement.input = "ni";
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    // 槽位是列表下标：英文挪到下标 3；emoji 留在默认的下一位。
    REQUIRE_EQ(items[0].word, std::string("你"));
    REQUIRE_EQ(items[1].source, CandidateSource::Emoji);
    REQUIRE_EQ(items[3].word, std::string("nice"));
    REQUIRE_EQ(*FanyImeIpc::SlottedEnglishIndex(items), size_t{3});
}

TEST_CASE(PinyinInputPrefersExactEnglishAndParksCompletionsOnTheFirstPage)
{
    const auto local = [](std::string word) { return WordItem("bus", std::move(word), 100); };
    FanyImeIpc::EnglishPlacement placement;
    placement.require_exact = true;
    placement.input = "bus";
    placement.page_size = 5;

    // 精确匹配占槽位、默认紧跟第一个中文候选，其余补全词（哪怕权重更高）排到末尾。
    std::vector<WordItem> items = {
        local("不是"),
        WordItem("business", "business", 900000, CandidateSource::EnglishDictionary),
        local("不少"),
        WordItem("bus", "bus", 0, CandidateSource::EnglishDictionary),
    };
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items[0].word, std::string("不是"));
    REQUIRE_EQ(items[1].word, std::string("bus"));
    REQUIRE_EQ(items[2].word, std::string("不少"));
    REQUIRE_EQ(items[3].word, std::string("business"));
    REQUIRE_EQ(*FanyImeIpc::SlottedEnglishIndex(items), size_t{1});

    // 没有精确匹配：最前的补全词默认放在首页末位（每页 5 个 → 下标 4），不沉到列表底部。
    placement.input = "zai";
    const auto zai_items = [] {
        return std::vector<WordItem>{
            WordItem("zai", "在", 19195987), WordItem("zaire", "zaire", 19196987, CandidateSource::EnglishDictionary),
            WordItem("zai", "再", 2681503),  WordItem("zai", "载", 127412),
            WordItem("zai", "灾", 83099),    WordItem("zai", "宰", 58463),
            WordItem("zai", "崽", 1000),
        };
    };
    items = zai_items();
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items[0].word, std::string("在"));
    REQUIRE_EQ(items[1].word, std::string("再"));
    REQUIRE_EQ(items[4].word, std::string("zaire"));
    REQUIRE_EQ(items[5].word, std::string("宰"));
    REQUIRE_EQ(*FanyImeIpc::SlottedEnglishIndex(items), size_t{4});

    // 学到的槽位优先于默认位置，补全词可以一直升到首位。
    placement.slot = 0;
    items = zai_items();
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items[0].word, std::string("zaire"));
    REQUIRE_EQ(items[1].word, std::string("在"));

    // 槽位不超过首页末位。
    placement.slot = 9;
    items = zai_items();
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(*FanyImeIpc::SlottedEnglishIndex(items), size_t{4});
}

TEST_CASE(FixedEnglishCandidateKeepsItsMixedCandidatePosition)
{
    const auto local = [](std::string word) { return WordItem("github", std::move(word), 100); };
    const auto cloud = [] { return WordItem("github", "云候选", 1, CandidateSource::CloudSuggestion); };
    const auto ai = [] { return WordItem("github", "AI联想", 1, CandidateSource::AiSuggestion); };
    WordItem english("github", "GitHub", 1100, CandidateSource::EnglishDictionary);
    english.fixed_position = 1;

    std::vector<WordItem> items = {local("个"), english, local("给")};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[0].word, std::string("GitHub"));

    items.push_back(cloud());
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[0].word, std::string("GitHub"));

    items.push_back(ai());
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[0].word, std::string("GitHub"));

    english.fixed_position = 3;
    items = {local("个"), english, ai(), cloud(), local("给")};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[2].word, std::string("GitHub"));
}

TEST_CASE(EmojiMixedCandidateFollowsEnglishAndShiftsWithCloudAndAi)
{
    const auto local = [](std::string word) { return WordItem("ni", std::move(word), 100); };
    const auto english = [] { return WordItem("ni", "nice", 1, CandidateSource::EnglishDictionary); };
    const auto emoji = [] { return WordItem("ni", "\xF0\x9F\x98\x80", 1, CandidateSource::Emoji); };
    const auto cloud = [] { return WordItem("ni", "云候选", 1, CandidateSource::CloudSuggestion); };
    const auto ai = [] { return WordItem("ni", "AI联想", 1, CandidateSource::AiSuggestion); };

    // Base case: English at slot 2, emoji at slot 3.
    std::vector<WordItem> items = {local("你"), emoji(), local("呢"), english()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[1].source, CandidateSource::EnglishDictionary);
    REQUIRE_EQ(items[2].source, CandidateSource::Emoji);

    // Cloud + AI occupy slots 2/3; English and emoji shift to slots 4/5.
    items = {local("你"), emoji(), english(), ai(), cloud()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[1].source, CandidateSource::CloudSuggestion);
    REQUIRE_EQ(items[2].source, CandidateSource::AiSuggestion);
    REQUIRE_EQ(items[3].source, CandidateSource::EnglishDictionary);
    REQUIRE_EQ(items[4].source, CandidateSource::Emoji);

    // Cloud only: English slot 3, emoji slot 4.
    items = {local("你"), emoji(), local("呢"), english(), cloud()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[1].source, CandidateSource::CloudSuggestion);
    REQUIRE_EQ(items[2].source, CandidateSource::EnglishDictionary);
    REQUIRE_EQ(items[3].source, CandidateSource::Emoji);
}

TEST_CASE(KaomojiMixedCandidateSitsRightAfterEmoji)
{
    const auto local = [](std::string word) { return WordItem("ni", std::move(word), 100); };
    const auto english = [] { return WordItem("ni", "nice", 1, CandidateSource::EnglishDictionary); };
    const auto emoji = [] { return WordItem("ni", "\xF0\x9F\x98\x80", 1, CandidateSource::Emoji); };
    const auto kaomoji = [] { return WordItem("ni", "(^_^)", 1, CandidateSource::Kaomoji); };
    const auto cloud = [] { return WordItem("ni", "云候选", 1, CandidateSource::CloudSuggestion); };
    const auto ai = [] { return WordItem("ni", "AI联想", 1, CandidateSource::AiSuggestion); };

    // Base case: English slot 2, emoji slot 3, kaomoji slot 4.
    std::vector<WordItem> items = {local("你"), kaomoji(), emoji(), local("呢"), english()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[1].source, CandidateSource::EnglishDictionary);
    REQUIRE_EQ(items[2].source, CandidateSource::Emoji);
    REQUIRE_EQ(items[3].source, CandidateSource::Kaomoji);

    // Cloud + AI occupy slots 2/3; emoji/kaomoji shift to slots 5/6.
    items = {local("你"), kaomoji(), emoji(), english(), ai(), cloud()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[1].source, CandidateSource::CloudSuggestion);
    REQUIRE_EQ(items[2].source, CandidateSource::AiSuggestion);
    REQUIRE_EQ(items[3].source, CandidateSource::EnglishDictionary);
    REQUIRE_EQ(items[4].source, CandidateSource::Emoji);
    REQUIRE_EQ(items[5].source, CandidateSource::Kaomoji);
}

TEST_CASE(DateTimeMixedCandidateLeadsAsyncCandidates)
{
    const auto local = [](std::string word) { return WordItem("rq", std::move(word), 100); };
    const auto date = [](std::string word) { return WordItem("", std::move(word), 1, CandidateSource::DateTime); };
    const auto english = [] { return WordItem("rq", "rq", 1, CandidateSource::EnglishDictionary); };
    const auto emoji = [] { return WordItem("rq", "\xF0\x9F\x98\x80", 1, CandidateSource::Emoji); };
    const auto cloud = [] { return WordItem("rq", "云候选", 1, CandidateSource::CloudSuggestion); };

    // 日期紧跟首个中文候选，排在云、英文、emoji 前面；其余日期格式追加在末尾。
    std::vector<WordItem> items = {local("人群"), emoji(),       english(),         date("2026年10月2日"),
                                   cloud(),       local("日期"), date("2026-10-02")};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[0].word, std::string("人群"));
    REQUIRE_EQ(items[1].word, std::string("2026年10月2日"));
    REQUIRE_EQ(items[2].source, CandidateSource::CloudSuggestion);
    REQUIRE_EQ(items[3].source, CandidateSource::EnglishDictionary);
    REQUIRE_EQ(items[4].source, CandidateSource::Emoji);
    REQUIRE_EQ(items[5].word, std::string("日期"));
    REQUIRE_EQ(items.back().word, std::string("2026-10-02"));

    // 快捷短语组仍排在日期前面，不被拆开。
    items = {local("人群"), date("2026年10月2日"), WordItem("rq", "如期而至", 100000, CandidateSource::QuickPhrase)};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[1].source, CandidateSource::QuickPhrase);
    REQUIRE_EQ(items[2].source, CandidateSource::DateTime);
}

TEST_CASE(QuickPhraseGroupStaysWholeAndAheadOfAsyncCandidates)
{
    const auto local = [](std::string word) { return WordItem("ni", std::move(word), 100); };
    const auto phrase = [](std::string word) {
        return WordItem("ni", std::move(word), 100000, CandidateSource::QuickPhrase);
    };
    const auto english = [](int weight) { return WordItem("ni", "nice", weight, CandidateSource::EnglishDictionary); };
    const auto cloud = [] { return WordItem("ni", "云候选", 1, CandidateSource::CloudSuggestion); };

    // 组在首位：异步候选排在第一个普通候选之后，不插进组里。
    std::vector<WordItem> items = {phrase("快捷一"), phrase("快捷二"), local("你"), local("呢"), cloud()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[0].word, std::string("快捷一"));
    REQUIRE_EQ(items[1].word, std::string("快捷二"));
    REQUIRE_EQ(items[2].word, std::string("你"));
    REQUIRE_EQ(items[3].source, CandidateSource::CloudSuggestion);

    // 组退到第一个普通候选之后：同一位置上组在异步候选前面。
    items = {local("你"), phrase("快捷一"), local("呢"), cloud()};
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    REQUIRE_EQ(items[1].word, std::string("快捷一"));
    REQUIRE_EQ(items[2].source, CandidateSource::CloudSuggestion);

    // 槽位学到最前的英文也排在首位的组后面。
    items = {phrase("快捷一"), local("你"), english(1)};
    FanyImeIpc::EnglishPlacement placement;
    placement.slot = 0;
    placement.input = "ni";
    FanyImeIpc::NormalizeMixedCandidateOrder(items, 1, placement);
    REQUIRE_EQ(items[0].word, std::string("快捷一"));
    REQUIRE_EQ(items[1].word, std::string("nice"));
    REQUIRE_EQ(items[2].word, std::string("你"));
}

TEST_CASE(JapaneseSingleKanaPairStaysAheadOfCloudCandidate)
{
    std::vector<WordItem> items = {
        WordItem("Ka", "か", 1000000, CandidateSource::Generated),
        WordItem("Ka", "カ", 999999, CandidateSource::Generated),
        WordItem("ka", "蚊", 1, CandidateSource::CloudSuggestion),
        WordItem("ka", "科", 100, CandidateSource::Database),
    };

    FanyImeIpc::NormalizeMixedCandidateOrder(items, 2);
    REQUIRE_EQ(items[0].word, std::string("か"));
    REQUIRE_EQ(items[1].word, std::string("カ"));
    REQUIRE_EQ(items[2].source, CandidateSource::CloudSuggestion);
}

TEST_CASE(EngineShuangpinAiCandidateConsumesFullRawInput)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputLetters(session, "geziaa");

    const auto transition = session.advance_composition_after_selection("geziaa", "鸽子啊", "ge'zi'a");
    REQUIRE(!transition.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("geziaa"));
}

TEST_CASE(EngineShuangpinSessionContinuesCompositionWithSingleHelpcode)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputLetters(session, "xitelea");

    const auto transition = session.advance_composition_after_selection("xi", "西", "xi");
    REQUIRE(transition.continues_composition);
    // The active helpcode is not part of what the selection consumed.
    REQUIRE_EQ(transition.consumed_raw_input_with_cases, std::string("xi"));
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("tele"));
}

TEST_CASE(EngineShuangpinUppercaseSingleHelpcodePrefersSecondCodeMatches)
{
    REQUIRE(HelpcodeUtils::select_helpcode_schema("lantian"));
    EngineInputSession session(SchemeType::Shuangpin);
    InputLetters(session, "nid");
    session.reset_state();
    InputLetters(session, "niD");

    const auto &candidates = session.get_candidates();
    const auto index_of = [&](const std::string &word) {
        const auto found = std::find_if(candidates.begin(), candidates.end(),
                                        [&](const IInputSession::WordItem &item) { return item.word == word; });
        return static_cast<size_t>(std::distance(candidates.begin(), found));
    };

    const size_t ni = index_of("泥");
    const size_t ni_second_code = index_of("溺");
    const size_t ni_other_second_code = index_of("腻");
    REQUIRE(ni < candidates.size());
    REQUIRE(ni_second_code < candidates.size());
    REQUIRE(ni_other_second_code < candidates.size());
    REQUIRE(ni_second_code < ni);
    REQUIRE(ni_other_second_code < ni);
}

TEST_CASE(EngineShuangpinSessionContinuesCompositionWithDoubleHelpcode)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputLetters(session, "xiteleaA");

    const auto transition = session.advance_composition_after_selection("xi", "西", "xi");
    REQUIRE(transition.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("tele"));
}

TEST_CASE(EngineShuangpinDoubleHelpcodesAreDisplayedAsOneSegment)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputLetters(session, "yakP");

    REQUIRE_EQ(session.get_pinyin_segmentation_with_cases(), std::string("ya'kP"));
}

TEST_CASE(EngineShuangpinSessionCloudQueryMatchesLegacyTiming)
{
    EngineInputSession session(SchemeType::Shuangpin);

    InputLetters(session, "xi");
    auto state = session.get_cloud_query_state();
    REQUIRE(state.should_query);

    session.reset_state();
    InputLetters(session, "xia");
    state = session.get_cloud_query_state();
    REQUIRE(!state.should_query);

    session.reset_state();
    InputLetters(session, "xiA");
    state = session.get_cloud_query_state();
    REQUIRE(!state.should_query);
    REQUIRE_EQ(state.cache_key, std::string("xi"));

    session.reset_state();
    InputLetters(session, "xI");
    state = session.get_cloud_query_state();
    REQUIRE(!state.should_query);
}

TEST_CASE(EngineShuangpinSessionCloudQueryDoesNotTriggerWhenHelpcodesApply)
{
    EngineInputSession session(SchemeType::Shuangpin);

    InputLetters(session, "xitelea");
    auto state = session.get_cloud_query_state();
    REQUIRE(!state.should_query);
    REQUIRE_EQ(state.cache_key, std::string("xitele"));

    session.reset_state();
    InputLetters(session, "xiteleaA");
    state = session.get_cloud_query_state();
    REQUIRE(!state.should_query);
    REQUIRE_EQ(state.cache_key, std::string("xitele"));
}

TEST_CASE(EngineShuangpinSessionCloudCommitUsesRawShuangpinSequence)
{
    EngineInputSession session(SchemeType::Shuangpin);

    InputLetters(session, "vh");
    const auto state = session.get_cloud_query_state();

    REQUIRE(state.should_query);
    REQUIRE_EQ(state.query_text, std::string("zhang"));
    REQUIRE_EQ(state.cache_key, std::string("vh"));
    REQUIRE_EQ(state.committed_pinyin, std::string("vh"));
}

TEST_CASE(EngineShuangpinSessionCloudQueryUsesGoogleSpellingForU)
{
    EngineInputSession session(SchemeType::Shuangpin);

    // 小鹤的 nt 转成词库那侧的 canonical 写法是 nve，但云输入和本地 Google 解码器
    // 的音节表里只有 nue：nve'dai'dong'wu 会被它们拆成 nv + e，「虐待动物」变成
    // 「女蛾黛动物」。送出去的那一份必须是 nue'dai'dong'wu，撇号照旧带上。
    InputLetters(session, "ntdddswu");
    const auto state = session.get_cloud_query_state();

    REQUIRE(state.should_query);
    REQUIRE_EQ(state.query_text, std::string("nue'dai'dong'wu"));
    REQUIRE_EQ(state.cache_key, std::string("ntdddswu"));
}

TEST_CASE(EngineShuangpinSessionStoresMultiSyllableDynamicPhrase)
{
    EngineInputSession session(SchemeType::Shuangpin);

    // Cloud and AI candidates are committed with their raw shuangpin sequence.
    // "vsgo" must retain the converted boundary "zhong'guo" before entering
    // the strict canonical-pinyin storage path. "中国" already exists in the
    // shipped dictionary, so this verifies the route without mutating user data.
    REQUIRE_EQ(session.store_user_phrase("vsgo", "中国"), 0);
}

TEST_CASE(EngineQuanpinSessionCloudQueryDoesNotTriggerWhenHelpcodesApply)
{
    EngineInputSession session(SchemeType::Quanpin);

    InputLetters(session, "xiteleA");
    auto state = session.get_cloud_query_state();
    REQUIRE(!state.should_query);
    REQUIRE_EQ(state.cache_key, std::string("xitele"));

    session.reset_state();
    InputLetters(session, "xiteleAA");
    state = session.get_cloud_query_state();
    REQUIRE(!state.should_query);
    REQUIRE_EQ(state.cache_key, std::string("xitele"));

    session.reset_state();
    InputLetters(session, "xitelR");
    state = session.get_cloud_query_state();
    REQUIRE(state.should_query);
}

TEST_CASE(EngineQuanpinSessionContinuesCompositionForCreatingWord)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "xitele");

    const auto transition = session.advance_composition_after_selection("xi", "西", "xi");
    REQUIRE(transition.continues_composition);
    REQUIRE_EQ(transition.consumed_raw_input_with_cases, std::string("xi"));
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("tele"));
    REQUIRE_EQ(session.get_pinyin_segmentation_with_cases(), std::string("te'le"));
}

TEST_CASE(EngineQuanpinSessionCompletesCreatingWordProgress)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "xitele");

    const auto first_transition = session.advance_composition_after_selection("xi", "西", "xi");
    const auto first_progress = session.update_creating_word_progress("", "", "西", first_transition);
    REQUIRE(!first_progress.completed);
    REQUIRE_EQ(first_progress.pinyin, std::string("xi"));
    REQUIRE_EQ(first_progress.word, std::string("西"));
    REQUIRE_EQ(first_progress.preedit, std::string("西te'le"));

    const auto second_transition = session.advance_composition_after_selection("te'le", "特乐", "te'le");
    REQUIRE(!second_transition.continues_composition);
    const auto second_progress =
        session.update_creating_word_progress(first_progress.pinyin, first_progress.word, "特乐", second_transition);
    REQUIRE(second_progress.completed);
    REQUIRE(second_progress.can_store);
    REQUIRE_EQ(second_progress.pinyin, std::string("xi'te'le"));
    REQUIRE_EQ(second_progress.word, std::string("西特乐"));
}

TEST_CASE(EngineQuanpinAbbreviationsContinueAndCreateWithCanonicalPinyin)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "zgrm");

    const auto first = session.advance_composition_after_selection("z'g", "中国", "zhong'guo");
    REQUIRE(first.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("rm"));
    REQUIRE_EQ(session.get_pinyin_segmentation_with_cases(), std::string("r'm"));

    const auto first_progress = session.update_creating_word_progress("", "", "中国", first);
    REQUIRE(!first_progress.completed);
    REQUIRE_EQ(first_progress.pinyin, std::string("zhong'guo"));
    REQUIRE_EQ(first_progress.word, std::string("中国"));

    const auto second = session.advance_composition_after_selection("r'm", "人民", "ren'min");
    REQUIRE(!second.continues_composition);
    const auto completed =
        session.update_creating_word_progress(first_progress.pinyin, first_progress.word, "人民", second);
    REQUIRE(completed.completed);
    REQUIRE(completed.can_store);
    REQUIRE_EQ(completed.pinyin, std::string("zhong'guo'ren'min"));
    REQUIRE_EQ(completed.word, std::string("中国人民"));
}

TEST_CASE(EngineQuanpinAbbreviationCandidatesRetainDatabaseCanonicalPinyin)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "zgrm");

    const auto &candidates = session.get_candidates();
    const auto candidate = std::find_if(candidates.begin(), candidates.end(), [](const IInputSession::WordItem &item) {
        return item.source == CandidateSource::Database && item.pinyin != item.canonical_pinyin;
    });
    REQUIRE(candidate != candidates.end());
    REQUIRE(!candidate->canonical_pinyin.empty());
    REQUIRE_EQ(quanpin::split_segments(candidate->canonical_pinyin).size(),
               HelpcodeUtils::count_han_chars(candidate->word));
}

TEST_CASE(EngineQuanpinSelectionPreservesManualSeparatorsInRemainingInput)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputSequence(session, "zgrm'gh");

    const auto first = session.advance_composition_after_selection("z'g", "中国", "zhong'guo");
    REQUIRE(first.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("rm'gh"));
    REQUIRE_EQ(session.get_pinyin_sequence_with_cases(), std::string("rm'gh"));

    const auto second = session.advance_composition_after_selection("r'm", "人民", "ren'min");
    REQUIRE(second.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("gh"));
}

TEST_CASE(EngineQuanpinSelectionConsumesOnlyTheLeadingManualBoundary)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputSequence(session, "zg'rm'gh");

    const auto transition = session.advance_composition_after_selection("z'g", "中国", "zhong'guo");
    REQUIRE(transition.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("rm'gh"));
}

TEST_CASE(EngineShuangpinIncompleteManualSegmentsContinueCreatingWord)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputSequence(session, "v'x'r'm");

    const auto first = session.advance_composition_after_selection("v", "中", "zhong");
    REQUIRE(first.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("x'r'm"));

    const auto first_progress = session.update_creating_word_progress("", "", "中", first);
    REQUIRE(!first_progress.completed);
    REQUIRE_EQ(first_progress.pinyin, std::string("zhong"));
    REQUIRE_EQ(first_progress.word, std::string("中"));

    const auto second = session.advance_composition_after_selection("x", "西", "xi");
    REQUIRE(second.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("r'm"));
    const auto second_progress =
        session.update_creating_word_progress(first_progress.pinyin, first_progress.word, "西", second);

    const auto third = session.advance_composition_after_selection("r", "人", "ren");
    REQUIRE(third.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("m"));
    const auto third_progress =
        session.update_creating_word_progress(second_progress.pinyin, second_progress.word, "人", third);

    const auto fourth = session.advance_composition_after_selection("m", "民", "min");
    REQUIRE(!fourth.continues_composition);
    const auto completed =
        session.update_creating_word_progress(third_progress.pinyin, third_progress.word, "民", fourth);
    REQUIRE(completed.completed);
    REQUIRE(completed.can_store);
    REQUIRE_EQ(completed.pinyin, std::string("zhong'xi'ren'min"));
    REQUIRE_EQ(completed.word, std::string("中西人民"));
}

TEST_CASE(EngineShuangpinWholeSentenceCandidateCompletesCreatingWordWithCanonicalPinyin)
{
    // 小鹤双拼 xi'ni'hc'vs'go：先选「西」，再用整句候选「你好中国」收尾。
    // 整句候选（Google 整句 fallback / lattice 整句）只有带上 canonical quanpin，
    // 造词才能拼出完整读音并落库。
    EngineInputSession session(SchemeType::Shuangpin);
    InputSequence(session, "xi'ni'hc'vs'go");

    const auto first = session.advance_composition_after_selection("xi", "西", "xi");
    REQUIRE(first.continues_composition);
    const auto first_progress = session.update_creating_word_progress("", "", "西", first);
    REQUIRE_EQ(first_progress.pinyin, std::string("xi"));
    REQUIRE_EQ(first_progress.word, std::string("西"));

    // 整句候选提交的是剩余的全部编码。
    const std::string remaining = session.get_pinyin_sequence();
    const auto second = session.advance_composition_after_selection(remaining, "你好中国", "ni'hao'zhong'guo");
    REQUIRE(!second.continues_composition);

    const auto completed =
        session.update_creating_word_progress(first_progress.pinyin, first_progress.word, "你好中国", second);
    REQUIRE(completed.completed);
    REQUIRE(completed.can_store);
    REQUIRE_EQ(completed.pinyin, std::string("xi'ni'hao'zhong'guo"));
    REQUIRE_EQ(completed.word, std::string("西你好中国"));
}

TEST_CASE(EngineShuangpinWholeSentenceCandidateWithoutCanonicalPinyinCannotBeStored)
{
    // 这条记录的是修复前的行为：整句 fallback 候选的 canonical_pinyin 为空时，
    // 前缀 + 整句只能上屏，永远学不到词库里。
    EngineInputSession session(SchemeType::Shuangpin);
    InputSequence(session, "xi'ni'hc'vs'go");

    const auto first = session.advance_composition_after_selection("xi", "西", "xi");
    const auto first_progress = session.update_creating_word_progress("", "", "西", first);

    const std::string remaining = session.get_pinyin_sequence();
    const auto second = session.advance_composition_after_selection(remaining, "你好中国", "");
    const auto completed =
        session.update_creating_word_progress(first_progress.pinyin, first_progress.word, "你好中国", second);
    REQUIRE(completed.completed);
    REQUIRE(!completed.can_store);
    REQUIRE(completed.pinyin.empty());
    REQUIRE_EQ(completed.word, std::string("西你好中国"));
}

TEST_CASE(WholeSentenceCandidatesAlwaysCarryAStoreableCanonicalPinyin)
{
    // 整句候选（lattice 的 Generated、Google 整句的 Fallback）是造词最后一段
    // 最常选中的东西，必须带上完整的 canonical quanpin，否则 update_creating_word_progress
    // 拼不出读音，前缀 + 整句只上屏、学不到词库里。
    // 整句是否产出取决于装了哪套词库/模型，所以这里只校验产出的那些；
    // 「双拼编码能转出完整 quanpin 读音」这一前提则无条件断言，
    // 那正是修复里挂到整句候选上的那个字符串。
    const auto check = [](EngineInputSession &session) {
        for (const auto &item : session.get_candidates())
        {
            if (item.source != CandidateSource::Generated && item.source != CandidateSource::Fallback)
                continue;
            REQUIRE(!item.canonical_pinyin.empty());
            REQUIRE_EQ(quanpin::split_segments(item.canonical_pinyin).size(),
                       HelpcodeUtils::count_han_chars(item.word));
        }
    };

    EngineInputSession shuangpin(SchemeType::Shuangpin);
    InputSequence(shuangpin, "ni'hc'vs'go");
    REQUIRE_EQ(shuangpin.get_quanpin(), std::string("nihaozhongguo"));
    check(shuangpin);

    EngineInputSession quanpin(SchemeType::Quanpin);
    InputSequence(quanpin, "ni'hao'zhong'guo");
    check(quanpin);
}

TEST_CASE(QuanpinGoogleSentenceIsNotRepeatedForShorterPrefixes)
{
    // 前缀查不到词时曾各补一条 Google 整句，长句后面于是缀上「输入长据的是 / 输入长据的 /
    // 输入长据」这样一串越来越短的〔Unigram〕子串。整句联想只对完整输入出一句。
    EngineInputSession session(SchemeType::Quanpin);
    InputSequence(session, "shuruchangjudeshihou");

    size_t google_sentences = 0;
    for (const auto &item : session.get_candidates())
    {
        if (item.source != CandidateSource::Fallback || !item.sentence_association)
            continue;
        ++google_sentences;
        REQUIRE_EQ(HelpcodeUtils::count_han_chars(item.word), static_cast<size_t>(7));
    }
    REQUIRE(google_sentences <= 1);
}

TEST_CASE(EngineQuanpinIncompleteUppercaseSuffixIsNotConsumedAsHelpcode)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputSequence(session, "zgR");

    const auto transition = session.advance_composition_after_selection("z", "中", "zhong");
    REQUIRE(transition.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("gr"));
    REQUIRE_EQ(session.get_pinyin_sequence_with_cases(), std::string("gR"));
}

TEST_CASE(EngineQuanpinCompleteHelpcodeIsDiscardedButManualRemainderIsPreserved)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputSequence(session, "ni'shuo'neNV");

    const auto transition = session.advance_composition_after_selection("ni'shuo", "你说", "ni'shuo");
    REQUIRE(transition.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("ne"));
    REQUIRE_EQ(session.get_pinyin_sequence_with_cases(), std::string("ne"));
}

TEST_CASE(EngineCreatingWordDoesNotStoreWhenAnySelectedPartLacksCanonicalPinyin)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "zgrm");

    const auto first = session.advance_composition_after_selection("z'g", "中国", "");
    REQUIRE(first.continues_composition);
    const auto first_progress = session.update_creating_word_progress("", "", "中国", first);
    REQUIRE(first_progress.pinyin.empty());

    const auto second = session.advance_composition_after_selection("r'm", "人民", "ren'min");
    const auto completed =
        session.update_creating_word_progress(first_progress.pinyin, first_progress.word, "人民", second);
    REQUIRE(completed.completed);
    REQUIRE(!completed.can_store);
    REQUIRE(completed.pinyin.empty());
}

TEST_CASE(EngineQuanpinSessionContinuesCompositionAfterMultiSyllableSelection)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "zhengxianghuafen");

    const auto transition = session.advance_composition_after_selection("zheng'xiang", "正向", "zheng'xiang");
    REQUIRE(transition.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("huafen"));
    REQUIRE_EQ(session.get_pinyin_segmentation_with_cases(), std::string("hua'fen"));

    const auto progress = session.update_creating_word_progress("", "", "正向", transition);
    REQUIRE_EQ(progress.pinyin, std::string("zheng'xiang"));
    REQUIRE_EQ(progress.word, std::string("正向"));
    REQUIRE_EQ(progress.preedit, std::string("正向hua'fen"));
}

TEST_CASE(EngineQuanpinSessionContinuesCompositionWithoutRetainingHelpcodes)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "nishuoneNV");

    const auto transition = session.advance_composition_after_selection("ni'shuo", "你说", "ni'shuo");
    REQUIRE(transition.continues_composition);
    REQUIRE_EQ(session.get_pinyin_sequence(), std::string("ne"));
    REQUIRE_EQ(session.get_pinyin_sequence_with_cases(), std::string("ne"));
    REQUIRE_EQ(session.get_pinyin_segmentation_with_cases(), std::string("ne"));

    const auto progress = session.update_creating_word_progress("", "", "你说", transition);
    REQUIRE_EQ(progress.pinyin, std::string("ni'shuo"));
    REQUIRE_EQ(progress.word, std::string("你说"));
    REQUIRE_EQ(progress.preedit, std::string("你说ne"));
}

TEST_CASE(EngineShuangpinSessionDynamicCloudCandidateParticipatesInHelpcodesQuery)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputLetters(session, "xitele");

    const auto state = session.get_cloud_query_state();
    REQUIRE(state.should_query);

    session.cache_dynamic_candidate(state.cache_key, "云词", CandidateSource::CloudSuggestion);
    InputLetters(session, "a");

    const auto &candidates = session.get_candidates();
    const auto found = std::find_if(candidates.begin(), candidates.end(),
                                    [](const IInputSession::WordItem &item) { return item.word == "云词"; });
    REQUIRE(found != candidates.end());
    REQUIRE(found->source == CandidateSource::CloudSuggestion);
}

TEST_CASE(EngineShuangpinSessionDynamicCandidateCacheDedupesRepeatedInserts)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputLetters(session, "xitele");

    const auto state = session.get_cloud_query_state();
    REQUIRE(state.should_query);

    session.cache_dynamic_candidate(state.cache_key, "云词", CandidateSource::CloudSuggestion);
    session.cache_dynamic_candidate(state.cache_key, "云词", CandidateSource::CloudSuggestion);
    session.cache_dynamic_candidate(state.cache_key, "AI词", CandidateSource::AiSuggestion);
    session.cache_dynamic_candidate(state.cache_key, "AI词", CandidateSource::AiSuggestion);
    session.cache_dynamic_candidate(state.cache_key, "AI词2", CandidateSource::AiSuggestion);

    // Force a fresh query so candidates are rebuilt from the series cache.
    session.handle_key(VK_BACK, 0, 0);
    InputLetters(session, "e");

    const auto &candidates = session.get_candidates();
    const auto cloud_count =
        std::count_if(candidates.begin(), candidates.end(), [](const IInputSession::WordItem &item) {
            return item.source == CandidateSource::CloudSuggestion && item.word == "云词";
        });
    const auto ai_count = std::count_if(candidates.begin(), candidates.end(), [](const IInputSession::WordItem &item) {
        return item.source == CandidateSource::AiSuggestion;
    });
    const auto ai_latest = std::count_if(candidates.begin(), candidates.end(), [](const IInputSession::WordItem &item) {
        return item.source == CandidateSource::AiSuggestion && item.word == "AI词2";
    });
    const auto ai_stale = std::count_if(candidates.begin(), candidates.end(), [](const IInputSession::WordItem &item) {
        return item.source == CandidateSource::AiSuggestion && item.word == "AI词";
    });

    REQUIRE_EQ(cloud_count, 1);
    REQUIRE_EQ(ai_count, 1);
    REQUIRE_EQ(ai_latest, 1);
    REQUIRE_EQ(ai_stale, 0);
}

TEST_CASE(EngineQuanpinSessionDynamicCloudCandidateParticipatesInHelpcodesQuery)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "xitele");

    const auto state = session.get_cloud_query_state();
    REQUIRE(state.should_query);

    session.cache_dynamic_candidate(state.cache_key, "云词", CandidateSource::CloudSuggestion);
    InputLetters(session, "A");

    const auto &candidates = session.get_candidates();
    const auto found = std::find_if(candidates.begin(), candidates.end(),
                                    [](const IInputSession::WordItem &item) { return item.word == "云词"; });
    REQUIRE(found != candidates.end());
    REQUIRE(found->source == CandidateSource::CloudSuggestion);
}

TEST_CASE(EngineShuangpinSessionHasActiveHelpcodeTracksCloudQueryGate)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputLetters(session, "xitele");

    REQUIRE(session.is_all_complete_pure_pinyin());
    REQUIRE(!session.has_active_helpcode());
    REQUIRE(session.get_cloud_query_state().should_query);

    InputLetters(session, "a");

    REQUIRE(session.is_all_complete_pure_pinyin());
    REQUIRE(session.has_active_helpcode());
    REQUIRE(!session.get_cloud_query_state().should_query);
}

TEST_CASE(EngineQuanpinSessionHasActiveHelpcodeTracksCloudQueryGate)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "xitele");

    REQUIRE(session.is_all_complete_pure_pinyin());
    REQUIRE(!session.has_active_helpcode());
    REQUIRE(session.get_cloud_query_state().should_query);

    InputLetters(session, "A");

    REQUIRE(session.is_all_complete_pure_pinyin());
    REQUIRE(session.has_active_helpcode());
    REQUIRE(!session.get_cloud_query_state().should_query);
}

TEST_CASE(EngineShuangpinSessionUsesZiranmaProfileEndToEnd)
{
    EngineInputSession session(SchemeType::Shuangpin, GetZiranmaShuangpinProfile());

    InputLetters(session, "xd");
    const auto state = session.get_cloud_query_state();

    REQUIRE(state.should_query);
    REQUIRE_EQ(state.query_text, std::string("xiang"));
    REQUIRE_EQ(session.get_quanpin(), std::string("xiang"));
}

TEST_CASE(LegacyShuangpinDictionaryUsesZiranmaProfile)
{
    ShuangpinDictionary dictionary(GetZiranmaShuangpinProfile());
    dictionary.handleVkCode('X', 0, L'x');
    dictionary.handleVkCode('D', 0, L'd');

    REQUIRE_EQ(dictionary.get_quanpin(), std::string("xiang"));
}

TEST_CASE(EngineShuangpinInitialVQueriesZhCandidatesAndExpands)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputLetters(session, "v");

    const auto &initial_candidates = session.get_candidates();
    REQUIRE_EQ(initial_candidates.size(), static_cast<std::size_t>(24));
    REQUIRE(std::all_of(initial_candidates.begin(), initial_candidates.end(), [](const IInputSession::WordItem &item) {
        return item.pinyin == "v" && item.canonical_pinyin.rfind("zh", 0) == 0;
    }));
    const auto initial_count = initial_candidates.size();

    REQUIRE(session.expand_initial_candidates());
    const auto &expanded_candidates = session.get_candidates();
    REQUIRE(expanded_candidates.size() > initial_count);
    REQUIRE(
        std::all_of(expanded_candidates.begin(), expanded_candidates.end(), [](const IInputSession::WordItem &item) {
            return item.pinyin == "v" && item.canonical_pinyin.rfind("zh", 0) == 0;
        }));
}

TEST_CASE(EngineQuanpinInitialCandidatesStartLimitedAndExpand)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "z");

    const auto &initial_candidates = session.get_candidates();
    REQUIRE_EQ(initial_candidates.size(), static_cast<std::size_t>(24));
    REQUIRE(std::all_of(initial_candidates.begin(), initial_candidates.end(),
                        [](const IInputSession::WordItem &item) { return item.pinyin.rfind("z", 0) == 0; }));
    const auto initial_count = initial_candidates.size();

    REQUIRE(session.expand_initial_candidates());
    const auto &expanded_candidates = session.get_candidates();
    REQUIRE(expanded_candidates.size() > initial_count);
}

TEST_CASE(EngineQuanpinSegmentedInitialCandidatesExpandInPlace)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputSequence(session, "l'shi");

    const auto count_initials = [](const std::vector<IInputSession::WordItem> &items) {
        return std::count_if(items.begin(), items.end(), [](const IInputSession::WordItem &item) {
            return item.source == CandidateSource::Database && item.pinyin == "l";
        });
    };
    const auto count_other_candidates = [](const std::vector<IInputSession::WordItem> &items) {
        return std::count_if(items.begin(), items.end(), [](const IInputSession::WordItem &item) {
            return item.source != CandidateSource::Database || item.pinyin != "l";
        });
    };

    const auto initial_candidates = session.get_candidates();
    REQUIRE_EQ(count_initials(initial_candidates), 24);
    const auto other_count = count_other_candidates(initial_candidates);

    REQUIRE(session.expand_initial_candidates());
    const auto &expanded_candidates = session.get_candidates();
    REQUIRE(count_initials(expanded_candidates) > 24);
    REQUIRE_EQ(count_other_candidates(expanded_candidates), other_count);
}

TEST_CASE(EngineShuangpinSegmentedInitialCandidatesExpandInPlace)
{
    EngineInputSession session(SchemeType::Shuangpin);
    InputSequence(session, "v'ui");

    const auto count_initials = [](const std::vector<IInputSession::WordItem> &items) {
        return std::count_if(items.begin(), items.end(), [](const IInputSession::WordItem &item) {
            return item.source == CandidateSource::Database && item.pinyin == "v";
        });
    };
    const auto count_other_candidates = [](const std::vector<IInputSession::WordItem> &items) {
        return std::count_if(items.begin(), items.end(), [](const IInputSession::WordItem &item) {
            return item.source != CandidateSource::Database || item.pinyin != "v";
        });
    };

    const auto initial_candidates = session.get_candidates();
    REQUIRE_EQ(count_initials(initial_candidates), 24);
    const auto other_count = count_other_candidates(initial_candidates);

    REQUIRE(session.expand_initial_candidates());
    const auto &expanded_candidates = session.get_candidates();
    REQUIRE(count_initials(expanded_candidates) > 24);
    REQUIRE_EQ(count_other_candidates(expanded_candidates), other_count);
}

TEST_CASE(EngineSessionsKeepHelpcodeFilteringAndAnnotationsTogether)
{
    // The integration runner supplies an isolated config and the locked release dictionaries.
    InitImeConfig();
    struct RestoreConfiguration
    {
        std::string quanpin = GetConfiguredQuanpinHelpcodeSchema();
        std::string shuangpin = GetConfiguredShuangpinHelpcodeSchema();
        bool quanpin_enabled = GetConfiguredQuanpinHelpcodeEnabled();
        bool shuangpin_enabled = GetConfiguredShuangpinHelpcodeEnabled();
        ~RestoreConfiguration()
        {
            SetConfiguredQuanpinHelpcodeSchema(quanpin);
            SetConfiguredShuangpinHelpcodeSchema(shuangpin);
            SetConfiguredQuanpinHelpcodeEnabled(quanpin_enabled);
            SetConfiguredShuangpinHelpcodeEnabled(shuangpin_enabled);
        }
    } restore;
    REQUIRE(SetConfiguredQuanpinHelpcodeEnabled(true));
    REQUIRE(SetConfiguredShuangpinHelpcodeEnabled(true));
    REQUIRE(SetConfiguredQuanpinHelpcodeSchema("lantian"));
    REQUIRE(SetConfiguredShuangpinHelpcodeSchema("ziranma"));

    EngineInputSession quanpin(SchemeType::Quanpin);
    EngineInputSession shuangpin(SchemeType::Shuangpin);
    REQUIRE_EQ(quanpin.get_helpcode_annotation("你", true), std::string("(RX)"));
    REQUIRE_EQ(shuangpin.get_helpcode_annotation("你", false), std::string("(rE)"));

    InputLetters(quanpin, "niRX");
    REQUIRE(!quanpin.get_candidates().empty());
    REQUIRE(std::any_of(quanpin.get_candidates().begin(), quanpin.get_candidates().end(),
                        [](const auto &item) { return item.word == "你"; }));
    InputLetters(shuangpin, "ni");
    shuangpin.recompute_candidates();
    quanpin.recompute_candidates();
    REQUIRE_EQ(quanpin.get_helpcode_annotation("你", true), std::string("(RX)"));
    REQUIRE_EQ(shuangpin.get_helpcode_annotation("你", false), std::string("(rE)"));
    REQUIRE(std::any_of(quanpin.get_candidates().begin(), quanpin.get_candidates().end(),
                        [](const auto &item) { return item.word == "你"; }));

    // A settings change is adopted by the next refresh, including candidate annotations.
    REQUIRE(SetConfiguredQuanpinHelpcodeSchema("ziranma"));
    quanpin.reset_state();
    InputLetters(quanpin, "niRE");
    REQUIRE_EQ(quanpin.get_helpcode_annotation("你", true), std::string("(RE)"));
    REQUIRE(std::any_of(quanpin.get_candidates().begin(), quanpin.get_candidates().end(),
                        [](const auto &item) { return item.word == "你"; }));
    REQUIRE_EQ(shuangpin.get_helpcode_annotation("你", false), std::string("(rE)"));
    quanpin.switch_scheme(SchemeType::Shuangpin);
    REQUIRE_EQ(quanpin.get_helpcode_annotation("你", false), std::string("(rE)"));
}

TEST_CASE(SegmentBoundariesFollowTheDisplayedQuanpinSyllables)
{
    EngineInputSession session(SchemeType::Quanpin);
    InputLetters(session, "nihaoma");
    REQUIRE_EQ(session.segment_raw_boundaries(), std::vector<std::size_t>({0, 2, 5, 7}));

    // Manual delimiters are boundaries too, and every offset indexes the raw
    // spelling (case included) rather than the displayed segmentation.
    EngineInputSession delimited(SchemeType::Quanpin);
    InputSequence(delimited, "ni'hao");
    REQUIRE_EQ(delimited.segment_raw_boundaries(), std::vector<std::size_t>({0, 3, 6}));

    EngineInputSession cased(SchemeType::Quanpin);
    InputLetters(cased, "NiHaoMa");
    REQUIRE_EQ(cased.segment_raw_boundaries(), std::vector<std::size_t>({0, 2, 5, 7}));

    // A correction display that rewrites letters stays one unit when the cut
    // reports one syllable, and an incomplete tail is a unit of its own so only
    // the tail is deleted.
    EngineInputSession corrected(SchemeType::Quanpin);
    InputLetters(corrected, "sahng");
    REQUIRE_EQ(corrected.segment_raw_boundaries(), std::vector<std::size_t>({0, 5}));

    EngineInputSession partial(SchemeType::Quanpin);
    InputLetters(partial, "nih");
    REQUIRE_EQ(partial.segment_raw_boundaries(), std::vector<std::size_t>({0, 2, 3}));

    EngineInputSession empty(SchemeType::Quanpin);
    REQUIRE(empty.segment_raw_boundaries().empty());
}

TEST_CASE(SegmentBoundariesFollowShuangpinSyllableSegmentation)
{
    EngineInputSession xiaohe(SchemeType::Shuangpin);
    InputLetters(xiaohe, "nihaoma");
    REQUIRE_EQ(xiaohe.segment_raw_boundaries(), std::vector<std::size_t>({0, 2, 4, 5, 7}));

    // One complete syllable is one unit.
    EngineInputSession single(SchemeType::Shuangpin);
    InputLetters(single, "ni");
    REQUIRE_EQ(single.segment_raw_boundaries(), std::vector<std::size_t>({0, 2}));

    // The Microsoft ';' final key belongs to the syllable that consumes it.
    EngineInputSession microsoft(SchemeType::Shuangpin, GetMicrosoftShuangpinProfile());
    InputLetters(microsoft, "b");
    microsoft.handle_key(VK_OEM_1, 0, L';');
    InputLetters(microsoft, "ni");
    REQUIRE_EQ(microsoft.segment_raw_boundaries(), std::vector<std::size_t>({0, 2, 4}));

    // Same ';' final after several syllables: 你好病 = ni | hk | b; in the
    // Microsoft layout (ao sits on 'k'), so the last unit spans 'b' and ';'.
    EngineInputSession microsoft_long(SchemeType::Shuangpin, GetMicrosoftShuangpinProfile());
    InputLetters(microsoft_long, "nihkb");
    microsoft_long.handle_key(VK_OEM_1, 0, L';');
    REQUIRE_EQ(microsoft_long.segment_raw_boundaries(), std::vector<std::size_t>({0, 2, 4, 6}));

    // Greedy longest-pair parsing is the contract: 'cb' is a valid Microsoft
    // code (cou), so it wins over pairing 'b' with the trailing ';'. The
    // preedit shows the same cut, and the deletion follows the preedit.
    EngineInputSession microsoft_greedy(SchemeType::Shuangpin, GetMicrosoftShuangpinProfile());
    InputLetters(microsoft_greedy, "nihcb");
    microsoft_greedy.handle_key(VK_OEM_1, 0, L';');
    REQUIRE_EQ(microsoft_greedy.segment_raw_boundaries(), std::vector<std::size_t>({0, 2, 3, 5, 6}));
}

TEST_CASE(UnitlessSchemesReportNoSegmentBoundaries)
{
    EngineInputSession wubi(SchemeType::Wubi);
    InputLetters(wubi, "nihao");
    REQUIRE(wubi.segment_raw_boundaries().empty());

    EngineInputSession japanese(SchemeType::JapaneseRomaji);
    InputLetters(japanese, "nihao");
    REQUIRE(japanese.segment_raw_boundaries().empty());
}

TEST_CASE(SegmentBoundariesKeepHelpcodeAndJianpinTailAsTheirOwnUnits)
{
    // The deletion follows the preedit, so the correction switches decide what
    // the preedit looks like. Keep them off here so the test pins the plain
    // syllable separators instead of the developer's own configuration.
    ScopedConfigRoot config_root;
    InitImeConfig();
    REQUIRE(SetConfiguredQuanpinAutocorrectTransposition(false));
    REQUIRE(SetConfiguredQuanpinAutocorrectNeighbor(false));
    InitImeConfig();
    REQUIRE(!GetConfiguredQuanpinAutocorrectTransposition());
    REQUIRE(!GetConfiguredQuanpinAutocorrectNeighbor());

    // A single trailing helpcode is one editable unit of its own, exactly as the
    // preedit draws the separator before it.
    EngineInputSession helpcode(SchemeType::Quanpin);
    InputLetters(helpcode, "nihao");
    InputLetters(helpcode, "V");
    REQUIRE_EQ(helpcode.segment_raw_boundaries(), std::vector<std::size_t>({0, 2, 5, 6}));

    // A jianpin tail after a complete syllable stays a unit of its own so only
    // the tail is deleted.
    EngineInputSession jianpin(SchemeType::Quanpin);
    InputLetters(jianpin, "zheg");
    REQUIRE_EQ(jianpin.segment_raw_boundaries(), std::vector<std::size_t>({0, 3, 4}));
}

TEST_CASE(EngineShuangpinMidSentenceHelpcodeConstrainsSentenceSources)
{
    // 句中辅助码走 Server 的真实路径：整串写回会话再重算。词格和 Google 整句都打开，约束要在
    // 解码时生效——所有覆盖到第一个音节的汉字候选，首字都得满足那一码。
    struct ReloadConfigOnExit
    {
        ~ReloadConfigOnExit()
        {
            InitImeConfig();
        }
    } reload;
    ScopedConfigRoot config_root;
    InitImeConfig();
    REQUIRE(SetConfiguredShuangpinMidSentenceHelpcodeEnabled(true));
    REQUIRE(SetConfiguredShuangpinHelpcodeSchema("ziranma"));
    REQUIRE(SetConfiguredAssocSentenceWordLattice(true));
    REQUIRE(SetConfiguredAssocSentenceGoogle(true));
    REQUIRE_EQ(FormatMidSentenceHelpcodeWorkerPayload(),
               std::wstring(GetConfiguredInputScheme() == SchemeType::Shuangpin ? L"1" : L"0"));

    EngineInputSession session(SchemeType::Shuangpin);
    const auto first_code = [&session](const std::string &hanzi) {
        const std::string annotation = session.get_helpcode_annotation(hanzi, false);
        return annotation.size() > 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(annotation[1])))
                                     : '\0';
    };
    const char code = first_code("泥");
    REQUIRE(code != '\0');

    const auto apply = [&session](const std::string &raw) {
        session.set_pinyin_sequence(raw);
        session.set_pinyin_sequence_with_cases(raw);
        session.recompute_candidates();
    };
    apply("ni");
    REQUIRE(session.accepts_mid_sentence_helpcode_marker(2));
    apply("n");
    REQUIRE(!session.accepts_mid_sentence_helpcode_marker(1));
    // 光标移回句中：只看光标前的部分，落在音节中间不收。
    apply("nihc");
    REQUIRE(session.accepts_mid_sentence_helpcode_marker(2));
    REQUIRE(!session.accepts_mid_sentence_helpcode_marker(3));
    REQUIRE(session.accepts_mid_sentence_helpcode_marker(4));
    REQUIRE(!session.has_mid_sentence_helpcode());

    const std::string raw = std::string("ni`") + code + "hc";
    apply(raw);
    REQUIRE_EQ(session.get_pinyin_sequence_with_cases(), raw);
    REQUIRE(!session.get_candidates().empty());
    REQUIRE(session.has_mid_sentence_helpcode());
    REQUIRE(!session.accepts_mid_sentence_helpcode_marker(2));
    // 调频的参照是去掉约束后的候选：里面要有首字不满足这一码的词，且组合本身不动。
    const auto unconstrained = session.candidates_without_mid_sentence_helpcode();
    REQUIRE(std::any_of(unconstrained.begin(), unconstrained.end(), [&](const auto &item) {
        return item.source == CandidateSource::Database && HelpcodeUtils::count_han_chars(item.word) > 0 &&
               first_code(HelpcodeUtils::get_first_han_char(item.word)) != code;
    }));
    REQUIRE_EQ(session.get_pinyin_sequence_with_cases(), raw);
    bool has_sentence = false;
    for (const auto &item : session.get_candidates())
    {
        if (item.source != CandidateSource::Database && item.source != CandidateSource::UserDatabase &&
            item.source != CandidateSource::Generated && item.source != CandidateSource::Fallback)
            continue;
        if (HelpcodeUtils::count_han_chars(item.word) == 0 ||
            HelpcodeUtils::count_han_chars(item.word) != HelpcodeUtils::count_utf8_chars(item.word))
            continue;
        has_sentence = has_sentence || item.sentence_association;
        REQUIRE_EQ(first_code(HelpcodeUtils::get_first_han_char(item.word)), code);
    }
    REQUIRE(has_sentence);
}