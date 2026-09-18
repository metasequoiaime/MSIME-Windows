#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "statistics/stats_aggregator.h"
#include "statistics/stats_store.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
constexpr std::uint32_t kWriterProcessId = 4242;
constexpr std::uint32_t kMaxRecordCount = static_cast<std::uint32_t>(
    (FANY_IME_STATS_MAX_FRAME_BYTES - sizeof(FanyImeStatsBatchHeader)) / sizeof(FanyImeStatsRecord));

std::filesystem::path MakeTempRoot(const wchar_t *name)
{
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (std::wstring(name) + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    REQUIRE(!ec);
    return root;
}

FanyImeStatsRecord MakeRecord(uint32_t day, uint16_t hour, uint16_t per_class)
{
    FanyImeStatsRecord record{};
    record.day_key = day;
    record.hour = hour;
    record.cjk = per_class;
    record.latin = per_class;
    record.digit = per_class;
    record.punct = per_class;
    record.other = per_class;
    record.active_ms = 500;
    return record;
}

struct FrameOptions
{
    uint32_t magic = FANY_IME_STATS_MAGIC;
    uint32_t version = FANY_IME_STATS_VERSION;
    uint32_t header_size = static_cast<uint32_t>(sizeof(FanyImeStatsBatchHeader));
    // -1 means "derive from the records actually serialized"; anything else is written verbatim so
    // a test can declare a shape the payload does not have.
    int64_t payload_bytes = -1;
    int64_t record_count = -1;
    uint32_t source_process_id = kWriterProcessId;
};

std::vector<unsigned char> BuildFrame(const std::vector<FanyImeStatsRecord> &records, const FrameOptions &options = {})
{
    FanyImeStatsBatchHeader header{};
    header.magic = options.magic;
    header.version = options.version;
    header.header_size = options.header_size;
    header.record_count =
        options.record_count >= 0 ? static_cast<uint32_t>(options.record_count) : static_cast<uint32_t>(records.size());
    header.payload_bytes = options.payload_bytes >= 0
                               ? static_cast<uint32_t>(options.payload_bytes)
                               : static_cast<uint32_t>(records.size() * sizeof(FanyImeStatsRecord));
    header.source_process_id = options.source_process_id;

    std::vector<unsigned char> frame(sizeof(header) + records.size() * sizeof(FanyImeStatsRecord));
    std::memcpy(frame.data(), &header, sizeof(header));
    if (!records.empty())
    {
        std::memcpy(frame.data() + sizeof(header), records.data(), records.size() * sizeof(FanyImeStatsRecord));
    }
    return frame;
}
} // namespace

