#include "session_factory.h"

#include "config/ime_config.h"
#include "engine_input_session.h"
#include "engine/shuangpin/shuangpin_profile.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace
{
std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string ResolveEffectiveBackend(std::string configured_backend, SchemeType scheme)
{
    // Old installations may still persist "legacy". It is a configuration alias
    // for the shared engine now; keeping a second composition owner is unsafe.
    if (configured_backend.empty() || configured_backend == "legacy" || configured_backend == "engine")
    {
        switch (scheme)
        {
        case SchemeType::Shuangpin:
            return "engine-shuangpin";
        case SchemeType::Quanpin:
            return "engine-quanpin";
        case SchemeType::Wubi:
            return "engine-wubi";
        case SchemeType::JapaneseRomaji:
            return "engine-japanese-romaji";
        case SchemeType::JapaneseKana:
            return "engine-japanese-kana";
        default:
            throw std::runtime_error("Unknown input scheme.");
        }
    }

    throw std::runtime_error("Unsupported input session backend: " + configured_backend);
}
} // namespace

std::shared_ptr<IInputSession> CreateInputSessionFromConfig()
{
    const std::string backend = DescribeEffectiveInputSessionBackendFromConfig();
    const ShuangpinProfile &shuangpin_profile = GetShuangpinProfile(GetConfiguredShuangpinSchema());
    if (backend == "engine-shuangpin")
    {
        return std::make_shared<EngineInputSession>(SchemeType::Shuangpin, shuangpin_profile);
    }
    if (backend == "engine-quanpin")
    {
        return std::make_shared<EngineInputSession>(SchemeType::Quanpin);
    }
    if (backend == "engine-wubi")
    {
        return std::make_shared<EngineInputSession>(SchemeType::Wubi);
    }
    if (backend == "engine-japanese-romaji")
    {
        return std::make_shared<EngineInputSession>(SchemeType::JapaneseRomaji);
    }
    if (backend == "engine-japanese-kana")
    {
        return std::make_shared<EngineInputSession>(SchemeType::JapaneseKana);
    }

    throw std::runtime_error("Unsupported effective input session backend: " + backend);
}

std::shared_ptr<IInputSession> CreateTemporaryJapaneseInputSession()
{
    return std::make_shared<EngineInputSession>(SchemeType::JapaneseRomaji);
}

std::string DescribeConfiguredInputSessionBackendFromConfig()
{
    return ToLowerAscii(GetConfiguredSessionBackend());
}

std::string DescribeEffectiveInputSessionBackendFromConfig()
{
    return ResolveEffectiveBackend(DescribeConfiguredInputSessionBackendFromConfig(), GetConfiguredActiveInputScheme());
}
