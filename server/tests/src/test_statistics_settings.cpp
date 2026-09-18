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

TEST_CASE(statistics_settings_clear_answers_with_what_is_left)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-清理");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    const int32_t today = DayKeyDaysAgo(0);
    const int32_t old = DayKeyDaysAgo(100);
    REQUIRE(store.Apply(MakeRecord(today, 9, 5, 0, 0, 0, 0, 100)));
    REQUIRE(store.Apply(MakeRecord(old, 9, 7, 0, 0, 0, 0, 100)));

    const json::object cleared = SettingsStatistics::HandleRequest(Request("req-3", "clear", "30d"), &store);
    RequireValidResponse(cleared);
    REQUIRE(cleared.at("ok").as_bool());
    // 30d keeps the most recent 30 days, so the 100-day-old row is the one that goes.
    const json::array &daily = cleared.at("daily").as_array();
    REQUIRE_EQ(daily.size(), std::size_t{1});
    REQUIRE_EQ(json::value_to<int>(daily[0].as_object().at("day")), today);
    REQUIRE_EQ(json::value_to<int>(cleared.at("meta").as_object().at("firstDay")), today);

    const json::object emptied = SettingsStatistics::HandleRequest(Request("req-4", "clear", "all"), &store);
    RequireValidResponse(emptied);
    REQUIRE(emptied.at("ok").as_bool());
    REQUIRE(emptied.at("daily").as_array().empty());
    REQUIRE(emptied.at("hourly").as_array().empty());
    REQUIRE(emptied.at("meta").as_object().if_contains("firstDay") == nullptr);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_settings_rejects_unknown_actions_and_ranges)
{
    const std::filesystem::path root = MakeTempRoot(L"msime-stats-settings-拒绝");
    Statistics::StatsStore store(test::Utf8(root / L"msime_stats.db"));
    REQUIRE(store.Apply(MakeRecord(DayKeyDaysAgo(0), 9, 1, 0, 0, 0, 0, 100)));

    const json::object unknown_action = SettingsStatistics::HandleRequest(Request("req-5", "erase"), &store);
    RequireValidResponse(unknown_action);
    REQUIRE(!unknown_action.at("ok").as_bool());
    REQUIRE(!json::value_to<std::string>(unknown_action.at("message")).empty());
    REQUIRE_EQ(unknown_action.at("daily").as_array().size(), std::size_t{1});

    const json::object missing_range = SettingsStatistics::HandleRequest(Request("req-6", "clear"), &store);
    RequireValidResponse(missing_range);
    REQUIRE(!missing_range.at("ok").as_bool());
    REQUIRE(!json::value_to<std::string>(missing_range.at("message")).empty());
    REQUIRE_EQ(missing_range.at("daily").as_array().size(), std::size_t{1});

    const json::object bad_range = SettingsStatistics::HandleRequest(Request("req-7", "clear", "7d"), &store);
    RequireValidResponse(bad_range);
    REQUIRE(!bad_range.at("ok").as_bool());
    REQUIRE_EQ(bad_range.at("daily").as_array().size(), std::size_t{1});

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
