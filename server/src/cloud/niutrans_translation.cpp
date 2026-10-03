#include "niutrans_translation.h"

#include "translation_gloss.h"
#include <Windows.h>
#include <bcrypt.h>
#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <string_view>

#pragma comment(lib, "bcrypt.lib")

namespace
{
constexpr const char *kUrl = "https://api.niutrans.com/v2/text/translate";
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

std::string HexEncodeLower(const unsigned char *data, size_t size)
{
    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (size_t i = 0; i < size; ++i)
    {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

std::string Md5Hex(std::string_view data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0)
        return {};
    const NTSTATUS created = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    const NTSTATUS hashed = created == 0
                                ? BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                                                 static_cast<ULONG>(data.size()), 0)
                                : created;
    unsigned char digest[16]{};
    const NTSTATUS finished = hashed == 0 ? BCryptFinishHash(hash, digest, sizeof(digest), 0) : hashed;
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return finished == 0 ? HexEncodeLower(digest, sizeof(digest)) : std::string{};
}

std::string UrlEncode(CURL *curl, const std::string &value)
{
    char *escaped = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    if (!escaped)
        return {};
    std::string out(escaped);
    curl_free(escaped);
    return out;
}

std::string MillisecondTimestamp()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}
} // namespace

namespace NiuTransTranslation
{
bool IsUsableConfig(const Config &config)
{
    return CloudTranslation::IsUsableSecret(config.app_id) && CloudTranslation::IsUsableSecret(config.apikey);
}

std::string BuildAuthString(const Config &config, const std::string &from, const std::string &to,
                            const std::string &timestamp, const std::string &src_text)
{
    // NiuTrans sorts the request parameters plus apikey by key name and joins
    // them as "key=value&...". For the fixed field set the ASCII order is:
    // apikey, appId, from, srcText, timestamp, to.
    const std::string param_str = "apikey=" + config.apikey + "&appId=" + config.app_id + "&from=" + from +
                                  "&srcText=" + src_text + "&timestamp=" + timestamp + "&to=" + to;
    return Md5Hex(param_str);
}

std::string ParseTranslationResponse(const std::string &response)
{
    try
    {
        const auto root = nlohmann::json::parse(response);
        const auto tgt = root.find("tgtText");
        if (tgt != root.end() && tgt->is_string())
            return tgt->get<std::string>();
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
    if (texts.empty() || !IsUsableConfig(config) || source.empty() || target.empty())
        return results;

    CURL *curl = curl_easy_init();
    if (!curl)
        return results;

    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded; charset=utf-8");

    curl_easy_setopt(curl, CURLOPT_URL, kUrl);
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
        const std::string timestamp = MillisecondTimestamp();
        // authStr is computed over the raw (unencoded) parameter values; the POST
        // body sends the same values url-encoded.
        const std::string auth = BuildAuthString(config, source, target, timestamp, texts[i]);
        if (auth.empty())
            continue;
        const std::string payload = "from=" + UrlEncode(curl, source) + "&to=" + UrlEncode(curl, target) +
                                    "&appId=" + UrlEncode(curl, config.app_id) + "&timestamp=" + timestamp +
                                    "&srcText=" + UrlEncode(curl, texts[i]) + "&authStr=" + auth;
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
} // namespace NiuTransTranslation
