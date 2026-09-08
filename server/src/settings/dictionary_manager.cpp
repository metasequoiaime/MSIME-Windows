#include "settings/dictionary_manager.h"
#include "settings/dictionary_page.h"
#include <set>

#include "config/ime_config.h"
#include "defines/defines.h"
#include "settings/dictionary_validation.h"
#include "utils/common_utils.h"
#include "MetasequoiaImeEngine/common/helpcode_utils.h"
#include "MetasequoiaImeEngine/quanpin/quanpin_query.h"
#include "MetasequoiaImeEngine/quanpin/quanpin_utils.h"
#include "MetasequoiaImeEngine/user_dictionary/user_dictionary_journal.h"
#include "MetasequoiaImeEngine/english/english_dictionary.h"

#include <cpp-pinyin/G2pglobal.h>
#include <cpp-pinyin/Pinyin.h>

#include <windows.h>
#include <sqlite3.h>
#include <utf8.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace SettingsDictionary
{
namespace json = boost::json;
namespace
{
struct DbCloser
{
    void operator()(sqlite3 *db) const
    {
        if (db)
            sqlite3_close(db);
    }
};
using Db = std::unique_ptr<sqlite3, DbCloser>;
struct StmtCloser
{
    void operator()(sqlite3_stmt *stmt) const
    {
        if (stmt)
            sqlite3_finalize(stmt);
    }
};
using Stmt = std::unique_ptr<sqlite3_stmt, StmtCloser>;

json::object Result(bool ok, std::string message)
{
    return {{"ok", ok}, {"message", std::move(message)}, {"rows", json::array{}}};
}

void NotifyImeServerClearDictCache()
{
    if (const HWND hwnd = FindWindowW(L"metasequoiaime_windows", L"metaseuqoiaimecandwnd"))
        PostMessageW(hwnd, WM_CLS_DICT_CACHE, 0, 0);
    else if (const HWND hwnd = FindWindowW(L"metasequoiaime_windows", nullptr))
        PostMessageW(hwnd, WM_CLS_DICT_CACHE, 0, 0);
}

std::filesystem::path SettingsExecutableDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return std::filesystem::path(path).parent_path();
}

Pinyin::Pinyin &PinyinAnnotator()
{
    static std::once_flag once;
    static std::unique_ptr<Pinyin::Pinyin> engine;
    std::call_once(once, [] {
        const auto dict_path = SettingsExecutableDirectory() / "dict";
        Pinyin::setDictionaryPath(dict_path);
        engine = std::make_unique<Pinyin::Pinyin>();
    });
    return *engine;
}

std::string NormalizeAnnotatedSyllable(std::string syllable)
{
    // cpp-pinyin may emit ü; IME quanpin keys use v.
    const std::string ue = "\xC3\xBC"; // ü
    const std::string UE = "\xC3\x9C"; // Ü
    for (std::string::size_type pos = 0; (pos = syllable.find(ue, pos)) != std::string::npos;)
        syllable.replace(pos, ue.size(), "v");
    for (std::string::size_type pos = 0; (pos = syllable.find(UE, pos)) != std::string::npos;)
        syllable.replace(pos, UE.size(), "v");
    std::transform(syllable.begin(), syllable.end(), syllable.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    syllable.erase(
        std::remove_if(syllable.begin(), syllable.end(), [](unsigned char ch) { return ch < 'a' || ch > 'z'; }),
        syllable.end());
    return syllable;
}

bool IsPureHanPhrase(const std::string &word)
{
    if (word.empty())
        return false;
    try
    {
        auto it = word.begin();
        size_t count = 0;
        while (it != word.end())
        {
            const uint32_t codepoint = utf8::next(it, word.end());
            // cpp-pinyin only supports CJK Unified Ideographs in [0x4E00, 0x9FFF].
            if (codepoint < 0x4E00 || codepoint > 0x9FFF)
                return false;
            ++count;
        }
        return count > 0;
    }
    catch (...)
    {
        return false;
    }
}

bool AnnotateHansToPinyin(const std::string &word, std::string &code, std::string &message)
{
    if (word.empty())
    {
        message = "词条不能为空";
        return false;
    }
    if (!IsPureHanPhrase(word))
    {
        message = "仅支持纯汉字词组";
        return false;
    }

    const auto results = PinyinAnnotator().hanziToPinyin(word, Pinyin::ManTone::Style::NORMAL, Pinyin::Error::Default,
                                                         false, false, false);
    if (results.empty())
    {
        message = "注音失败";
        return false;
    }

    std::string joined;
    for (const auto &item : results)
    {
        if (item.error)
        {
            message = "注音失败：无法识别的汉字";
            return false;
        }
        const std::string syllable = NormalizeAnnotatedSyllable(item.pinyin);
        if (syllable.empty())
        {
            message = "注音失败：拼音为空";
            return false;
        }
        if (!joined.empty())
            joined.push_back('\'');
        joined += syllable;
    }
    code = std::move(joined);
    return true;
}

std::string StringValue(const json::object &obj, const char *key)
{
    const auto *value = obj.if_contains(key);
    return value && value->is_string() ? std::string(value->as_string()) : std::string{};
}

int IntValue(const json::object &obj, const char *key, int fallback)
{
    const auto *value = obj.if_contains(key);
    return value && value->is_int64() ? static_cast<int>(value->as_int64()) : fallback;
}

bool IsAsciiWord(const std::string &value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '\'';
    });
}

