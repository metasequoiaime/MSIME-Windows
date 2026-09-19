#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "config/ime_config.h"
#include "engine/contracts/webview/validator.h"
#include "settings/statistics_settings.h"
#include "statistics/stats_store.h"

#include <boost/json.hpp>
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
namespace json = boost::json;

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

// Pins every path the config loader consults, so a machine with the IME installed cannot decide
// where these tests read and write.
class ScopedConfigLocation
{
  public:
    explicit ScopedConfigLocation(const std::filesystem::path &config_dir)
        : local_app_data_(L"LOCALAPPDATA", config_dir.parent_path().wstring()),
          config_dir_(L"METASEQUOIA_IME_CONFIG_DIR", config_dir.wstring()),
          data_dir_(L"METASEQUOIA_IME_DATA_DIR", config_dir.wstring())
    {
    }

  private:
    ScopedEnv local_app_data_;
    ScopedEnv config_dir_;
    ScopedEnv data_dir_;
};

void WriteText(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadText(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void SeedShippedTemplate(const std::filesystem::path &config_dir)
{
    std::error_code ec;
    std::filesystem::copy_file(MSIME_DEFAULT_CONFIG_PATH, config_dir / L"config.default.toml",
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE(!ec);
}

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

json::object Request(const char *request_id, const char *action, const char *range = nullptr)
{
    json::object request{{"requestId", json::string(request_id)}, {"action", json::string(action)}};
    if (range != nullptr)
    {
        request["range"] = json::string(range);
    }
    return request;
}

// The page only accepts a response that satisfies the generated schema, so the test asserts the
// same thing while the fixture stays the single source of truth.
void RequireValidResponse(const json::object &payload)
{
    json::value envelope = {
        {"type", "statsResponse"}, {"protocolVersion", metasequoia::webview::Version}, {"data", payload}};
    REQUIRE(metasequoia::webview::Validate(envelope, "server"));
}

void RequireValidRecentResponse(const json::object &payload)
{
    json::value envelope = {
        {"type", "statsRecentResponse"}, {"protocolVersion", metasequoia::webview::Version}, {"data", payload}};
    REQUIRE(metasequoia::webview::Validate(envelope, "server"));
}

void RequireZeroWindow(const json::object &response, const char *key)
{
    const json::object &window = response.at(key).as_object();
    REQUIRE_EQ(json::value_to<int>(window.at("chars")), 0);
    REQUIRE_EQ(json::value_to<int>(window.at("activeMs")), 0);
}
} // namespace

TEST_CASE(statistics_settings_query_reports_the_empty_state)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-空");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));

    const json::object response = SettingsStatistics::HandleRequest(Request("req-1", "query"), &store);
    RequireValidResponse(response);

    REQUIRE_EQ(json::value_to<std::string>(response.at("requestId")), std::string("req-1"));
    REQUIRE(response.at("ok").as_bool());
    REQUIRE(response.at("daily").as_array().empty());
    REQUIRE(response.at("hourly").as_array().empty());
    const json::object &meta = response.at("meta").as_object();
    REQUIRE(meta.at("enabled").is_bool());
    // No records: no firstDay, which is how the panel tells an empty profile from an all-zero one.
    REQUIRE(meta.if_contains("firstDay") == nullptr);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_settings_query_serializes_the_recorded_rows)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-查询");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t yesterday = DayKeyDaysAgo(1);
    REQUIRE(store.Apply(MakeRecord(yesterday, 23, 2, 3, 4, 5, 6, 700)));
    REQUIRE(store.Apply(MakeRecord(today, 8, 1, 0, 0, 0, 0, 100)));

    const json::object response = SettingsStatistics::HandleRequest(Request("req-2", "query"), &store);
    RequireValidResponse(response);
    REQUIRE(response.at("ok").as_bool());

    const json::array &daily = response.at("daily").as_array();
    REQUIRE_EQ(daily.size(), std::size_t{2});
    const json::object &oldest = daily[0].as_object();
    REQUIRE_EQ(json::value_to<int>(oldest.at("day")), yesterday);
    REQUIRE_EQ(json::value_to<int>(oldest.at("cjk")), 2);
    REQUIRE_EQ(json::value_to<int>(oldest.at("latin")), 3);
    REQUIRE_EQ(json::value_to<int>(oldest.at("digit")), 4);
    REQUIRE_EQ(json::value_to<int>(oldest.at("punct")), 5);
    REQUIRE_EQ(json::value_to<int>(oldest.at("other")), 6);
    REQUIRE_EQ(json::value_to<int>(oldest.at("activeMs")), 700);
    REQUIRE_EQ(json::value_to<int>(daily[1].as_object().at("day")), today);

    const json::array &hourly = response.at("hourly").as_array();
    REQUIRE_EQ(hourly.size(), std::size_t{2});
    REQUIRE_EQ(json::value_to<int>(hourly[0].as_object().at("hour")), 23);
    REQUIRE_EQ(json::value_to<int>(hourly[1].as_object().at("chars")), 1);

    const json::object &meta = response.at("meta").as_object();
    REQUIRE_EQ(json::value_to<int>(meta.at("firstDay")), yesterday);
    REQUIRE_EQ(meta.at("enabled").as_bool(), GetConfiguredStatisticsEnabled());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_settings_retired_clear_action_is_an_unknown_action)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-退役清理");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t old = DayKeyDaysAgo(100);
    REQUIRE(store.Apply(MakeRecord(today, 9, 5, 0, 0, 0, 0, 100)));
    REQUIRE(store.Apply(MakeRecord(old, 9, 7, 0, 0, 0, 0, 100)));

    // "clear" retired with the manual button: retention is a standing policy now. A stale page
    // asking for it must be answered as unknown rather than deleting anything.
    const json::object clear = SettingsStatistics::HandleRequest(Request("req-3", "clear", "30d"), &store);
    RequireValidResponse(clear);
    REQUIRE(!clear.at("ok").as_bool());
    REQUIRE(!json::value_to<std::string>(clear.at("message")).empty());
    REQUIRE_EQ(clear.at("daily").as_array().size(), std::size_t{2});
    REQUIRE_EQ(clear.at("hourly").as_array().size(), std::size_t{2});
    REQUIRE_EQ(json::value_to<int>(clear.at("meta").as_object().at("firstDay")), old);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_settings_recent_reports_zeroed_windows_before_any_input)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-近期空");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));

    const json::object response =
        SettingsStatistics::BuildRecentResponse(json::object{{"requestId", "recent-1"}}, &store);
    RequireValidRecentResponse(response);

    REQUIRE_EQ(json::value_to<std::string>(response.at("requestId")), std::string("recent-1"));
    REQUIRE(response.at("ok").as_bool());
    RequireZeroWindow(response, "m5");
    RequireZeroWindow(response, "h1");
    RequireZeroWindow(response, "d1");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_settings_recent_serializes_the_recorded_samples)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-近期计数");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    const int32_t today = DayKeyDaysAgo(0);
    // Two batches: the response must carry the folded sums, exactly as the persistent store folds
    // its rows (five character classes plus active_ms).
    REQUIRE(store.Apply(MakeRecord(today, 9, 3, 1, 2, 4, 0, 1000)));
    REQUIRE(store.Apply(MakeRecord(today, 9, 1, 1, 1, 1, 1, 500)));

    const json::object response =
        SettingsStatistics::BuildRecentResponse(json::object{{"requestId", "recent-2"}}, &store);
    RequireValidRecentResponse(response);
    REQUIRE(response.at("ok").as_bool());

    const json::object &m5 = response.at("m5").as_object();
    REQUIRE_EQ(json::value_to<int>(m5.at("chars")), 15);
    REQUIRE_EQ(json::value_to<int>(m5.at("activeMs")), 1500);
    const json::object &h1 = response.at("h1").as_object();
    REQUIRE_EQ(json::value_to<int>(h1.at("chars")), 15);
    REQUIRE_EQ(json::value_to<int>(h1.at("activeMs")), 1500);
    const json::object &d1 = response.at("d1").as_object();
    REQUIRE_EQ(json::value_to<int>(d1.at("chars")), 15);
    REQUIRE_EQ(json::value_to<int>(d1.at("activeMs")), 1500);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_settings_recent_rejects_a_request_without_request_id)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-近期非法");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    REQUIRE(store.Apply(MakeRecord(DayKeyDaysAgo(0), 9, 5, 0, 0, 0, 0, 100)));

    // Without a requestId the response cannot be correlated with the poll that asked for it. It is
    // refused with the full shape and zeroed windows so the page keeps its previous numbers.
    const json::object response = SettingsStatistics::BuildRecentResponse(json::object{}, &store);
    RequireValidRecentResponse(response);
    REQUIRE(!response.at("ok").as_bool());
    REQUIRE(!json::value_to<std::string>(response.at("message")).empty());
    REQUIRE_EQ(json::value_to<std::string>(response.at("requestId")), std::string(""));
    RequireZeroWindow(response, "m5");
    RequireZeroWindow(response, "h1");
    RequireZeroWindow(response, "d1");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_settings_retention_change_trims_immediately)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-变更即清理");
    SeedShippedTemplate(root);
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t old = DayKeyDaysAgo(100);
    REQUIRE(store.Apply(MakeRecord(old, 9, 7, 0, 0, 0, 0, 100)));
    REQUIRE(store.Apply(MakeRecord(today, 9, 5, 0, 0, 0, 0, 100)));

    {
        const ScopedConfigLocation location(root);
        InitImeConfig();
        REQUIRE_EQ(GetConfiguredStatisticsRetention(), std::string("forever"));
        REQUIRE(SettingsStatistics::ApplyRetentionPolicy("30d", &store));
        REQUIRE_EQ(GetConfiguredStatisticsRetention(), std::string("30d"));
        REQUIRE(ReadText(root / L"config.toml").find("retention = \"30d\"") != std::string::npos);

        // The trim happened in the same call, not at the next day boundary.
        Statistics::Snapshot snapshot;
        REQUIRE(store.Query(snapshot));
        REQUIRE_EQ(snapshot.daily.size(), std::size_t{1});
        REQUIRE_EQ(snapshot.daily[0].day, today);
        REQUIRE_EQ(snapshot.hourly.size(), std::size_t{1});
        REQUIRE(snapshot.has_first_day);
        REQUIRE_EQ(snapshot.first_day, today);

        // An unknown value is refused before anything is written or deleted.
        REQUIRE(!SettingsStatistics::ApplyRetentionPolicy("weekly", &store));
        REQUIRE_EQ(GetConfiguredStatisticsRetention(), std::string("30d"));
        REQUIRE(store.Query(snapshot));
        REQUIRE_EQ(snapshot.daily.size(), std::size_t{1});
    }

    // The policy survives a reopen of the settings page (and of the process).
    {
        const ScopedConfigLocation location(root);
        InitImeConfig();
        REQUIRE_EQ(GetConfiguredStatisticsRetention(), std::string("30d"));
        REQUIRE(SettingsStatistics::ApplyRetentionPolicy("forever", &store));
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_settings_retention_forever_keeps_every_row)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-永久保留");
    SeedShippedTemplate(root);
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t old = DayKeyDaysAgo(500);
    REQUIRE(store.Apply(MakeRecord(old, 9, 7, 0, 0, 0, 0, 100)));
    REQUIRE(store.Apply(MakeRecord(today, 9, 5, 0, 0, 0, 0, 100)));

    {
        const ScopedConfigLocation location(root);
        InitImeConfig();
        REQUIRE(SettingsStatistics::ApplyRetentionPolicy("forever", &store));

        // "forever" writes the policy and deletes nothing, even rows older than any window.
        Statistics::Snapshot snapshot;
        REQUIRE(store.Query(snapshot));
        REQUIRE_EQ(snapshot.daily.size(), std::size_t{2});
        REQUIRE_EQ(snapshot.daily[0].day, old);
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_settings_retention_write_failure_trims_nothing)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-策略写失败");
    SeedShippedTemplate(root);
    const std::filesystem::path blocker = root / L"not-a-directory";
    WriteText(blocker, "x");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t old = DayKeyDaysAgo(100);
    REQUIRE(store.Apply(MakeRecord(old, 9, 7, 0, 0, 0, 0, 100)));
    REQUIRE(store.Apply(MakeRecord(today, 9, 5, 0, 0, 0, 0, 100)));

    {
        const ScopedConfigLocation blocked(blocker);
        InitImeConfig();
        // The order matters: no config write, no deletion. A failed save must never look like a
        // successful policy change.
        REQUIRE(!SettingsStatistics::ApplyRetentionPolicy("30d", &store));
        REQUIRE_EQ(GetConfiguredStatisticsRetention(), std::string("forever"));
    }

    Statistics::Snapshot snapshot;
    REQUIRE(store.Query(snapshot));
    REQUIRE_EQ(snapshot.daily.size(), std::size_t{2});
    REQUIRE_EQ(snapshot.daily[0].day, old);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
