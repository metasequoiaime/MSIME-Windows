#pragma once

#include "engine/contracts/windows_ipc.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Aggregated local input statistics. The store holds counters only: day, hour bucket, the five
// character classes and active time. No committed text, pinyin or candidate ever reaches this
// layer -- the TSF DLL classifies inside its own process and sends five integers per composition.
namespace Statistics
{
// %LOCALAPPDATA%\metasequoiaime\msime_stats.db, resolved through the same data directory as the
// user dictionary (METASEQUOIA_IME_DATA_DIR / the installer's DataDir override included).
// Statistics live in their own file so clearing them can never touch the user dictionary and a
// damaged counts database cannot take the dictionary down with it.
std::string default_stats_db_path();

struct DailyRow
{
    int32_t day = 0; // YYYYMMDD, local time
    int64_t cjk = 0;
    int64_t latin = 0;
    int64_t digit = 0;
    int64_t punct = 0;
    int64_t other = 0;
    int64_t active_ms = 0;
};

struct HourlyRow
{
    int32_t day = 0;
    int32_t hour = 0;
    int64_t chars = 0;
    int64_t active_ms = 0;
};

struct Snapshot
{
    std::vector<DailyRow> daily;
    std::vector<HourlyRow> hourly;
    bool has_first_day = false;
    int32_t first_day = 0;
};

// One connection per operation. The aggregator thread and the settings worker both touch this
// store, and a sqlite3 handle is never shared across threads; the mutex serializes the whole
// operation so a write batch cannot interleave with a clear.
class StatsStore
{
  public:
    explicit StatsStore(std::string db_path);

    // Folds one DLL record into the daily and hourly buckets. Returns false on any storage
    // failure; callers treat statistics as droppable data and never surface the error.
    bool Apply(const FanyImeStatsRecord &record);
    bool ApplyBatch(const FanyImeStatsRecord *records, std::size_t count);

    bool Query(Snapshot &snapshot);

    // range: "30d" | "90d" | "all". Keeps the most recent 30/90 local days and deletes everything
    // older; "all" deletes everything. Keeps stats_meta.first_day consistent with what is left.
    bool Clear(const std::string &range);

  private:
    std::mutex mutex_;
    std::string db_path_;
};

// Process-wide store shared by the pipe listener (writes) and the settings worker (queries).
StatsStore &SharedStatsStore();
} // namespace Statistics
