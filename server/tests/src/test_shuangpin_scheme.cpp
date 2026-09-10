#include "tests/includes/test_framework.h"
#include "MetasequoiaImeEngine/core/ime_session.h"
#include "MetasequoiaImeEngine/schemes/shuangpin_scheme.h"
#include "MetasequoiaImeEngine/shuangpin/shuangpin_query.h"
#include <utility>
#include <vector>

namespace
{
void InputKey(ShuangpinScheme &scheme, UINT vk, WCHAR wch, UINT modifiers_down = 0)
{
    scheme.handle_key(vk, modifiers_down, wch);
}

void InputSessionKeys(ImeSession &session, const std::string &keys)
{
    for (const char ch : keys)
    {
        const char vk = ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - ('a' - 'A')) : ch;
        session.handle_key(static_cast<UINT>(vk), 0, static_cast<WCHAR>(ch));
    }
}

const ShuangpinProfile &GetTestShuangpinProfile()
{
    static const ShuangpinProfile profile = [] {
        ShuangpinProfile value = GetXiaoheShuangpinProfile();
        value.name = "test";
        value.finals["iang"] = "d";
        value.finals["uang"] = "d";
        return value;
    }();
    return profile;
}
} // namespace

TEST_CASE(ShuangpinSchemeBuildRequestPreservesCaseAndNormalizesQuery)
{
    ShuangpinScheme scheme;
    InputKey(scheme, 'X', L'X', 1);
    InputKey(scheme, 'I', L'i');

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input, std::string("xi"));
    REQUIRE_EQ(request.raw_input_with_cases, std::string("Xi"));
    REQUIRE_EQ(request.raw_segmentation, std::string("Xi"));
    REQUIRE_EQ(request.normalized_input, std::string("xi"));
}

TEST_CASE(ShuangpinDoubleHelpcodesRemainAnUnsegmentedPreeditSuffix)
{
    ImeSession session(SchemeType::Shuangpin);
    session.set_shuangpin_helpcode_enabled(true);
    InputSessionKeys(session, "yakP");
    REQUIRE_EQ(session.get_request().raw_segmentation, std::string("ya'kP"));
    REQUIRE_EQ(session.get_request().normalized_segmentation, std::string("ya'kP"));

    session.reset();
    InputSessionKeys(session, "yaKp");
    REQUIRE_EQ(session.get_request().raw_segmentation, std::string("ya'Kp"));
    REQUIRE_EQ(session.get_request().normalized_segmentation, std::string("ya'Kp"));
}

TEST_CASE(ShuangpinSchemeSpaceDoesNotResetComposition)
{
    ShuangpinScheme scheme;
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, 'I', L'i');
    InputKey(scheme, VK_SPACE, L' ');

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input, std::string("xi"));
}

TEST_CASE(ShuangpinSchemeDeduplicatesConsecutiveManualSeparators)
{
    ShuangpinScheme scheme;
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, 'I', L'i');
    InputKey(scheme, VK_OEM_7, L'\'');
    InputKey(scheme, VK_OEM_7, L'\'');
    InputKey(scheme, VK_OEM_7, L'\'');

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input, std::string("xi'"));
    REQUIRE_EQ(request.key_strokes.size(), static_cast<size_t>(3));
}

TEST_CASE(ShuangpinSchemeEnterResetsComposition)
{
    ShuangpinScheme scheme;
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, 'I', L'i');
    InputKey(scheme, VK_RETURN, 0);

    const QueryRequest request = scheme.build_request();
    REQUIRE(!request.valid);
    REQUIRE_EQ(request.raw_input, std::string(""));
}

TEST_CASE(ShuangpinSchemeBackspaceRemovesLastInput)
{
    ShuangpinScheme scheme;
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, 'I', L'i');
    InputKey(scheme, VK_BACK, 0);

    const QueryRequest request = scheme.build_request();
    REQUIRE(request.valid);
    REQUIRE_EQ(request.raw_input, std::string("x"));
    REQUIRE_EQ(request.raw_input_with_cases, std::string("x"));
}

TEST_CASE(ShuangpinSchemeUsesInjectedProfile)
{
    ShuangpinScheme xiaohe;
    InputKey(xiaohe, 'X', L'x');
    InputKey(xiaohe, 'L', L'l');
    REQUIRE_EQ(xiaohe.build_request().normalized_segmentation, std::string("xiang"));

    ShuangpinScheme custom(GetTestShuangpinProfile());
    InputKey(custom, 'X', L'x');
    InputKey(custom, 'D', L'd');
    REQUIRE_EQ(custom.build_request().normalized_segmentation, std::string("xiang"));
}

