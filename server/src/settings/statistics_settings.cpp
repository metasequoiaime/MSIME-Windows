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

// Every response carries the full current data set: a clear answers with what is left, so the
// panel refreshes from one round trip instead of querying again after every deletion.
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
    if (action == "clear")
    {
        // Clears history older than the selected window: 30d/90d keep the most recent 30/90 local
        // days, "all" drops everything. An empty or unknown range is refused rather than treated
        // as "all".
        if (!target.Clear(StringField(request, "range")))
        {
            return BuildResponse(request, target, false, "清理参数无效或清理失败");
        }
        return BuildResponse(request, target, true, {});
    }
    return BuildResponse(request, target, false, "未知统计操作");
}
} // namespace SettingsStatistics
