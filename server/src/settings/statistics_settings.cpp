#include "settings/statistics_settings.h"

#include <string>
#include <utility>
#include <vector>

#include "config/ime_config.h"

namespace SettingsStatistics
{
namespace
{
namespace json = boost::json;

std::string StringField(const json::object &object, const char *key)
{
    const json::value *value = object.if_contains(key);
    if (value == nullptr || !value->is_string())
    {
        return {};
    }
    return std::string(value->as_string());
}

json::array BuildDaily(const std::vector<Statistics::DailyRow> &rows)
{
    json::array result;
    result.reserve(rows.size());
    for (const Statistics::DailyRow &row : rows)
    {
        result.push_back(json::object{{"day", row.day},
                                      {"cjk", row.cjk},
                                      {"latin", row.latin},
                                      {"digit", row.digit},
                                      {"punct", row.punct},
                                      {"other", row.other},
                                      {"activeMs", row.active_ms}});
    }
    return result;
}

json::array BuildHourly(const std::vector<Statistics::HourlyRow> &rows)
{
    json::array result;
    result.reserve(rows.size());
    for (const Statistics::HourlyRow &row : rows)
    {
        result.push_back(
            json::object{{"day", row.day}, {"hour", row.hour}, {"chars", row.chars}, {"activeMs", row.active_ms}});
    }
    return result;
}

json::object BuildWindowEntry(const Statistics::RecentWindow &window)
{
    return json::object{{"chars", window.chars}, {"activeMs", window.active_ms}};
}

// The failure shape is the success shape with zeroed windows: the page only needs a structurally
// complete message so a rejected request keeps its previous numbers instead of clearing them.
json::object BuildRecent(const std::string &request_id, bool ok, const std::string &message,
                         const Statistics::RecentWindows &windows)
{
    json::object response{{"requestId", request_id},
                          {"ok", ok},
                          {"m5", BuildWindowEntry(windows.m5)},
                          {"h1", BuildWindowEntry(windows.h1)},
                          {"d1", BuildWindowEntry(windows.d1)}};
    if (!message.empty())
    {
        response["message"] = message;
    }
    return response;
}

// Every response carries the full current data set, so the panel renders from one round trip and
// does not need a follow-up query.
json::object BuildResponse(const json::object &request, Statistics::StatsStore &store, bool ok,
                           const std::string &message)
{
    Statistics::Snapshot snapshot;
    const bool loaded = store.Query(snapshot);

    json::object meta{{"enabled", GetConfiguredStatisticsEnabled()}};
    if (loaded && snapshot.has_first_day)
    {
        meta["firstDay"] = snapshot.first_day;
    }

    json::object response{{"requestId", StringField(request, "requestId")},
                          {"ok", ok && loaded},
                          {"daily", loaded ? BuildDaily(snapshot.daily) : json::array{}},
                          {"hourly", loaded ? BuildHourly(snapshot.hourly) : json::array{}},
                          {"meta", std::move(meta)}};
    if (!message.empty())
    {
        response["message"] = message;
    }
    else if (!loaded)
    {
        response["message"] = "读取统计失败";
    }
    return response;
}
} // namespace

json::object HandleRequest(const json::object &request, Statistics::StatsStore *store)
{
    Statistics::StatsStore &target = store != nullptr ? *store : Statistics::SharedStatsStore();
    const std::string action = StringField(request, "action");

    if (action == "query")
    {
        return BuildResponse(request, target, true, {});
    }
    // "clear" retired with the manual button: retention is a standing policy now, applied when the
    // value changes and on the first write of a new day. A request that still asks for it is an
    // unknown action, not a silently accepted deletion.
    return BuildResponse(request, target, false, "未知统计操作");
}

json::object BuildRecentResponse(const json::object &request, Statistics::StatsStore *store)
{
    const std::string request_id = StringField(request, "requestId");
    // A request without a requestId cannot be correlated with its response, so it is refused
    // instead of answered. The page drops any response whose id it does not recognise, which is
    // exactly the "keep the previous numbers" behaviour a rejected request should have.
    if (request_id.empty())
    {
        return BuildRecent("", false, "无效的统计请求", {});
    }

    Statistics::StatsStore &target = store != nullptr ? *store : Statistics::SharedStatsStore();
    Statistics::RecentWindows windows;
    if (!target.QueryRecent(Statistics::NowSeconds(), windows))
    {
        return BuildRecent(request_id, false, "读取统计失败", {});
    }
    return BuildRecent(request_id, true, {}, windows);
}

bool ApplyRetentionPolicy(const std::string &range, Statistics::StatsStore *store)
{
    if (!SetConfiguredStatisticsRetention(range))
    {
        return false;
    }
    if (range == "forever")
    {
        return true;
    }
    Statistics::StatsStore &target = store != nullptr ? *store : Statistics::SharedStatsStore();
    return target.Clear(range);
}
} // namespace SettingsStatistics
