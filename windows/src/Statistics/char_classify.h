#pragma once

#include <cstddef>
#include <cstdint>

namespace MsimeStats
{
// User-perceived character counts for one committed composition, split into the
// five classes the statistics panel reports. Only these integers leave the TSF
// process; the text they were derived from never does.
struct CharClassCounts
{
    uint32_t cjk = 0;
    uint32_t latin = 0;
    uint32_t digit = 0;
    uint32_t punct = 0;
    uint32_t other = 0;

    uint32_t Total() const
    {
        return cjk + latin + digit + punct + other;
    }
};

// Classifies a UTF-16 sequence by Unicode code point. A surrogate pair counts
// as one character of its decoded class; an unpaired surrogate counts once as
// `other` instead of being dropped. A null pointer or zero length yields zero.
CharClassCounts ClassifyText(const wchar_t *text, size_t length);
} // namespace MsimeStats
