#pragma once

#include <boost/json.hpp>

#include "statistics/stats_store.h"

namespace SettingsStatistics
{
// Handles one statsRequest payload and returns the statsResponse "data" object (requestId, ok,
// daily, hourly, meta). The settings host only adds the envelope and protocolVersion.
//
// store == nullptr uses the process-wide store; tests pass their own so the result does not depend
// on where METASEQUOIA_IME_DATA_DIR pointed when the process-wide instance was first constructed.
boost::json::object HandleRequest(const boost::json::object &request, Statistics::StatsStore *store = nullptr);

// Persists a new retention policy and trims the store once with it, so a policy change takes effect
// immediately instead of at the next day boundary. The config write happens first and must succeed:
// a save that failed must never delete history the user is not actually committed to dropping.
// "forever" only writes the key.
bool ApplyRetentionPolicy(const std::string &range, Statistics::StatsStore *store = nullptr);
} // namespace SettingsStatistics
