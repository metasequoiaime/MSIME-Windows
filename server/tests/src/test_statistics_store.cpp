#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "statistics/stats_store.h"

#include <sqlite3.h>
#include <windows.h>

#include <cstdint>
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
class ScopedEnv
{
  public:
    ScopedEnv(const wchar_t *name, const std::wstring &value) : name_(name)
    {
        wchar_t buffer[32768];
        const DWORD length = GetEnvironmentVariableW(name, buffer, 32768);
        had_previous_ = length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        previous_.assign(buffer, length);
        SetEnvironmentVariableW(name, value.c_str());
    }
    ~ScopedEnv()
    {
        SetEnvironmentVariableW(name_.c_str(), had_previous_ ? previous_.c_str() : nullptr);
    }

    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

  private:
    std::wstring name_;
    std::wstring previous_;
    bool had_previous_ = false;
};

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

// Mirrors what the store does for "the last N days", written independently so the boundary is
// checked against the calendar rather than against the implementation.
int32_t DayKeyDaysAgo(int days_ago)
{
    SYSTEMTIME local{};
    GetLocalTime(&local);
    FILETIME stamp{};
    REQUIRE(SystemTimeToFileTime(&local, &stamp));
    ULARGE_INTEGER ticks{};
    ticks.LowPart = stamp.dwLowDateTime;
    ticks.HighPart = stamp.dwHighDateTime;
    constexpr unsigned long long kTicksPerDay = 24ull * 60ull * 60ull * 10000000ull;
    ticks.QuadPart -= static_cast<unsigned long long>(days_ago) * kTicksPerDay;
    FILETIME adjusted{};
    adjusted.dwLowDateTime = ticks.LowPart;
    adjusted.dwHighDateTime = ticks.HighPart;
    SYSTEMTIME result{};
    REQUIRE(FileTimeToSystemTime(&adjusted, &result));
    return static_cast<int32_t>(result.wYear) * 10000 + static_cast<int32_t>(result.wMonth) * 100 + result.wDay;
}

FanyImeStatsRecord MakeRecord(int32_t day, int hour, int cjk, int latin, int digit, int punct, int other,
                              uint32_t active_ms)
{
    FanyImeStatsRecord record{};
    record.day_key = static_cast<uint32_t>(day);
    record.hour = static_cast<uint16_t>(hour);
    record.cjk = static_cast<uint16_t>(cjk);
    record.latin = static_cast<uint16_t>(latin);
    record.digit = static_cast<uint16_t>(digit);
    record.punct = static_cast<uint16_t>(punct);
    record.other = static_cast<uint16_t>(other);
    record.active_ms = active_ms;
    return record;
}

