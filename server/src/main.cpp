#include "ipc/event_listener.h"
#include "log/fanylog.h"
#include "window/ime_windows.h"
#include <commctrl.h>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include "ipc/ipc.h"
#include "config/ime_config.h"
#include "clipboard/clipboard_history.h"
#include <windows.h>
#include <fmt/xchar.h>
#include <spdlog/spdlog.h>
#include "cloud/cloud_ime.h"
#include "cloud/cloud_translation.h"
#include "ai/ai_assistant.h"
#include "english/english_ime.h"
#include "emoji/emoji_ime.h"
#include "kaomoji/kaomoji_ime.h"
#include "utils/common_utils.h"
#include "utils/single_instance.h"
#include "session/session_factory.h"
#include "webview2/windows_webview2.h"
#include "voice-input/voice_input_service.h"

namespace
{
void StartWatchdogIfNeeded(const char *command_line)
{
    // The Watchdog passes this marker when it owns the Server lifecycle.
    // A Server revived directly by TSF has no marker and restores the
    // Watchdog, preserving crash recovery after the six-keystroke fallback.
    if (command_line && std::strstr(command_line, "--watchdog-managed"))
        return;

    wchar_t server_path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, server_path, MAX_PATH))
        return;
    std::wstring watchdog_path(server_path);
    const size_t separator = watchdog_path.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
        return;
    watchdog_path.replace(separator + 1, std::wstring::npos, L"MetasequoiaImeWatchdog.exe");
    if (GetFileAttributesW(watchdog_path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    std::wstring command = L"\"" + watchdog_path + L"\"";
    STARTUPINFOW startup_info{sizeof(startup_info)};
    PROCESS_INFORMATION process_info{};
    if (CreateProcessW(watchdog_path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup_info, &process_info))
    {
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
    }
}

bool IsPipeProbe(const char *command_line)
{
    return command_line && std::strstr(command_line, "--pipe-probe");
}

const char *SchemeTypeToString(SchemeType scheme_type)
{
    switch (scheme_type)
    {
    case SchemeType::Quanpin:
        return "quanpin";
    case SchemeType::Shuangpin:
        return "shuangpin";
    case SchemeType::Wubi:
        return "wubi";
    case SchemeType::JapaneseRomaji:
        return "japanese-romaji";
    default:
        return "unknown";
    }
}

} // namespace

int CALLBACK WinMain(_In_ HINSTANCE hInstance, _In_ HINSTANCE /*hPrevInstance*/, _In_ LPSTR lpCmdLine,
                     _In_ int /*nCmdShow*/)
{
    const bool pipe_probe = IsPipeProbe(lpCmdLine);
    CommonUtils::SingleInstanceGuard single_instance(L"Local\\MetasequoiaImeServer_SingleInstance");
    if (!single_instance.is_valid())
    {
        (void)0;
        return 0;
    }
    if (single_instance.already_running())
    {
        (void)0;
        return 0;
    }

    if (!pipe_probe)
        StartWatchdogIfNeeded(lpCmdLine);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult))
    {
        (void)0;
        return 1;
    }

    // #ifdef FANY_DEBUG
    // Initialize for logging
    InitializeSpdLog();
    // #endif

    // Initialize config
    InitImeConfig();
    VoiceInput::Initialize();
    Global::candidate_ui.page_size = GetConfiguredCandidatePageSize();

    ::InitIpc();
    ::InitNamedPipe();
    g_inputSession = CreateInputSessionFromConfig();
    const std::string configured_backend = DescribeConfiguredInputSessionBackendFromConfig();
    const std::string effective_backend = DescribeEffectiveInputSessionBackendFromConfig();
    const std::string session_summary =
        fmt::format("Input session backend configured={}, effective={}, scheme={}",
                    configured_backend.empty() ? "legacy" : configured_backend, effective_backend,
                    SchemeTypeToString(g_inputSession->current_scheme_type()));
    (void)0;
#ifdef FANY_DEBUG
    (void)0;
