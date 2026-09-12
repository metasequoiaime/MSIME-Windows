#include "number_query.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace metasequoia::local_modes
{
namespace
{
constexpr const char *kLowerDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
constexpr const char *kUpperDigits[] = {"零", "壹", "贰", "叁", "肆", "伍", "陆", "柒", "捌", "玖"};
constexpr const char *kLowerUnits[] = {"", "十", "百", "千"};
constexpr const char *kUpperUnits[] = {"", "拾", "佰", "仟"};
// Four-digit group units. 万亿 keeps the reading conventional for amounts
// that exceed 亿 by another 万, which is as far as the 16-digit cap reaches.
constexpr const char *kGroupUnits[] = {"", "万", "亿", "万亿"};

// 9999万亿 is the largest integer the group units above can express.
constexpr std::size_t kMaxIntegerDigits = 16;
// 角 / 分 are the only fractional units in a financial amount.
constexpr std::size_t kMaxFinancialFractionDigits = 2;

struct ParsedNumber
{
    // The integer digits as typed, leading zeros included.
    std::string typed_integer;
    // The integer digits without leading zeros; "0" for zero.
    std::string integer;
    // The fraction digits as typed, possibly empty.
    std::string fraction;
    bool has_point = false;
};

bool is_digit(unsigned char character)
{
    return character >= '0' && character <= '9';
}

bool parse(const std::string &text, ParsedNumber &parsed)
{
    if (text.empty() || !is_digit(static_cast<unsigned char>(text.front())))
    {
        return false;
    }
    const std::size_t point = text.find('.');
    parsed.typed_integer = text.substr(0, point);
    parsed.has_point = point != std::string::npos;
    parsed.fraction = parsed.has_point ? text.substr(point + 1) : std::string{};
    if (!std::all_of(parsed.typed_integer.begin(), parsed.typed_integer.end(),
                     [](unsigned char character) { return is_digit(character); }) ||
        !std::all_of(parsed.fraction.begin(), parsed.fraction.end(),
                     [](unsigned char character) { return is_digit(character); }))
    {
        return false;
    }
    const std::size_t first_nonzero = parsed.typed_integer.find_first_not_of('0');
    parsed.integer = first_nonzero == std::string::npos ? std::string("0") : parsed.typed_integer.substr(first_nonzero);
    return true;
}

// Reads an integer digit string (no leading zeros) with positional units, for
// example "120034" -> 十二万零三十四 / 壹拾贰万零叁拾肆. A run of zeros is
// rendered as a single 零 only when a non-zero digit follows it, and a group
// unit (万 / 亿) is written only for a group that has a non-zero digit.
std::string positional_reading(const std::string &integer, const char *const digits[], const char *const units[])
{
    if (integer == "0")
    {
        return digits[0];
    }

    std::string result;
    bool zero_pending = false;
    bool group_has_value = false;
    const std::size_t length = integer.size();
    for (std::size_t index = 0; index < length; ++index)
    {
        const std::size_t position = length - 1 - index;
        const unsigned digit = static_cast<unsigned>(integer[index] - '0');
        const std::size_t unit = position % 4;
        const std::size_t group = position / 4;
        if (digit != 0)
        {
            if (zero_pending)
            {
                result += digits[0];
                zero_pending = false;
            }
            result += digits[digit];
            result += units[unit];
            group_has_value = true;
        }
        else if (!result.empty())
        {
            zero_pending = true;
        }
        if (unit == 0 && group > 0)
        {
            if (group_has_value)
            {
                result += kGroupUnits[group];
            }
            group_has_value = false;
        }
    }
    return result;
}

std::string digit_by_digit(const std::string &integer, const std::string &fraction, bool has_point,
                           const char *const digits[])
{
    std::string result;
    for (const char character : integer)
    {
        result += digits[character - '0'];
    }
    if (has_point && !fraction.empty())
    {
        result += "点";
        for (const char character : fraction)
        {
            result += digits[character - '0'];
        }
    }
    return result;
}

std::string trimmed_fraction(const std::string &fraction)
{
    const std::size_t last_nonzero = fraction.find_last_not_of('0');
    return last_nonzero == std::string::npos ? std::string{} : fraction.substr(0, last_nonzero + 1);
}

std::string uppercase_reading(const ParsedNumber &number)
{
    std::string result = positional_reading(number.integer, kUpperDigits, kUpperUnits);
    const std::string fraction = trimmed_fraction(number.fraction);
    if (!fraction.empty())
    {
        result += "点";
        result += digit_by_digit(fraction, "", false, kUpperDigits);
    }
    return result;
}

std::string lowercase_reading(const ParsedNumber &number)
{
    std::string result = positional_reading(number.integer, kLowerDigits, kLowerUnits);
    // 十二 rather than 一十二 at the very start of a number, as it is read aloud.
    static const std::string kLeadingTen = std::string(kLowerDigits[1]) + kLowerUnits[1];
    if (result.compare(0, kLeadingTen.size(), kLeadingTen) == 0)
    {
        result.erase(0, std::string(kLowerDigits[1]).size());
    }
    const std::string fraction = trimmed_fraction(number.fraction);
    if (!fraction.empty())
    {
        result += "点";
        result += digit_by_digit(fraction, "", false, kLowerDigits);
    }
    return result;
}

// 人民币大写金额, following the accounting convention: 整 closes an amount
// that stops at 元 or 角, a zero 角 before a non-zero 分 is written as 零, and
// 分 never takes 整.
std::string financial_amount(const ParsedNumber &number)
{
    std::string result = positional_reading(number.integer, kUpperDigits, kUpperUnits) + "元";
    std::string fraction = number.fraction;
    fraction.resize(kMaxFinancialFractionDigits, '0');
    const unsigned jiao = static_cast<unsigned>(fraction[0] - '0');
    const unsigned fen = static_cast<unsigned>(fraction[1] - '0');
    if (jiao == 0 && fen == 0)
    {
        return result + "整";
    }
    if (fen == 0)
    {
        return result + kUpperDigits[jiao] + "角整";
    }
    if (jiao == 0)
    {
        return result + kUpperDigits[0] + kUpperDigits[fen] + "分";
    }
    return result + kUpperDigits[jiao] + "角" + kUpperDigits[fen] + "分";
}

// Arabic digits grouped by `group` digits: 3 is the Western thousands
// convention (123,456), 4 follows the Chinese 万 units (12,3456).
std::string grouped_digits(const ParsedNumber &number, std::size_t group)
{
    std::string result;
    const std::size_t length = number.integer.size();
    for (std::size_t index = 0; index < length; ++index)
    {
        if (index != 0 && (length - index) % group == 0)
        {
            result += ',';
        }
        result += number.integer[index];
    }
    if (number.has_point && !number.fraction.empty())
    {
        result += '.';
        result += number.fraction;
    }
    return result;
}
} // namespace

bool is_number_text(const std::string &number_part)
{
    ParsedNumber parsed;
    return parse(number_part, parsed);
}

std::vector<WordItem> query_number(const std::string &number_part, int limit)
{
    ParsedNumber number;
    if (limit <= 0 || !parse(number_part, number) || number.integer.size() > kMaxIntegerDigits)
    {
        return {};
    }

    std::vector<std::string> readings;
    const auto add = [&](std::string reading) {
        if (!reading.empty() && std::find(readings.begin(), readings.end(), reading) == readings.end())
        {
            readings.push_back(std::move(reading));
        }
    };
    if (number.fraction.size() <= kMaxFinancialFractionDigits)
    {
        add(financial_amount(number));
    }
    add(uppercase_reading(number));
    add(lowercase_reading(number));
    add(digit_by_digit(number.typed_integer, number.fraction, number.has_point, kUpperDigits));
    add(digit_by_digit(number.typed_integer, number.fraction, number.has_point, kLowerDigits));
    for (const std::size_t group : {std::size_t{3}, std::size_t{4}})
    {
        const std::string separated = grouped_digits(number, group);
        if (separated != number_part && separated != number.typed_integer)
        {
            add(separated);
        }
    }

    const std::size_t count = std::min(readings.size(), static_cast<std::size_t>(limit));
    std::vector<WordItem> results;
    results.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        results.emplace_back("", std::move(readings[index]), static_cast<std::int64_t>(count - index),
                             CandidateSource::Generated);
    }
    return results;
}
} // namespace metasequoia::local_modes
