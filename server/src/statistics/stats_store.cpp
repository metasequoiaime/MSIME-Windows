#include "statistics/stats_store.h"

#include <windows.h>

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <utility>

#include "utils/common_utils.h"

namespace Statistics
{
namespace
{
struct DatabaseCloser
{
    void operator()(sqlite3 *db) const
    {
        if (db)
        {
            sqlite3_close(db);
        }
    }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

struct StatementFinalizer
{
    void operator()(sqlite3_stmt *stmt) const
    {
        if (stmt)
        {
            sqlite3_finalize(stmt);
        }
    }
};
using Statement = std::unique_ptr<sqlite3_stmt, StatementFinalizer>;

Statement Prepare(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK)
    {
        return {};
    }
    return Statement(raw);
}

bool Execute(sqlite3 *db, const char *sql)
{
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

// The daily and hourly tables hold counters only. Nothing here can express a character, a pinyin
// string or a candidate word, which is what keeps a fully read database uninteresting to anyone
// who copies it.
bool EnsureSchema(sqlite3 *db)
{
    static const char schema[] = "CREATE TABLE IF NOT EXISTS stats_daily ("
                                 "  day_key   INTEGER PRIMARY KEY,"
                                 "  cjk       INTEGER NOT NULL DEFAULT 0,"
                                 "  latin     INTEGER NOT NULL DEFAULT 0,"
                                 "  digit     INTEGER NOT NULL DEFAULT 0,"
                                 "  punct     INTEGER NOT NULL DEFAULT 0,"
                                 "  other     INTEGER NOT NULL DEFAULT 0,"
                                 "  active_ms INTEGER NOT NULL DEFAULT 0"
                                 ");"
                                 "CREATE TABLE IF NOT EXISTS stats_hourly ("
                                 "  day_key   INTEGER NOT NULL,"
                                 "  hour      INTEGER NOT NULL,"
                                 "  chars     INTEGER NOT NULL DEFAULT 0,"
                                 "  active_ms INTEGER NOT NULL DEFAULT 0,"
                                 "  PRIMARY KEY (day_key, hour)"
                                 ");"
                                 "CREATE TABLE IF NOT EXISTS stats_meta ("
                                 "  key   TEXT PRIMARY KEY,"
                                 "  value TEXT NOT NULL"
                                 ");"
                                 // One merged row per second that saw input. The day/hour buckets
                                 // cannot answer "how fast am I typing right now": the sliding
                                 // windows need a second-level timeline (design.md §1).
                                 "CREATE TABLE IF NOT EXISTS stats_recent ("
                                 "  second    INTEGER PRIMARY KEY,"
                                 "  chars     INTEGER NOT NULL DEFAULT 0,"
                                 "  active_ms INTEGER NOT NULL DEFAULT 0"
                                 ");";
    if (!Execute(db, schema))
    {
        return false;
    }
    return Execute(db, "INSERT OR IGNORE INTO stats_meta(key,value) VALUES('schema_version','1');");
}

Database OpenDatabase(const std::string &db_path)
{
    if (db_path.empty())
    {
        return {};
    }
    const std::filesystem::path path = std::filesystem::u8path(db_path);
    if (!path.parent_path().empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    sqlite3 *raw = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
    {
        if (raw)
        {
            sqlite3_close(raw);
        }
        return {};
    }
    Database db(raw);
    sqlite3_busy_timeout(db.get(), 3000);
    // Statistics are droppable data written every 250ms while the user types, so the write side is
    // what needs the cheaper durability: WAL plus synchronous=NORMAL collapses the per-batch fsync
    // and lets the settings panel read while a batch is being written. The cost is losing the last
    // few seconds of counts on a power loss or a hard kill, which is acceptable here.
    //
    // journal_mode lives in the database file and changing it may need a write lock, while the
    // settings process now opens a connection every second to poll the live windows. Read it back
    // and only write when it is not WAL yet, so the polling connection never competes with a batch
    // for the lock. The read is scoped on purpose: an unfinalized statement would keep a read
    // transaction open and the WAL switch would fail with "cannot change into wal mode from within
    // a transaction". synchronous is per connection and lock-free, so it is still set every open.
    std::string current_mode;
    {
        Statement journal = Prepare(db.get(), "PRAGMA journal_mode");
        if (!journal || sqlite3_step(journal.get()) != SQLITE_ROW)
        {
            return {};
        }
        const unsigned char *mode = sqlite3_column_text(journal.get(), 0);
        if (mode != nullptr)
        {
            current_mode = reinterpret_cast<const char *>(mode);
        }
    }
    if ((current_mode != "wal" && !Execute(db.get(), "PRAGMA journal_mode = WAL;")) ||
        !Execute(db.get(), "PRAGMA synchronous = NORMAL;"))
    {
        return {};
    }
    return db;
}

int32_t LocalDayKey()
{
    SYSTEMTIME local{};
    GetLocalTime(&local);
    return static_cast<int32_t>(local.wYear) * 10000 + static_cast<int32_t>(local.wMonth) * 100 + local.wDay;
}

// Window lengths of the live typing speed, defined once so the query bounds and the daily prune
// cannot drift apart (design.md §2).
constexpr std::int64_t kFiveMinutesSeconds = 5 * 60;
constexpr std::int64_t kOneHourSeconds = 60 * 60;
constexpr std::int64_t kRecentRetentionSeconds = 24 * 60 * 60;

// Local calendar date `days_ago` days before today, as YYYYMMDD. The wall-clock fields are fed
// through file-time arithmetic on purpose: no timezone conversion happens, so the result is simply
// today's date counted back, which is what "the last 30 days" means to the user.
int32_t LocalDayKeyDaysAgo(int days_ago)
{
    SYSTEMTIME local{};
    GetLocalTime(&local);
    FILETIME stamp{};
    if (!SystemTimeToFileTime(&local, &stamp))
    {
        return LocalDayKey();
    }
    ULARGE_INTEGER ticks{};
    ticks.LowPart = stamp.dwLowDateTime;
    ticks.HighPart = stamp.dwHighDateTime;
    constexpr unsigned long long kTicksPerDay = 24ull * 60ull * 60ull * 10000000ull;
    ticks.QuadPart -= static_cast<unsigned long long>(days_ago) * kTicksPerDay;
    FILETIME adjusted{};
    adjusted.dwLowDateTime = ticks.LowPart;
    adjusted.dwHighDateTime = ticks.HighPart;
    SYSTEMTIME result{};
    if (!FileTimeToSystemTime(&adjusted, &result))
    {
        return LocalDayKey();
    }
    return static_cast<int32_t>(result.wYear) * 10000 + static_cast<int32_t>(result.wMonth) * 100 + result.wDay;
}

bool StepDone(sqlite3_stmt *stmt)
{
    return sqlite3_step(stmt) == SQLITE_DONE;
}

bool DeleteFromDay(sqlite3 *db, const char *sql, int32_t cutoff_day)
{
    Statement stmt = Prepare(db, sql);
    if (!stmt || sqlite3_bind_int(stmt.get(), 1, cutoff_day) != SQLITE_OK)
    {
        return false;
    }
    return StepDone(stmt.get());
}

// stats_meta.first_day is what the panel uses to tell "no records yet" from "records exist"; a
// clear that leaves it behind would keep an empty panel showing charts.
bool RecomputeFirstDay(sqlite3 *db)
{
    if (!Execute(db, "DELETE FROM stats_meta WHERE key='first_day'"))
    {
        return false;
    }
    Statement oldest = Prepare(db, "SELECT MIN(day_key) FROM stats_daily");
    if (!oldest || sqlite3_step(oldest.get()) != SQLITE_ROW)
    {
        return false;
    }
    if (sqlite3_column_type(oldest.get(), 0) == SQLITE_NULL)
    {
        return true;
    }
    const sqlite3_int64 first_day = sqlite3_column_int64(oldest.get(), 0);
    Statement insert = Prepare(db, "INSERT INTO stats_meta(key,value) VALUES('first_day',?1)");
    if (!insert || sqlite3_bind_int64(insert.get(), 1, first_day) != SQLITE_OK)
    {
        return false;
    }
    return StepDone(insert.get());
}

// Shared by Clear and the automatic cross-day cleanup: the mapping from a retention value to a
// number of days exists once so the two paths cannot drift apart. 0 means "no window" -- either
// "forever" or an unknown value -- and callers must treat it as "delete nothing".
int RetentionWindowDays(const std::string &range)
{
    if (range == "30d")
    {
        return 30;
    }
    if (range == "90d")
    {
        return 90;
    }
    if (range == "180d")
    {
        return 180;
    }
    if (range == "365d")
    {
        return 365;
    }
    return 0;
}

// The single deletion implementation, used both by Clear (retention changed) and by the automatic
// cleanup inside ApplyBatch. The caller owns the transaction, so a failure here rolls back
// together with whatever else the transaction was doing.
bool DeleteRowsOlderThan(sqlite3 *db, int32_t cutoff_day)
{
    // cutoff is the oldest day that survives, so the comparison is strict.
    return DeleteFromDay(db, "DELETE FROM stats_daily WHERE day_key < ?1", cutoff_day) &&
           DeleteFromDay(db, "DELETE FROM stats_hourly WHERE day_key < ?1", cutoff_day) && RecomputeFirstDay(db);
}

// stats_meta.last_retention_day throttles the automatic trim to one run per local day, and lives in
// the database so a server restart cannot re-trim the same day. A missing or unreadable marker
// means "never ran": trimming twice is idempotent, so that is the safe direction.
bool ShouldRunRetention(sqlite3 *db, int32_t today)
{
    Statement stmt = Prepare(db, "SELECT value FROM stats_meta WHERE key='last_retention_day'");
    if (!stmt)
    {
        return false;
    }
    const int step = sqlite3_step(stmt.get());
    if (step == SQLITE_DONE)
    {
        return true;
    }
    if (step != SQLITE_ROW)
    {
        return false;
    }
    const unsigned char *value = sqlite3_column_text(stmt.get(), 0);
    if (value == nullptr)
    {
        return true;
    }
    char *end = nullptr;
    const long parsed = std::strtol(reinterpret_cast<const char *>(value), &end, 10);
    if (end == reinterpret_cast<const char *>(value))
    {
        return true;
    }
    return static_cast<int32_t>(parsed) != today;
}

// Stamped on every batch so the marker always means "maintenance was attempted today", whether or
// not the policy asked for a history trim.
bool UpdateLastRetentionDay(sqlite3 *db, int32_t today)
{
    Statement stmt = Prepare(db, "INSERT INTO stats_meta(key,value) VALUES('last_retention_day',?1)"
                                 " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    if (!stmt || sqlite3_bind_int(stmt.get(), 1, today) != SQLITE_OK)
    {
        return false;
    }
    return StepDone(stmt.get());
}

// One accumulated row per day and one per hour bucket. The upsert adds into the existing row so a
// batch that arrives twice (a retried connection, a replayed frame) would still be counted twice --
// which is why the DLL never re-sends a frame it already wrote.
bool ApplyRecords(sqlite3 *db, const FanyImeStatsRecord *records, std::size_t count)
{
    Statement daily = Prepare(db, "INSERT INTO stats_daily(day_key,cjk,latin,digit,punct,other,active_ms)"
                                  " VALUES(?1,?2,?3,?4,?5,?6,?7)"
                                  " ON CONFLICT(day_key) DO UPDATE SET"
                                  "  cjk = cjk + excluded.cjk,"
                                  "  latin = latin + excluded.latin,"
                                  "  digit = digit + excluded.digit,"
                                  "  punct = punct + excluded.punct,"
                                  "  other = other + excluded.other,"
                                  "  active_ms = active_ms + excluded.active_ms");
    Statement hourly = Prepare(db, "INSERT INTO stats_hourly(day_key,hour,chars,active_ms) VALUES(?1,?2,?3,?4)"
                                   " ON CONFLICT(day_key,hour) DO UPDATE SET"
                                   "  chars = chars + excluded.chars,"
                                   "  active_ms = active_ms + excluded.active_ms");
    Statement first_day = Prepare(db, "INSERT INTO stats_meta(key,value) VALUES('first_day',?1)"
                                      " ON CONFLICT(key) DO UPDATE SET value ="
                                      " CASE WHEN CAST(excluded.value AS INTEGER) < CAST(stats_meta.value AS INTEGER)"
                                      " THEN excluded.value ELSE stats_meta.value END");
    if (!daily || !hourly || !first_day)
    {
        return false;
    }

    for (std::size_t index = 0; index < count; ++index)
    {
        const FanyImeStatsRecord &record = records[index];
        const int64_t chars =
            static_cast<int64_t>(record.cjk) + record.latin + record.digit + record.punct + record.other;

        sqlite3_reset(daily.get());
        sqlite3_clear_bindings(daily.get());
        sqlite3_bind_int(daily.get(), 1, static_cast<int>(record.day_key));
        sqlite3_bind_int64(daily.get(), 2, static_cast<int64_t>(record.cjk));
        sqlite3_bind_int64(daily.get(), 3, static_cast<int64_t>(record.latin));
        sqlite3_bind_int64(daily.get(), 4, static_cast<int64_t>(record.digit));
        sqlite3_bind_int64(daily.get(), 5, static_cast<int64_t>(record.punct));
        sqlite3_bind_int64(daily.get(), 6, static_cast<int64_t>(record.other));
        sqlite3_bind_int64(daily.get(), 7, record.active_ms);

        sqlite3_reset(hourly.get());
        sqlite3_clear_bindings(hourly.get());
        sqlite3_bind_int(hourly.get(), 1, static_cast<int>(record.day_key));
        sqlite3_bind_int(hourly.get(), 2, static_cast<int>(record.hour));
        sqlite3_bind_int64(hourly.get(), 3, chars);
        sqlite3_bind_int64(hourly.get(), 4, record.active_ms);

        sqlite3_reset(first_day.get());
        sqlite3_clear_bindings(first_day.get());
        sqlite3_bind_int(first_day.get(), 1, static_cast<int>(record.day_key));

        if (!StepDone(daily.get()) || !StepDone(hourly.get()) || !StepDone(first_day.get()))
        {
            return false;
        }
    }
    return true;
}

// One merged row per second: repeated batches inside the same second add into it instead of
// overwriting (design.md §2). The caller owns the transaction, so a failure rolls back together
// with the batch that produced it.
bool ApplyRecentSample(sqlite3 *db, std::int64_t second, std::int64_t chars, std::int64_t active_ms)
{
    Statement stmt = Prepare(db, "INSERT INTO stats_recent(second,chars,active_ms) VALUES(?1,?2,?3)"
                                 " ON CONFLICT(second) DO UPDATE SET"
                                 "  chars = chars + excluded.chars,"
                                 "  active_ms = active_ms + excluded.active_ms");
    if (!stmt || sqlite3_bind_int64(stmt.get(), 1, second) != SQLITE_OK ||
        sqlite3_bind_int64(stmt.get(), 2, chars) != SQLITE_OK ||
        sqlite3_bind_int64(stmt.get(), 3, active_ms) != SQLITE_OK)
    {
        return false;
    }
    return StepDone(stmt.get());
}

// Samples older than the longest window can never be read again, so they only have to survive
// until the next daily maintenance. That is the whole growth bound on stats_recent: the caller
// runs this at most once a day, and the worst case is ~48 hours of rows.
bool PruneRecentSamples(sqlite3 *db, std::int64_t cutoff_second)
{
    Statement stmt = Prepare(db, "DELETE FROM stats_recent WHERE second < ?1");
    if (!stmt || sqlite3_bind_int64(stmt.get(), 1, cutoff_second) != SQLITE_OK)
    {
        return false;
    }
    return StepDone(stmt.get());
}

bool ReadFirstDay(sqlite3 *db, Snapshot &snapshot)
{
    Statement stmt = Prepare(db, "SELECT value FROM stats_meta WHERE key='first_day'");
    if (!stmt)
    {
        return false;
    }
    const int step = sqlite3_step(stmt.get());
    if (step == SQLITE_DONE)
    {
        return true;
    }
    if (step != SQLITE_ROW)
    {
        return false;
    }
    const unsigned char *value = sqlite3_column_text(stmt.get(), 0);
    if (value == nullptr)
    {
        return true;
    }
    char *end = nullptr;
    const long parsed = std::strtol(reinterpret_cast<const char *>(value), &end, 10);
    if (end != reinterpret_cast<const char *>(value) && parsed > 0)
    {
        snapshot.has_first_day = true;
        snapshot.first_day = static_cast<int32_t>(parsed);
    }
    return true;
}
} // namespace

std::string default_stats_db_path()
{
    const std::string directory = CommonUtils::get_ime_data_path();
    if (directory.empty())
    {
        return {};
    }
    return directory + "\\msime_stats.db";
}

std::int64_t NowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

StatsStore::StatsStore(std::string db_path) : db_path_(std::move(db_path))
{
}

bool StatsStore::Apply(const FanyImeStatsRecord &record)
{
    return ApplyBatch(&record, 1);
}

bool StatsStore::ApplyBatch(const FanyImeStatsRecord *records, std::size_t count, const RetentionPolicy &policy,
                            std::int64_t now_seconds)
{
    if (count == 0)
    {
        return true;
    }
    if (records == nullptr)
    {
        return false;
    }
    // The live sample is one second of merged activity: the five character classes and the active
    // time of the whole batch. `arrival` is the second the frame arrived on -- the records
    // themselves only carry hour precision (design.md §2).
    const std::int64_t arrival = now_seconds < 0 ? NowSeconds() : now_seconds;
    std::int64_t recent_chars = 0;
    std::int64_t recent_active_ms = 0;
    for (std::size_t index = 0; index < count; ++index)
    {
        const FanyImeStatsRecord &record = records[index];
        recent_chars +=
            static_cast<std::int64_t>(record.cjk) + record.latin + record.digit + record.punct + record.other;
        recent_active_ms += record.active_ms;
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    Database db = OpenDatabase(db_path_);
    if (!db || !EnsureSchema(db.get()))
    {
        return false;
    }
    // One transaction per batch: a frame is all-or-nothing, so a failed write never leaves a day
    // half-counted. The sample row and the daily trim ride the same transaction for the same
    // reason -- a batch that fails to land must not leave activity counted or history trimmed.
    if (!Execute(db.get(), "BEGIN IMMEDIATE"))
    {
        return false;
    }
    const int32_t today = LocalDayKey();
    const int window_days = RetentionWindowDays(policy.range);
    // The first write of a new local day also does the daily maintenance. The per-second samples
    // are always pruned to 24 hours (the live window is one day whatever the policy is), while the
    // historical aggregates follow the retention policy.
    if (ShouldRunRetention(db.get(), today))
    {
        if (!PruneRecentSamples(db.get(), arrival - kRecentRetentionSeconds) ||
            (window_days > 0 && !DeleteRowsOlderThan(db.get(), LocalDayKeyDaysAgo(window_days - 1))))
        {
            Execute(db.get(), "ROLLBACK");
            return false;
        }
    }
    if (!UpdateLastRetentionDay(db.get(), today) || !ApplyRecords(db.get(), records, count) ||
        !ApplyRecentSample(db.get(), arrival, recent_chars, recent_active_ms))
    {
        Execute(db.get(), "ROLLBACK");
        return false;
    }
    if (!Execute(db.get(), "COMMIT"))
    {
        Execute(db.get(), "ROLLBACK");
        return false;
    }
    return true;
}

bool StatsStore::QueryRecent(std::int64_t now_seconds, RecentWindows &windows) const
{
    windows = RecentWindows{};
    const std::lock_guard<std::mutex> lock(mutex_);
    Database db = OpenDatabase(db_path_);
    if (!db)
    {
        return false;
    }
    // No EnsureSchema here on purpose: this query runs once a second, and CREATE TABLE IF NOT
    // EXISTS still takes the write lock when the table already exists, which would make the
    // polling settings process contend with every batch the server writes. The table is missing
    // only when no batch has ever landed, which is the same "no activity yet" answer as an empty
    // table (design.md §3).
    Statement exists = Prepare(db.get(), "SELECT 1 FROM sqlite_master WHERE type='table' AND name='stats_recent'");
    if (!exists)
    {
        return false;
    }
    if (sqlite3_step(exists.get()) != SQLITE_ROW)
    {
        return true;
    }
    // One scan for all three windows: the panel polls this once a second, so walking the table
    // three times (or reading the daily/hourly history) is not an option (design.md §2). The
    // boundaries are inclusive: a sample exactly `window` seconds old still counts.
    Statement stmt = Prepare(db.get(), "SELECT"
                                       " COALESCE(SUM(CASE WHEN second >= ?1 THEN chars END), 0),"
                                       " COALESCE(SUM(CASE WHEN second >= ?1 THEN active_ms END), 0),"
                                       " COALESCE(SUM(CASE WHEN second >= ?2 THEN chars END), 0),"
                                       " COALESCE(SUM(CASE WHEN second >= ?2 THEN active_ms END), 0),"
                                       " COALESCE(SUM(chars), 0), COALESCE(SUM(active_ms), 0)"
                                       " FROM stats_recent WHERE second >= ?3");
    if (!stmt || sqlite3_bind_int64(stmt.get(), 1, now_seconds - kFiveMinutesSeconds) != SQLITE_OK ||
        sqlite3_bind_int64(stmt.get(), 2, now_seconds - kOneHourSeconds) != SQLITE_OK ||
        sqlite3_bind_int64(stmt.get(), 3, now_seconds - kRecentRetentionSeconds) != SQLITE_OK ||
        sqlite3_step(stmt.get()) != SQLITE_ROW)
    {
        return false;
    }
    windows.m5.chars = sqlite3_column_int64(stmt.get(), 0);
    windows.m5.active_ms = sqlite3_column_int64(stmt.get(), 1);
    windows.h1.chars = sqlite3_column_int64(stmt.get(), 2);
    windows.h1.active_ms = sqlite3_column_int64(stmt.get(), 3);
    windows.d1.chars = sqlite3_column_int64(stmt.get(), 4);
    windows.d1.active_ms = sqlite3_column_int64(stmt.get(), 5);
    return true;
}

bool StatsStore::Query(Snapshot &snapshot)
{
    snapshot = Snapshot{};
    const std::lock_guard<std::mutex> lock(mutex_);
    Database db = OpenDatabase(db_path_);
    if (!db || !EnsureSchema(db.get()))
    {
        return false;
    }

    Statement daily = Prepare(db.get(), "SELECT day_key,cjk,latin,digit,punct,other,active_ms FROM stats_daily"
                                        " ORDER BY day_key");
    if (!daily)
    {
        return false;
    }
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(daily.get())) == SQLITE_ROW)
    {
        DailyRow row;
        row.day = sqlite3_column_int(daily.get(), 0);
        row.cjk = sqlite3_column_int64(daily.get(), 1);
        row.latin = sqlite3_column_int64(daily.get(), 2);
        row.digit = sqlite3_column_int64(daily.get(), 3);
        row.punct = sqlite3_column_int64(daily.get(), 4);
        row.other = sqlite3_column_int64(daily.get(), 5);
        row.active_ms = sqlite3_column_int64(daily.get(), 6);
        snapshot.daily.push_back(row);
    }
    if (step != SQLITE_DONE)
    {
        return false;
    }

    Statement hourly = Prepare(db.get(), "SELECT day_key,hour,chars,active_ms FROM stats_hourly ORDER BY day_key,hour");
    if (!hourly)
    {
        return false;
    }
    while ((step = sqlite3_step(hourly.get())) == SQLITE_ROW)
    {
        HourlyRow row;
        row.day = sqlite3_column_int(hourly.get(), 0);
        row.hour = sqlite3_column_int(hourly.get(), 1);
        row.chars = sqlite3_column_int64(hourly.get(), 2);
        row.active_ms = sqlite3_column_int64(hourly.get(), 3);
        snapshot.hourly.push_back(row);
    }
    if (step != SQLITE_DONE)
    {
        return false;
    }

    return ReadFirstDay(db.get(), snapshot);
}

bool StatsStore::ReadPragmaState(PragmaState &state)
{
    state = PragmaState{};
    const std::lock_guard<std::mutex> lock(mutex_);
    Database db = OpenDatabase(db_path_);
    if (!db)
    {
        return false;
    }
    Statement journal = Prepare(db.get(), "PRAGMA journal_mode");
    if (!journal || sqlite3_step(journal.get()) != SQLITE_ROW)
    {
        return false;
    }
    const unsigned char *mode = sqlite3_column_text(journal.get(), 0);
    state.journal_mode = mode != nullptr ? reinterpret_cast<const char *>(mode) : "";
    Statement synchronous = Prepare(db.get(), "PRAGMA synchronous");
    if (!synchronous || sqlite3_step(synchronous.get()) != SQLITE_ROW)
    {
        return false;
    }
    state.synchronous = sqlite3_column_int(synchronous.get(), 0);
    return true;
}

bool StatsStore::Clear(const std::string &range)
{
    // The panel picks a retention window: keep the most recent N local days and drop everything
    // older. "forever" means the user wants to keep everything, so it is a successful no-op rather
    // than a rejected value -- it is part of the contract, not an invalid input. Months and years
    // are approximated by days (3m = 90, 6m = 180, 1y = 365): the difference of a few days is
    // immaterial for trimming old history.
    if (range == "forever")
    {
        return true;
    }
    const int window_days = RetentionWindowDays(range);
    if (window_days <= 0)
    {
        return false;
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    Database db = OpenDatabase(db_path_);
    if (!db || !EnsureSchema(db.get()))
    {
        return false;
    }
    if (!Execute(db.get(), "BEGIN IMMEDIATE"))
    {
        return false;
    }
    if (!DeleteRowsOlderThan(db.get(), LocalDayKeyDaysAgo(window_days - 1)))
    {
        Execute(db.get(), "ROLLBACK");
        return false;
    }
    if (!Execute(db.get(), "COMMIT"))
    {
        Execute(db.get(), "ROLLBACK");
        return false;
    }
    return true;
}

StatsStore &SharedStatsStore()
{
    static StatsStore store(default_stats_db_path());
    return store;
}
} // namespace Statistics