Db OpenDatabase(const std::string &name, std::string &error)
{
    sqlite3 *raw = nullptr;
    const std::string path = CommonUtils::get_ime_data_path() + "\\" + name;
    if (sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    {
        error = raw ? sqlite3_errmsg(raw) : "无法打开数据库";
        if (raw)
            sqlite3_close(raw);
        return {};
    }
    sqlite3_busy_timeout(raw, 3000);
    return Db(raw);
}

Stmt Prepare(sqlite3 *db, const std::string &sql, std::string &error)
{
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    {
        error = sqlite3_errmsg(db);
        return {};
    }
    return Stmt(raw);
}

bool BindText(sqlite3_stmt *stmt, int index, const std::string &value)
{
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string SanitizeExportField(std::string value)
{
    std::replace(value.begin(), value.end(), '\t', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}

json::object ExportUserDictionary(const std::string &dictionary)
{
    std::string journal_kind;
    std::string filename;
    if (dictionary == "quanpin")
    {
        journal_kind = "pinyin";
        filename = "水杉IME-拼音用户词库.txt";
    }
    else if (dictionary == "wubi")
    {
        journal_kind = "wubi";
        filename = "水杉IME-五笔用户词库.txt";
    }
    else if (dictionary == "english")
    {
        journal_kind = "english";
        filename = "水杉IME-英文用户词库.txt";
    }
    else if (dictionary == "quick")
    {
        journal_kind = "quick";
        filename = "水杉IME-快捷短语用户词库.txt";
    }
    else
    {
        return Result(false, "未知词库");
    }

    const std::string path = user_dictionary::default_user_db_path();
    if (!std::filesystem::exists(path))
        return Result(false, "当前没有可导出的用户新增词条");
    if (!user_dictionary::ensure_user_database(path))
        return Result(false, "升级用户词库格式失败");

    sqlite3 *raw = nullptr;
    if (sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    {
        const std::string error = raw ? sqlite3_errmsg(raw) : "无法打开用户词库";
        if (raw)
            sqlite3_close(raw);
        return Result(false, "打开用户词库失败：" + error);
    }
    Db db(raw);
    sqlite3_busy_timeout(db.get(), 3000);

    std::string error;
    const std::string export_filter = dictionary == "quanpin"
                                          ? "WHERE dictionary=?1 AND operation='upsert' "
                                          : "WHERE dictionary=?1 AND operation='upsert' AND user_inserted=1 ";
    Stmt stmt = Prepare(db.get(),
                        "SELECT key,value,weight,display FROM user_dictionary_operations " + export_filter +
                            "ORDER BY key,value",
                        error);
    if (!stmt || !BindText(stmt.get(), 1, journal_kind))
        return Result(false, "读取用户词库失败：" + error);

    std::ostringstream content;
    int count = 0;
    while (sqlite3_step(stmt.get()) == SQLITE_ROW)
    {
        const auto text = [&](int column) {
            const unsigned char *value = sqlite3_column_text(stmt.get(), column);
            return SanitizeExportField(value ? reinterpret_cast<const char *>(value) : "");
        };
        const std::string key = text(0);
        const std::string value = text(1);
        const auto weight = sqlite3_column_int64(stmt.get(), 2);
        const std::string display = text(3);

        if (dictionary == "quanpin" && HelpcodeUtils::count_utf8_chars(value) <= 1)
            continue;
        if (dictionary == "quanpin" || dictionary == "wubi")
            content << value << '\t' << key << '\t' << weight << '\n';
        else if (dictionary == "quick")
            content << key << '\t' << value << '\t' << weight << '\n';
        else
            content << key << '\t' << display << '\t' << weight << '\n';
        ++count;
    }
    if (count == 0)
        return Result(false, dictionary == "quanpin" ? "当前没有可导出的多字拼音词条" : "当前没有可导出的用户新增词条");

    json::object result = Result(true, "已导出 " + std::to_string(count) + " 条用户词条");
    result["content"] = content.str();
    result["filename"] = std::move(filename);
    result["entryCount"] = count;
    return result;
}

bool NormalizePinyin(const std::string &mode, const std::string &input, quanpin::Segments &segments,
                     std::string &normalized, std::string &message)
{
    (void)mode;
    if (!Validation::NormalizeFullPinyin(input, segments, normalized))
    {
        message = "全拼必须由拼音表中的完整音节组成，不能使用简拼";
        return false;
    }
    return true;
}

bool ValidateChineseEntry(const std::string &mode, const std::string &code, const std::string &word,
                          quanpin::Segments &segments, std::string &normalized, std::string &message)
{
    (void)mode;
    if (word.empty())
    {
        message = "词条不能为空";
        return false;
    }
    const size_t han_count = HelpcodeUtils::count_han_chars(word);
    if (!Validation::NormalizeFullPinyin(code, segments, normalized, han_count))
    {
        message = "全拼必须由拼音表中的完整音节组成，不能使用简拼";
        return false;
    }
    if (han_count != segments.size())
    {
        message = "拼音音节数量必须与汉字数量一致（词 " + std::to_string(han_count) + " 字，拼音 " +
                  std::to_string(segments.size()) + " 音节）";
        return false;
    }
    return true;
}

bool EnsureEnglishSchema()
{
    // Installed dictionaries keep their schema for the lifetime of Settings.
    // Do not open a write transaction for every read-only search.
    static std::mutex mutex;
    static std::set<std::string> initialized;
    std::lock_guard<std::mutex> lock(mutex);
    const auto path = CommonUtils::get_ime_data_path() + "\\english.db";
    if (initialized.count(path))
        return true;
    if (!EnglishDictionary::ensure_schema(path))
        return false;
    initialized.insert(path);
    return true;
}

json::object QueryChinese(const std::string &mode, const std::string &search, const json::object &request)
{
    std::string error;
    Db db = OpenDatabase("msime.db", error);
    if (!db)
        return Result(false, "打开拼音词库失败：" + error);
    quanpin::Segments segments;
    std::string normalized;
    if (!search.empty() && !NormalizePinyin(mode, search, segments, normalized, error))
        return Result(false, error);

    json::array rows;
    if (!normalized.empty())
    {
        const std::string table = quanpin::build_table_name(segments);
        Stmt stmt = Prepare(db.get(),
                            "SELECT key,value,weight FROM \"" + table + "\" WHERE key=?1 ORDER BY weight DESC,value" +
                                Paging::Sql(request),
                            error);
        if (!stmt || !BindText(stmt.get(), 1, normalized))
            return Result(false, "查询失败：" + error);
        return Paging::Read(stmt.get(), request);
    }
    json::object result = Result(true, rows.empty() ? "没有找到词条" : "查询成功");
    result["rows"] = std::move(rows);
    return result;
}

bool ChineseExists(sqlite3 *db, const quanpin::Segments &segments, const std::string &key, const std::string &word)
{
    std::string error;
    Stmt stmt = Prepare(
        db, "SELECT 1 FROM \"" + quanpin::build_table_name(segments) + "\" WHERE key=?1 AND value=?2 LIMIT 1", error);
    if (!stmt || !BindText(stmt.get(), 1, key) || !BindText(stmt.get(), 2, word))
        return false;
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

bool InsertChineseWord(sqlite3 *db, const quanpin::Segments &segments, const std::string &key, const std::string &word,
                       int weight, std::string &error)
{
    const std::string table = quanpin::build_table_name(segments);
    Stmt stmt = Prepare(db, "INSERT INTO \"" + table + "\"(key,jp,value,weight) VALUES(?1,?2,?3,?4)", error);
    if (!stmt || !BindText(stmt.get(), 1, key) || !BindText(stmt.get(), 2, quanpin::segments_to_jianpin(segments)) ||
        !BindText(stmt.get(), 3, word) || sqlite3_bind_int(stmt.get(), 4, weight) != SQLITE_OK ||
        sqlite3_step(stmt.get()) != SQLITE_DONE)
    {
        error = sqlite3_errmsg(db);
        return false;
    }
    (void)user_dictionary::record_user_insert(user_dictionary::default_user_db_path(),
                                              user_dictionary::DictionaryKind::Pinyin, key, word, weight);
    return true;
}

bool ParseImportLine(const std::string &line, std::string &word, std::string &code, int &weight, std::string &message)
{
    return Validation::ParseCodedImportLine(line, word, code, weight, message);
}

json::object ImportChinese(const json::object &request)
{
    std::string content = StringValue(request, "content");
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF)
        content.erase(0, 3);

    if (content.find_first_not_of(" \t\r\n") == std::string::npos)
        return Result(false, "文件内容为空");

    std::string error;
    Db db = OpenDatabase("msime.db", error);
    if (!db)
        return Result(false, "打开拼音词库失败：" + error);

    int inserted = 0;
    int skipped = 0;
    int failed = 0;
    std::vector<std::string> error_details;
    const auto append_error = [&](int line_no, const std::string &detail) {
        ++failed;
        if (error_details.size() < 5)
            error_details.push_back("第 " + std::to_string(line_no) + " 行：" + detail);
    };

    sqlite3_exec(db.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    std::istringstream stream(content);
    std::string line;
    int line_no = 0;
    while (std::getline(stream, line))
    {
        ++line_no;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos)
            continue;

        std::string word, code;
        int weight = 0;
        std::string message;
        if (!ParseImportLine(line, word, code, weight, message))
        {
            append_error(line_no, message);
            continue;
        }

        quanpin::Segments segments;
        std::string key;
        if (!ValidateChineseEntry("quanpin", code, word, segments, key, message))
        {
            append_error(line_no, message);
            continue;
        }
        if (ChineseExists(db.get(), segments, key, word))
        {
            ++skipped;
            continue;
        }
        if (!InsertChineseWord(db.get(), segments, key, word, weight, message))
        {
            append_error(line_no, "写入失败：" + message);
            continue;
        }
        ++inserted;
    }
    sqlite3_exec(db.get(), "COMMIT", nullptr, nullptr, nullptr);

    if (inserted == 0 && skipped == 0 && failed == 0)
        return Result(false, "文件中没有可导入的词条");

    std::string message;
    if (inserted > 0)
        message += "成功导入 " + std::to_string(inserted) + " 条";
    if (skipped > 0)
    {
        if (!message.empty())
            message += "，";
        message += "跳过 " + std::to_string(skipped) + " 条（已存在）";
    }
    if (failed > 0)
    {
        if (!message.empty())
            message += "，";
        message += "失败 " + std::to_string(failed) + " 条";
        if (!error_details.empty())
        {
            message += "。";
            for (size_t i = 0; i < error_details.size(); ++i)
            {
                if (i)
                    message += "；";
                message += error_details[i];
            }
            if (static_cast<int>(error_details.size()) < failed)
                message += "等";
        }
    }
    if (message.empty())
        message = "没有导入任何词条";
    if (inserted > 0)
        NotifyImeServerClearDictCache();
    return Result(inserted > 0 || (failed == 0 && skipped > 0), message);
}

json::object ImportHans(const json::object &request)
{
    std::string content = StringValue(request, "content");
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF)
        content.erase(0, 3);

    if (content.find_first_not_of(" \t\r\n") == std::string::npos)
        return Result(false, "文件内容为空");

    try
    {
        (void)PinyinAnnotator();
    }
    catch (...)
    {
        return Result(false, "拼音注音引擎初始化失败");
    }
    if (!PinyinAnnotator().initialized())
        return Result(false, "拼音词典未就绪，请确认 dict 目录与设置程序同级");

    std::string error;
    Db db = OpenDatabase("msime.db", error);
    if (!db)
        return Result(false, "打开拼音词库失败：" + error);

    constexpr int kDefaultWeight = 10000;
    int inserted = 0;
    int skipped = 0;
    int failed = 0;
    std::vector<std::string> error_details;
    const auto append_error = [&](int line_no, const std::string &detail) {
        ++failed;
        if (error_details.size() < 5)
            error_details.push_back("第 " + std::to_string(line_no) + " 行：" + detail);
    };

    sqlite3_exec(db.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    std::istringstream stream(content);
    std::string line;
    int line_no = 0;
    while (std::getline(stream, line))
    {
        ++line_no;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto begin = line.find_first_not_of(" \t");
        if (begin == std::string::npos)
            continue;
        const auto end = line.find_last_not_of(" \t");
        const std::string word = line.substr(begin, end - begin + 1);

        std::string code;
        std::string message;
        if (!AnnotateHansToPinyin(word, code, message))
        {
            append_error(line_no, message);
            continue;
        }

        quanpin::Segments segments;
        std::string key;
        if (!ValidateChineseEntry("quanpin", code, word, segments, key, message))
        {
            append_error(line_no, message);
            continue;
        }
        if (ChineseExists(db.get(), segments, key, word))
        {
            ++skipped;
            continue;
        }
        if (!InsertChineseWord(db.get(), segments, key, word, kDefaultWeight, message))
        {
            append_error(line_no, "写入失败：" + message);
            continue;
        }
        ++inserted;
    }
    sqlite3_exec(db.get(), "COMMIT", nullptr, nullptr, nullptr);

    if (inserted == 0 && skipped == 0 && failed == 0)
        return Result(false, "文件中没有可导入的词条");

    std::string message;
    if (inserted > 0)
        message += "成功导入 " + std::to_string(inserted) + " 条";
    if (skipped > 0)
    {
        if (!message.empty())
            message += "，";
        message += "跳过 " + std::to_string(skipped) + " 条（已存在）";
    }
    if (failed > 0)
    {
        if (!message.empty())
            message += "，";
        message += "失败 " + std::to_string(failed) + " 条";
        if (!error_details.empty())
        {
            message += "。";
            for (size_t i = 0; i < error_details.size(); ++i)
            {
                if (i)
                    message += "；";
                message += error_details[i];
            }
            if (static_cast<int>(error_details.size()) < failed)
                message += "等";
        }
    }
    if (message.empty())
        message = "没有导入任何词条";
    if (inserted > 0)
        NotifyImeServerClearDictCache();
    return Result(inserted > 0 || (failed == 0 && skipped > 0), message);
}

json::object MutateChinese(const json::object &request)
{
    const std::string mode = StringValue(request, "dictionary");
    const std::string action = StringValue(request, "action");
    const std::string code = StringValue(request, "code");
    const std::string word = StringValue(request, "word");
    const int weight = (std::max)(0, IntValue(request, "weight", 10));
    quanpin::Segments segments;
    std::string key, error;
    if (!ValidateChineseEntry(mode, code, word, segments, key, error))
        return Result(false, error);
    Db db = OpenDatabase("msime.db", error);
    if (!db)
        return Result(false, "打开拼音词库失败：" + error);

    if (action == "create")
    {
        if (ChineseExists(db.get(), segments, key, word))
            return Result(false, "词条已经存在");
        if (!InsertChineseWord(db.get(), segments, key, word, weight, error))
            return Result(false, "新增失败：" + error);
        NotifyImeServerClearDictCache();
        return Result(true, "词条新增成功");
    }

    const std::string old_code = StringValue(request, "oldCode");
    const std::string old_word = StringValue(request, "oldWord");
    quanpin::Segments old_segments;
    std::string old_key;
    if (!ValidateChineseEntry(mode, old_code, old_word, old_segments, old_key, error))
        return Result(false, "原词条无效：" + error);
    const std::string old_table = quanpin::build_table_name(old_segments);
    if (!ChineseExists(db.get(), old_segments, old_key, old_word))
        return Result(false, "原词条不存在或已被删除");

    if (action == "delete")
    {
        Stmt stmt = Prepare(db.get(), "DELETE FROM \"" + old_table + "\" WHERE key=?1 AND value=?2", error);
        if (!stmt || !BindText(stmt.get(), 1, old_key) || !BindText(stmt.get(), 2, old_word) ||
            sqlite3_step(stmt.get()) != SQLITE_DONE)
            return Result(false, "删除失败：" + std::string(sqlite3_errmsg(db.get())));
        (void)user_dictionary::record_delete(user_dictionary::default_user_db_path(),
                                             user_dictionary::DictionaryKind::Pinyin, old_key, old_word);
        NotifyImeServerClearDictCache();
        return Result(true, "词条删除成功");
    }

    if (action != "update")
        return Result(false, "未知操作");
    if ((old_key != key || old_word != word) && ChineseExists(db.get(), segments, key, word))
        return Result(false, "修改后的词条已经存在");
    const std::string table = quanpin::build_table_name(segments);
    sqlite3_exec(db.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    Stmt remove = Prepare(db.get(), "DELETE FROM \"" + old_table + "\" WHERE key=?1 AND value=?2", error);
    Stmt insert = Prepare(db.get(), "INSERT INTO \"" + table + "\"(key,jp,value,weight) VALUES(?1,?2,?3,?4)", error);
    const bool ok = remove && insert && BindText(remove.get(), 1, old_key) && BindText(remove.get(), 2, old_word) &&
                    sqlite3_step(remove.get()) == SQLITE_DONE && BindText(insert.get(), 1, key) &&
                    BindText(insert.get(), 2, quanpin::segments_to_jianpin(segments)) &&
                    BindText(insert.get(), 3, word) && sqlite3_bind_int(insert.get(), 4, weight) == SQLITE_OK &&
                    sqlite3_step(insert.get()) == SQLITE_DONE;
    sqlite3_exec(db.get(), ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
    if (ok)
    {
        const bool user_inserted = user_dictionary::is_user_inserted(
            user_dictionary::default_user_db_path(), user_dictionary::DictionaryKind::Pinyin, old_key, old_word);
        if (old_key != key || old_word != word)
            (void)user_dictionary::record_delete(user_dictionary::default_user_db_path(),
                                                 user_dictionary::DictionaryKind::Pinyin, old_key, old_word);
        if (user_inserted)
            (void)user_dictionary::record_user_insert(user_dictionary::default_user_db_path(),
                                                      user_dictionary::DictionaryKind::Pinyin, key, word, weight);
        else
            (void)user_dictionary::record_upsert(user_dictionary::default_user_db_path(),
                                                 user_dictionary::DictionaryKind::Pinyin, key, word, weight);
        NotifyImeServerClearDictCache();
    }
    return Result(ok, ok ? "词条修改成功" : "修改失败：" + std::string(sqlite3_errmsg(db.get())));
}

json::object ImportEnglish(const json::object &request)
{
    std::string content = StringValue(request, "content");
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF)
        content.erase(0, 3);
    if (content.find_first_not_of(" \t\r\n") == std::string::npos)
        return Result(false, "文件内容为空");

    std::string error;
    if (!EnsureEnglishSchema())
        return Result(false, "英文词库初始化失败");
    Db db = OpenDatabase("english.db", error);
    if (!db)
        return Result(false, "打开英文词库失败：" + error);
    Stmt insert = Prepare(db.get(), "INSERT OR IGNORE INTO english_words(word,display,weight) VALUES(?1,?2,?3)", error);
    if (!insert)
        return Result(false, "准备导入失败：" + error);

    const auto trim = [](std::string value) {
        const auto begin = value.find_first_not_of(" \t");
        if (begin == std::string::npos)
            return std::string{};
        const auto end = value.find_last_not_of(" \t");
        return value.substr(begin, end - begin + 1);
    };
    int inserted = 0;
    int skipped = 0;
    int failed = 0;
    std::vector<std::string> error_details;
    const auto append_error = [&](int line_no, const std::string &detail) {
        ++failed;
        if (error_details.size() < 5)
            error_details.push_back("第 " + std::to_string(line_no) + " 行：" + detail);
    };

    sqlite3_exec(db.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    std::istringstream stream(content);
    std::string line;
    int line_no = 0;
    while (std::getline(stream, line))
    {
        ++line_no;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (trim(line).empty())
            continue;

        std::vector<std::string> fields;
        std::string::size_type start = 0;
        for (;;)
        {
            const auto separator = line.find('\t', start);
            fields.push_back(trim(line.substr(start, separator == std::string::npos ? separator : separator - start)));
            if (separator == std::string::npos)
                break;
            start = separator + 1;
        }
        if (fields.size() != 2 && fields.size() != 3)
        {
            append_error(line_no, "格式错误，应为：单词<Tab>显示内容<Tab>权重");
            continue;
        }

        std::string word = fields[0];
        const std::string display = fields[1];
        std::transform(word.begin(), word.end(), word.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!IsAsciiWord(word) || display.empty())
        {
            append_error(line_no, "英文单词仅支持英文字母、连字符和撇号，显示内容不能为空");
            continue;
        }
        int weight = 0;
        if (fields.size() == 3)
        {
            const std::string &weight_text = fields[2];
            if (weight_text.empty() ||
                !std::all_of(weight_text.begin(), weight_text.end(), [](unsigned char ch) { return std::isdigit(ch); }))
            {
                append_error(line_no, "权重必须是非负整数");
                continue;
            }
            try
            {
                weight = std::stoi(weight_text);
            }
            catch (...)
            {
                append_error(line_no, "权重数值无效");
                continue;
            }
        }

        sqlite3_reset(insert.get());
        sqlite3_clear_bindings(insert.get());
        const bool ok = BindText(insert.get(), 1, word) && BindText(insert.get(), 2, display) &&
                        sqlite3_bind_int(insert.get(), 3, weight) == SQLITE_OK &&
                        sqlite3_step(insert.get()) == SQLITE_DONE;
        if (!ok)
        {
            append_error(line_no, "写入失败：" + std::string(sqlite3_errmsg(db.get())));
            continue;
        }
        if (sqlite3_changes(db.get()) == 0)
        {
            ++skipped;
            continue;
        }
        (void)user_dictionary::record_user_insert(user_dictionary::default_user_db_path(),
                                                  user_dictionary::DictionaryKind::English, word, display, weight,
                                                  display);
        ++inserted;
    }
    sqlite3_exec(db.get(), "COMMIT", nullptr, nullptr, nullptr);

    if (inserted == 0 && skipped == 0 && failed == 0)
        return Result(false, "文件中没有可导入的词条");
    std::string message;
    if (inserted > 0)
        message += "成功导入 " + std::to_string(inserted) + " 条";
    if (skipped > 0)
    {
        if (!message.empty())
            message += "，";
        message += "跳过 " + std::to_string(skipped) + " 条（已存在）";
    }
    if (failed > 0)
    {
        if (!message.empty())
            message += "，";
        message += "失败 " + std::to_string(failed) + " 条";
        if (!error_details.empty())
        {
            message += "。";
            for (size_t i = 0; i < error_details.size(); ++i)
            {
                if (i)
                    message += "；";
                message += error_details[i];
            }
            if (static_cast<int>(error_details.size()) < failed)
                message += "等";
        }
    }
    if (inserted > 0)
        NotifyImeServerClearDictCache();
    return Result(inserted > 0 || (failed == 0 && skipped > 0), message);
}

json::object HandleEnglish(const json::object &request)
{
    const std::string action = StringValue(request, "action");
    if (action == "import")
        return ImportEnglish(request);
    std::string word = StringValue(request, "word");
    const std::string display = StringValue(request, "display");
    const int weight = (std::max)(0, IntValue(request, "weight", 10));
    std::transform(word.begin(), word.end(), word.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::string error;
    if (!EnsureEnglishSchema())
        return Result(false, "英文词库初始化失败");
    Db db = OpenDatabase("english.db", error);
    if (!db)
        return Result(false, "打开英文词库失败：" + error);
    if (action == "query")
    {
        if (!IsAsciiWord(word))
            return Result(false, "请输入英文单词前缀");
        std::string upper = word;
        ++upper.back(); // ASCII prefix upper bound, compatible with the BINARY primary key.
        Stmt stmt = Prepare(db.get(),
                            "SELECT word,display,weight FROM english_words WHERE word>=?1 AND word<?3 "
                            "ORDER BY CASE WHEN word=?2 THEN 0 ELSE 1 END,weight DESC,length(word),word,display" +
                                Paging::Sql(request),
                            error);
        if (!stmt || !BindText(stmt.get(), 1, word) || !BindText(stmt.get(), 2, word) ||
            !BindText(stmt.get(), 3, upper))
            return Result(false, "查询失败：" + error);
        return Paging::Read(stmt.get(), request, true);
    }

    if (!IsAsciiWord(word) || display.empty())
        return Result(false, "英文单词仅支持英文字母、连字符和撇号，显示内容不能为空");
    std::string old_word = StringValue(request, "oldWord");
    const std::string old_display = StringValue(request, "oldDisplay");
    std::transform(old_word.begin(), old_word.end(), old_word.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::string sql;
    if (action == "create")
        sql = "INSERT INTO english_words(word,display,weight) VALUES(?1,?2,?3)";
    else if (action == "update")
        sql = "UPDATE english_words SET word=?1,display=?2,weight=?3 WHERE word=?4 AND display=?5";
    else if (action == "delete")
        sql = "DELETE FROM english_words WHERE word=?1 AND display=?2";
    else
        return Result(false, "未知操作");
    Stmt stmt = Prepare(db.get(), sql, error);
    bool ok = stmt != nullptr;
    if (ok && action == "delete")
        ok = BindText(stmt.get(), 1, old_word) && BindText(stmt.get(), 2, old_display);
    else if (ok)
    {
        ok = BindText(stmt.get(), 1, word) && BindText(stmt.get(), 2, display) &&
             sqlite3_bind_int(stmt.get(), 3, weight) == SQLITE_OK;
        if (ok && action == "update")
            ok = BindText(stmt.get(), 4, old_word) && BindText(stmt.get(), 5, old_display);
    }
    ok = ok && sqlite3_step(stmt.get()) == SQLITE_DONE && sqlite3_changes(db.get()) > 0;
    if (ok)
    {
        const bool user_inserted =
            action == "create" ||
            user_dictionary::is_user_inserted(user_dictionary::default_user_db_path(),
                                              user_dictionary::DictionaryKind::English, old_word, old_display);
        if (action == "delete")
            (void)user_dictionary::record_delete(user_dictionary::default_user_db_path(),
                                                 user_dictionary::DictionaryKind::English, old_word, old_display);
        else
        {
            if (action == "update" && (old_word != word || old_display != display))
                (void)user_dictionary::record_delete(user_dictionary::default_user_db_path(),
                                                     user_dictionary::DictionaryKind::English, old_word, old_display);
            if (user_inserted)
                (void)user_dictionary::record_user_insert(user_dictionary::default_user_db_path(),
                                                          user_dictionary::DictionaryKind::English, word, display,
                                                          weight, display);
            else
                (void)user_dictionary::record_upsert(user_dictionary::default_user_db_path(),
                                                     user_dictionary::DictionaryKind::English, word, display, weight,
                                                     display);
        }
    }
    const char *label = action == "create" ? "新增" : action == "update" ? "修改" : "删除";
    return Result(ok, ok ? std::string("英文词条") + label + "成功"
                         : std::string(label) + "失败：" + sqlite3_errmsg(db.get()));
}

json::object ImportWubi(const json::object &request)
{
    std::string content = StringValue(request, "content");
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF)
        content.erase(0, 3);
    if (content.find_first_not_of(" \t\r\n") == std::string::npos)
        return Result(false, "文件内容为空");

    std::string error;
    Db db = OpenDatabase("msime.db", error);
    if (!db)
        return Result(false, "打开五笔词库失败：" + error);
    Stmt insert = Prepare(db.get(),
                          "INSERT INTO wubi86(key,value,weight) SELECT ?1,?2,?3 "
                          "WHERE NOT EXISTS (SELECT 1 FROM wubi86 WHERE key=?1 AND value=?2)",
                          error);
    if (!insert)
        return Result(false, "准备导入失败：" + error);

    const auto valid_code = [](const std::string &value) {
        return !value.empty() &&
               std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; });
    };
    int inserted = 0;
    int skipped = 0;
    int failed = 0;
    std::vector<std::string> error_details;
    const auto append_error = [&](int line_no, const std::string &detail) {
        ++failed;
        if (error_details.size() < 5)
            error_details.push_back("第 " + std::to_string(line_no) + " 行：" + detail);
    };

    sqlite3_exec(db.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    std::istringstream stream(content);
    std::string line;
    int line_no = 0;
    while (std::getline(stream, line))
    {
        ++line_no;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos)
            continue;

        std::string word;
        std::string code;
        std::string message;
        int weight = 0;
        if (!Validation::ParseCodedImportLine(line, word, code, weight, message))
        {
            append_error(line_no, message);
            continue;
        }
        std::transform(code.begin(), code.end(), code.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!valid_code(code))
        {
            append_error(line_no, "五笔编码只能包含英文字母");
            continue;
        }

        sqlite3_reset(insert.get());
        sqlite3_clear_bindings(insert.get());
        const bool ok = BindText(insert.get(), 1, code) && BindText(insert.get(), 2, word) &&
                        sqlite3_bind_int(insert.get(), 3, weight) == SQLITE_OK &&
                        sqlite3_step(insert.get()) == SQLITE_DONE;
        if (!ok)
        {
            append_error(line_no, "写入失败：" + std::string(sqlite3_errmsg(db.get())));
            continue;
        }
        if (sqlite3_changes(db.get()) == 0)
        {
            ++skipped;
            continue;
        }
        (void)user_dictionary::record_user_insert(user_dictionary::default_user_db_path(),
                                                  user_dictionary::DictionaryKind::Wubi, code, word, weight);
        ++inserted;
    }
    sqlite3_exec(db.get(), "COMMIT", nullptr, nullptr, nullptr);

    if (inserted == 0 && skipped == 0 && failed == 0)
        return Result(false, "文件中没有可导入的词条");
    std::string message;
    if (inserted > 0)
        message += "成功导入 " + std::to_string(inserted) + " 条";
    if (skipped > 0)
    {
        if (!message.empty())
            message += "，";
        message += "跳过 " + std::to_string(skipped) + " 条（已存在）";
    }
    if (failed > 0)
    {
        if (!message.empty())
            message += "，";
        message += "失败 " + std::to_string(failed) + " 条";
        if (!error_details.empty())
        {
            message += "。";
            for (size_t i = 0; i < error_details.size(); ++i)
            {
                if (i)
                    message += "；";
                message += error_details[i];
            }
            if (static_cast<int>(error_details.size()) < failed)
                message += "等";
        }
    }
    if (inserted > 0)
        NotifyImeServerClearDictCache();
    return Result(inserted > 0 || (failed == 0 && skipped > 0), message);
}

json::object HandleWubi(const json::object &request)
{
    const std::string action = StringValue(request, "action");
    if (action == "import")
        return ImportWubi(request);
    std::string code = StringValue(request, "code");
    const std::string word = StringValue(request, "word");
    const int weight = (std::max)(0, IntValue(request, "weight", 10));
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::string error;
    Db db = OpenDatabase("msime.db", error);
    if (!db)
        return Result(false, "打开五笔词库失败：" + error);
    if (action == "query")
    {
        if (code.empty() ||
            !std::all_of(code.begin(), code.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; }))
            return Result(false, "请输入合法的五笔编码");
        Stmt stmt = Prepare(db.get(),
                            "SELECT key,value,weight FROM wubi86 WHERE key LIKE ?1 ORDER BY weight DESC,key,value" +
                                Paging::Sql(request),
                            error);
        if (!stmt || !BindText(stmt.get(), 1, code + "%"))
            return Result(false, "查询失败：" + error);
        return Paging::Read(stmt.get(), request);
    }
    if (code.empty() || word.empty() ||
        !std::all_of(code.begin(), code.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; }))
        return Result(false, "五笔编码和词条不能为空，编码只能包含英文字母");
    const std::string old_code = StringValue(request, "oldCode");
    const std::string old_word = StringValue(request, "oldWord");
    std::string sql;
    if (action == "create")
        sql = "INSERT INTO wubi86(key,value,weight) VALUES(?1,?2,?3)";
    else if (action == "update")
        sql = "UPDATE wubi86 SET key=?1,value=?2,weight=?3 WHERE key=?4 AND value=?5";
    else if (action == "delete")
        sql = "DELETE FROM wubi86 WHERE key=?1 AND value=?2";
    else
        return Result(false, "未知操作");
    Stmt stmt = Prepare(db.get(), sql, error);
    bool ok = stmt != nullptr;
    if (ok && action == "delete")
        ok = BindText(stmt.get(), 1, old_code) && BindText(stmt.get(), 2, old_word);
    else if (ok)
    {
        ok = BindText(stmt.get(), 1, code) && BindText(stmt.get(), 2, word) &&
             sqlite3_bind_int(stmt.get(), 3, weight) == SQLITE_OK;
        if (ok && action == "update")
            ok = BindText(stmt.get(), 4, old_code) && BindText(stmt.get(), 5, old_word);
    }
    ok = ok && sqlite3_step(stmt.get()) == SQLITE_DONE && sqlite3_changes(db.get()) > 0;
    if (ok)
    {
        const bool user_inserted = action == "create" || user_dictionary::is_user_inserted(
                                                             user_dictionary::default_user_db_path(),
                                                             user_dictionary::DictionaryKind::Wubi, old_code, old_word);
        if (action == "delete")
            (void)user_dictionary::record_delete(user_dictionary::default_user_db_path(),
                                                 user_dictionary::DictionaryKind::Wubi, old_code, old_word);
        else
        {
            if (action == "update" && (old_code != code || old_word != word))
                (void)user_dictionary::record_delete(user_dictionary::default_user_db_path(),
                                                     user_dictionary::DictionaryKind::Wubi, old_code, old_word);
            if (user_inserted)
                (void)user_dictionary::record_user_insert(user_dictionary::default_user_db_path(),
                                                          user_dictionary::DictionaryKind::Wubi, code, word, weight);
            else
                (void)user_dictionary::record_upsert(user_dictionary::default_user_db_path(),
                                                     user_dictionary::DictionaryKind::Wubi, code, word, weight);
        }
    }
    const char *label = action == "create" ? "新增" : action == "update" ? "修改" : "删除";
    return Result(ok, ok ? std::string("五笔词条") + label + "成功"
                         : std::string(label) + "失败：" + sqlite3_errmsg(db.get()));
}

json::object ImportQuickPhrase(const json::object &request)
{
    std::string content = StringValue(request, "content");
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF)
        content.erase(0, 3);
    if (content.find_first_not_of(" \t\r\n") == std::string::npos)
        return Result(false, "文件内容为空");

    std::string error;
    Db db = OpenDatabase("msime.db", error);
    if (!db)
        return Result(false, "打开快捷短语表失败：" + error);
    Stmt insert = Prepare(db.get(), "INSERT OR IGNORE INTO quick_parases(key,value,weight) VALUES(?1,?2,?3)", error);
    if (!insert)
        return Result(false, "准备导入失败：" + error);

    const auto trim = [](std::string value) {
        const auto begin = value.find_first_not_of(" \t");
        if (begin == std::string::npos)
            return std::string{};
        const auto end = value.find_last_not_of(" \t");
        return value.substr(begin, end - begin + 1);
    };
    const auto valid_code = [](const std::string &value) {
        return !value.empty() &&
               std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; });
    };
    int inserted = 0;
    int skipped = 0;
    int failed = 0;
    std::vector<std::string> error_details;
    const auto append_error = [&](int line_no, const std::string &detail) {
        ++failed;
        if (error_details.size() < 5)
            error_details.push_back("第 " + std::to_string(line_no) + " 行：" + detail);
    };

    sqlite3_exec(db.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    std::istringstream stream(content);
    std::string line;
    int line_no = 0;
    while (std::getline(stream, line))
    {
        ++line_no;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (trim(line).empty())
            continue;

        std::vector<std::string> fields;
        std::string::size_type start = 0;
        for (;;)
        {
            const auto separator = line.find('\t', start);
            fields.push_back(trim(line.substr(start, separator == std::string::npos ? separator : separator - start)));
            if (separator == std::string::npos)
                break;
            start = separator + 1;
        }
        if (fields.size() != 2 && fields.size() != 3)
        {
            append_error(line_no, "格式错误，应为：编码<Tab>短语<Tab>权重");
            continue;
        }

        std::string code = fields[0];
        const std::string phrase = fields[1];
        std::transform(code.begin(), code.end(), code.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (!valid_code(code) || phrase.empty())
        {
            append_error(line_no, "编码只能包含英文字母，短语不能为空");
            continue;
        }
        if (!Validation::QuickPhraseFitsNamedPipe(phrase))
        {
            append_error(line_no, "快捷短语不能超过 199 个 wchar 字符");
            continue;
        }
        int weight = 10;
        if (fields.size() == 3)
        {
            const std::string &weight_text = fields[2];
            if (weight_text.empty() ||
                !std::all_of(weight_text.begin(), weight_text.end(), [](unsigned char ch) { return std::isdigit(ch); }))
            {
                append_error(line_no, "权重必须是非负整数");
                continue;
            }
            try
            {
                weight = std::stoi(weight_text);
            }
            catch (...)
            {
                append_error(line_no, "权重数值无效");
                continue;
            }
        }

        sqlite3_reset(insert.get());
        sqlite3_clear_bindings(insert.get());
        const bool ok = BindText(insert.get(), 1, code) && BindText(insert.get(), 2, phrase) &&
                        sqlite3_bind_int(insert.get(), 3, weight) == SQLITE_OK &&
                        sqlite3_step(insert.get()) == SQLITE_DONE;
        if (!ok)
        {
            append_error(line_no, "写入失败：" + std::string(sqlite3_errmsg(db.get())));
            continue;
        }
        if (sqlite3_changes(db.get()) == 0)
        {
            ++skipped;
            continue;
        }
        (void)user_dictionary::record_user_insert(user_dictionary::default_user_db_path(),
                                                  user_dictionary::DictionaryKind::QuickPhrase, code, phrase, weight);
        ++inserted;
    }
    sqlite3_exec(db.get(), "COMMIT", nullptr, nullptr, nullptr);

    if (inserted == 0 && skipped == 0 && failed == 0)
        return Result(false, "文件中没有可导入的词条");
    std::string message;
    if (inserted > 0)
        message += "成功导入 " + std::to_string(inserted) + " 条";
    if (skipped > 0)
    {
        if (!message.empty())
            message += "，";
        message += "跳过 " + std::to_string(skipped) + " 条（已存在）";
    }
    if (failed > 0)
    {
        if (!message.empty())
            message += "，";
        message += "失败 " + std::to_string(failed) + " 条";
        if (!error_details.empty())
        {
            message += "。";
            for (size_t i = 0; i < error_details.size(); ++i)
            {
                if (i)
                    message += "；";
                message += error_details[i];
            }
            if (static_cast<int>(error_details.size()) < failed)
                message += "等";
        }
    }
    return Result(inserted > 0 || (failed == 0 && skipped > 0), message);
}

json::object HandleQuickPhrase(const json::object &request)
{
    const std::string action = StringValue(request, "action");
    if (action == "import")
        return ImportQuickPhrase(request);
    std::string code = StringValue(request, "code");
    const std::string phrase = StringValue(request, "word");
    const int weight = (std::max)(0, IntValue(request, "weight", 10));
    std::transform(code.begin(), code.end(), code.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto valid_code = [](const std::string &value) {
        return !value.empty() &&
               std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; });
    };
    std::string error;
    Db db = OpenDatabase("msime.db", error);
    if (!db)
        return Result(false, "打开快捷短语表失败：" + error);

    if (action == "query")
    {
        if (!code.empty() && !valid_code(code))
            return Result(false, "编码只能包含英文字母");
        Stmt stmt = Prepare(db.get(),
                            "SELECT key,value,weight FROM quick_parases WHERE key LIKE ?1 "
                            "ORDER BY weight DESC,key,value" +
                                Paging::Sql(request),
                            error);
        if (!stmt || !BindText(stmt.get(), 1, code + "%"))
            return Result(false, "查询失败：" + error);
        return Paging::Read(stmt.get(), request);
    }

    if (!valid_code(code) || phrase.empty())
        return Result(false, "编码只能包含英文字母，短语不能为空");
    if ((action == "create" || action == "update") && !Validation::QuickPhraseFitsNamedPipe(phrase))
        return Result(false, "快捷短语不能超过 199 个 wchar 字符");
    const std::string old_code = StringValue(request, "oldCode");
    const std::string old_phrase = StringValue(request, "oldWord");
    std::string sql;
    if (action == "create")
        sql = "INSERT INTO quick_parases(key,value,weight) VALUES(?1,?2,?3)";
    else if (action == "update")
        sql = "UPDATE quick_parases SET key=?1,value=?2,weight=?3 WHERE key=?4 AND value=?5";
    else if (action == "delete")
        sql = "DELETE FROM quick_parases WHERE key=?1 AND value=?2";
    else
        return Result(false, "未知操作");

    Stmt stmt = Prepare(db.get(), sql, error);
    bool ok = stmt != nullptr;
    if (ok && action == "delete")
        ok = BindText(stmt.get(), 1, old_code) && BindText(stmt.get(), 2, old_phrase);
    else if (ok)
    {
        ok = BindText(stmt.get(), 1, code) && BindText(stmt.get(), 2, phrase) &&
             sqlite3_bind_int(stmt.get(), 3, weight) == SQLITE_OK;
        if (ok && action == "update")
            ok = BindText(stmt.get(), 4, old_code) && BindText(stmt.get(), 5, old_phrase);
    }
    ok = ok && sqlite3_step(stmt.get()) == SQLITE_DONE && sqlite3_changes(db.get()) > 0;
    if (ok)
    {
        const bool user_inserted =
            action == "create" ||
            user_dictionary::is_user_inserted(user_dictionary::default_user_db_path(),
                                              user_dictionary::DictionaryKind::QuickPhrase, old_code, old_phrase);
        if (action == "delete")
            (void)user_dictionary::record_delete(user_dictionary::default_user_db_path(),
                                                 user_dictionary::DictionaryKind::QuickPhrase, old_code, old_phrase);
        else
        {
            if (action == "update" && (old_code != code || old_phrase != phrase))
                (void)user_dictionary::record_delete(user_dictionary::default_user_db_path(),
                                                     user_dictionary::DictionaryKind::QuickPhrase, old_code,
                                                     old_phrase);
            if (user_inserted)
                (void)user_dictionary::record_user_insert(user_dictionary::default_user_db_path(),
                                                          user_dictionary::DictionaryKind::QuickPhrase, code, phrase,
                                                          weight);
            else
                (void)user_dictionary::record_upsert(user_dictionary::default_user_db_path(),
                                                     user_dictionary::DictionaryKind::QuickPhrase, code, phrase,
                                                     weight);
        }
    }
    const char *label = action == "create" ? "新增" : action == "update" ? "修改" : "删除";
    return Result(ok, ok ? std::string("快捷短语") + label + "成功"
                         : std::string(label) + "失败：" + sqlite3_errmsg(db.get()));
}
} // namespace

json::object HandleRequest(const json::object &request)
{
    const std::string dictionary = StringValue(request, "dictionary");
    const std::string action = StringValue(request, "action");
    if (action == "export")
        return ExportUserDictionary(dictionary);
    // Pure-Chinese import always targets the quanpin dictionary.
    if (action == "importHans")
        return ImportHans(request);
    if (dictionary == "english")
        return HandleEnglish(request);
    if (dictionary == "wubi")
        return HandleWubi(request);
    if (dictionary == "quick")
        return HandleQuickPhrase(request);
    if (dictionary != "quanpin")
        return Result(false, "未知词库");
    if (action == "query")
        return QueryChinese(dictionary, StringValue(request, "code"), request);
    if (action == "import")
        return ImportChinese(request);
    return MutateChinese(request);
}
} // namespace SettingsDictionary
