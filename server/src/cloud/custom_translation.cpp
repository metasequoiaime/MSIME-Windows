#include "custom_translation.h"

#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>

namespace
{
constexpr long kTimeoutMs = 2500;
constexpr size_t kMaxResponseBytes = 1024 * 1024;
constexpr auto kBatchBudget = std::chrono::seconds(6);

struct ResponseBuffer
{
    std::string text;
    bool overflow = false;
};

size_t WriteResponse(char *data, size_t size, size_t count, void *user)
{
    const size_t bytes = size * count;
    auto *buffer = static_cast<ResponseBuffer *>(user);
    if (bytes > kMaxResponseBytes - (std::min)(buffer->text.size(), kMaxResponseBytes))
    {
        buffer->overflow = true;
        return 0;
    }
    buffer->text.append(data, bytes);
    return bytes;
}

std::string UppercaseLanguage(std::string language)
{
    std::transform(language.begin(), language.end(), language.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return language;
}

std::string StringFromValue(const nlohmann::json &value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (!value.is_object())
        return {};
    for (const char *key : {"text", "translation", "data"})
    {
        const auto found = value.find(key);
        if (found != value.end() && found->is_string())
            return found->get<std::string>();
    }
    return {};
}
} // namespace

namespace CustomTranslation
{
bool IsSupportedEndpoint(const std::string &endpoint)
{
    if (endpoint.empty() || endpoint.size() > 2048 || endpoint.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        return false;
    return endpoint.rfind("https://", 0) == 0 || endpoint.rfind("http://", 0) == 0;
}

std::string ParseTranslationResponse(const std::string &response)
{
    try
    {
        const auto root = nlohmann::json::parse(response);
        const auto code = root.find("code");
        if (code != root.end())
        {
            if (code->is_number_integer() && code->get<int>() != 200)
                return {};
            if (code->is_string() && code->get<std::string>() != "200")
                return {};
        }

        for (const char *key : {"data", "translation", "result"})
        {
            const auto found = root.find(key);
            if (found == root.end())
                continue;
            const std::string translated = StringFromValue(*found);
            if (!translated.empty())
                return translated;
            if (found->is_array() && !found->empty())
            {
                const std::string first = StringFromValue(found->front());
                if (!first.empty())
                    return first;
            }
        }

        const auto translations = root.find("translations");
        if (translations != root.end() && translations->is_array() && !translations->empty())
            return StringFromValue(translations->front());
    }
    catch (...)
    {
    }
    return {};
}

std::vector<std::string> TextTranslateBatch(const Config &config, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target)
{
    std::vector<std::string> results(texts.size());
    if (texts.empty() || !IsSupportedEndpoint(config.endpoint) || source.empty() || target.empty())
        return results;

    CURL *curl = curl_easy_init();
    if (!curl)
        return results;

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    std::string authorization;
    if (!config.api_key.empty())
    {
        authorization = "Authorization: Bearer " + config.api_key;
        headers = curl_slist_append(headers, authorization.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, config.endpoint.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < texts.size(); ++i)
    {
        if (std::chrono::steady_clock::now() - started >= kBatchBudget)
            break;
        const nlohmann::json body = {
            {"text", texts[i]}, {"source_lang", UppercaseLanguage(source)}, {"target_lang", UppercaseLanguage(target)}};
        const std::string payload = body.dump();
        ResponseBuffer response;
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        const CURLcode performed = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (performed == CURLE_OK && !response.overflow && status >= 200 && status < 300)
            results[i] = ParseTranslationResponse(response.text);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return results;
}
} // namespace CustomTranslation