// Reads the schema the way an inspector would: straight from the file, no store API involved.
std::vector<std::pair<std::string, std::string>> TableColumns(const std::filesystem::path &db_path,
                                                              const std::string &table)
{
    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open_v2(test::Utf8(db_path).c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    std::vector<std::pair<std::string, std::string>> columns;
    sqlite3_stmt *stmt = nullptr;
    REQUIRE_EQ(sqlite3_prepare_v2(db, "SELECT name,type FROM pragma_table_info(?1)", -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        columns.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)),
                             reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return columns;
}

std::int64_t ScalarInt(const std::filesystem::path &db_path, const std::string &sql)
{
    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open_v2(test::Utf8(db_path).c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    REQUIRE_EQ(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr), SQLITE_OK);
    REQUIRE_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const std::int64_t value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

// Raw SQL for arranging states the public API cannot reach -- a throttle marker that belongs to
// yesterday, a row that appears after the trim ran, a trigger that makes a batch fail.
void ExecuteSql(const std::filesystem::path &db_path, const std::string &sql)
{
    sqlite3 *db = nullptr;
    REQUIRE_EQ(sqlite3_open_v2(test::Utf8(db_path).c_str(), &db, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
    char *error = nullptr;
    const int result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
    if (error != nullptr)
    {
        sqlite3_free(error);
    }
    sqlite3_close(db);
    REQUIRE_EQ(result, SQLITE_OK);
}
} // namespace

TEST_CASE(statistics_store_accumulates_into_daily_and_hourly_buckets)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-累加");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    const int32_t today = DayKeyDaysAgo(0);

    REQUIRE(store.Apply(MakeRecord(today, 9, 3, 1, 2, 4, 0, 1000)));
    REQUIRE(store.Apply(MakeRecord(today, 9, 1, 0, 0, 0, 5, 500)));

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{1});
    REQUIRE_EQ(snapshot.daily[0].day, today);
    REQUIRE_EQ(snapshot.daily[0].cjk, std::int64_t{4});
    REQUIRE_EQ(snapshot.daily[0].latin, std::int64_t{1});
    REQUIRE_EQ(snapshot.daily[0].digit, std::int64_t{2});
    REQUIRE_EQ(snapshot.daily[0].punct, std::int64_t{4});
    REQUIRE_EQ(snapshot.daily[0].other, std::int64_t{5});
    REQUIRE_EQ(snapshot.daily[0].active_ms, std::int64_t{1500});

    REQUIRE_EQ(snapshot.hourly.size(), std::size_t{1});
    REQUIRE_EQ(snapshot.hourly[0].day, today);
    REQUIRE_EQ(snapshot.hourly[0].hour, 9);
    REQUIRE_EQ(snapshot.hourly[0].chars, std::int64_t{16});
    REQUIRE_EQ(snapshot.hourly[0].active_ms, std::int64_t{1500});

    REQUIRE(snapshot.has_first_day);
    REQUIRE_EQ(snapshot.first_day, today);

    // A second Query on the same store must see the same committed data (idempotent open).
    Statistics::Snapshot again;
    REQUIRE(store.Query(again));
    REQUIRE_EQ(again.daily[0].cjk, snapshot.daily[0].cjk);

    // A second store over the same file must reuse the existing schema rather than fail on it, and
    // keep accumulating into the same row.
    {
        Statistics::StatsStore reopened(test::Utf8(db_path));
        REQUIRE(reopened.Apply(MakeRecord(today, 9, 1, 0, 0, 0, 0, 100)));
        Statistics::Snapshot after_reopen;
        REQUIRE(reopened.Query(after_reopen));
        REQUIRE_EQ(after_reopen.daily[0].cjk, snapshot.daily[0].cjk + 1);
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_buckets_across_days_and_hours)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-分桶");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t yesterday = DayKeyDaysAgo(1);

    REQUIRE(store.Apply(MakeRecord(today, 9, 2, 0, 0, 0, 0, 100)));
    REQUIRE(store.Apply(MakeRecord(today, 10, 3, 0, 0, 0, 0, 200)));
    REQUIRE(store.Apply(MakeRecord(yesterday, 9, 4, 0, 0, 0, 0, 300)));

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.daily[0].day, yesterday);
    REQUIRE_EQ(snapshot.daily[0].cjk, std::int64_t{4});
    REQUIRE_EQ(snapshot.daily[1].day, today);
    REQUIRE_EQ(snapshot.daily[1].cjk, std::int64_t{5});
    REQUIRE_EQ(snapshot.daily[1].active_ms, std::int64_t{300});

    REQUIRE_EQ(snapshot.hourly.size(), std::size_t{3});
    REQUIRE_EQ(snapshot.hourly[0].day, yesterday);
    REQUIRE_EQ(snapshot.hourly[1].hour, 9);
    REQUIRE_EQ(snapshot.hourly[1].chars, std::int64_t{2});
    REQUIRE_EQ(snapshot.hourly[2].hour, 10);
    REQUIRE_EQ(snapshot.hourly[2].chars, std::int64_t{3});

    // first_day tracks the oldest recorded day, not the most recent insert.
    REQUIRE(snapshot.has_first_day);
    REQUIRE_EQ(snapshot.first_day, yesterday);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_applies_a_batch_in_one_transaction)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-批次");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    const int32_t today = DayKeyDaysAgo(0);

    std::vector<FanyImeStatsRecord> batch;
    for (int index = 0; index < 5; ++index)
    {
        batch.push_back(MakeRecord(today, 8 + index, 1, 1, 1, 1, 1, 1000));
    }
    REQUIRE(store.ApplyBatch(batch.data(), batch.size()));

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{1});
    REQUIRE_EQ(snapshot.daily[0].cjk, std::int64_t{5});
    REQUIRE_EQ(snapshot.daily[0].latin, std::int64_t{5});
    REQUIRE_EQ(snapshot.daily[0].digit, std::int64_t{5});
    REQUIRE_EQ(snapshot.daily[0].punct, std::int64_t{5});
    REQUIRE_EQ(snapshot.daily[0].other, std::int64_t{5});
    REQUIRE_EQ(snapshot.daily[0].active_ms, std::int64_t{5000});
    REQUIRE_EQ(snapshot.hourly.size(), std::size_t{5});
    for (const Statistics::HourlyRow &row : snapshot.hourly)
    {
        REQUIRE_EQ(row.chars, std::int64_t{5});
    }

    // Empty and malformed batches are no-ops, not errors.
    REQUIRE(store.ApplyBatch(nullptr, 0));
    REQUIRE(!store.ApplyBatch(nullptr, 2));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_clear_keeps_only_the_recent_window)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-清理");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));

    const int32_t today = DayKeyDaysAgo(0);
    const int32_t recent = DayKeyDaysAgo(10);
    const int32_t middle = DayKeyDaysAgo(40);
    const int32_t old = DayKeyDaysAgo(100);
    REQUIRE(store.Apply(MakeRecord(today, 9, 1, 0, 0, 0, 0, 10)));
    REQUIRE(store.Apply(MakeRecord(recent, 9, 1, 0, 0, 0, 0, 10)));
    REQUIRE(store.Apply(MakeRecord(middle, 9, 1, 0, 0, 0, 0, 10)));
    REQUIRE(store.Apply(MakeRecord(old, 9, 1, 0, 0, 0, 0, 10)));

    // 1y keeps everything here (every row is younger), so nothing goes.
    REQUIRE(store.Clear("365d"));
    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{4});

    // 90d keeps the most recent 90 days, so only the 100-day-old row goes.
    REQUIRE(store.Clear("90d"));
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{3});
    REQUIRE_EQ(snapshot.daily[0].day, middle);
    REQUIRE_EQ(snapshot.daily[1].day, recent);
    REQUIRE_EQ(snapshot.daily[2].day, today);
    REQUIRE_EQ(snapshot.hourly.size(), std::size_t{3});
    REQUIRE(snapshot.has_first_day);
    REQUIRE_EQ(snapshot.first_day, middle);

    // 30d then drops the 40-day-old row as well.
    REQUIRE(store.Clear("30d"));
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.daily[0].day, recent);
    REQUIRE_EQ(snapshot.daily[1].day, today);
    REQUIRE_EQ(snapshot.hourly.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.first_day, recent);

    // "forever" is a valid choice that means "keep everything" -- it must not delete a row.
    REQUIRE(store.Clear("forever"));
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.hourly.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.first_day, recent);

    // An unknown, missing or retired range is refused and changes nothing.
    // "all" was the pre-retention-window spelling; it is no longer a legal value.
    REQUIRE(!store.Clear("7d"));
    REQUIRE(!store.Clear(""));
    REQUIRE(!store.Clear("all"));
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{2});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_clear_that_drops_every_row_reports_an_empty_panel)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-清空");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    REQUIRE(store.Apply(MakeRecord(DayKeyDaysAgo(100), 9, 7, 0, 0, 0, 0, 100)));
    REQUIRE(store.Apply(MakeRecord(DayKeyDaysAgo(40), 9, 3, 0, 0, 0, 0, 100)));

    // A window that keeps none of the recorded days must drop first_day together with the rows. A
    // leftover first_day would make the panel render an all-zero chart instead of its empty state.
    REQUIRE(store.Clear("30d"));
    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE(snapshot.daily.empty());
    REQUIRE(snapshot.hourly.empty());
    REQUIRE(!snapshot.has_first_day);
    REQUIRE_EQ(ScalarInt(db_path, "SELECT COUNT(*) FROM stats_meta WHERE key='first_day'"), std::int64_t{0});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_serializes_concurrent_writes)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-并发");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));

    static constexpr int kThreads = 4;
    static constexpr int kPerThread = 25;
    std::atomic<int> thread_failures{0};
    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int index = 0; index < kThreads; ++index)
    {
        writers.emplace_back([&store, &thread_failures, index] {
            const int32_t day = DayKeyDaysAgo(index);
            for (int repeat = 0; repeat < kPerThread; ++repeat)
            {
                // Not REQUIRE: a throw inside a thread would terminate the test process.
                if (!store.Apply(MakeRecord(day, 12, 1, 1, 1, 1, 1, 100)))
                {
                    ++thread_failures;
                }
            }
        });
    }
    for (std::thread &writer : writers)
    {
        writer.join();
    }
    REQUIRE_EQ(thread_failures.load(), 0);

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{kThreads});
    for (const Statistics::DailyRow &row : snapshot.daily)
    {
        REQUIRE_EQ(row.cjk, std::int64_t{kPerThread});
        REQUIRE_EQ(row.active_ms, std::int64_t{kPerThread} * 100);
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_can_be_read_safely_without_any_text_columns)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-隐私");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    const int32_t today = DayKeyDaysAgo(0);
    REQUIRE(store.Apply(MakeRecord(today, 11, 1, 2, 3, 4, 5, 100)));

    // Exactly the counters, nothing that could hold text: a column added later that carries input
    // content would be a privacy regression, so both names and types are pinned here.
    const std::vector<std::string> expected_columns = {"day_key", "cjk",   "latin",    "digit",
                                                       "punct",   "other", "active_ms"};
    const auto daily_columns = TableColumns(db_path, "stats_daily");
    REQUIRE_EQ(daily_columns.size(), expected_columns.size());
    for (std::size_t index = 0; index < expected_columns.size(); ++index)
    {
        REQUIRE_EQ(daily_columns[index].first, expected_columns[index]);
        REQUIRE_EQ(daily_columns[index].second, std::string("INTEGER"));
    }
    const std::vector<std::string> expected_hourly = {"day_key", "hour", "chars", "active_ms"};
    const auto hourly_columns = TableColumns(db_path, "stats_hourly");
    REQUIRE_EQ(hourly_columns.size(), expected_hourly.size());
    for (std::size_t index = 0; index < expected_hourly.size(); ++index)
    {
        REQUIRE_EQ(hourly_columns[index].first, expected_hourly[index]);
        REQUIRE_EQ(hourly_columns[index].second, std::string("INTEGER"));
    }
    // stats_meta holds a schema version, the first recorded day and the last automatic-retention
    // date, nothing else.
    const auto meta_columns = TableColumns(db_path, "stats_meta");
    REQUIRE_EQ(meta_columns.size(), std::size_t{2});
    REQUIRE_EQ(ScalarInt(db_path, "SELECT COUNT(*) FROM stats_meta"), std::int64_t{3});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_retention_trims_on_the_first_write_of_a_new_day)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-跨天清理");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t yesterday = DayKeyDaysAgo(1);
    const int32_t old = DayKeyDaysAgo(100);

    // Default policy: both rows land untouched while the policy is still "forever".
    REQUIRE(store.Apply(MakeRecord(old, 9, 7, 0, 0, 0, 0, 100)));
    REQUIRE(store.Apply(MakeRecord(today, 8, 1, 0, 0, 0, 0, 100)));

    // Move the throttle marker back a day so this batch is the first write of the new local day.
    ExecuteSql(db_path,
               "UPDATE stats_meta SET value='" + std::to_string(yesterday) + "' WHERE key='last_retention_day'");
    Statistics::RetentionPolicy policy;
    policy.range = "30d";
    const FanyImeStatsRecord record = MakeRecord(today, 9, 2, 0, 0, 0, 0, 100);
    REQUIRE(store.ApplyBatch(&record, 1, policy));

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{1});
    REQUIRE_EQ(snapshot.daily[0].day, today);
    REQUIRE_EQ(snapshot.daily[0].cjk, std::int64_t{3});
    REQUIRE_EQ(snapshot.hourly.size(), std::size_t{2});
    // first_day follows what is left rather than staying on the deleted row.
    REQUIRE(snapshot.has_first_day);
    REQUIRE_EQ(snapshot.first_day, today);
    // The check date is stamped in the same transaction, so the next write today skips the check.
    REQUIRE_EQ(ScalarInt(db_path, "SELECT value FROM stats_meta WHERE key='last_retention_day'"), std::int64_t{today});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_retention_runs_at_most_once_per_day)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-节流");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t yesterday = DayKeyDaysAgo(1);
    const int32_t old = DayKeyDaysAgo(100);

    Statistics::RetentionPolicy policy;
    policy.range = "30d";
    // The first write of the day runs the trim and stamps today.
    const FanyImeStatsRecord first = MakeRecord(today, 9, 1, 0, 0, 0, 0, 100);
    REQUIRE(store.ApplyBatch(&first, 1, policy));

    // A row old enough to be trimmed shows up after the trim. A same-day write must not touch it:
    // that would mean the delete runs on every batch instead of once a day.
    ExecuteSql(db_path, "INSERT INTO stats_daily(day_key,cjk) VALUES(" + std::to_string(old) + ",5)");
    const FanyImeStatsRecord second = MakeRecord(today, 10, 1, 0, 0, 0, 0, 100);
    REQUIRE(store.ApplyBatch(&second, 1, policy));
    REQUIRE_EQ(ScalarInt(db_path, "SELECT COUNT(*) FROM stats_daily WHERE day_key=" + std::to_string(old)),
               std::int64_t{1});

    // Once the marker belongs to another day, the next write trims it.
    ExecuteSql(db_path,
               "UPDATE stats_meta SET value='" + std::to_string(yesterday) + "' WHERE key='last_retention_day'");
    REQUIRE(store.ApplyBatch(&second, 1, policy));
    REQUIRE_EQ(ScalarInt(db_path, "SELECT COUNT(*) FROM stats_daily WHERE day_key=" + std::to_string(old)),
               std::int64_t{0});
    REQUIRE_EQ(ScalarInt(db_path, "SELECT value FROM stats_meta WHERE key='last_retention_day'"), std::int64_t{today});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_forever_keeps_every_row_and_still_stamps_the_check_day)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-永久");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t old = DayKeyDaysAgo(500);

    Statistics::RetentionPolicy forever;
    forever.range = "forever";
    const FanyImeStatsRecord old_record = MakeRecord(old, 9, 7, 0, 0, 0, 0, 100);
    const FanyImeStatsRecord today_record = MakeRecord(today, 9, 1, 0, 0, 0, 0, 100);
    REQUIRE(store.ApplyBatch(&old_record, 1, forever));
    REQUIRE(store.ApplyBatch(&today_record, 1));

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.daily[0].day, old);
    // "forever" still stamps the check date: otherwise every write would re-run the lookup.
    REQUIRE_EQ(ScalarInt(db_path, "SELECT value FROM stats_meta WHERE key='last_retention_day'"), std::int64_t{today});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_unknown_policy_deletes_nothing)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-未知策略");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t old = DayKeyDaysAgo(500);

    REQUIRE(store.Apply(MakeRecord(old, 9, 7, 0, 0, 0, 0, 100)));
    Statistics::RetentionPolicy unknown;
    unknown.range = "7d";
    const FanyImeStatsRecord record = MakeRecord(today, 9, 1, 0, 0, 0, 0, 100);
    REQUIRE(store.ApplyBatch(&record, 1, unknown));

    // A policy the store does not recognise must fail safe (delete nothing), never guess a window.
    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{2});
    REQUIRE_EQ(ScalarInt(db_path, "SELECT COUNT(*) FROM stats_daily WHERE day_key=" + std::to_string(old)),
               std::int64_t{1});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_rolls_back_retention_together_with_a_failed_batch)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-回滚");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t yesterday = DayKeyDaysAgo(1);
    const int32_t old = DayKeyDaysAgo(100);

    REQUIRE(store.Apply(MakeRecord(old, 9, 7, 0, 0, 0, 0, 100)));
    REQUIRE(store.Apply(MakeRecord(today, 8, 1, 0, 0, 0, 0, 100)));
    ExecuteSql(db_path,
               "UPDATE stats_meta SET value='" + std::to_string(yesterday) + "' WHERE key='last_retention_day'");
    // Make the daily insert fail so the whole batch is rolled back after the trim already ran.
    ExecuteSql(db_path, "CREATE TRIGGER reject_daily_insert BEFORE INSERT ON stats_daily"
                        " BEGIN SELECT RAISE(ABORT,'test'); END");

    Statistics::RetentionPolicy policy;
    policy.range = "30d";
    const FanyImeStatsRecord record = MakeRecord(today, 9, 2, 0, 0, 0, 0, 100);
    REQUIRE(!store.ApplyBatch(&record, 1, policy));

    // Nothing may survive a failed batch: not the trim, not the marker, not the counters.
    REQUIRE_EQ(ScalarInt(db_path, "SELECT COUNT(*) FROM stats_daily WHERE day_key=" + std::to_string(old)),
               std::int64_t{1});
    REQUIRE_EQ(ScalarInt(db_path, "SELECT cjk FROM stats_daily WHERE day_key=" + std::to_string(today)),
               std::int64_t{1});
    REQUIRE_EQ(ScalarInt(db_path, "SELECT value FROM stats_meta WHERE key='last_retention_day'"),
               std::int64_t{yesterday});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_store_sets_wal_and_normal_synchronous_on_every_connection)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-pragma");
    const std::filesystem::path db_path = root / L"msime_stats.db";
    Statistics::StatsStore store(test::Utf8(db_path));
    REQUIRE(store.Apply(MakeRecord(DayKeyDaysAgo(0), 9, 1, 0, 0, 0, 0, 100)));

    Statistics::PragmaState state;
    REQUIRE(store.ReadPragmaState(state));
    REQUIRE_EQ(state.journal_mode, std::string("wal"));
    REQUIRE_EQ(state.synchronous, 1);

    // A fresh store and a write-then-read cycle both open a new connection. synchronous is
    // per connection, so a value of 1 here is the proof it is applied on every open rather than
    // only on the first one of the process.
    Statistics::StatsStore reopened(test::Utf8(db_path));
    Statistics::PragmaState reopened_state;
    REQUIRE(reopened.ReadPragmaState(reopened_state));
    REQUIRE_EQ(reopened_state.journal_mode, std::string("wal"));
    REQUIRE_EQ(reopened_state.synchronous, 1);
    REQUIRE(reopened.Apply(MakeRecord(DayKeyDaysAgo(0), 10, 1, 0, 0, 0, 0, 100)));
    REQUIRE(reopened.ReadPragmaState(reopened_state));
    REQUIRE_EQ(reopened_state.synchronous, 1);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_default_db_path_follows_the_data_directory_override)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-store-路径");
    const ScopedEnv data_dir(L"METASEQUOIA_IME_DATA_DIR", root.wstring());

    REQUIRE_EQ(Statistics::default_stats_db_path(), test::Utf8(root / L"msime_stats.db"));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