TEST_CASE(statistics_aggregator_applies_a_valid_frame)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-agg-正常");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    Statistics::StatsAggregator aggregator(store);

    const std::vector<FanyImeStatsRecord> records = {MakeRecord(20260101u, 9, 2), MakeRecord(20260101u, 9, 3),
                                                     MakeRecord(20260101u, 10, 1)};
    const std::vector<unsigned char> frame = BuildFrame(records);
    REQUIRE(aggregator.HandleFrame(frame.data(), frame.size(), kWriterProcessId));

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{1});
    REQUIRE_EQ(snapshot.daily[0].day, 20260101);
    REQUIRE_EQ(snapshot.daily[0].cjk, std::int64_t{6});
    REQUIRE_EQ(snapshot.daily[0].active_ms, std::int64_t{1500});
    REQUIRE_EQ(snapshot.hourly.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.hourly[0].hour, 9);
    REQUIRE_EQ(snapshot.hourly[0].chars, std::int64_t{25});
    REQUIRE_EQ(snapshot.hourly[1].chars, std::int64_t{5});

    // Skipping the writer check is what tests and the listener's fallback path do.
    REQUIRE(aggregator.HandleFrame(frame.data(), frame.size(), 0));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_aggregator_rejects_malformed_frames_as_a_whole)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-agg-坏帧");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    Statistics::StatsAggregator aggregator(store);

    const std::vector<FanyImeStatsRecord> records = {MakeRecord(20260101u, 9, 1)};
    const std::vector<unsigned char> good = BuildFrame(records);
    REQUIRE(aggregator.HandleFrame(good.data(), good.size()));

    FrameOptions magic;
    magic.magic = 0x54415453u + 1u;
    FrameOptions version;
    version.version = FANY_IME_STATS_VERSION + 1u;
    FrameOptions header_size;
    header_size.header_size = static_cast<uint32_t>(sizeof(FanyImeStatsBatchHeader)) - 4u;
    FrameOptions empty;
    empty.record_count = 0;
    // Declared payload does not match the record count.
    FrameOptions stride_mismatch;
    stride_mismatch.payload_bytes = static_cast<int64_t>(sizeof(FanyImeStatsRecord)) + 1;
    // Declared payload is larger than what the frame actually carries.
    FrameOptions over_declared;
    over_declared.payload_bytes = static_cast<int64_t>(sizeof(FanyImeStatsRecord)) * 2;
    over_declared.record_count = 2;
    // Declared payload is smaller than the frame, leaving an unexplained remainder.
    FrameOptions under_declared;
    under_declared.payload_bytes = 0;
    under_declared.record_count = 1;

    const std::vector<std::pair<const char *, FrameOptions>> bad_frames = {
        {"bad magic", magic},
        {"bad version", version},
        {"bad header size", header_size},
        {"zero record count", empty},
        {"stride mismatch", stride_mismatch},
        {"payload larger than frame", over_declared},
        {"payload smaller than frame", under_declared},
    };
    for (const auto &[label, options] : bad_frames)
    {
        const std::vector<unsigned char> frame = BuildFrame(records, options);
        REQUIRE(!aggregator.HandleFrame(frame.data(), frame.size(), kWriterProcessId));
    }

    // Frames that are not even a header, or that carry a trailing remainder, are refused too.
    const unsigned char truncated[8] = {};
    REQUIRE(!aggregator.HandleFrame(truncated, sizeof(truncated), kWriterProcessId));
    REQUIRE(!aggregator.HandleFrame(good.data(), good.size() - 1, kWriterProcessId));
    std::vector<unsigned char> trailing = good;
    trailing.push_back(0);
    REQUIRE(!aggregator.HandleFrame(trailing.data(), trailing.size(), kWriterProcessId));
    REQUIRE(!aggregator.HandleFrame(nullptr, good.size(), kWriterProcessId));

    // Nothing from the rejected frames may have reached the store: exactly the first record is in.
    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{1});
    REQUIRE_EQ(snapshot.daily[0].cjk, std::int64_t{1});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_aggregator_rejects_out_of_range_records_whole_frame)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-agg-越界");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    Statistics::StatsAggregator aggregator(store);

    const FanyImeStatsRecord valid = MakeRecord(20260101u, 9, 1);
    FanyImeStatsRecord hour_overflow = MakeRecord(20260101u, 24, 1);
    FanyImeStatsRecord zero_day = MakeRecord(0u, 9, 1);
    FanyImeStatsRecord bad_month = MakeRecord(20261301u, 9, 1);
    FanyImeStatsRecord bad_day = MakeRecord(20260132u, 9, 1);
    FanyImeStatsRecord ancient = MakeRecord(18991231u, 9, 1);

    const std::vector<FanyImeStatsRecord> bad_records = {hour_overflow, zero_day, bad_month, bad_day, ancient};
    for (const FanyImeStatsRecord &bad : bad_records)
    {
        // The bad record rides along with a good one: one invalid record drops the whole frame.
        const std::vector<FanyImeStatsRecord> records = {valid, bad};
        const std::vector<unsigned char> frame = BuildFrame(records);
        REQUIRE(!aggregator.HandleFrame(frame.data(), frame.size(), kWriterProcessId));
    }

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE(snapshot.daily.empty());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_aggregator_rejects_a_frame_from_a_different_process)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-agg-来源");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    Statistics::StatsAggregator aggregator(store);

    const std::vector<FanyImeStatsRecord> records = {MakeRecord(20260101u, 9, 1)};
    const std::vector<unsigned char> frame = BuildFrame(records);

    REQUIRE(!aggregator.HandleFrame(frame.data(), frame.size(), kWriterProcessId + 1));
    REQUIRE(aggregator.HandleFrame(frame.data(), frame.size(), kWriterProcessId));

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{1});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_aggregator_accepts_the_largest_frame_only)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-agg-上限");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    Statistics::StatsAggregator aggregator(store);

    std::vector<FanyImeStatsRecord> records(kMaxRecordCount, MakeRecord(20260101u, 9, 1));
    const std::vector<unsigned char> at_limit = BuildFrame(records);
    REQUIRE(at_limit.size() <= FANY_IME_STATS_MAX_FRAME_BYTES);
    REQUIRE(aggregator.HandleFrame(at_limit.data(), at_limit.size(), kWriterProcessId));

    records.push_back(MakeRecord(20260101u, 9, 1));
    const std::vector<unsigned char> over_limit = BuildFrame(records);
    REQUIRE(over_limit.size() > FANY_IME_STATS_MAX_FRAME_BYTES);
    REQUIRE(!aggregator.HandleFrame(over_limit.data(), over_limit.size(), kWriterProcessId));

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily[0].cjk, static_cast<std::int64_t>(kMaxRecordCount));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
