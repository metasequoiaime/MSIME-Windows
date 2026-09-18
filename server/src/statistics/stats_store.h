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

// Effective SQLite settings of the connection OpenDatabase hands out. Exposed for tests: WAL is
// persisted in the database file, but synchronous is per connection, and the only way to prove it
// is applied on every open -- not just the first one of the process -- is to read it back through
// the same open path the store uses.
struct PragmaState
{
    std::string journal_mode;
    int synchronous = 0;
};

// One deletion policy, shared by the automatic cross-day cleanup and the settings host's trim on
// change. The store never reads configuration; the caller resolves the policy and passes it in.
struct RetentionPolicy
{
    // "30d" | "90d" | "180d" | "365d" | "forever".
    std::string range = "forever";
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
    //
    // Seeding helper: it runs the batch default, i.e. policy "forever", so it stamps the day's
    // retention check WITHOUT trimming. Production writes must go through ApplyBatch with a real
    // policy -- using this one on the live path would silently skip that day's trim.
    bool Apply(const FanyImeStatsRecord &record);
    // The policy is applied on the first write of a new local day (throttled through stats_meta),
    // in the same transaction as the batch.
    bool ApplyBatch(const FanyImeStatsRecord *records, std::size_t count, const RetentionPolicy &policy = {});

    bool Query(Snapshot &snapshot);

    // Reads the pragmas back through a freshly opened connection.
    bool ReadPragmaState(PragmaState &state);

    // range: "30d" | "90d" | "180d" | "365d" | "forever". Keeps the most recent 30/90/180/365
    // local days and deletes everything older; "forever" keeps everything (a no-op). Keeps
    // stats_meta.first_day consistent with what is left.
    bool Clear(const std::string &range);

  private:
    std::mutex mutex_;
    std::string db_path_;
};

// Process-wide store shared by the pipe listener (writes) and the settings worker (queries).
StatsStore &SharedStatsStore();
} // namespace Statistics
