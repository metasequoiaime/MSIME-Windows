#include "../contracts/assets/assets.h"
#include "../contracts/dictionary/format.h"
#include "user_dictionary_journal.h"
#include <metasequoia/personal_dictionary.h>

#include <sqlite3.h>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>
#include <algorithm>
#include <limits>
#include <mutex>
#include <numeric>
#include "../core/data_path.h"
#include "../english/english_dictionary.h"

namespace user_dictionary
{
namespace
{
struct DbCloser
{
    void operator()(sqlite3 *db) const
    {
        if (db != nullptr)
            sqlite3_close(db);
    }
};
using Db = std::unique_ptr<sqlite3, DbCloser>;

struct StmtCloser
{
    void operator()(sqlite3_stmt *stmt) const
    {
        if (stmt != nullptr)
            sqlite3_finalize(stmt);
    }
};
using Stmt = std::unique_ptr<sqlite3_stmt, StmtCloser>;

const char *kind_name(DictionaryKind kind)
{
    switch (kind)
    {
    case DictionaryKind::Pinyin:
        return "pinyin";
    case DictionaryKind::Wubi:
        return "wubi";
    case DictionaryKind::QuickPhrase:
        return "quick";
    case DictionaryKind::English:
        return "english";
    }
    return "";
}

Db open_database(const std::string &path, int flags)
{
    sqlite3 *raw = nullptr;
    if (sqlite3_open_v2(path.c_str(), &raw, flags | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    {
        if (raw != nullptr)
            sqlite3_close(raw);
        return {};
    }
    sqlite3_busy_timeout(raw, 5000);
    return Db(raw);
}

Stmt prepare(sqlite3 *db, const std::string &sql)
{
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        return {};
    return Stmt(raw);
}

bool execute_sql(sqlite3 *db, const char *sql)
{
    return db != nullptr && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool bind_text(sqlite3_stmt *stmt, int index, const std::string &value)
{
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

Stmt prepare_upsert_journal(sqlite3 *db)
{
    return prepare(db, "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display)"
                       " VALUES(?1,?2,?3,'upsert',?4,?5)"
                       " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',weight=excluded.weight,"
                       " display=excluded.display,updated_at=unixepoch()");
}

bool write_upsert_journal(sqlite3_stmt *stmt, DictionaryKind kind, const std::string &key, const std::string &value,
                          std::int64_t weight, const std::string &display = {})
{
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return bind_text(stmt, 1, kind_name(kind)) && bind_text(stmt, 2, key) && bind_text(stmt, 3, value) &&
           sqlite3_bind_int64(stmt, 4, weight) == SQLITE_OK && bind_text(stmt, 5, display) &&
           sqlite3_step(stmt) == SQLITE_DONE;
}

bool ensure_schema(sqlite3 *db)
{
    constexpr const char *sql =
        "CREATE TABLE IF NOT EXISTS user_dictionary_operations("
        "dictionary TEXT NOT NULL,"
        "key TEXT NOT NULL,"
        "value TEXT NOT NULL,"
        "operation TEXT NOT NULL CHECK(operation IN ('upsert','delete')),"
        "weight INTEGER NOT NULL DEFAULT 0,"
        "display TEXT NOT NULL DEFAULT '',"
        "user_inserted INTEGER NOT NULL DEFAULT 0,"
        "updated_at INTEGER NOT NULL DEFAULT(unixepoch()),"
        "PRIMARY KEY(dictionary,key,value));"
        "CREATE TABLE IF NOT EXISTS personal_dictionary_receipts( request_id TEXT PRIMARY KEY, payload TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS candidate_selection_state("
        "context_key TEXT NOT NULL,entry_key TEXT NOT NULL,value TEXT NOT NULL,"
        "selection_count INTEGER NOT NULL DEFAULT 0,"
        "PRIMARY KEY(context_key,entry_key,value));"
        "CREATE TABLE IF NOT EXISTS fixed_candidate_positions("
        "context_key TEXT NOT NULL,entry_key TEXT NOT NULL,value TEXT NOT NULL,"
        "position INTEGER NOT NULL CHECK(position BETWEEN 1 AND 5),"
        "PRIMARY KEY(context_key,entry_key,value),UNIQUE(context_key,position));";
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;

    bool has_user_inserted = false;
    auto columns = prepare(db, "PRAGMA table_info(user_dictionary_operations)");
    if (!columns)
        return false;
    while (sqlite3_step(columns.get()) == SQLITE_ROW)
    {
        const unsigned char *name = sqlite3_column_text(columns.get(), 1);
        if (name != nullptr && std::string(reinterpret_cast<const char *>(name)) == "user_inserted")
        {
            has_user_inserted = true;
            break;
        }
    }
    columns.reset();
    if (!has_user_inserted && sqlite3_exec(db,
                                           "ALTER TABLE user_dictionary_operations "
                                           "ADD COLUMN user_inserted INTEGER NOT NULL DEFAULT 0",
                                           nullptr, nullptr, nullptr) != SQLITE_OK)
        return false;
    return sqlite3_exec(db, "PRAGMA user_version=3", nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::vector<std::string> pinyin_segments(const std::string &key)
{
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= key.size())
    {
        const size_t delimiter = key.find('\'', start);
        const std::string segment =
            key.substr(start, delimiter == std::string::npos ? std::string::npos : delimiter - start);
        if (segment.empty())
            return {};
        segments.push_back(segment);
        if (delimiter == std::string::npos)
            break;
        start = delimiter + 1;
    }
    return segments;
}

std::string pinyin_table(const std::string &key)
{
    const auto segments = pinyin_segments(key);
    if (segments.empty())
        return {};
    return metasequoia::dictionary_format::quanpin_table(segments.size(), segments.front().front());
}

constexpr std::int64_t kManagedWeightCeiling = 100000000LL;
constexpr std::int64_t kManagedWeightFloor = 1LL;
constexpr std::int64_t kRebalanceGap = 1000LL;
constexpr size_t kRebalanceCount = 16;

std::int64_t clamp_managed_weight(std::int64_t weight)
{
    if (weight < kManagedWeightFloor)
        return kManagedWeightFloor;
    if (weight > kManagedWeightCeiling)
        return kManagedWeightCeiling;
    return weight;
}

size_t utf8_char_count(const std::string &text)
{
    return static_cast<size_t>(
        std::count_if(text.begin(), text.end(), [](unsigned char ch) { return (ch & 0xC0) != 0x80; }));
}

std::string candidate_dictionary_key(const WordItem &item, const std::string &context_key)
{
    if (!item.canonical_pinyin.empty())
        return item.canonical_pinyin;
    std::string item_key = item.pinyin;
    const auto context_segments = pinyin_segments(context_key);
    if (context_segments.size() <= 1)
        return item_key;
    auto item_segments = context_segments;
    const size_t char_count = utf8_char_count(item.word);
    if (char_count > 0 && item_segments.size() > char_count)
        item_segments.resize(char_count);
    item_key.clear();
    for (size_t j = 0; j < item_segments.size(); ++j)
    {
        if (j)
            item_key += '\'';
        item_key += item_segments[j];
    }
    return item_key;
}

bool update_pinyin_weight(sqlite3 *main_db, sqlite3_stmt *journal_upsert, const std::string &key,
                          const std::string &value, std::int64_t weight)
{
    weight = clamp_managed_weight(weight);
    const std::string table = pinyin_table(key);
    if (table.empty() || main_db == nullptr || journal_upsert == nullptr)
        return false;
    auto stmt = prepare(main_db, "UPDATE \"" + table + "\" SET weight=?1 WHERE key=?2 AND value=?3");
    return stmt && sqlite3_bind_int64(stmt.get(), 1, weight) == SQLITE_OK && bind_text(stmt.get(), 2, key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_DONE &&
           write_upsert_journal(journal_upsert, DictionaryKind::Pinyin, key, value, weight);
}

bool update_wubi_weight(sqlite3 *main_db, sqlite3_stmt *journal_upsert, const std::string &key,
                        const std::string &value, std::int64_t weight)
{
    weight = clamp_managed_weight(weight);
    if (main_db == nullptr || journal_upsert == nullptr || key.empty() || value.empty())
        return false;
    auto stmt = prepare(main_db, "UPDATE wubi86 SET weight=?1 WHERE key=?2 AND value=?3");
    return stmt && sqlite3_bind_int64(stmt.get(), 1, weight) == SQLITE_OK && bind_text(stmt.get(), 2, key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_DONE && sqlite3_changes(main_db) > 0 &&
           write_upsert_journal(journal_upsert, DictionaryKind::Wubi, key, value, weight);
}

bool update_ranked_weight(sqlite3 *main_db, sqlite3_stmt *journal_upsert, DictionaryKind kind, const std::string &key,
                          const std::string &value, std::int64_t weight)
{
    if (kind == DictionaryKind::Wubi)
        return update_wubi_weight(main_db, journal_upsert, key, value, weight);
    return update_pinyin_weight(main_db, journal_upsert, key, value, weight);
}

size_t ranking_target(size_t rank, const std::string &mode, int linear_step, bool force_top)
{
    if (force_top)
        return 0;
    if (mode == "halve")
        return rank / 2;
    if (mode == "linear")
        return rank > static_cast<size_t>(linear_step) ? rank - static_cast<size_t>(linear_step) : 0;
    if (mode == "promote")
        return rank > 4 ? 4 : rank - 1;
    return 0;
}

std::string jianpin(const std::vector<std::string> &segments)
{
    std::string result;
    result.reserve(segments.size());
    for (const auto &segment : segments)
        result.push_back(segment.front());
    return result;
}

bool apply_pinyin(sqlite3 *db, const std::string &key, const std::string &value, const std::string &operation,
                  std::int64_t weight)
{
    const auto segments = pinyin_segments(key);
    if (segments.empty())
        return false;
    const std::string table = pinyin_table(key);
    if (operation == "delete")
    {
        auto stmt = prepare(db, "DELETE FROM \"" + table + "\" WHERE key=?1 AND value=?2");
        return stmt && bind_text(stmt.get(), 1, key) && bind_text(stmt.get(), 2, value) &&
               sqlite3_step(stmt.get()) == SQLITE_DONE;
    }

    auto update = prepare(db, "UPDATE \"" + table + "\" SET jp=?1,weight=?2 WHERE key=?3 AND value=?4");
    if (!update || !bind_text(update.get(), 1, jianpin(segments)) ||
        sqlite3_bind_int64(update.get(), 2, weight) != SQLITE_OK || !bind_text(update.get(), 3, key) ||
        !bind_text(update.get(), 4, value) || sqlite3_step(update.get()) != SQLITE_DONE)
        return false;
    if (sqlite3_changes(db) > 0)
        return true;

    auto insert = prepare(db, "INSERT INTO \"" + table + "\"(key,jp,value,weight) VALUES(?1,?2,?3,?4)");
    return insert && bind_text(insert.get(), 1, key) && bind_text(insert.get(), 2, jianpin(segments)) &&
           bind_text(insert.get(), 3, value) && sqlite3_bind_int64(insert.get(), 4, weight) == SQLITE_OK &&
           sqlite3_step(insert.get()) == SQLITE_DONE;
}

bool apply_simple(sqlite3 *db, const std::string &table, const std::string &key_column, const std::string &value_column,
                  const std::string &key, const std::string &value, const std::string &operation, std::int64_t weight)
{
    if (operation == "delete")
    {
        auto stmt = prepare(db, "DELETE FROM \"" + table + "\" WHERE \"" + key_column + "\"=?1 AND \"" + value_column +
                                    "\"=?2");
        return stmt && bind_text(stmt.get(), 1, key) && bind_text(stmt.get(), 2, value) &&
               sqlite3_step(stmt.get()) == SQLITE_DONE;
    }
    auto update = prepare(db, "UPDATE \"" + table + "\" SET weight=?1 WHERE \"" + key_column + "\"=?2 AND \"" +
                                  value_column + "\"=?3");
    if (!update || sqlite3_bind_int64(update.get(), 1, weight) != SQLITE_OK || !bind_text(update.get(), 2, key) ||
        !bind_text(update.get(), 3, value) || sqlite3_step(update.get()) != SQLITE_DONE)
        return false;
    if (sqlite3_changes(db) > 0)
        return true;
    auto insert = prepare(db, "INSERT INTO \"" + table + "\"(\"" + key_column + "\",\"" + value_column +
                                  "\",weight) VALUES(?1,?2,?3)");
    return insert && bind_text(insert.get(), 1, key) && bind_text(insert.get(), 2, value) &&
           sqlite3_bind_int64(insert.get(), 3, weight) == SQLITE_OK && sqlite3_step(insert.get()) == SQLITE_DONE;
}

bool apply_english(sqlite3 *db, const std::string &key, const std::string &value, const std::string &operation,
                   std::int64_t weight, const std::string &display)
{
    const std::string effective_display = display.empty() ? value : display;
    if (operation == "delete")
    {
        auto stmt = prepare(db, "DELETE FROM replay_english.english_words WHERE word=?1 AND display=?2");
        return stmt && bind_text(stmt.get(), 1, key) && bind_text(stmt.get(), 2, effective_display) &&
               sqlite3_step(stmt.get()) == SQLITE_DONE;
    }
    auto upsert = prepare(db, "INSERT INTO replay_english.english_words(word,display,weight) VALUES(?1,?2,?3) "
                              "ON CONFLICT(word,display) DO UPDATE SET weight=excluded.weight");
    return upsert && bind_text(upsert.get(), 1, key) && bind_text(upsert.get(), 2, effective_display) &&
           sqlite3_bind_int64(upsert.get(), 3, weight) == SQLITE_OK && sqlite3_step(upsert.get()) == SQLITE_DONE;
}
} // namespace

std::string default_user_db_path()
{
    return metasequoia::path_to_utf8(metasequoia::data_file_path(metasequoia::assets::user_journal));
}

namespace
{
// Opening the journal costs a file open plus a full ensure_schema() pass, and
// callers on the hot path pay it per keystroke: apply_fixed_positions runs while
// the candidate list for every input is being built. Measured on Windows with a
// busy disk, that open dominated the candidate build -- 90% of its time, with
// single calls reaching 737 ms -- which stalled the IME server's task thread long
// enough for the client's in-edit-session reply wait to time out and drop the
// commit key. Keep one connection per path alive instead; sqlite3 connections are
// opened with SQLITE_OPEN_FULLMUTEX, so sharing one across threads is safe.
struct CachedDatabase
{
    std::string path;
    std::shared_ptr<sqlite3> connection;
};

std::mutex &database_cache_mutex()
{
    static std::mutex mutex;
    return mutex;
}

CachedDatabase &default_database_cache()
{
    static CachedDatabase cache;
    return cache;
}

// The handle is shared rather than borrowed so that close_default_user_database()
// can drop the cache entry at any time without invalidating a connection an
// operation is still running on; the file is released once the last user is done.
std::shared_ptr<sqlite3> open_shared_database(const std::string &path)
{
    Db opened = open_database(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (!opened || !ensure_schema(opened.get()))
        return {};
    return std::shared_ptr<sqlite3>(opened.release(), [](sqlite3 *db) { sqlite3_close(db); });
}

std::shared_ptr<sqlite3> acquire_database(const std::string &path)
{
    // Only the default journal is worth caching, and only it is safe to cache:
    // every other path belongs to an import tool or a test that works on a
    // database and then deletes the directory holding it, which on Windows fails
    // while a connection is still open. Those keep the original one-per-call
    // connection, so nothing but the hot path changes behaviour.
    if (path != default_user_db_path())
        return open_shared_database(path);

    const std::lock_guard<std::mutex> guard(database_cache_mutex());
    auto &cache = default_database_cache();
    if (cache.connection && cache.path == path)
        return cache.connection;
    auto connection = open_shared_database(path);
    if (!connection)
        return {};
    // Assigning also releases a connection cached for an earlier data directory,
    // so at most one journal file is ever held open.
    cache.path = path;
    cache.connection = connection;
    return connection;
}

class UserDatabase
{
  public:
    explicit UserDatabase(const std::string &path) : db_(acquire_database(path))
    {
    }

    // A caller that returns between BEGIN IMMEDIATE and COMMIT used to be rescued
    // by its private connection closing, which rolls back implicitly. A cached
    // connection outlives the call, so an abandoned transaction would keep its
    // write lock and fail every later operation. End it here instead.
    ~UserDatabase()
    {
        if (db_ && sqlite3_get_autocommit(db_.get()) == 0)
            sqlite3_exec(db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
    }

    UserDatabase(const UserDatabase &) = delete;
    UserDatabase &operator=(const UserDatabase &) = delete;

    explicit operator bool() const
    {
        return db_ != nullptr;
    }
    sqlite3 *get() const
    {
        return db_.get();
    }

  private:
    std::shared_ptr<sqlite3> db_;
};
} // namespace

void close_default_user_database()
{
    // Callers use this to let go of the journal before deleting or replacing the
    // data directory; a connection left open would block that on Windows.
    const std::lock_guard<std::mutex> guard(database_cache_mutex());
    default_database_cache() = {};
}

bool ensure_user_database(const std::string &user_db_path)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    // Drop ranking rows left by the old rebalance staircase so installer replay
    // cannot bury shipped frequencies such as 先/xian under negative weights.
    sqlite3_exec(db.get(),
                 "DELETE FROM user_dictionary_operations "
                 "WHERE dictionary='pinyin' AND operation='upsert' AND weight < 1",
                 nullptr, nullptr, nullptr);
    return true;
}

bool record_upsert(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                   const std::string &value, std::int64_t weight, const std::string &display)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt = prepare_upsert_journal(db.get());
    return stmt && write_upsert_journal(stmt.get(), kind, key, value, weight, display);
}

bool record_user_insert(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                        const std::string &value, std::int64_t weight, const std::string &display)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt =
        prepare(db.get(), "INSERT INTO user_dictionary_operations("
                          "dictionary,key,value,operation,weight,display,user_inserted)"
                          " VALUES(?1,?2,?3,'upsert',?4,?5,1)"
                          " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='upsert',weight=excluded.weight,"
                          " display=excluded.display,user_inserted=1,updated_at=unixepoch()");
    return stmt && bind_text(stmt.get(), 1, kind_name(kind)) && bind_text(stmt.get(), 2, key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_bind_int64(stmt.get(), 4, weight) == SQLITE_OK &&
           bind_text(stmt.get(), 5, display) && sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool record_delete(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                   const std::string &value)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt =
        prepare(db.get(), "INSERT INTO user_dictionary_operations(dictionary,key,value,operation)"
                          " VALUES(?1,?2,?3,'delete')"
                          " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='delete',weight=0,display='',"
                          " updated_at=unixepoch()");
    return stmt && bind_text(stmt.get(), 1, kind_name(kind)) && bind_text(stmt.get(), 2, key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool is_user_inserted(const std::string &user_db_path, DictionaryKind kind, const std::string &key,
                      const std::string &value)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt = prepare(db.get(), "SELECT user_inserted FROM user_dictionary_operations "
                                  "WHERE dictionary=?1 AND key=?2 AND value=?3 LIMIT 1");
    return stmt && bind_text(stmt.get(), 1, kind_name(kind)) && bind_text(stmt.get(), 2, key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_ROW &&
           sqlite3_column_int(stmt.get(), 0) != 0;
}

bool record_pinyin_upsert_from_database(const std::string &main_db_path, const std::string &key,
                                        const std::string &value, const std::string &user_db_path)
{
    auto db = open_database(main_db_path, SQLITE_OPEN_READONLY);
    const std::string table = pinyin_table(key);
    if (!db || table.empty())
        return false;
    auto stmt = prepare(db.get(), "SELECT weight FROM \"" + table + "\" WHERE key=?1 AND value=?2 LIMIT 1");
    if (!stmt || !bind_text(stmt.get(), 1, key) || !bind_text(stmt.get(), 2, value) ||
        sqlite3_step(stmt.get()) != SQLITE_ROW)
        return false;
    return record_upsert(user_db_path, DictionaryKind::Pinyin, key, value, sqlite3_column_int64(stmt.get(), 0));
}

bool set_fixed_position(const std::string &user_db_path, const std::string &context_key, const std::string &entry_key,
                        const std::string &value, int position)
{
    if (position < 1 || position > 5 || context_key.empty() || entry_key.empty() || value.empty())
        return false;
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    sqlite3_exec(db.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    auto clear_slot = prepare(db.get(), "DELETE FROM fixed_candidate_positions WHERE context_key=?1 AND position=?2");
    auto upsert = prepare(
        db.get(), "INSERT INTO fixed_candidate_positions(context_key,entry_key,value,position) VALUES(?1,?2,?3,?4)"
                  " ON CONFLICT(context_key,entry_key,value) DO UPDATE SET position=excluded.position");
    const bool ok = clear_slot && upsert && bind_text(clear_slot.get(), 1, context_key) &&
                    sqlite3_bind_int(clear_slot.get(), 2, position) == SQLITE_OK &&
                    sqlite3_step(clear_slot.get()) == SQLITE_DONE && bind_text(upsert.get(), 1, context_key) &&
                    bind_text(upsert.get(), 2, entry_key) && bind_text(upsert.get(), 3, value) &&
                    sqlite3_bind_int(upsert.get(), 4, position) == SQLITE_OK &&
                    sqlite3_step(upsert.get()) == SQLITE_DONE;
    sqlite3_exec(db.get(), ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
    return ok;
}

bool adjust_english_candidate_ranking(const std::string &english_db_path, const std::string &user_db_path,
                                      const std::string &context_key, const std::vector<WordItem> &ordered_candidates,
                                      const std::string &entry_key, const std::string &value, const std::string &mode,
                                      int linear_step, int trigger_count, bool force_top, bool *ranking_changed)
{
    if (ranking_changed)
        *ranking_changed = false;
    if (entry_key.empty() || value.empty() || ordered_candidates.empty() ||
        is_fixed(user_db_path, context_key, entry_key, value))
        return false;
    if (!force_top && mode == "disabled")
        return true;

    UserDatabase user_db(user_db_path);
    if (!user_db)
        return false;
    auto journal = prepare_upsert_journal(user_db.get());
    if (!journal)
        return false;
    if (!execute_sql(user_db.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback_user = [&]() { (void)execute_sql(user_db.get(), "ROLLBACK"); };
    trigger_count = (std::max)(1, (std::min)(10, trigger_count));
    if (!force_top)
    {
        auto counter = prepare(
            user_db.get(),
            "INSERT INTO candidate_selection_state(context_key,entry_key,value,selection_count) VALUES(?1,?2,?3,1)"
            " ON CONFLICT(context_key,entry_key,value) DO UPDATE SET selection_count=selection_count+1"
            " RETURNING selection_count");
        if (!counter || !bind_text(counter.get(), 1, context_key) || !bind_text(counter.get(), 2, entry_key) ||
            !bind_text(counter.get(), 3, value) || sqlite3_step(counter.get()) != SQLITE_ROW)
        {
            rollback_user();
            return false;
        }
        if (sqlite3_column_int(counter.get(), 0) < trigger_count)
        {
            counter.reset();
            const bool committed = execute_sql(user_db.get(), "COMMIT");
            if (!committed)
                rollback_user();
            return committed;
        }
    }

    const auto selected = std::find_if(ordered_candidates.begin(), ordered_candidates.end(), [&](const WordItem &item) {
        return item.source == CandidateSource::EnglishDictionary && item.pinyin == entry_key && item.word == value;
    });
    if (selected == ordered_candidates.end())
    {
        rollback_user();
        return false;
    }
    const size_t rank = static_cast<size_t>(selected - ordered_candidates.begin());
    if (rank == 0)
    {
        const bool committed = execute_sql(user_db.get(), "COMMIT");
        if (!committed)
            rollback_user();
        return committed;
    }
    size_t target = 0;
    if (!force_top && mode == "halve")
        target = rank / 2;
    else if (!force_top && mode == "linear")
        target = rank > static_cast<size_t>((std::max)(1, linear_step))
                     ? rank - static_cast<size_t>((std::max)(1, linear_step))
                     : 0;
    else if (!force_top && mode == "promote")
        target = rank > 4 ? 4 : rank - 1;
    const std::int64_t maximum_weight =
        std::accumulate(ordered_candidates.begin(), ordered_candidates.end(), std::int64_t{0},
                        [](std::int64_t maximum, const WordItem &item) { return (std::max)(maximum, item.weight); });
    const std::int64_t new_weight =
        target == 0 ? (std::max)(maximum_weight, selected->weight) + 1000 : ordered_candidates[target - 1].weight + 1;

    auto english_db = open_database(english_db_path, SQLITE_OPEN_READWRITE);
    auto update = english_db
                      ? prepare(english_db.get(), "UPDATE english_words SET weight=?1 WHERE word=?2 AND display=?3")
                      : Stmt{};
    auto existing =
        english_db ? prepare(english_db.get(), "SELECT 1 FROM english_words WHERE word=?1 AND display=?2") : Stmt{};
    // Journal the new weight before english.db carries it. The journal upsert is idempotent and replay reapplies it, so
    // a journal row one step ahead of the dictionary is recoverable, while a boosted weight that never reached the
    // journal is silently reverted at the next dictionary upgrade. The row must already exist, otherwise replay would
    // insert a word the user never learned.
    if (!update || !existing || !bind_text(existing.get(), 1, entry_key) || !bind_text(existing.get(), 2, value) ||
        sqlite3_step(existing.get()) != SQLITE_ROW ||
        !write_upsert_journal(journal.get(), DictionaryKind::English, entry_key, value, new_weight, value))
    {
        rollback_user();
        return false;
    }
    existing.reset();
    auto reset = prepare(user_db.get(),
                         "DELETE FROM candidate_selection_state WHERE context_key=?1 AND entry_key=?2 AND value=?3");
    const bool reset_ok = reset && bind_text(reset.get(), 1, context_key) && bind_text(reset.get(), 2, entry_key) &&
                          bind_text(reset.get(), 3, value) && sqlite3_step(reset.get()) == SQLITE_DONE;
    reset.reset();
    if (!reset_ok || !execute_sql(user_db.get(), "COMMIT"))
    {
        rollback_user();
        return false;
    }
    const bool ok = sqlite3_bind_int64(update.get(), 1, new_weight) == SQLITE_OK &&
                    bind_text(update.get(), 2, entry_key) && bind_text(update.get(), 3, value) &&
                    sqlite3_step(update.get()) == SQLITE_DONE && sqlite3_changes(english_db.get()) > 0;
    if (ok && ranking_changed)
        *ranking_changed = true;
    return ok;
}

bool delete_dictionary_candidate(const std::string &dictionary_db_path, const std::string &user_db_path,
                                 DictionaryKind kind, const std::string &entry_key, const std::string &value)
{
    if (entry_key.empty() || value.empty())
        return false;
    std::string table;
    switch (kind)
    {
    case DictionaryKind::Pinyin:
        table = pinyin_table(entry_key);
        break;
    case DictionaryKind::Wubi:
        table = "wubi86";
        break;
    case DictionaryKind::English:
        table = "english_words";
        break;
    default:
        return false;
    }
    if (table.empty())
        return false;
    // Initialize the journal before attaching it; every operation owns its connections.
    if (!ensure_user_database(user_db_path))
        return false;
    auto database = open_database(dictionary_db_path, SQLITE_OPEN_READWRITE);
    auto attach = database ? prepare(database.get(), "ATTACH DATABASE ?1 AS candidate_journal") : Stmt{};
    if (!attach || !bind_text(attach.get(), 1, user_db_path) || sqlite3_step(attach.get()) != SQLITE_DONE)
        return false;
    attach.reset();
    if (!execute_sql(database.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback = [&]() {
        (void)execute_sql(database.get(), "ROLLBACK");
        return false;
    };
    const std::string columns = kind == DictionaryKind::English ? "word=?1 AND display=?2" : "key=?1 AND value=?2";
    auto remove = prepare(database.get(), "DELETE FROM main.\"" + table + "\" WHERE " + columns);
    if (!remove || !bind_text(remove.get(), 1, entry_key) || !bind_text(remove.get(), 2, value) ||
        sqlite3_step(remove.get()) != SQLITE_DONE || sqlite3_changes(database.get()) == 0)
        return rollback();
    remove.reset();
    auto journal = prepare(database.get(),
                           "INSERT INTO candidate_journal.user_dictionary_operations(dictionary,key,value,operation)"
                           " VALUES(?1,?2,?3,'delete')"
                           " ON CONFLICT(dictionary,key,value) DO UPDATE SET operation='delete',weight=0,display='',"
                           " updated_at=unixepoch()");
    if (!journal || !bind_text(journal.get(), 1, kind_name(kind)) || !bind_text(journal.get(), 2, entry_key) ||
        !bind_text(journal.get(), 3, value) || sqlite3_step(journal.get()) != SQLITE_DONE)
        return rollback();
    journal.reset();
    if (!execute_sql(database.get(), "COMMIT"))
        return rollback();
    return true;
}

bool delete_english_candidate(const std::string &english_db_path, const std::string &user_db_path,
                              const std::string &entry_key, const std::string &value)
{
    return delete_dictionary_candidate(english_db_path, user_db_path, DictionaryKind::English, entry_key, value);
}

bool learn_entered_english_word(const std::string &english_db_path, const std::string &user_db_path,
                                const std::string &display, std::int64_t weight)
{
    constexpr size_t kMaximumLearnedEnglishWordLength = 64;
    if (display.empty() || display.size() > kMaximumLearnedEnglishWordLength ||
        !std::all_of(display.begin(), display.end(),
                     [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }))
        return false;

    std::string word = display;
    std::transform(word.begin(), word.end(), word.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    weight = (std::max)(std::int64_t{0}, weight);
    if (!EnglishDictionary::ensure_schema(english_db_path))
        return false;

    auto database = open_database(english_db_path, SQLITE_OPEN_READWRITE);
    auto insert =
        database ? prepare(database.get(), "INSERT OR IGNORE INTO english_words(word,display,weight) VALUES(?1,?2,?3)")
                 : Stmt{};
    if (!insert || !bind_text(insert.get(), 1, word) || !bind_text(insert.get(), 2, display) ||
        sqlite3_bind_int64(insert.get(), 3, weight) != SQLITE_OK || sqlite3_step(insert.get()) != SQLITE_DONE)
        return false;
    if (sqlite3_changes(database.get()) == 0)
        return true;

    if (record_user_insert(user_db_path, DictionaryKind::English, word, display, weight, display))
        return true;

    // Do not leave an entry that cannot survive a dictionary upgrade.
    auto rollback_insert = prepare(database.get(), "DELETE FROM english_words WHERE word=?1 AND display=?2");
    if (rollback_insert && bind_text(rollback_insert.get(), 1, word) && bind_text(rollback_insert.get(), 2, display))
        (void)sqlite3_step(rollback_insert.get());
    return false;
}

bool clear_fixed_position(const std::string &user_db_path, const std::string &context_key, const std::string &entry_key,
                          const std::string &value)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt =
        prepare(db.get(), "DELETE FROM fixed_candidate_positions WHERE context_key=?1 AND entry_key=?2 AND value=?3");
    return stmt && bind_text(stmt.get(), 1, context_key) && bind_text(stmt.get(), 2, entry_key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool is_fixed(const std::string &user_db_path, const std::string &context_key, const std::string &entry_key,
              const std::string &value)
{
    UserDatabase db(user_db_path);
    if (!db)
        return false;
    auto stmt =
        prepare(db.get(), "SELECT 1 FROM fixed_candidate_positions WHERE context_key=?1 AND entry_key=?2 AND value=?3");
    return stmt && bind_text(stmt.get(), 1, context_key) && bind_text(stmt.get(), 2, entry_key) &&
           bind_text(stmt.get(), 3, value) && sqlite3_step(stmt.get()) == SQLITE_ROW;
}

void apply_fixed_positions(
    const std::string &user_db_path, const std::string &context_key, std::vector<WordItem> &candidates,
    bool include_missing,
    const std::function<std::optional<WordItem>(const std::string &, const std::string &)> &find_candidate,
    bool keep_dynamic_candidate_positions)
{
    if (context_key.empty() || candidates.empty())
        return;
    UserDatabase user_db(user_db_path);
    if (!user_db)
        return;
    auto fixed = prepare(
        user_db.get(),
        "SELECT entry_key,value,position FROM fixed_candidate_positions WHERE context_key=?1 ORDER BY position");
    if (!fixed || !bind_text(fixed.get(), 1, context_key))
        return;
    std::vector<WordItem> dynamic_candidates;
    if (!keep_dynamic_candidate_positions)
    {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const WordItem &item) {
                                            if (item.source != CandidateSource::CloudSuggestion &&
                                                item.source != CandidateSource::AiSuggestion)
                                                return false;
                                            dynamic_candidates.push_back(item);
                                            return true;
                                        }),
                         candidates.end());
    }

    struct Fixed
    {
        WordItem item;
        int position;
    };
    std::vector<Fixed> rows;
    while (sqlite3_step(fixed.get()) == SQLITE_ROW)
    {
        const std::string key = reinterpret_cast<const char *>(sqlite3_column_text(fixed.get(), 0));
        const std::string value = reinterpret_cast<const char *>(sqlite3_column_text(fixed.get(), 1));
        const int position = sqlite3_column_int(fixed.get(), 2);
        const auto existing = std::find_if(candidates.begin(), candidates.end(),
                                           [&](const WordItem &item) { return item.word == value; });
        if (existing != candidates.end())
        {
            WordItem item = *existing;
            item.fixed_position = position;
            rows.push_back({std::move(item), position});
        }
        else if (include_missing && find_candidate)
        {
            if (auto found = find_candidate(key, value))
            {
                WordItem item = std::move(*found);
                item.fixed_position = position;
                rows.push_back({std::move(item), position});
            }
        }
    }
    for (const auto &row : rows)
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                        [&](const WordItem &item) { return item.word == row.item.word; }),
                         candidates.end());
    for (const auto &row : rows)
    {
        const size_t index = (std::min)(static_cast<size_t>(row.position - 1), candidates.size());
        candidates.insert(candidates.begin() + index, row.item);
    }
    for (const auto &item : dynamic_candidates)
    {
        const size_t preferred_index = item.source == CandidateSource::CloudSuggestion ? 1 : 2;
        const size_t index = (std::min)(preferred_index, candidates.size());
        candidates.insert(candidates.begin() + index, item);
    }
}

bool adjust_candidate_ranking(const std::string &main_db_path, const std::string &user_db_path,
                              const std::string &context_key, const std::vector<WordItem> &ordered_candidates,
                              const std::string &entry_key, const std::string &value, const std::string &mode,
                              int linear_step, int trigger_count, bool force_top, bool *ranking_changed,
                              DictionaryKind kind)
{
    if (ranking_changed)
        *ranking_changed = false;
    if (entry_key.empty() || value.empty() || ordered_candidates.empty() ||
        is_fixed(user_db_path, context_key, entry_key, value))
        return false;
    if (!force_top && mode == "disabled")
        return true;
    UserDatabase user_db(user_db_path);
    if (!user_db)
        return false;
    auto journal_upsert = prepare_upsert_journal(user_db.get());
    if (!journal_upsert)
        return false;
    if (!execute_sql(user_db.get(), "BEGIN IMMEDIATE"))
        return false;
    const auto rollback_user = [&]() { (void)execute_sql(user_db.get(), "ROLLBACK"); };
    trigger_count = (std::max)(1, (std::min)(10, trigger_count));
    if (!force_top)
    {
        auto counter = prepare(
            user_db.get(),
            "INSERT INTO candidate_selection_state(context_key,entry_key,value,selection_count) VALUES(?1,?2,?3,1)"
            " ON CONFLICT(context_key,entry_key,value) DO UPDATE SET selection_count=selection_count+1"
            " RETURNING selection_count");
        if (!counter || !bind_text(counter.get(), 1, context_key) || !bind_text(counter.get(), 2, entry_key) ||
            !bind_text(counter.get(), 3, value) || sqlite3_step(counter.get()) != SQLITE_ROW)
        {
            rollback_user();
            return false;
        }
        if (sqlite3_column_int(counter.get(), 0) < trigger_count)
        {
            counter.reset();
            const bool committed = execute_sql(user_db.get(), "COMMIT");
            if (!committed)
                rollback_user();
            return committed;
        }
    }

    // One list mixes several entry keys: single-letter and jianpin contexts show
    // 一/yi next to 有/you, nine-key digits mix whole single-char tables, and a
    // re-segmentation shows 吉安 (ji'an) inside the jian list. All of them have to
    // share one scale, because the user's 调频 is "move this above what I can see".
    //
    // The midpoint math below assumes this vector is sorted by weight, descending:
    // it brackets the target position between its neighbours' weights and splits
    // the gap. The displayed order does NOT satisfy that -- an alternative
    // segmentation takes a protected slot near the top and pinned candidates are
    // hoisted to fixed positions, both regardless of weight. Ranking on display
    // order is what wrote 写 as 506 (= 西鄂's weight 6 + 500) in #400. Sorting by
    // weight fixes that at its source, so no weight basis can ever come from a row
    // sitting above its own weight.
    //
    // #400 instead restricted the set to keys with the same syllable count. That
    // also kept 西鄂 out, but it walled every candidate into its own group: 吉安
    // (ji'an, w=1) could only ever be compared against 积案/几案/急案/即按, so its
    // learnable weight was capped around 10k and it could never pass 见 (jian,
    // w=3460998) no matter how many times the user picked it. Weight order removes
    // the wall without bringing the bug back. Writes still go to entry_key rows
    // only, so a rebalance cannot land on another key's row the way it did in #36.
    std::vector<WordItem> database_candidates;
    for (const auto &item : ordered_candidates)
    {
        if (item.source != CandidateSource::Database && item.source != CandidateSource::UserDatabase)
            continue;
        database_candidates.push_back(item);
    }
    std::stable_sort(database_candidates.begin(), database_candidates.end(),
                     [](const WordItem &lhs, const WordItem &rhs) { return lhs.weight > rhs.weight; });
    std::vector<bool> owns_entry_key;
    owns_entry_key.reserve(database_candidates.size());
    for (const auto &item : database_candidates)
    {
        const std::string item_key =
            kind == DictionaryKind::Wubi ? item.pinyin : candidate_dictionary_key(item, context_key);
        owns_entry_key.push_back(item_key == entry_key);
    }
    const auto selected = std::find_if(database_candidates.begin(), database_candidates.end(),
                                       [&](const WordItem &item) { return item.word == value; });
    if (selected == database_candidates.end())
    {
        rollback_user();
        return false;
    }
    const size_t rank = static_cast<size_t>(selected - database_candidates.begin());
    if (rank == 0)
    {
        auto reset = prepare(
            user_db.get(), "DELETE FROM candidate_selection_state WHERE context_key=?1 AND entry_key=?2 AND value=?3");
        const bool reset_ok = reset && bind_text(reset.get(), 1, context_key) && bind_text(reset.get(), 2, entry_key) &&
                              bind_text(reset.get(), 3, value) && sqlite3_step(reset.get()) == SQLITE_DONE;
        reset.reset();
        const bool committed = reset_ok && execute_sql(user_db.get(), "COMMIT");
        if (!committed)
            rollback_user();
        return committed;
    }
    const size_t target = ranking_target(rank, mode, linear_step, force_top);
    auto main_db = open_database(main_db_path, SQLITE_OPEN_READWRITE);
    if (!main_db || !execute_sql(main_db.get(), "BEGIN IMMEDIATE"))
    {
        rollback_user();
        return false;
    }
    const auto rollback_main = [&]() { (void)execute_sql(main_db.get(), "ROLLBACK"); };
    // Keep learned weights in the same general range as the shipped dictionary
    // (currently below 77 million). Large spacing is unnecessary and previously
    // caused local rebalances to create values around 10^12. A later bug used a
    // tiny `upper` (weight=1 rare words) as the staircase base and wrote
    // negative weights onto neighboring keys from query_series().
    std::int64_t new_weight = 0;
    const bool top_near_limit =
        target == 0 && database_candidates[0].weight > (std::numeric_limits<std::int64_t>::max)() - 1000;
    const std::int64_t upper = target == 0 && !top_near_limit ? database_candidates[0].weight + 1000
                                                              : (target == 0 ? database_candidates[0].weight
                                                                             : database_candidates[target - 1].weight);
    const std::int64_t lower = database_candidates[target].weight;
    const bool oversized = upper > kManagedWeightCeiling || lower > kManagedWeightCeiling;
    bool need_rebalance =
        top_near_limit || lower == (std::numeric_limits<std::int64_t>::max)() || upper <= lower || oversized;
    if (!need_rebalance)
    {
        if (upper == lower + 1)
        {
            // There is no integer midpoint. Move one point above the upper
            // candidate and skip a short run of occupied weights. This may
            // advance one extra position, but keeps ordering deterministic
            // without paying for a rebalance in the common case.
            new_weight = upper + 1;
            size_t conflicts = 0;
            for (size_t i = target; i-- > 0;)
            {
                const std::int64_t occupied = database_candidates[i].weight;
                if (occupied < new_weight)
                    continue;
                if (occupied > new_weight)
                    break;
                if (++conflicts >= 16 || new_weight >= kManagedWeightCeiling)
                {
                    need_rebalance = true;
                    break;
                }
                ++new_weight;
            }
        }
        else
        {
            new_weight = lower + (upper - lower) / 2;
        }
    }
    if (need_rebalance && !oversized && !top_near_limit && target != 0)
    {
        const size_t rebalance_end = (std::min)(database_candidates.size(), target + kRebalanceCount);
        const std::int64_t last_index = static_cast<std::int64_t>(rebalance_end - 1);
        const std::int64_t last_weight =
            upper - kRebalanceGap - (last_index - static_cast<std::int64_t>(target)) * kRebalanceGap;
        if (last_weight < kManagedWeightFloor)
        {
            // Equal/low weights have no room for a 16-slot descending staircase.
            // Promote only the selected row instead of writing negatives onto
            // neighbors (and onto shorter-syllable singles mixed into the UI list).
            const std::int64_t cluster = (std::max)(upper, lower);
            if (cluster > (std::numeric_limits<std::int64_t>::max)() - kRebalanceGap)
            {
                // Fall through to the ceiling compact path below.
            }
            else
            {
                new_weight = clamp_managed_weight(cluster + kRebalanceGap);
                need_rebalance = false;
            }
        }
    }
    if (need_rebalance)
    {
        const size_t rebalance_begin = target;
        const size_t rebalance_end = (std::min)(database_candidates.size(), rebalance_begin + kRebalanceCount);
        std::int64_t base = target == 0 ? kManagedWeightCeiling - kRebalanceGap : kManagedWeightCeiling;
        if (target != 0 && !top_near_limit && !oversized)
        {
            const std::int64_t requested_base =
                upper + static_cast<std::int64_t>(target) * kRebalanceGap - kRebalanceGap;
            base = (std::min)(requested_base, kManagedWeightCeiling);
        }
        bool rebalance_ok = true;
        for (size_t i = rebalance_begin; i < rebalance_end && rebalance_ok; ++i)
        {
            if (!owns_entry_key[i])
                continue;
            const std::int64_t weight = clamp_managed_weight(base - static_cast<std::int64_t>(i) * kRebalanceGap);
            rebalance_ok = update_ranked_weight(main_db.get(), journal_upsert.get(), kind, entry_key,
                                                database_candidates[i].word, weight);
        }
        if (!rebalance_ok)
        {
            rollback_main();
            rollback_user();
            return false;
        }
        new_weight = target == 0 ? base + kRebalanceGap
                                 : base - static_cast<std::int64_t>(target) * kRebalanceGap + kRebalanceGap / 2;
    }
    new_weight = clamp_managed_weight(new_weight);
    const bool ok = update_ranked_weight(main_db.get(), journal_upsert.get(), kind, entry_key, value, new_weight);
    if (!ok)
    {
        rollback_main();
        rollback_user();
        return false;
    }
    auto reset = prepare(user_db.get(),
                         "DELETE FROM candidate_selection_state WHERE context_key=?1 AND entry_key=?2 AND value=?3");
    const bool reset_ok = reset && bind_text(reset.get(), 1, context_key) && bind_text(reset.get(), 2, entry_key) &&
                          bind_text(reset.get(), 3, value) && sqlite3_step(reset.get()) == SQLITE_DONE;
    reset.reset();
    if (!reset_ok || !execute_sql(user_db.get(), "COMMIT"))
    {
        rollback_main();
        rollback_user();
        return false;
    }
    if (!execute_sql(main_db.get(), "COMMIT"))
    {
        rollback_main();
        return false;
    }
    if (ranking_changed)
        *ranking_changed = true;
    return true;
}

ReplayResult replay(const std::string &user_db_path, const std::string &main_db_path,
                    const std::string &english_db_path)
{
    ReplayResult result;
    if (!std::filesystem::exists(metasequoia::path_from_utf8(user_db_path.c_str())))
        return result;
    auto journal = open_database(user_db_path, SQLITE_OPEN_READONLY);
    if (!journal)
    {
        result.error = "cannot open user dictionary database";
        return result;
    }
    if (!EnglishDictionary::ensure_schema(english_db_path))
    {
        result.error = "cannot migrate English dictionary database";
        return result;
    }
    auto main_db = open_database(main_db_path, SQLITE_OPEN_READWRITE);
    if (!main_db)
    {
        result.error = "cannot open target dictionary database";
        return result;
    }
    auto attach_english = prepare(main_db.get(), "ATTACH DATABASE ?1 AS replay_english");
    if (!attach_english || !bind_text(attach_english.get(), 1, english_db_path) ||
        sqlite3_step(attach_english.get()) != SQLITE_DONE)
    {
        result.error = "cannot attach English dictionary database";
        return result;
    }
    attach_english.reset();
    auto rows = prepare(journal.get(), "SELECT dictionary,key,value,operation,weight,display"
                                       " FROM user_dictionary_operations ORDER BY updated_at,rowid");
    if (!rows)
    {
        result.error = "invalid user dictionary database";
        return result;
    }

    if (!execute_sql(main_db.get(), "BEGIN IMMEDIATE"))
    {
        result.error = "cannot start replay transaction";
        return result;
    }
    int row_result = SQLITE_ROW;
    while ((row_result = sqlite3_step(rows.get())) == SQLITE_ROW)
    {
        const std::string kind = reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 0));
        const std::string key = reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 1));
        const std::string value = reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 2));
        const std::string operation = reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 3));
        const std::int64_t weight = sqlite3_column_int64(rows.get(), 4);
        const std::string display = reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 5));
        if (kind == "pinyin" && operation == "upsert" && weight < 1)
        {
            ++result.skipped;
            continue;
        }
        bool ok = false;
        if (kind == "pinyin")
            ok = apply_pinyin(main_db.get(), key, value, operation, weight);
        else if (kind == "wubi")
            ok = apply_simple(main_db.get(), "wubi86", "key", "value", key, value, operation, weight);
        else if (kind == "quick")
            ok = apply_simple(main_db.get(), "quick_parases", "key", "value", key, value, operation, weight);
        else if (kind == "english")
            ok = apply_english(main_db.get(), key, value, operation, weight, display);
        ok ? ++result.applied : ++result.failed;
    }
    rows.reset();
    if (row_result != SQLITE_DONE)
    {
        result.error = execute_sql(main_db.get(), "ROLLBACK")
                           ? "cannot read the complete user dictionary journal; changes were rolled back"
                           : "cannot read the complete user dictionary journal and rollback also failed";
        return result;
    }
    if (result.failed == 0)
    {
        if (!execute_sql(main_db.get(), "COMMIT"))
        {
            result.error = execute_sql(main_db.get(), "ROLLBACK")
                               ? "cannot commit replay transaction; changes were rolled back"
                               : "cannot commit or roll back replay transaction";
        }
    }
    else
    {
        result.error = execute_sql(main_db.get(), "ROLLBACK")
                           ? "one or more operations failed; changes were rolled back"
                           : "one or more operations failed and rollback also failed";
    }
    return result;
}
} // namespace user_dictionary

namespace metasequoia
{
namespace
{
user_dictionary::DictionaryKind journal_kind(PersonalDictionaryKind kind)
{
    switch (kind)
    {
    case PersonalDictionaryKind::Pinyin:
        return user_dictionary::DictionaryKind::Pinyin;
    case PersonalDictionaryKind::Wubi:
        return user_dictionary::DictionaryKind::Wubi;
    case PersonalDictionaryKind::QuickPhrase:
        return user_dictionary::DictionaryKind::QuickPhrase;
    case PersonalDictionaryKind::English:
        return user_dictionary::DictionaryKind::English;
    }
    return user_dictionary::DictionaryKind::Pinyin;
}
bool apply_personal_edit(sqlite3 *db, const PersonalDictionaryEntry &entry, bool remove)
{
    using namespace user_dictionary;
    const std::string operation = remove ? "delete" : "upsert";
    bool applied = false;
    switch (entry.kind)
    {
    case PersonalDictionaryKind::Pinyin:
        applied = apply_pinyin(db, entry.key, entry.value, operation, entry.weight);
        break;
    case PersonalDictionaryKind::Wubi:
        applied = apply_simple(db, "wubi86", "key", "value", entry.key, entry.value, operation, entry.weight);
        break;
    case PersonalDictionaryKind::QuickPhrase:
        applied = apply_simple(db, "quick_parases", "key", "value", entry.key, entry.value, operation, entry.weight);
        break;
    case PersonalDictionaryKind::English:
        applied = apply_english(db, entry.key, entry.value, operation, entry.weight, entry.value);
        break;
    }
    if (!applied)
        return false;
    auto journal = prepare(
        db, "INSERT INTO "
            "personal_journal.user_dictionary_operations(dictionary,key,value,operation,weight,display,user_inserted)"
            " VALUES(?1,?2,?3,?4,?5,?6,1) ON CONFLICT(dictionary,key,value) DO UPDATE SET operation=excluded.operation,"
            " weight=excluded.weight,display=excluded.display,user_inserted=1,updated_at=unixepoch()");
    return journal && bind_text(journal.get(), 1, kind_name(journal_kind(entry.kind))) &&
           bind_text(journal.get(), 2, entry.key) && bind_text(journal.get(), 3, entry.value) &&
           bind_text(journal.get(), 4, operation) &&
           sqlite3_bind_int64(journal.get(), 5, remove ? 0 : entry.weight) == SQLITE_OK &&
           bind_text(journal.get(), 6, entry.kind == PersonalDictionaryKind::English ? entry.value : "") &&
           sqlite3_step(journal.get()) == SQLITE_DONE;
}
} // namespace
PersonalDictionaryEditResult edit_personal_dictionary(const RuntimePaths &paths,
                                                      const std::optional<PersonalDictionaryEntry> &previous,
                                                      const std::optional<PersonalDictionaryEntry> &replacement,
                                                      const std::string &request_id)
{
    using namespace user_dictionary;
    if (request_id.size() > 128 || !std::all_of(request_id.begin(), request_id.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                   ch == '_';
        }))
        return {false, "Invalid personal dictionary request ID"};
    if (!previous && !replacement)
        return {false, "An entry is required"};
    std::optional<PersonalDictionaryEntry> old_entry, new_entry;
    if (previous)
    {
        auto checked = validate_personal_dictionary_entry(*previous);
        if (!checked.entry)
            return {false, checked.error};
        old_entry = std::move(checked.entry);
    }
    if (replacement)
    {
        auto checked = validate_personal_dictionary_entry(*replacement);
        if (!checked.entry)
            return {false, checked.error};
        new_entry = std::move(checked.entry);
    }
    try
    {
        paths.validate();
        const auto journal_path = path_to_utf8(paths.user(assets::user_journal));
        const auto english_path = path_to_utf8(paths.dictionary(assets::english_dictionary));
        if (!ensure_user_database(journal_path) || !EnglishDictionary::ensure_schema(english_path))
            return {false, "Cannot prepare personal dictionary storage"};
        auto db = open_database(path_to_utf8(paths.dictionary(assets::main_dictionary)), SQLITE_OPEN_READWRITE);
        if (!db)
            return {false, "Cannot open dictionary"};
        for (const auto &attachment :
             {std::make_pair("personal_journal", journal_path), std::make_pair("replay_english", english_path)})
        {
            auto attach = prepare(db.get(), std::string("ATTACH DATABASE ?1 AS ") + attachment.first);
            if (!attach || !bind_text(attach.get(), 1, attachment.second) || sqlite3_step(attach.get()) != SQLITE_DONE)
                return {false, "Cannot attach personal dictionary storage"};
        }
        if (!execute_sql(db.get(), "BEGIN IMMEDIATE"))
            return {false, "Cannot begin personal dictionary edit"};
        const auto fail = [&](const char *message) {
            execute_sql(db.get(), "ROLLBACK");
            return PersonalDictionaryEditResult{false, message};
        };
        std::string payload;
        const auto append_payload = [&](const std::optional<PersonalDictionaryEntry> &entry) {
            if (!entry)
            {
                payload += "none;";
                return;
            }
            payload += std::to_string(static_cast<int>(entry->kind)) + ";" + std::to_string(entry->weight) + ";";
            for (const auto &value : {entry->key, entry->value})
                payload += std::to_string(value.size()) + ":" + value;
        };
        if (!request_id.empty())
        {
            append_payload(old_entry);
            append_payload(new_entry);
            auto receipt = prepare(
                db.get(), "SELECT payload FROM personal_journal.personal_dictionary_receipts WHERE request_id=?1");
            if (!receipt || !bind_text(receipt.get(), 1, request_id))
                return fail("Cannot read edit receipt");
            const int step = sqlite3_step(receipt.get());
            if (step == SQLITE_ROW)
            {
                const auto *stored = sqlite3_column_text(receipt.get(), 0);
                const bool same = stored && payload == reinterpret_cast<const char *>(stored);
                receipt.reset();
                if (!same)
                    return fail("Request ID was already used for another edit");
                if (!execute_sql(db.get(), "COMMIT"))
                    return fail("Cannot finish edit retry");
                return {true, {}};
            }
            if (step != SQLITE_DONE)
                return fail("Cannot finish reading edit receipt");
        }
        if (old_entry)
        {
            auto match = prepare(db.get(), "SELECT 1 FROM personal_journal.user_dictionary_operations"
                                           " WHERE dictionary=?1 AND key=?2 AND value=?3 AND weight=?4 AND "
                                           "operation='upsert' AND user_inserted=1");
            if (!match || !bind_text(match.get(), 1, kind_name(journal_kind(old_entry->kind))) ||
                !bind_text(match.get(), 2, old_entry->key) || !bind_text(match.get(), 3, old_entry->value) ||
                sqlite3_bind_int64(match.get(), 4, old_entry->weight) != SQLITE_OK ||
                sqlite3_step(match.get()) != SQLITE_ROW)
                return fail("The entry changed; reload it before editing");
            match.reset();
            if (!apply_personal_edit(db.get(), *old_entry, true))
                return fail("Cannot remove previous personal entry");
        }
        if (new_entry && !apply_personal_edit(db.get(), *new_entry, false))
            return fail("Cannot save personal entry");
        if (!request_id.empty())
        {
            auto receipt =
                prepare(db.get(),
                        "INSERT INTO personal_journal.personal_dictionary_receipts(request_id,payload) VALUES(?1,?2)");
            if (!receipt || !bind_text(receipt.get(), 1, request_id) || !bind_text(receipt.get(), 2, payload) ||
                sqlite3_step(receipt.get()) != SQLITE_DONE)
                return fail("Cannot save edit receipt");
        }
        if (!execute_sql(db.get(), "COMMIT"))
            return fail("Cannot commit personal dictionary edit");
        return {true, {}};
    }
    catch (const std::exception &)
    {
        return {false, "Cannot access personal dictionary storage"};
    }
}
PersonalDictionaryPage personal_dictionary_entries(const RuntimePaths &paths, std::size_t offset, std::size_t limit)
{
    using namespace user_dictionary;
    PersonalDictionaryPage result;
    if (limit == 0 || limit > 1000 || offset > 1000000)
    {
        result.error = "Invalid personal dictionary page";
        return result;
    }
    try
    {
        paths.validate();
        const auto journal_path = paths.user(assets::user_journal);
        if (!std::filesystem::exists(journal_path))
            return result;
        auto db = open_database(path_to_utf8(journal_path), SQLITE_OPEN_READONLY);
        auto rows =
            db ? prepare(
                     db.get(),
                     "SELECT dictionary,key,value,weight FROM user_dictionary_operations"
                     " WHERE user_inserted=1 AND operation='upsert' ORDER BY dictionary,key,value LIMIT ?1 OFFSET ?2")
               : Stmt{};
        if (!rows || sqlite3_bind_int64(rows.get(), 1, static_cast<sqlite3_int64>(limit + 1)) != SQLITE_OK ||
            sqlite3_bind_int64(rows.get(), 2, static_cast<sqlite3_int64>(offset)) != SQLITE_OK)
        {
            result.error = "Cannot read personal dictionary";
            return result;
        }
        int step;
        while ((step = sqlite3_step(rows.get())) == SQLITE_ROW)
        {
            if (result.entries.size() == limit)
            {
                result.has_more = true;
                break;
            }
            auto text = [&](int index) {
                const auto *value = sqlite3_column_text(rows.get(), index);
                return value
                           ? std::string(reinterpret_cast<const char *>(value), sqlite3_column_bytes(rows.get(), index))
                           : std::string{};
            };
            const auto kind = text(0);
            PersonalDictionaryKind entry_kind;
            if (kind == "pinyin")
                entry_kind = PersonalDictionaryKind::Pinyin;
            else if (kind == "wubi")
                entry_kind = PersonalDictionaryKind::Wubi;
            else if (kind == "quick")
                entry_kind = PersonalDictionaryKind::QuickPhrase;
            else if (kind == "english")
                entry_kind = PersonalDictionaryKind::English;
            else
                continue;
            result.entries.push_back({entry_kind, text(1), text(2), sqlite3_column_int64(rows.get(), 3)});
        }
        if (step != SQLITE_DONE && !result.has_more)
        {
            result.entries.clear();
            result.error = "Cannot finish reading personal dictionary";
        }
    }
    catch (const std::exception &)
    {
        result.entries.clear();
        result.error = "Cannot access personal dictionary storage";
    }
    return result;
}
} // namespace metasequoia
