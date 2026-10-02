#include "shuangpin_query.h"

#include "../common/helpcode_utils.h"
#include "shuangpin_utils.h"
#include <algorithm>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/case_conv.hpp>

namespace shuangpin
{

namespace
{
std::string segment_chunk(const std::string &chunk, const ShuangpinProfile &profile)
{
    return chunk.empty() ? std::string{} : ShuangpinUtil::pinyin_segmentation(chunk, profile);
}
} // namespace

std::string segment_input(const std::string &raw_input, const ShuangpinProfile &profile)
{
    if (raw_input.empty())
    {
        return {};
    }

    std::string result;
    size_t segment_start = 0;
    bool first_segment = true;
    while (segment_start <= raw_input.size())
    {
        const size_t separator = raw_input.find('\'', segment_start);
        const std::string chunk = separator == std::string::npos
                                      ? raw_input.substr(segment_start)
                                      : raw_input.substr(segment_start, separator - segment_start);
        if (!chunk.empty())
        {
            if (!first_segment)
            {
                result.push_back('\'');
            }
            result += segment_chunk(chunk, profile);
            first_segment = false;
        }

        if (separator == std::string::npos)
        {
            break;
        }
        segment_start = separator + 1;
    }

    return result;
}

std::vector<std::size_t> segment_raw_boundaries(const std::string &raw_input, const ShuangpinProfile &profile)
{
    std::vector<std::size_t> boundaries;
    if (raw_input.empty())
    {
        return boundaries;
    }

    boundaries.push_back(0);
    std::size_t chunk_start = 0;
    while (chunk_start <= raw_input.size())
    {
        const std::size_t separator = raw_input.find('\'', chunk_start);
        const std::size_t chunk_end = separator == std::string::npos ? raw_input.size() : separator;
        std::size_t position = chunk_start;
        while (position < chunk_end)
        {
            // Same forward-greedy rule as pinyin_segmentation: take two keys
            // when they form an accepted syllable, otherwise one.
            const bool two_key = (chunk_end - position) >= 2 &&
                                 ShuangpinUtil::is_accepted_syllable_code(
                                     boost::algorithm::to_lower_copy(raw_input.substr(position, 2)), profile);
            position += two_key ? 2u : 1u;
            boundaries.push_back(position);
        }

        if (separator == std::string::npos)
        {
            break;
        }
        chunk_start = separator + 1;
        if (chunk_start < raw_input.size())
        {
            boundaries.push_back(chunk_start);
        }
    }

    // A trailing delimiter starts no unit, but the end of the spelling always
    // belongs to the last unit; collapse any duplicate a delimiter next to a
    // chunk start produced.
    if (boundaries.back() != raw_input.size())
    {
        boundaries.push_back(raw_input.size());
    }
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

std::string to_quanpin_segmentation(const std::string &segmented_input, const ShuangpinProfile &profile)
{
    return ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(segmented_input, profile);
}

std::string normalize_input_with_delimiters(const std::string &raw_input, const ShuangpinProfile &profile)
{
    return to_quanpin_segmentation(segment_input(raw_input, profile), profile);
}

std::string remove_manual_delimiters(const std::string &text)
{
    return boost::replace_all_copy(text, "'", "");
}

std::string normalize_input(const std::string &raw_input, const ShuangpinProfile &profile)
{
    return remove_manual_delimiters(normalize_input_with_delimiters(raw_input, profile));
}

size_t effective_input_length(const std::string &raw_input)
{
    return remove_manual_delimiters(raw_input).size();
}

size_t raw_length_for_effective_prefix(const std::string &raw_input, size_t effective_length)
{
    size_t raw_length = 0;
    size_t effective_count = 0;
    while (raw_length < raw_input.size() && effective_count < effective_length)
    {
        if (raw_input[raw_length] != '\'')
        {
            ++effective_count;
        }
        ++raw_length;
    }
    return raw_length;
}

std::string trim_trailing_letters_preserve_delimiters(const std::string &raw_input, size_t letter_count)
{
    if (letter_count == 0 || raw_input.empty())
    {
        return raw_input;
    }

    size_t remaining = letter_count;
    size_t pos = raw_input.size();
    while (pos > 0)
    {
        --pos;
        if (raw_input[pos] == '\'')
        {
            continue;
        }

        --remaining;
        if (remaining == 0)
        {
            return raw_input.substr(0, pos);
        }
    }

    return {};
}

size_t detect_active_double_helpcode_length(const std::string &raw_input, const std::string &raw_input_with_cases,
                                            const ShuangpinProfile &profile)
{
    const std::string effective_input = remove_manual_delimiters(raw_input);
    const std::string effective_input_with_cases =
        remove_manual_delimiters(raw_input_with_cases.empty() ? raw_input : raw_input_with_cases);
    if (!ShuangpinUtil::IsFullHelpMode(effective_input_with_cases, profile))
    {
        return 0;
    }

    const size_t base_length = effective_input.size() - 2;
    const size_t raw_base_length = raw_length_for_effective_prefix(raw_input, base_length);
    if (raw_base_length < raw_input.size() && raw_input[raw_base_length] == '\'')
    {
        return 0;
    }

    return is_complete_input(raw_input.substr(0, raw_base_length), profile) ? 2 : 0;
}

bool is_complete_input(const std::string &raw_input, const ShuangpinProfile &profile)
{
    if (raw_input.empty() || raw_input.front() == '\'' || raw_input.back() == '\'' ||
        raw_input.find("''") != std::string::npos)
    {
        return false;
    }

    size_t segment_start = 0;
    while (segment_start <= raw_input.size())
    {
        const size_t separator = raw_input.find('\'', segment_start);
        const std::string chunk = separator == std::string::npos
                                      ? raw_input.substr(segment_start)
                                      : raw_input.substr(segment_start, separator - segment_start);
        if (chunk.empty() ||
            !ShuangpinUtil::is_all_complete_pinyin(chunk, ShuangpinUtil::pinyin_segmentation(chunk, profile)))
        {
            return false;
        }
        if (separator == std::string::npos)
        {
            break;
        }
        segment_start = separator + 1;
    }
    return true;
}

std::string apply_segmentation_cases(const std::string &segmented_input, const std::string &raw_input_with_cases)
{
    if (segmented_input.empty() || raw_input_with_cases.empty())
    {
        return {};
    }

    std::string extracted_input;
    extracted_input.reserve(segmented_input.size());
    for (const char ch : segmented_input)
    {
        if (ch != '\'')
        {
            extracted_input.push_back(ch);
        }
    }

    if (extracted_input != boost::algorithm::to_lower_copy(remove_manual_delimiters(raw_input_with_cases)))
    {
        return segmented_input;
    }

    std::string result;
    result.reserve(segmented_input.size());
    size_t index = 0;
    for (const char ch : segmented_input)
    {
        if (ch == '\'')
        {
            result.push_back(ch);
            continue;
        }

        if (index >= raw_input_with_cases.size())
        {
            return segmented_input;
        }

        while (index < raw_input_with_cases.size() && raw_input_with_cases[index] == '\'')
        {
            ++index;
        }

        if (index >= raw_input_with_cases.size())
        {
            return segmented_input;
        }

        const char cased = raw_input_with_cases[index];
        if (ch == cased || ch == cased + ('a' - 'A'))
        {
            result.push_back(cased);
        }
        else
        {
            result.push_back(ch);
        }
        ++index;
    }

    return result;
}

std::string get_first_han_char(const std::string &words)
{
    return HelpcodeUtils::get_first_han_char(words);
}

std::string get_last_han_char(const std::string &words)
{
    return HelpcodeUtils::get_last_han_char(words);
}

std::string::size_type count_utf8_chars(const std::string &text)
{
    return ShuangpinUtil::count_utf8_chars(text);
}

std::string::size_type count_han_chars(const std::string &text)
{
    return HelpcodeUtils::count_han_chars(text);
}

namespace
{
char to_ascii_lower(char ch)
{
    return FanyImeMidSentenceHelpcode::IsAsciiUpper(ch) ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

std::size_t mid_sentence_block_end(const std::string &raw_input, std::size_t marker)
{
    return FanyImeMidSentenceHelpcode::BlockEnd(raw_input.data(), raw_input.size(), marker);
}

std::size_t count_segments(const std::string &segmentation)
{
    return segmentation.empty()
               ? 0
               : static_cast<std::size_t>(std::count(segmentation.begin(), segmentation.end(), '\'')) + 1;
}
} // namespace

bool has_mid_sentence_helpcode(const std::string &raw_input)
{
    return raw_input.find(kMidSentenceHelpcodeMarker) != std::string::npos;
}

MidSentenceHelpcodeInput parse_mid_sentence_helpcodes(const std::string &raw_input_with_cases,
                                                      const ShuangpinProfile &profile)
{
    MidSentenceHelpcodeInput parsed;
    parsed.input.reserve(raw_input_with_cases.size());
    parsed.source_index.reserve(raw_input_with_cases.size() + 1);
    std::size_t index = 0;
    while (index < raw_input_with_cases.size())
    {
        if (raw_input_with_cases[index] != kMidSentenceHelpcodeMarker)
        {
            parsed.input.push_back(raw_input_with_cases[index]);
            parsed.source_index.push_back(index);
            ++index;
            continue;
        }

        const std::size_t end = mid_sentence_block_end(raw_input_with_cases, index);
        const std::size_t syllables =
            count_segments(segment_input(boost::algorithm::to_lower_copy(parsed.input), profile));
        if (syllables > 0)
        {
            const std::size_t syllable = syllables - 1;
            parsed.decorations.emplace_back(syllable, raw_input_with_cases.substr(index, end - index));
            if (end > index + 1)
            {
                SyllableHelpcode helpcode;
                helpcode.syllable = syllable;
                helpcode.first = to_ascii_lower(raw_input_with_cases[index + 1]);
                helpcode.second = end > index + 2 ? to_ascii_lower(raw_input_with_cases[index + 2]) : 0;
                // 同一个音节敲了两段，以后一段为准。
                auto existing = std::find_if(parsed.helpcodes.begin(), parsed.helpcodes.end(),
                                             [&](const SyllableHelpcode &item) { return item.syllable == syllable; });
                if (existing != parsed.helpcodes.end())
                    *existing = helpcode;
                else
                    parsed.helpcodes.push_back(helpcode);
            }
        }
        // 反引号段同时是一个确定的音节边界，换成手动分隔符；已经有分隔符就不再叠一个。
        if (!parsed.input.empty() && parsed.input.back() != '\'')
        {
            parsed.input.push_back('\'');
            parsed.source_index.push_back(index);
        }
        index = end;
    }
    parsed.source_index.push_back(raw_input_with_cases.size());
    return parsed;
}

std::string decorate_mid_sentence_segmentation(const std::string &segmentation, const std::string &raw_input_with_cases,
                                               const ShuangpinProfile &profile)
{
    if (!has_mid_sentence_helpcode(raw_input_with_cases) || segmentation.empty())
    {
        return segmentation;
    }
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t separator = segmentation.find('\'', start);
        parts.push_back(
            segmentation.substr(start, separator == std::string::npos ? std::string::npos : separator - start));
        if (separator == std::string::npos)
            break;
        start = separator + 1;
    }
    for (const auto &[syllable, text] : parse_mid_sentence_helpcodes(raw_input_with_cases, profile).decorations)
    {
        if (syllable < parts.size())
            parts[syllable] += text;
    }
    std::string decorated;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
            decorated.push_back('\'');
        decorated += parts[i];
    }
    return decorated;
}

bool accepts_mid_sentence_helpcode_marker(const std::string &raw_input)
{
    return FanyImeMidSentenceHelpcode::AcceptsMarker(raw_input.data(), raw_input.size());
}

bool accepts_mid_sentence_helpcode_marker_at(const std::string &raw_input, std::size_t caret)
{
    return FanyImeMidSentenceHelpcode::AcceptsMarkerAt(raw_input.data(), raw_input.size(), caret);
}

bool accepts_mid_sentence_second_code(const std::string &raw_input, char ch)
{
    const std::size_t size = raw_input.size();
    return FanyImeMidSentenceHelpcode::IsAsciiUpper(ch) && size >= 2 &&
           raw_input[size - 2] == kMidSentenceHelpcodeMarker &&
           FanyImeMidSentenceHelpcode::IsAsciiLetter(raw_input[size - 1]);
}

} // namespace shuangpin
