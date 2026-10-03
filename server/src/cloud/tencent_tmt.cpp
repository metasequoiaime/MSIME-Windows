#include "tencent_tmt.h"

#include "translation_gloss.h"
#include <Windows.h>
#include <bcrypt.h>
#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ctime>
#include <string_view>

#pragma comment(lib, "bcrypt.lib")

namespace
{
constexpr const char *kHost = "tmt.tencentcloudapi.com";
constexpr const char *kUrl = "https://tmt.tencentcloudapi.com";
constexpr const char *kService = "tmt";
constexpr const char *kAction = "TextTranslateBatch";
constexpr const char *kVersion = "2018-03-21";
constexpr const char *kAlgorithm = "TC3-HMAC-SHA256";
constexpr long kTimeoutMs = 2000;

std::string HexEncode(const unsigned char *data, size_t size)
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

bool Sha256(std::string_view data, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    const NTSTATUS created = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    const NTSTATUS hashed = created == 0
                                ? BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                                                 static_cast<ULONG>(data.size()), 0)
                                : created;
    const NTSTATUS finished = hashed == 0 ? BCryptFinishHash(hash, out, 32, 0) : hashed;
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return finished == 0;
}

bool HmacSha256(std::string_view key, std::string_view data, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return false;
    const NTSTATUS created =
        BCryptCreateHash(alg, &hash, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(key.data())),
                         static_cast<ULONG>(key.size()), 0);
    const NTSTATUS hashed = created == 0
                                ? BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                                                 static_cast<ULONG>(data.size()), 0)
                                : created;
    const NTSTATUS finished = hashed == 0 ? BCryptFinishHash(hash, out, 32, 0) : hashed;
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    return finished == 0;
}

std::string Sha256Hex(std::string_view data)
{
    unsigned char digest[32]{};
    return Sha256(data, digest) ? HexEncode(digest, 32) : std::string{};
}

std::string HmacSha256Raw(std::string_view key, std::string_view data)
{
    unsigned char digest[32]{};
    if (!HmacSha256(key, data, digest))
        return {};
    return std::string(reinterpret_cast<const char *>(digest), 32);
}

std::string UtcDate(time_t timestamp)
{
    std::tm utc{};
    gmtime_s(&utc, &timestamp);
    char buf[16]{};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &utc);
    return buf;
}

size_t WriteResponse(char *data, size_t size, size_t count, void *user)
{
    static_cast<std::string *>(user)->append(data, size * count);
    return size * count;
}

std::string Tc3Authorization(const TencentTmt::Credentials &credentials, const std::string &payload, time_t timestamp)
{
    const std::string date = UtcDate(timestamp);
    const std::string canonical_request =
        std::string("POST\n/\n\n") + "content-type:application/json; charset=utf-8\n" + "host:" + kHost + "\n" +
        "x-tc-action:texttranslatebatch\n\n" + "content-type;host;x-tc-action\n" + Sha256Hex(payload);
    const std::string credential_scope = date + "/" + kService + "/tc3_request";
    const std::string string_to_sign = std::string(kAlgorithm) + "\n" + std::to_string(timestamp) + "\n" +
                                       credential_scope + "\n" + Sha256Hex(canonical_request);
    const std::string secret_date = HmacSha256Raw(std::string("TC3") + credentials.secret_key, date);
    const std::string secret_service = HmacSha256Raw(secret_date, kService);
    const std::string secret_signing = HmacSha256Raw(secret_service, "tc3_request");
    unsigned char signature[32]{};
    if (secret_date.empty() || secret_service.empty() || secret_signing.empty() ||
        !HmacSha256(secret_signing, string_to_sign, signature))
        return {};
    return std::string(kAlgorithm) + " Credential=" + credentials.secret_id + "/" + credential_scope +
           ", SignedHeaders=content-type;host;x-tc-action, Signature=" + HexEncode(signature, 32);
}
} // namespace

namespace TencentTmt
{
std::vector<std::string> TextTranslateBatch(const Credentials &credentials, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target)
{
    std::vector<std::string> results(texts.size());
    if (texts.empty() || !CloudTranslation::IsUsableSecret(credentials.secret_id) ||
        !CloudTranslation::IsUsableSecret(credentials.secret_key) || source.empty() || target.empty())
        return results;

    nlohmann::json body = {{"Source", source}, {"Target", target}, {"ProjectId", 0}, {"SourceTextList", texts}};
    const std::string payload = body.dump();
    const time_t timestamp = std::time(nullptr);
    const std::string authorization = Tc3Authorization(credentials, payload, timestamp);
    if (authorization.empty())
        return results;

    CURL *curl = curl_easy_init();
    if (!curl)
        return results;

    std::string response;
    curl_slist *headers = nullptr;
    const std::string content_type = "Content-Type: application/json; charset=utf-8";
    const std::string host = std::string("Host: ") + kHost;
    const std::string action = std::string("X-TC-Action: ") + kAction;
    const std::string ts = std::string("X-TC-Timestamp: ") + std::to_string(timestamp);
    const std::string version = std::string("X-TC-Version: ") + kVersion;
    const std::string region =
        std::string("X-TC-Region: ") + (credentials.region.empty() ? "ap-guangzhou" : credentials.region);
    const std::string auth = "Authorization: " + authorization;
    headers = curl_slist_append(headers, content_type.c_str());
    headers = curl_slist_append(headers, host.c_str());
    headers = curl_slist_append(headers, action.c_str());
    headers = curl_slist_append(headers, ts.c_str());
    headers = curl_slist_append(headers, version.c_str());
    headers = curl_slist_append(headers, region.c_str());
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, kUrl);
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode performed = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (performed != CURLE_OK || status < 200 || status >= 300)
        return results;

    try
    {
        const auto root = nlohmann::json::parse(response);
        const auto &list = root.at("Response").at("TargetTextList");
        if (!list.is_array())
            return results;
        const size_t n = (std::min)(list.size(), results.size());
        for (size_t i = 0; i < n; ++i)
        {
            if (list[i].is_string())
                results[i] = list[i].get<std::string>();
        }
    }
    catch (...)
    {
        return results;
    }
    return results;
}
} // namespace TencentTmt
