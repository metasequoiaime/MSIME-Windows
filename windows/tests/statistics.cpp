#include "Statistics/char_classify.h"
#include "Statistics/stats_collector.h"

#include <cstring>
#include <vector>

namespace
{
bool SameCounts(const MsimeStats::CharClassCounts &actual, const MsimeStats::CharClassCounts &expected)
{
    return actual.cjk == expected.cjk && actual.latin == expected.latin && actual.digit == expected.digit &&
           actual.punct == expected.punct && actual.other == expected.other;
}

int CheckClassify(const wchar_t *text, size_t length, const MsimeStats::CharClassCounts &expected, int code)
{
    return SameCounts(MsimeStats::ClassifyText(text, length), expected) ? 0 : code;
}

int CheckActiveDelta(uint64_t nowMs, uint64_t &lastCommitMs, uint32_t expected, int code)
{
    return MsimeStats::ComputeActiveDeltaMs(nowMs, lastCommitMs) == expected ? 0 : code;
}
} // namespace

int main()
{
    // Do not use assert: Release builds must execute these checks too.

    // --- Classification: one class at a time -------------------------------
    {
        MsimeStats::CharClassCounts expected;
        expected.latin = 6;
        if (int code = CheckClassify(L"abcXYZ", 6, expected, 1))
            return code;
    }
    {
        MsimeStats::CharClassCounts expected;
        expected.digit = 4;
        if (int code = CheckClassify(L"0123", 4, expected, 2))
            return code;
    }
    {
        // Fullwidth digits U+FF10 .. U+FF19 count as digits.
        MsimeStats::CharClassCounts expected;
        expected.digit = 2;
        if (int code = CheckClassify(L"\uFF10\uFF19", 2, expected, 3))
            return code;
    }
    {
        // ASCII space and ASCII punctuation.
        MsimeStats::CharClassCounts expected;
        expected.punct = 5;
        if (int code = CheckClassify(L" ,.!?", 5, expected, 4))
            return code;
    }
    {
        // BMP CJK boundaries: U+4DFF is outside Extension A, U+4E00 starts
        // Unified Ideographs, U+9FFF ends it and U+A000 is Yi script.
        MsimeStats::CharClassCounts expected;
        expected.cjk = 6;
        if (int code = CheckClassify(L"\u3400\u4DBF\u4E00\u9FFF\uF900\uFAFF", 6, expected, 5))
            return code;
    }
    {
        MsimeStats::CharClassCounts expected;
        expected.other = 1;
        if (int code = CheckClassify(L"\u4DFF", 1, expected, 6))
            return code;
    }
    {
        MsimeStats::CharClassCounts expected;
        expected.other = 1;
        if (int code = CheckClassify(L"\uA000", 1, expected, 7))
            return code;
    }
    {
        // Fullwidth/ideographic punctuation: U+3002, U+3001, U+FF0C, U+3000.
        MsimeStats::CharClassCounts expected;
        expected.punct = 4;
        if (int code = CheckClassify(L"\u3002\u3001\uFF0C\u3000", 4, expected, 8))
            return code;
    }
    {
        // Hiragana and Hangul are not CJK Unified Ideographs.
        MsimeStats::CharClassCounts expected;
        expected.other = 2;
        if (int code = CheckClassify(L"\u3042\uAC00", 2, expected, 9))
            return code;
    }
    {
        // A3 sample check: fullwidth symbols ￥ U+FF04, ＋ U+FF0B and ～ U+FF5E
        // are Unicode symbols, not punctuation, and stay in `other`.
        MsimeStats::CharClassCounts expected;
        expected.other = 3;
        if (int code = CheckClassify(L"\uFF04\uFF0B\uFF5E", 3, expected, 10))
            return code;
    }

    // --- Classification: surrogate pairs -----------------------------------
    {
        // U+20000 (CJK Extension B) is a surrogate pair and one character.
        MsimeStats::CharClassCounts expected;
        expected.cjk = 1;
        if (int code = CheckClassify(L"\U00020000", 2, expected, 20))
            return code;
    }
    {
        // U+2FA1F is the last CJK compatibility supplement code point.
        MsimeStats::CharClassCounts expected;
        expected.cjk = 1;
        if (int code = CheckClassify(L"\U0002FA1F", 2, expected, 21))
            return code;
    }
    {
        MsimeStats::CharClassCounts expected;
        expected.other = 1;
        if (int code = CheckClassify(L"\U0002FA20", 2, expected, 22))
            return code;
    }
    {
        // An emoji is one `other` character, never two.
        MsimeStats::CharClassCounts expected;
        expected.other = 1;
        const MsimeStats::CharClassCounts counts = MsimeStats::ClassifyText(L"\U0001F600", 2);
        if (!SameCounts(counts, expected) || counts.Total() != 1)
        {
            return 23;
        }
    }
    {
        // Unpaired surrogates are counted once as `other` instead of dropped.
        const wchar_t highOnly[] = {static_cast<wchar_t>(0xD800)};
        MsimeStats::CharClassCounts expected;
        expected.other = 1;
        if (int code = CheckClassify(highOnly, 1, expected, 24))
            return code;

        const wchar_t lowOnly[] = {static_cast<wchar_t>(0xDC00)};
        if (int code = CheckClassify(lowOnly, 1, expected, 25))
            return code;

        // A high surrogate followed by an ordinary unit: the high half is one
        // `other`, the letter stays a letter.
        const wchar_t splitPair[] = {static_cast<wchar_t>(0xD800), L'A'};
        MsimeStats::CharClassCounts mixed;
        mixed.other = 1;
        mixed.latin = 1;
        if (int code = CheckClassify(splitPair, 2, mixed, 26))
            return code;
    }
    {
        // Mixed text sums to the user-perceived character count.
        MsimeStats::CharClassCounts expected;
        expected.latin = 1;
        expected.cjk = 1;
        expected.punct = 1;
        expected.digit = 1;
        const MsimeStats::CharClassCounts counts = MsimeStats::ClassifyText(L"A\u4E00 1", 4);
        if (!SameCounts(counts, expected) || counts.Total() != 4)
        {
            return 27;
        }
    }
    {
        MsimeStats::CharClassCounts empty;
        if (int code = CheckClassify(L"", 0, empty, 28))
            return code;
        if (int code = CheckClassify(nullptr, 8, empty, 29))
            return code;
    }

    // --- Active typing time -------------------------------------------------
    {
        uint64_t lastCommitMs = 0;
        if (int code = CheckActiveDelta(1000, lastCommitMs, 0, 40))
            return code; // first commit
        if (lastCommitMs != 1000)
            return 41;
        if (int code = CheckActiveDelta(2000, lastCommitMs, 1000, 42))
            return code; // 1 s
        if (int code = CheckActiveDelta(6900, lastCommitMs, 4900, 43))
            return code; // 4.9 s
        if (int code = CheckActiveDelta(12000, lastCommitMs, 0, 44))
            return code; // 5.1 s is idle
        if (int code = CheckActiveDelta(22000, lastCommitMs, 0, 45))
            return code; // 10 s is idle
        if (int code = CheckActiveDelta(23000, lastCommitMs, 1000, 46))
            return code; // typing resumed
        lastCommitMs = 5000;
        if (int code = CheckActiveDelta(4000, lastCommitMs, 0, 47))
            return code; // clock went backwards
    }

    // --- Pending queue: merge, eviction, batching ---------------------------
    {
        MsimeStats::PendingStatsQueue queue(2);

        FanyImeStatsRecord first;
        first.day_key = 20250101;
        first.hour = 9;
        first.cjk = 2;
        first.active_ms = 100;
        queue.Accumulate(first);
        if (queue.size() != 1 || queue.dropped() != 0)
            return 60;

        FanyImeStatsRecord second;
        second.day_key = 20250101;
        second.hour = 10;
        second.latin = 1;
        second.active_ms = 200;
        queue.Accumulate(second);

        // The same (day, hour) bucket merges in place instead of queueing again.
        FanyImeStatsRecord firstMore;
        firstMore.day_key = 20250101;
        firstMore.hour = 9;
        firstMore.cjk = 3;
        firstMore.active_ms = 50;
        queue.Accumulate(firstMore);
        if (queue.size() != 2 || queue.dropped() != 0)
            return 61;

        // Overflow evicts the oldest bucket, not the newest.
        FanyImeStatsRecord third;
        third.day_key = 20250102;
        third.hour = 9;
        third.digit = 1;
        queue.Accumulate(third);
        if (queue.size() != 2 || queue.dropped() != 1)
            return 62;

        std::vector<FanyImeStatsRecord> batch;
        if (queue.TakeBatch(batch, 16) != 2)
            return 63;
        if (batch[0].day_key != 20250101 || batch[0].hour != 10)
            return 64;
        if (batch[1].day_key != 20250102 || batch[1].digit != 1)
            return 65;
        if (queue.TakeDropped() != 1 || queue.dropped() != 0)
            return 66;
        if (!queue.Empty())
            return 67;
    }
    {
        // TakeBatch never exceeds the requested record count.
        MsimeStats::PendingStatsQueue queue(4);
        for (uint16_t hour = 0; hour < 3; ++hour)
        {
            FanyImeStatsRecord record;
            record.day_key = 20250101;
            record.hour = hour;
            record.other = 1;
            queue.Accumulate(record);
        }
        std::vector<FanyImeStatsRecord> batch;
        if (queue.TakeBatch(batch, 1) != 1 || queue.size() != 2)
            return 68;
        batch.clear();
        if (queue.TakeBatch(batch, 8) != 2)
            return 69;
    }
    {
        // Counters saturate instead of wrapping when an hour bucket is merged
        // throughout a very long session.
        MsimeStats::PendingStatsQueue queue(1);
        FanyImeStatsRecord maxed;
        maxed.day_key = 20250101;
        maxed.hour = 0;
        maxed.cjk = 0xFFFFu;
        maxed.other = 0xFFF0u;
        maxed.active_ms = 0xFFFFFFF0u;
        queue.Accumulate(maxed);
        FanyImeStatsRecord extra;
        extra.day_key = 20250101;
        extra.hour = 0;
        extra.cjk = 10;
        extra.other = 0x20u;
        extra.active_ms = 0x20u;
        queue.Accumulate(extra);

        std::vector<FanyImeStatsRecord> batch;
        if (queue.TakeBatch(batch, 1) != 1)
            return 70;
        if (batch[0].cjk != 0xFFFFu || batch[0].other != 0xFFFFu || batch[0].active_ms != 0xFFFFFFFFu)
        {
            return 71;
        }
    }
    {
        // A disabled/killed flush discards the queue and its dropped report.
        MsimeStats::PendingStatsQueue queue(1);
        FanyImeStatsRecord record;
        record.day_key = 20250101;
        record.other = 1;
        queue.Accumulate(record);
        queue.Accumulate(record); // evicts the first, dropped becomes 1
        queue.Reset();
        if (!queue.Empty() || queue.dropped() != 0)
            return 72;
        // A failed send reports the batch as dropped for the next frame.
        queue.AddDropped(7);
        if (queue.TakeDropped() != 7)
            return 73;
    }

    return 0;
}
