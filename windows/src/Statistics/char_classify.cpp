#include "char_classify.h"

#include <Windows.h>

namespace MsimeStats
{
namespace
{
constexpr wchar_t kHighSurrogateFirst = 0xD800;
constexpr wchar_t kHighSurrogateLast = 0xDBFF;
constexpr wchar_t kLowSurrogateFirst = 0xDC00;
constexpr wchar_t kLowSurrogateLast = 0xDFFF;

bool IsHighSurrogate(wchar_t ch)
{
    return ch >= kHighSurrogateFirst && ch <= kHighSurrogateLast;
}

bool IsLowSurrogate(wchar_t ch)
{
    return ch >= kLowSurrogateFirst && ch <= kLowSurrogateLast;
}

uint32_t DecodeSurrogatePair(wchar_t high, wchar_t low)
{
    return 0x10000u + ((static_cast<uint32_t>(high) - kHighSurrogateFirst) << 10) +
           (static_cast<uint32_t>(low) - kLowSurrogateFirst);
}

bool IsCjkCodePoint(uint32_t codePoint)
{
    return (codePoint >= 0x4E00u && codePoint <= 0x9FFFu) || // CJK Unified Ideographs
           (codePoint >= 0x3400u && codePoint <= 0x4DBFu) || // Extension A
           (codePoint >= 0xF900u && codePoint <= 0xFAFFu) || // Compatibility Ideographs
           (codePoint >= 0x20000u && codePoint <= 0x2FA1Fu); // Extensions B-F and compatibility supplement
}

bool IsAsciiLetter(uint32_t codePoint)
{
    return (codePoint >= 'A' && codePoint <= 'Z') || (codePoint >= 'a' && codePoint <= 'z');
}

bool IsDigitCodePoint(uint32_t codePoint)
{
    return (codePoint >= '0' && codePoint <= '9') || (codePoint >= 0xFF10u && codePoint <= 0xFF19u);
}

// GetStringTypeW(CT_CTYPE1) classifies the fullwidth block precisely: the
// sample probe over U+FF01..U+FF65 matched the Unicode categories, so ￥ (Sc)
// and ＋ (Sm) stay symbols while ！ ， ： are punctuation. No extra fullwidth
// whitelist is needed (design.md A3 resolved by samples).
bool IsPunctuationCodePoint(uint32_t codePoint)
{
    if (codePoint > 0xFFFFu)
    {
        // GetStringTypeW only takes UTF-16 units. Everything supplementary that
        // is not CJK (emoji, symbols) is reported as `other` per the taxonomy.
        return false;
    }
    const wchar_t unit = static_cast<wchar_t>(codePoint);
    WORD type = 0;
    if (GetStringTypeW(CT_CTYPE1, &unit, 1, &type) == FALSE)
    {
        return false;
    }
    return (type & (C1_PUNCT | C1_SPACE)) != 0;
}
} // namespace

CharClassCounts ClassifyText(const wchar_t *text, size_t length)
{
    CharClassCounts counts;
    if (text == nullptr)
    {
        return counts;
    }

    size_t index = 0;
    while (index < length)
    {
        const wchar_t first = text[index];
        uint32_t codePoint = static_cast<uint32_t>(first);
        size_t consumed = 1;
        if (IsHighSurrogate(first) && index + 1 < length && IsLowSurrogate(text[index + 1]))
        {
            codePoint = DecodeSurrogatePair(first, text[index + 1]);
            consumed = 2;
        }
        index += consumed;

        if (IsCjkCodePoint(codePoint))
        {
            ++counts.cjk;
        }
        else if (IsAsciiLetter(codePoint))
        {
            ++counts.latin;
        }
        else if (IsDigitCodePoint(codePoint))
        {
            ++counts.digit;
        }
        else if (IsPunctuationCodePoint(codePoint))
        {
            ++counts.punct;
        }
        else
        {
            ++counts.other;
        }
    }
    return counts;
}
} // namespace MsimeStats