#endif

    if (!pipe_probe)
    {
        RegisterCandidateWindowMessage();

        WNDCLASSEX wcex;
        RegisterIMEWindowsClass(wcex, hInstance);
    }

    //
    // Pipe
    //
    /* Named Pipe for IPC between tsf and server */
    std::thread pipe_worker(FanyNamedPipe::WorkerThread);
    std::thread pipe_listener(FanyNamedPipe::EventListenerLoopThread);
    /* Named Pipe for IPC, used to pass data to TSF */
    std::thread to_tsf_pipe_listener(FanyNamedPipe::ToTsfPipeEventListenerLoopThread);
    ::mainPipeThread = pipe_listener.native_handle();
    ::toTsfPipeThread = to_tsf_pipe_listener.native_handle();
    /* To Tsf Worker Thread Named Pipe for IPC, used to pass data to TSF */
    std::thread to_tsf_worker_thread_pipe_listener(FanyNamedPipe::ToTsfWorkerThreadPipeEventListenerLoopThread);
    ::toTsfWorkerThreadPipeThread = to_tsf_worker_thread_pipe_listener.native_handle();
    /* Aux Named Pipe: session-less UI notices (e.g. langbar right-click menu) */
    std::thread aux_pipe_listener(FanyNamedPipe::AuxPipeEventListenerLoopThread);
    /* Short-lived TSF diagnostic batches; isolated from all control pipes. */
    std::thread tsf_diagnostic_pipe_listener(FanyNamedPipe::TsfDiagnosticPipeEventListenerLoopThread);
    /* Aggregated input statistics from TSF; own pipe so its switch stays independent. */
    std::thread stats_pipe_listener(FanyNamedPipe::StatsPipeEventListenerLoopThread);

    if (pipe_probe)
    {
        // The release runner is a Windows service without an interactive
        // window station. The pipe probe still exercises the real Server and
        // all IPC worker threads, but must not create candidate/WebView2
        // windows or start UI-bound background services.
        Sleep(INFINITE);
    }

    CloudIme::Start([](const std::string &candidate, const std::string &pinyin, uint64_t generation) {
        FanyNamedPipe::EnqueueCloudCandidate(candidate, pinyin, generation);
    });
    AiAssistant::Start([](const std::string &candidate, const std::string &identity, uint64_t generation) {
        FanyNamedPipe::EnqueueAiCandidate(candidate, identity, generation);
    });
    EnglishIme::Start(
        CommonUtils::get_ime_data_path() + "\\english.db",
        [](std::vector<WordItem> candidates, const std::string &input, uint64_t generation) {
            FanyNamedPipe::EnqueueEnglishCandidates(std::move(candidates), input, generation);
        },
        [](std::vector<EnglishIme::TranslationResult> results, uint64_t generation) {
            FanyNamedPipe::EnqueueCandidateTranslations(std::move(results), generation);
        });
    CloudTranslation::Start(CommonUtils::get_ime_data_path() + "\\english.db",
                            [](std::vector<EnglishIme::TranslationResult> results, uint64_t generation) {
                                FanyNamedPipe::EnqueueCandidateTranslations(std::move(results), generation, true);
                            });
    EmojiIme::Start(CommonUtils::get_ime_data_path() + "\\others.db",
                    [](std::vector<WordItem> candidates, const std::string &input, uint64_t generation) {
                        FanyNamedPipe::EnqueueEmojiCandidates(std::move(candidates), input, generation);
                    });
    KaomojiIme::Start(CommonUtils::get_ime_data_path() + "\\others.db",
                      [](std::vector<WordItem> candidates, const std::string &input, uint64_t generation) {
                          FanyNamedPipe::EnqueueKaomojiCandidates(std::move(candidates), input, generation);
                      });

    int ret = CreateCandidateWindow(hInstance);

    FanyNamedPipe::RegisterStatusSnapshotWindow(nullptr);
    ShutdownWebviews();

    EnglishIme::Stop();
    CloudTranslation::Stop();
    EmojiIme::Stop();
    KaomojiIme::Stop();
    AiAssistant::Stop();
    CloudIme::Stop();
    ClipboardMonitor::Stop();
    VoiceInput::Shutdown();

    pipe_running = false;
    pipe_queueCv.notify_one();
    pipe_worker.join();
    pipe_listener.join();
    // To Tsf Pipe
    to_tsf_pipe_listener.join();
    to_tsf_worker_thread_pipe_listener.join();
    aux_pipe_listener.join();
    tsf_diagnostic_pipe_listener.join();
    stats_pipe_listener.join();

    ::CloseIpc();
    CoUninitialize();
    return ret;
}
