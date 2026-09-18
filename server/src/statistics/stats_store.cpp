#include "statistics/stats_store.h"

#include <windows.h>

#include <sqlite3.h>

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
    return db;
}

int32_t LocalDayKey()
{
    SYSTEMTIME local{};
    GetLocalTime(&local);
    return static_cast<int32_t>(local.wYear) * 10000 + static_cast<int32_t>(local.wMonth) * 100 + local.wDay;
}

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

StatsStore::StatsStore(std::string db_path) : db_path_(std::move(db_path))
{
}

bool StatsStore::Apply(const FanyImeStatsRecord &record)
{
    return ApplyBatch(&record, 1);
}

bool StatsStore::ApplyBatch(const FanyImeStatsRecord *records, std::size_t count)
{
    if (count == 0)
    {
        return true;
    }
    if (records == nullptr)
    {
        return false;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    Database db = OpenDatabase(db_path_);
    if (!db || !EnsureSchema(db.get()))
    {
        return false;
    }
    // One transaction per batch: a frame is all-or-nothing, so a failed write never leaves a day
    // half-counted.
    if (!Execute(db.get(), "BEGIN IMMEDIATE"))
    {
        return false;
    }
    if (!ApplyRecords(db.get(), records, count))
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

bool StatsStore::Clear(const std::string &range)
{
    bool clear_all = false;
    int days = 0;
    if (range == "all")
    {
        clear_all = true;
    }
    else if (range == "30d")
    {
        days = 30;
    }
    else if (range == "90d")
    {
        days = 90;
    }
    else
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

    bool ok = false;
    if (clear_all)
    {
        ok = Execute(db.get(), "DELETE FROM stats_daily") && Execute(db.get(), "DELETE FROM stats_hourly") &&
             Execute(db.get(), "DELETE FROM stats_meta WHERE key='first_day'");
    }
    else
    {
        // Keep the most recent `days` local days and drop everything older, matching the
        // panel's "clear data older than N days" wording. cutoff is the oldest day that
        // survives, so the comparison is strict.
        const int32_t cutoff = LocalDayKeyDaysAgo(days - 1);
        ok = DeleteFromDay(db.get(), "DELETE FROM stats_daily WHERE day_key < ?1", cutoff) &&
             DeleteFromDay(db.get(), "DELETE FROM stats_hourly WHERE day_key < ?1", cutoff) &&
             RecomputeFirstDay(db.get());
    }
    if (!ok)
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
