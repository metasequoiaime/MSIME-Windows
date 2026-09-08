#include "dictionary_validation.h"

#include "ipc/ipc_protocol_limits.h"
#include "utils/common_utils.h"
#include "MetasequoiaImeEngine/quanpin/quanpin_utils.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace SettingsDictionary::Validation
{
bool NormalizeFullPinyin(const std::string &input, quanpin::Segments &segments, std::string &normalized,
                         std::size_t expected_syllables)
{
    std::string source = input;
    source.erase(std::remove_if(source.begin(), source.end(), [](unsigned char ch) { return std::isspace(ch); }),
                 source.end());
    std::transform(source.begin(), source.end(), source.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (source.empty() || source.front() == '\'' || source.back() == '\'' || source.find("''") != std::string::npos)
    {
        return false;
    }

    if (source.find('\'') != std::string::npos)
    {
        segments = quanpin::split_segments(source);
    }
    else
    {
        const auto cuts = quanpin::cut_pinyin_by_mode(source, "correction");
        if (cuts.empty())
            return false;
        segments = cuts.front();
        if (expected_syllables != 0 && segments.size() != expected_syllables)
        {
            const auto alternatives = quanpin::enumerate_complete_segmentations(quanpin::build_syllable_graph(source));
            const auto match = std::find_if(
                alternatives.begin(), alternatives.end(),
                [expected_syllables](const quanpin::Segments &cut) { return cut.size() == expected_syllables; });
            if (match != alternatives.end())
                segments = *match;
        }
    }

    const auto &valid = quanpin::intact_pinyin_set();
    if (segments.empty() || !std::all_of(segments.begin(), segments.end(), [&valid](const std::string &segment) {
            return !segment.empty() && valid.find(segment) != valid.end();
        }))
    {
        return false;
    }

    normalized = quanpin::join_segments(segments);
    std::string without_delimiters = normalized;
    without_delimiters.erase(std::remove(without_delimiters.begin(), without_delimiters.end(), '\''),
                             without_delimiters.end());
    std::string source_without_delimiters = source;
    source_without_delimiters.erase(
        std::remove(source_without_delimiters.begin(), source_without_delimiters.end(), '\''),
        source_without_delimiters.end());
    return without_delimiters == source_without_delimiters;
}

bool ParseCodedImportLine(const std::string &line, std::string &word, std::string &code, int &weight,
                          std::string &message)
{
    const auto trim = [](std::string value) {
        const auto begin = value.find_first_not_of(' ');
        if (begin == std::string::npos)
            return std::string{};
        const auto end = value.find_last_not_of(' ');
        return value.substr(begin, end - begin + 1);
    };

    std::vector<std::string> fields;
    std::string::size_type start = 0;
    for (;;)
    {
        const auto separator = line.find('\t', start);
        fields.push_back(trim(line.substr(start, separator == std::string::npos ? separator : separator - start)));
        if (separator == std::string::npos)
            break;
        start = separator + 1;
    }
    if (fields.size() != 3)
    {
        message = "格式错误，应为：词语<Tab>编码<Tab>权重";
        return false;
    }

    word = fields[0];
    code = fields[1];
    const std::string &weight_text = fields[2];
    if (word.empty() || code.empty())
    {
        message = "词语和编码不能为空";
        return false;
    }
    if (weight_text.empty() ||
        !std::all_of(weight_text.begin(), weight_text.end(), [](unsigned char ch) { return std::isdigit(ch); }))
    {
        message = "权重必须是非负整数";
        return false;
    }
    try
    {
        weight = std::stoi(weight_text);
    }
    catch (...)
    {
        message = "权重数值无效";
        return false;
    }
    return true;
}

bool QuickPhraseFitsNamedPipe(const std::string &phrase)
{
    try
    {
        return string_to_wstring(phrase).size() <= FanyImePipeLimits::CandidateTextMaxLength;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace SettingsDictionary::Validation
