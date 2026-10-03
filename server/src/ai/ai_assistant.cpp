#include "ai_assistant.h"
#include "ai_assistant_cache.h"
#include "ai_assistant_cache_key.h"

#include "utils/network_proxy.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <fmt/format.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
constexpr auto kIdleDelay = std::chrono::milliseconds(650);
std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_worker;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_generation{0};
AiAssistant::Request g_latest;
std::chrono::steady_clock::time_point g_last_input;
AiAssistant::ApplyCallback g_callback;
AiAssistant::detail::SuggestionCache g_candidate_cache;

size_t WriteResponse(char *data, size_t size, size_t count, void *user)
{
    static_cast<std::string *>(user)->append(data, size * count);
    return size * count;
}

std::string Fetch(const AiAssistant::Request &request, uint64_t generation)
{
    const auto &config = request.config;
    if (!config.enabled || config.token.empty() || config.endpoint.empty() || config.model.empty() ||
        request.pinyin_segments.empty() || g_generation.load() != generation)
    {
        (void)0;
        return {};
    }

    nlohmann::json input = {{"segmented_pinyin", request.pinyin_segments},
                            {"context", request.context},
                            {"candidate_limit", config.candidate_limit}};
    nlohmann::json body = {
        {"model", config.model},
        {"stream", false},
        {"temperature", 0.2},
        {"max_tokens", 512},
        {"response_format", {{"type", "json_object"}}},
        {"messages",
         {{{"role", "system"}, {"content", config.prompt}}, {{"role", "user"}, {"content", input.dump()}}}}};
    if (config.provider == "deepseek")
    {
        // DeepSeek thinking can add hundreds of reasoning tokens and noticeably delay
        // an IME suggestion. Keep custom OpenAI-compatible providers untouched.
        body["thinking"] = {{"type", "disabled"}};
    }

    // Deliberately exclude API token and system prompt from logs.
    (void)0;

    CURL *curl = curl_easy_init();
    if (!curl)
        return {};
    std::string response;
    const std::string authorization = "Authorization: Bearer " + config.token;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, authorization.c_str());
    const std::string payload = body.dump();
    curl_easy_setopt(curl, CURLOPT_URL, config.endpoint.c_str());
    NetworkProxy::ApplyToCurl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    (void)0;
    if (result != CURLE_OK || status < 200 || status >= 300)
    {
        (void)0;
        return {};
    }
    if (g_generation.load() != generation)
    {
        (void)0;
        return {};
    }

    try
    {
        const auto outer = nlohmann::json::parse(response);
        const std::string content = outer.at("choices").at(0).at("message").at("content").get<std::string>();
        const auto result_json = nlohmann::json::parse(content);
        const auto &candidates = result_json.at("candidates");
        if (!candidates.is_array() || candidates.empty())
        {
            (void)0;
            return {};
        }
        const std::string candidate = candidates.at(0).value("text", std::string{});
        (void)0;
        return candidate;
    }
    catch (const std::exception &)
    {
        return {};
    }
}

void WorkerLoop()
{
    uint64_t observed = g_generation.load();
    while (g_running)
    {
        std::unique_lock lock(g_mutex);
        g_cv.wait(lock, [&] { return !g_running || g_generation.load() != observed; });
        if (!g_running)
            break;
        observed = g_generation.load();
        auto request = g_latest;
        const auto cached = g_candidate_cache.Find(AiAssistant::detail::BuildSuggestionCacheKey(request));
        if (!request.identity.empty() && cached.has_value())
        {
            const uint64_t cached_generation = observed;
            lock.unlock();
            (void)0;
            if (g_running && g_generation.load() == cached_generation && g_callback)
                g_callback(*cached, request.identity, cached_generation);
            continue;
        }
        auto target = g_last_input + kIdleDelay;
        while (g_running &&
               g_cv.wait_until(lock, target, [&] { return !g_running || g_generation.load() != observed; }))
        {
            if (!g_running)
                return;
            observed = g_generation.load();
            request = g_latest;
            target = g_last_input + kIdleDelay;
        }
        lock.unlock();
        if (request.identity.empty())
            continue;
        const std::string candidate = Fetch(request, observed);
        if (!candidate.empty() && g_generation.load() == observed && g_callback)
        {
            {
                std::lock_guard cache_lock(g_mutex);
                g_candidate_cache.Store(AiAssistant::detail::BuildSuggestionCacheKey(request), candidate);
            }
            (void)0;
            g_callback(candidate, request.identity, observed);
        }
    }
}
} // namespace

namespace AiAssistant
{
void Start(ApplyCallback callback)
{
    if (g_running)
        return;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_callback = std::move(callback);
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
    {
        std::lock_guard lock(g_mutex);
        g_candidate_cache.Clear();
    }
    curl_global_cleanup();
}
void OnInputChanged(Request request)
{
    {
        std::lock_guard lock(g_mutex);
        g_latest = std::move(request);
        g_last_input = std::chrono::steady_clock::now();
        ++g_generation;
        (void)0;
    }
    g_cv.notify_one();
}
void Clear()
{
    OnInputChanged({});
}
} // namespace AiAssistant
