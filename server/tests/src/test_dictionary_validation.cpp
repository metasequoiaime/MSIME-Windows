#include "settings/dictionary_validation.h"
#include "tests/includes/test_framework.h"

#include <string>

TEST_CASE(DictionaryFullPinyinValidationRejectsAbbreviatedSyllables)
{
    quanpin::Segments segments;
    std::string normalized;

    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("nihao", segments, normalized));
    REQUIRE_EQ(normalized, std::string("ni'hao"));
    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("xi'an", segments, normalized));
    REQUIRE_EQ(normalized, std::string("xi'an"));
    REQUIRE(!SettingsDictionary::Validation::NormalizeFullPinyin("nh", segments, normalized));
    REQUIRE(!SettingsDictionary::Validation::NormalizeFullPinyin("ni'h", segments, normalized));
    REQUIRE(!SettingsDictionary::Validation::NormalizeFullPinyin("ni'", segments, normalized));
}

TEST_CASE(DictionaryFullPinyinPicksSegmentationMatchingSyllableCount)
{
    quanpin::Segments segments;
    std::string normalized;

    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("xian", segments, normalized));
    REQUIRE_EQ(normalized, std::string("xian"));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(1));

    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("xian", segments, normalized, 2));
    REQUIRE_EQ(normalized, std::string("xi'an"));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(2));

    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("a'a'a'a'a'a'a'a", segments, normalized, 8));
    REQUIRE_EQ(normalized, std::string("a'a'a'a'a'a'a'a"));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(8));

    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin("abalatiyayunhai", segments, normalized, 7));
    REQUIRE_EQ(normalized, std::string("a'ba'la'ti'ya'yun'hai"));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(7));
}

TEST_CASE(QuickPhraseValidationMatchesNamedPipeWcharCapacity)
{
    REQUIRE(SettingsDictionary::Validation::QuickPhraseFitsNamedPipe(std::string(199, 'a')));
    REQUIRE(!SettingsDictionary::Validation::QuickPhraseFitsNamedPipe(std::string(200, 'a')));

    const std::string emoji = "\xF0\x9F\x98\x80";
    REQUIRE(SettingsDictionary::Validation::QuickPhraseFitsNamedPipe(std::string(197, 'a') + emoji));
    REQUIRE(!SettingsDictionary::Validation::QuickPhraseFitsNamedPipe(std::string(198, 'a') + emoji));
}

TEST_CASE(CodedDictionaryImportRequiresTabsAndPreservesSpacesInWords)
{
    std::string word;
    std::string code;
    std::string message;
    int weight = -1;

    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine("包含 空格的词\tbao'han'kong'ge'de'ci\t123", word,
                                                                 code, weight, message));
    REQUIRE_EQ(word, std::string("包含 空格的词"));
    REQUIRE_EQ(code, std::string("bao'han'kong'ge'de'ci"));
    REQUIRE_EQ(weight, 123);

    REQUIRE(!SettingsDictionary::Validation::ParseCodedImportLine("普通词 putongci 10", word, code, weight, message));
    REQUIRE(!SettingsDictionary::Validation::ParseCodedImportLine("普通词\tputongci", word, code, weight, message));
    REQUIRE(!SettingsDictionary::Validation::ParseCodedImportLine("普通词\tputongci\t10\textra", word, code, weight,
                                                                  message));
}

TEST_CASE(CodedQuanpinImportAcceptsExportedApostropheTsv)
{
    std::string word;
    std::string code;
    std::string message;
    int weight = -1;
    quanpin::Segments segments;
    std::string normalized;

    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine("啊啊啊啊啊啊啊啊\ta'a'a'a'a'a'a'a\t41", word, code,
                                                                 weight, message));
    REQUIRE_EQ(word, std::string("啊啊啊啊啊啊啊啊"));
    REQUIRE_EQ(code, std::string("a'a'a'a'a'a'a'a"));
    REQUIRE_EQ(weight, 41);
    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin(code, segments, normalized, 8));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(8));

    REQUIRE(SettingsDictionary::Validation::ParseCodedImportLine("阿巴拉提亚云海\ta'ba'la'ti'ya'yun'hai\t13", word,
                                                                 code, weight, message));
    REQUIRE_EQ(word, std::string("阿巴拉提亚云海"));
    REQUIRE_EQ(code, std::string("a'ba'la'ti'ya'yun'hai"));
    REQUIRE_EQ(weight, 13);
    REQUIRE(SettingsDictionary::Validation::NormalizeFullPinyin(code, segments, normalized, 7));
    REQUIRE_EQ(normalized, std::string("a'ba'la'ti'ya'yun'hai"));
    REQUIRE_EQ(segments.size(), static_cast<size_t>(7));
}
