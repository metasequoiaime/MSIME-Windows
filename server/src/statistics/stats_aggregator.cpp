#include "statistics/stats_aggregator.h"

#include "config/ime_config.h"

#include <cstring>
#include <vector>

namespace Statistics
{
namespace
{
constexpr std::size_t kHeaderSize = sizeof(FanyImeStatsBatchHeader);
constexpr std::size_t kRecordSize = sizeof(FanyImeStatsRecord);
// The payload of the largest frame that can still fit the transport. Expressed as a division so a
// corrupt record_count can never overflow the multiplication used to validate it.
constexpr std::uint32_t kMaxRecordCount =
    static_cast<std::uint32_t>((FANY_IME_STATS_MAX_FRAME_BYTES - kHeaderSize) / kRecordSize);

bool IsSaneDayKey(std::uint32_t day_key)
{
    if (day_key < 19000101u || day_key > 99991231u)
    {
        return false;
    }
    const std::uint32_t month = (day_key / 100u) % 100u;
    const std::uint32_t day = day_key % 100u;
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

bool IsPlausible(const FanyImeStatsRecord &record)
{
    return IsSaneDayKey(record.day_key) && record.hour <= 23;
}
} // namespace

StatsAggregator::StatsAggregator(StatsStore &store) : store_(store)
{
}

bool StatsAggregator::HandleFrame(const unsigned char *frame, std::size_t size, std::uint32_t connected_process_id)
{
    if (frame == nullptr || size < kHeaderSize)
    {
        return false;
    }
    FanyImeStatsBatchHeader header{};
    std::memcpy(&header, frame, kHeaderSize);

    if (header.magic != FANY_IME_STATS_MAGIC || header.version != FANY_IME_STATS_VERSION ||
        header.header_size != kHeaderSize || header.record_count == 0 || header.record_count > kMaxRecordCount ||
        header.payload_bytes != header.record_count * kRecordSize)
    {
        return false;
    }
    // The pipe delivers whole messages, so a trailing remainder means the writer and this parser
    // disagree about the frame layout; nothing in the frame is trustworthy at that point.
    if (kHeaderSize + header.payload_bytes != size)
    {
        return false;
    }
    if (connected_process_id != 0 && header.source_process_id != connected_process_id)
    {
        return false;
    }

    std::vector<FanyImeStatsRecord> records(header.record_count);
    std::memcpy(records.data(), frame + kHeaderSize, header.payload_bytes);
    for (const FanyImeStatsRecord &record : records)
    {
        if (!IsPlausible(record))
        {
            return false;
        }
    }
    // The policy is re-read per frame like the enabled switch, so changing it applies to the very
    // next batch instead of waiting for a restart.
    RetentionPolicy policy;
    policy.range = GetConfiguredStatisticsRetention();
    return store_.ApplyBatch(records.data(), records.size(), policy);
}
} // namespace Statistics
