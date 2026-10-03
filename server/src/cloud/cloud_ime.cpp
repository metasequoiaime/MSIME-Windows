#include "cloud_ime.h"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <winhttp.h>
#include <fmt/xchar.h>
#include "utils/common_utils.h"
#include "utils/network_proxy.h"

#pragma comment(lib, "winhttp.lib")

namespace
{
constexpr auto kIdleDelay = std::chrono::milliseconds(500);
constexpr int kTimeoutMs = 2000;

std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_generation{0};
std::string g_latest_pinyin;
bool g_latest_japanese = false;
std::chrono::steady_clock::time_point g_last_input_time;
CloudIme::ApplyCallback g_apply_callback;

std::string UrlEncode(const std::string &input)
{
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char c : input)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
        {
            out.push_back(static_cast<char>(c));
        }
        else
        {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

bool HttpGet(const std::wstring &host, const std::wstring &path, std::string &response_out)
{
    HINTERNET hSession = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeServer/1.0");
    if (!hSession)
        return false;

    WinHttpSetTimeouts(hSession, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    BOOL send_ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!send_ok)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    BOOL recv_ok = WinHttpReceiveResponse(hRequest, NULL);
    if (!recv_ok)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::string response;
    DWORD bytes_available = 0;
    do
    {
        if (!WinHttpQueryDataAvailable(hRequest, &bytes_available))
            break;
        if (bytes_available == 0)
            break;
        std::string buffer(bytes_available, '\0');
        DWORD bytes_read = 0;
        if (!WinHttpReadData(hRequest, buffer.data(), bytes_available, &bytes_read))
            break;
        response.append(buffer.data(), bytes_read);
    } while (bytes_available > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    response_out = std::move(response);
    return !response_out.empty();
}

std::string ParseCandidate(const std::string &response)
{
    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(response);
    }
    catch (...)
    {
        return "";
    }

    if (!root.is_array() || root.size() < 2)
        return "";
    if (!root[0].is_string() || root[0].get<std::string>() != "SUCCESS")
        return "";
    if (!root[1].is_array() || root[1].empty())
        return "";

    const auto &first_item = root[1][0];
    if (!first_item.is_array() || first_item.size() < 2 || !first_item[1].is_array())
        return "";

    const auto &cand_list = first_item[1];
    if (cand_list.empty() || !cand_list[0].is_string())
        return "";

    return cand_list[0].get<std::string>();
}

// TODO: 可以添加其他的云服务接口，甚至是自己搭建的接口，暂时先只支持 Google 的接口，Google 带善人
std::string FetchCloudCandidate(const std::string &pinyin, bool japanese, uint64_t generation)
{
    if (pinyin.empty())
        return "";

    if (!g_running || g_generation.load() != generation)
        return "";

    std::string encoded = UrlEncode(pinyin);
    const char *input_tool = japanese ? "ja-t-i0-und" : "zh-t-i0-pinyin";
    std::string path_utf8 = "/request?text=" + encoded + "&itc=" + input_tool + "&num=1&ie=utf-8&oe=utf-8";
    std::wstring path(path_utf8.begin(), path_utf8.end());

    std::string response;
    if (!HttpGet(L"inputtools.google.com", path, response))
    {
        (void)0;
        return "";
    }

    if (!g_running || g_generation.load() != generation)
        return "";

    // (void)0;
    std::string candidate = ParseCandidate(response);
    // (void)0;
    return candidate;
}

void WorkerLoop()
{
    uint64_t observed_generation = g_generation.load();
    while (g_running)
    {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [&] { return !g_running || g_generation.load() != observed_generation; });
        if (!g_running)
            break;

        observed_generation = g_generation.load();
        std::string pinyin = g_latest_pinyin;
        bool japanese = g_latest_japanese;
        auto target_time = g_last_input_time + kIdleDelay;

        while (g_running)
        {
            if (g_cv.wait_until(lock, target_time,
                                [&] { return !g_running || g_generation.load() != observed_generation; }))
            {
                if (!g_running)
                    return;
                observed_generation = g_generation.load();
                pinyin = g_latest_pinyin;
                japanese = g_latest_japanese;
                target_time = g_last_input_time + kIdleDelay;
                continue;
            }
            break; // debounce timeout
        }

        lock.unlock();

        if (!g_running)
            break;

        if (pinyin.empty())
            continue;

        uint64_t request_generation = observed_generation;
        std::string candidate = FetchCloudCandidate(pinyin, japanese, request_generation);
        if (candidate.empty())
            continue;
        if (g_generation.load() != request_generation)
            continue;

        if (g_apply_callback)
        {
            g_apply_callback(candidate, pinyin, request_generation);
        }
    }
}
} // namespace

namespace CloudIme
{
void Start(ApplyCallback apply_callback)
{
    if (g_running)
        return;
    g_apply_callback = std::move(apply_callback);
    g_running = true;
    g_worker = std::thread(WorkerLoop);
}

void Stop()
{
    if (!g_running)
        return;
    g_running = false;
    g_cv.notify_all();
    if (g_worker.joinable())
        g_worker.join();
}

void OnInputChanged(const std::string &pure_pinyin, bool japanese)
{
    {
        std::lock_guard lock(g_mutex);
        g_latest_pinyin = pure_pinyin;
        g_latest_japanese = japanese;
        g_last_input_time = std::chrono::steady_clock::now();
        g_generation.fetch_add(1);
    }
    g_cv.notify_one();
}

void Clear()
{
    OnInputChanged("", false);
}
} // namespace CloudIme