TEST_CASE(ZiranmaProfileDecodesWikipediaKeyboardLayout)
{
    const auto &profile = GetZiranmaShuangpinProfile();
    const std::vector<std::pair<std::string, std::string>> cases{
        {"jq", "jiu"},  {"xw", "xia"},  {"gw", "gua"},   {"he", "he"},    {"gr", "guan"},  {"xt", "xue"},
        {"my", "ming"}, {"ky", "kuai"}, {"du", "du"},    {"li", "li"},    {"bo", "bo"},    {"lo", "luo"},
        {"lp", "lun"},  {"da", "da"},   {"js", "jiong"}, {"ds", "dong"},  {"xd", "xiang"}, {"gd", "guang"},
        {"hf", "hen"},  {"dg", "deng"}, {"dh", "dang"},  {"dj", "dan"},   {"dk", "dao"},   {"dl", "dai"},
        {"fz", "fei"},  {"dx", "die"},  {"dc", "diao"},  {"dv", "dui"},   {"lv", "lv"},    {"db", "dou"},
        {"ln", "lin"},  {"lm", "lian"}, {"vh", "zhang"}, {"ih", "chang"}, {"uh", "shang"},
    };

    for (const auto &[input, expected] : cases)
    {
        REQUIRE_EQ(shuangpin::normalize_input(input, profile), expected);
    }
}

TEST_CASE(ZiranmaProfileDecodesZeroInitialSyllables)
{
    const auto &profile = GetZiranmaShuangpinProfile();
    const std::vector<std::pair<std::string, std::string>> cases{
        {"aa", "a"},   {"oo", "o"},  {"ee", "e"},  {"ai", "ai"},  {"an", "an"}, {"ao", "ao"},
        {"ah", "ang"}, {"ei", "ei"}, {"en", "en"}, {"eg", "eng"}, {"er", "er"}, {"ou", "ou"},
    };

    for (const auto &[input, expected] : cases)
    {
        REQUIRE_EQ(shuangpin::normalize_input(input, profile), expected);
    }
}

TEST_CASE(ShoudaoProfileDecodesKeyboardLayout)
{
    const auto &profile = GetShoudaoShuangpinProfile();
    const std::vector<std::pair<std::string, std::string>> cases{
        {"jq", "jiu"},   {"gw", "gua"}, {"he", "he"},    {"dr", "die"},   {"gt", "guan"},  {"dy", "dang"},
        {"du", "du"},    {"li", "li"},  {"bo", "bo"},    {"lo", "luo"},   {"dp", "diao"},  {"da", "da"},
        {"ds", "dou"},   {"dd", "dao"}, {"df", "deng"},  {"gg", "guai"},  {"mg", "ming"},  {"dh", "dong"},
        {"jh", "jiong"}, {"dj", "dan"}, {"hk", "hen"},   {"xk", "xia"},   {"dl", "dai"},   {"jl", "jue"},
        {"yl", "yue"},   {"dz", "dun"}, {"xx", "xiang"}, {"gx", "guang"}, {"lc", "lin"},   {"lv", "lv"},
        {"dv", "dui"},   {"lb", "lve"}, {"dn", "dian"},  {"fm", "fei"},   {"vy", "zhang"}, {"iy", "chang"},
        {"ey", "shang"}, {"ei", "shi"},
    };

    for (const auto &[input, expected] : cases)
    {
        REQUIRE_EQ(shuangpin::normalize_input(input, profile), expected);
    }
}

TEST_CASE(ShoudaoProfileDecodesZeroInitialSyllables)
{
    const auto &profile = GetShoudaoShuangpinProfile();
    const std::vector<std::pair<std::string, std::string>> cases{
        {"aa", "a"},  {"ai", "ai"}, {"an", "an"},  {"ay", "ang"}, {"ao", "ao"}, {"ue", "e"},
        {"ui", "ei"}, {"en", "en"}, {"uf", "eng"}, {"er", "er"},  {"oo", "o"},  {"ou", "ou"},
    };

    for (const auto &[input, expected] : cases)
    {
        REQUIRE_EQ(shuangpin::normalize_input(input, profile), expected);
    }
}

TEST_CASE(MicrosoftProfileDecodesKeyboardLayout)
{
    const auto &profile = GetMicrosoftShuangpinProfile();
    const std::vector<std::pair<std::string, std::string>> cases{
        {"ud", "shuang"}, {"pn", "pin"},  {"vs", "zhong"}, {"m;", "ming"}, {"x;", "xing"}, {"jy", "ju"},
        {"jt", "jue"},    {"yr", "yuan"}, {"ly", "lv"},    {"lv", "lve"},  {"gy", "guai"}, {"dp", "dun"},
    };

    for (const auto &[input, expected] : cases)
    {
        REQUIRE_EQ(shuangpin::normalize_input(input, profile), expected);
    }
}

