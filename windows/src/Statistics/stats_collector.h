#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "../../../engine/contracts/windows_ipc.h"
#include "char_classify.h"

namespace MsimeStats
{
// A flush frame carries at most this many 20-byte records (well under
// FANY_IME_STATS_MAX_FRAME_BYTES), and the pending queue holds at most the
// same count. Overflow evicts the oldest slot and is reported through
// dropped_count in the next frame.
inline constexpr size_t kMaxRecordsPerBatch = 256;
inline constexpr size_t kMaxQueuedRecords = 256;
// Two commits further apart than this are a thinking pause: the gap is not
// typing time and must not inflate the speed numbers.
inline constexpr uint64_t kActiveIdleThresholdMs = 5000;
// Mirrors the diagnostic channel's deferred batched flush.
inline constexpr uint32_t kFlushDelayMs = 250;

// Returns the active-typing delta for a commit at nowMs and advances
// lastCommitMs. The first commit (lastCommitMs == 0), a clock that moved
// backwards and any gap above the idle threshold contribute zero: the time
// before them is unknown or idle, not typing.
inline uint32_t ComputeActiveDeltaMs(uint64_t nowMs, uint64_t &lastCommitMs)
{
    const uint64_t previous = lastCommitMs;
    lastCommitMs = nowMs;
    if (previous == 0 || nowMs <= previous || nowMs - previous > kActiveIdleThresholdMs)
    {
        return 0;
    }
    return static_cast<uint32_t>(nowMs - previous);
}

// Bounded FIFO of per-(day, hour) counters. Merging in place keeps one record
// per hour bucket, so a fast typist never queues more than one slot per hour.
class PendingStatsQueue
{
  public:
    explicit PendingStatsQueue(size_t capacity = kMaxQueuedRecords) : capacity_(capacity)
    {
    }

    void Accumulate(const FanyImeStatsRecord &record)
    {
        for (FanyImeStatsRecord &pending : records_)
        {
            if (pending.day_key == record.day_key && pending.hour == record.hour)
            {
                MergeInto(pending, record);
                return;
            }
        }
        if (capacity_ == 0)
        {
            ++dropped_;
            return;
        }
        if (records_.size() >= capacity_)
        {
            records_.pop_front();
            ++dropped_;
        }
        records_.push_back(record);
    }

    // Moves up to maxRecords oldest records into out and returns the moved
    // count. The records leave the queue; a failed send reports their count as
    // dropped instead of keeping them queued for a retry storm.
    size_t TakeBatch(std::vector<FanyImeStatsRecord> &out, size_t maxRecords)
    {
        size_t moved = 0;
        while (moved < maxRecords && !records_.empty())
        {
            out.push_back(records_.front());
            records_.pop_front();
            ++moved;
        }
        return moved;
    }

    void Reset()
    {
        records_.clear();
        dropped_ = 0;
    }

    bool Empty() const
    {
        return records_.empty();
    }

    size_t size() const
    {
        return records_.size();
    }

    uint32_t dropped() const
    {
        return dropped_;
    }

    // Reads and clears the eviction counter: it is reported once, in the next
    // frame header.
    uint32_t TakeDropped()
    {
        const uint32_t dropped = dropped_;
        dropped_ = 0;
        return dropped;
    }

    void AddDropped(uint32_t count)
    {
        dropped_ += count;
    }

  private:
    static void AddSaturating(uint16_t &target, uint16_t value)
    {
        const uint32_t sum = static_cast<uint32_t>(target) + static_cast<uint32_t>(value);
        target = sum > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(sum);
    }

    static void MergeInto(FanyImeStatsRecord &target, const FanyImeStatsRecord &source)
    {
        AddSaturating(target.cjk, source.cjk);
        AddSaturating(target.latin, source.latin);
        AddSaturating(target.digit, source.digit);
        AddSaturating(target.punct, source.punct);
        AddSaturating(target.other, source.other);
        const uint64_t activeSum = static_cast<uint64_t>(target.active_ms) + source.active_ms;
        target.active_ms = activeSum > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(activeSum);
    }

    std::deque<FanyImeStatsRecord> records_;
    size_t capacity_;
    uint32_t dropped_ = 0;
};

// Records one committed composition. Does nothing (and does not touch the
// active-time state) while the user switch is off or the text was empty.
void QueueStatisticsCommit(const CharClassCounts &counts);

// Called when the user turns statistics off: the next enabled commit starts a
// fresh active-time measurement instead of counting the paused gap.
void ResetStatisticsActiveTimer();
} // namespace MsimeStats