TEST_CASE(MicrosoftProfileDecodesZeroInitialSyllables)
{
    const auto &profile = GetMicrosoftShuangpinProfile();
    const std::vector<std::pair<std::string, std::string>> cases{
        {"oa", "a"},  {"ol", "ai"}, {"oj", "an"},  {"oh", "ang"}, {"ok", "ao"}, {"oe", "e"},
        {"oz", "ei"}, {"of", "en"}, {"og", "eng"}, {"or", "er"},  {"oo", "o"},  {"ob", "ou"},
    };

    for (const auto &[input, expected] : cases)
    {
        REQUIRE_EQ(shuangpin::normalize_input(input, profile), expected);
    }
}

TEST_CASE(ShuangpinProfilesDecodeKei)
{
    const std::vector<std::pair<const ShuangpinProfile *, std::string>> cases{
        {&GetXiaoheShuangpinProfile(), "kw"},
        {&GetZiranmaShuangpinProfile(), "kz"},
        {&GetShoudaoShuangpinProfile(), "km"},
        {&GetMicrosoftShuangpinProfile(), "kz"},
    };

    for (const auto &[profile, input] : cases)
    {
        REQUIRE_EQ(shuangpin::normalize_input(input, *profile), std::string("kei"));
        REQUIRE(shuangpin::is_complete_input(input, *profile));
    }
}

TEST_CASE(MicrosoftSchemeAcceptsSemicolonAsIngFinalOnlyInSecondPosition)
{
    ShuangpinScheme scheme(GetMicrosoftShuangpinProfile());
    InputKey(scheme, 'M', L'm');
    InputKey(scheme, VK_OEM_1, L';');
    auto request = scheme.build_request();
    REQUIRE_EQ(request.raw_input, std::string("m;"));
    REQUIRE_EQ(request.raw_segmentation, std::string("m;"));
    REQUIRE_EQ(request.normalized_segmentation, std::string("ming"));

    scheme.reset();
    InputKey(scheme, 'X', L'x');
    InputKey(scheme, VK_OEM_1, L';');
    request = scheme.build_request();
    REQUIRE_EQ(request.raw_input, std::string("x;"));
    REQUIRE_EQ(request.raw_segmentation, std::string("x;"));
    REQUIRE_EQ(request.normalized_segmentation, std::string("xing"));

    scheme.reset();
    InputKey(scheme, VK_OEM_1, L';');
    REQUIRE(!scheme.build_request().valid);
}

TEST_CASE(ShuangpinProfileResolverSelectsNamedProfileAndFallsBackToXiaohe)
{
    REQUIRE_EQ(GetShuangpinProfile("jiajia").name, std::string("jiajia"));
    REQUIRE_EQ(GetShuangpinProfile("ziranma").name, std::string("ziranma"));
    REQUIRE_EQ(GetShuangpinProfile("shoudao").name, std::string("shoudao"));
    REQUIRE_EQ(GetShuangpinProfile("microsoft").name, std::string("microsoft"));
    REQUIRE_EQ(GetShuangpinProfile("xiaohe").name, std::string("xiaohe"));
    REQUIRE_EQ(GetShuangpinProfile("unknown").name, std::string("xiaohe"));
}

TEST_CASE(JiajiaSchemeUsesItsOwnFinalsAndKeepsManualSeparatorsAndBackspace)
{
    ShuangpinScheme scheme(GetShuangpinProfile("jiajia"));
    InputKey(scheme, 'N', L'n');
    InputKey(scheme, 'I', L'i');
    InputKey(scheme, VK_OEM_7, L'\'');
    InputKey(scheme, 'H', L'h');
    InputKey(scheme, 'D', L'd');
    REQUIRE_EQ(scheme.build_request().normalized_segmentation, std::string("ni'hao"));

    InputKey(scheme, VK_BACK, 0);
    REQUIRE_EQ(scheme.build_request().raw_input, std::string("ni'h"));
    InputKey(scheme, 'D', L'd');
    REQUIRE_EQ(scheme.build_request().normalized_segmentation, std::string("ni'hao"));

    scheme.reset();
    InputKey(scheme, VK_OEM_1, L';');
    REQUIRE(!scheme.build_request().valid);
    InputKey(scheme, 'M', L'm');
    InputKey(scheme, VK_OEM_1, L';');
    REQUIRE_EQ(scheme.build_request().raw_input, std::string("m"));
}
