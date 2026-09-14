#include <msime/voice/audio_capture.h>
#include <msime/voice/provider_protocol.h>
#include <msime/voice/stt_service.h>
#include <limits>

#include "voice_input_service.h"
#include "config/ime_config.h"
#include "doubao_asr_client.h"
#include "voice_providers.h"
#include "voice_batch_protocol.h"
#include "ipc/ipc.h"
#include "system_audio_muter.h"
#include "utils/common_utils.h"
#include "wave_overlay.h"
#include "cue_player.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr unsigned kSampleRate = 16000;
constexpr long kPolishTimeoutMs = 3000L;
metasequoia::voice::AudioCapture g_capture;
std::mutex g_mutex;
std::vector<float> g_samples;
std::unique_ptr<DoubaoAsrClient> g_doubao_asr;
std::atomic<std::size_t> g_recorded_frames{0};
std::atomic<std::uint64_t> g_voice_session{0};
std::atomic<std::uint64_t> g_dismissed_overlay_session{0};
std::vector<std::future<void>> g_network_tasks;
std::mutex g_send_mutex;
std::atomic<bool> g_stream_inline_this_session{false};
std::wstring g_last_inline_preedit;
std::thread g_control_thread;
std::mutex g_control_mutex;
std::condition_variable g_control_cv;
enum class ControlCommand
{
    Start,
    Stop,
    Toggle,
    Lock,
    Cancel,
    Exit
};
std::deque<ControlCommand> g_control_commands;
std::atomic<bool> g_recording{false};
std::atomic<bool> g_starting{false};
std::atomic<bool> g_abort_start{false};
std::atomic<bool> g_muted_system_audio{false};
std::atomic<bool> g_ime_active{false};
bool g_initialized = false;
WaveOverlay g_overlay;
CuePlayer g_cue_player;
HHOOK g_keyboard_hook = nullptr;
HWND g_message_window = nullptr;
std::atomic<bool> g_ralt_pressed{false};
std::atomic<bool> g_lctrl_pressed{false};
std::atomic<bool> g_rctrl_pressed{false};
std::atomic<bool> g_lwin_pressed{false};
std::atomic<bool> g_rwin_pressed{false};
std::atomic<bool> g_f9_pressed{false};
std::atomic<bool> g_ctrl_f9_consumed{false};
std::atomic<bool> g_ralt_lock_mode{false};
std::atomic<bool> g_suppress_ralt_until_up{false};
std::atomic<bool> g_suppress_win_until_up{false};
enum class HoldShortcut
{
    None,
    RAlt,
    CtrlWin,
    RCtrlRAlt
};
std::atomic<HoldShortcut> g_active_hold_shortcut{HoldShortcut::None};
constexpr UINT kStartRecordingMessage = WM_APP + 181;
constexpr UINT kStopRecordingMessage = WM_APP + 182;
constexpr UINT kToggleRecordingMessage = WM_APP + 183;
constexpr UINT kLockRecordingMessage = WM_APP + 184;
constexpr UINT kCancelRecordingMessage = WM_APP + 185;
constexpr wchar_t kMessageWindowClass[] = L"MetasequoiaImeVoiceInputMessageWindow";

std::wstring ResolveCuePath(const wchar_t *filename)
{
    const std::wstring ime_data = CommonUtils::get_ime_data_path_w();
    if (!ime_data.empty())
    {
        const std::wstring installed = ime_data + L"\\assets\\audios\\" + filename;
        if (GetFileAttributesW(installed.c_str()) != INVALID_FILE_ATTRIBUTES)
            return installed;
    }
    wchar_t executable[MAX_PATH]{};
    const DWORD executable_size = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!executable_size || executable_size >= MAX_PATH)
        return {};
    std::wstring directory(executable, executable_size);
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        directory.resize(slash);
    return directory + L"\\assets\\audios\\" + filename;
}

bool StartRecording();
void StopRecording();
void CancelRecording();

void MuteSystemAudioIfEnabled(const VoiceInputConfig &config)
{
    if (!config.mute_system_audio)
        return;
    VoiceInput::MuteOtherSystemAudio();
    g_muted_system_audio = true;
}

void RestoreSystemAudioIfMuted()
{
    if (!g_muted_system_audio.exchange(false))
        return;
    VoiceInput::RestoreOtherSystemAudio();
}

void EnqueueControlCommand(ControlCommand command)
{
    {
        std::lock_guard<std::mutex> lock(g_control_mutex);
        if (command == ControlCommand::Start)
        {
            if (!g_control_commands.empty() && g_control_commands.back() == ControlCommand::Start)
                return;
        }
        else if (command == ControlCommand::Stop || command == ControlCommand::Cancel)
        {
            if (g_starting)
                g_abort_start = true;
            if (!g_control_commands.empty() && g_control_commands.back() == ControlCommand::Start)
            {
                g_control_commands.pop_back();
                if (!g_recording && !g_starting)
                {
                    g_abort_start = false;
                    return;
                }
            }
            if (!g_recording && !g_starting)
                return;
            if (!g_control_commands.empty() && (g_control_commands.back() == ControlCommand::Stop ||
                                                g_control_commands.back() == ControlCommand::Cancel))
                return;
        }
        g_control_commands.push_back(command);
    }
    g_control_cv.notify_one();
}

void ControlLoop()
{
    for (;;)
    {
        ControlCommand command;
        {
            std::unique_lock<std::mutex> lock(g_control_mutex);
            g_control_cv.wait(lock, [] { return !g_control_commands.empty(); });
            command = g_control_commands.front();
            g_control_commands.pop_front();
        }
        if (command == ControlCommand::Exit)
        {
            g_cue_player.shutdown();
            return;
        }
        if (command == ControlCommand::Start)
            StartRecording();
        else if (command == ControlCommand::Stop)
            StopRecording();
        else if (command == ControlCommand::Toggle)
        {
            if (g_recording)
                StopRecording();
            else
                StartRecording();
        }
        else if (command == ControlCommand::Lock)
        {
            g_ralt_lock_mode = true;
            g_overlay.set_actions_visible(true);
        }
        else if (command == ControlCommand::Cancel)
            CancelRecording();
    }
}

bool IsCtrlPressed()
{
    return g_lctrl_pressed || g_rctrl_pressed;
}
bool IsWinPressed()
{
    return g_lwin_pressed || g_rwin_pressed;
}

bool ShouldInstallKeyboardHook(const VoiceInputConfig &config)
{
    return config.enabled &&
           (config.hotkey_ralt || config.hotkey_ctrl_f9 || config.hotkey_ctrl_win || config.hotkey_rctrl_ralt);
}

void ResetKeyboardShortcutState()
{
    g_ralt_pressed = false;
    g_lctrl_pressed = false;
    g_rctrl_pressed = false;
    g_lwin_pressed = false;
    g_rwin_pressed = false;
    g_f9_pressed = false;
    g_ctrl_f9_consumed = false;
    g_suppress_ralt_until_up = false;
    g_suppress_win_until_up = false;
    g_active_hold_shortcut = HoldShortcut::None;
}

bool IsHoldShortcutPressed(HoldShortcut shortcut)
{
    switch (shortcut)
    {
    case HoldShortcut::RAlt:
        return g_ralt_pressed;
    case HoldShortcut::CtrlWin:
        return IsCtrlPressed() && IsWinPressed();
    case HoldShortcut::RCtrlRAlt:
        return g_rctrl_pressed && g_ralt_pressed;
    default:
        return false;
    }
}

void ActivateHoldShortcut(HoldShortcut shortcut)
{
    g_active_hold_shortcut = shortcut;
    if (shortcut == HoldShortcut::RAlt || shortcut == HoldShortcut::RCtrlRAlt)
        g_suppress_ralt_until_up = true;
    else if (shortcut == HoldShortcut::CtrlWin)
        g_suppress_win_until_up = true;
    PostMessageW(g_message_window, g_ralt_lock_mode ? kStopRecordingMessage : kStartRecordingMessage, 0, 0);
}

void ForceReleaseRAlt()
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_RMENU;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

LRESULT CALLBACK VoiceMessageWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case kStartRecordingMessage:
        EnqueueControlCommand(ControlCommand::Start);
        return 0;
    case kStopRecordingMessage:
        EnqueueControlCommand(ControlCommand::Stop);
        return 0;
    case kToggleRecordingMessage:
        EnqueueControlCommand(ControlCommand::Toggle);
        return 0;
    case kLockRecordingMessage:
        EnqueueControlCommand(ControlCommand::Lock);
        return 0;
    case kCancelRecordingMessage:
        EnqueueControlCommand(ControlCommand::Cancel);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code != HC_ACTION)
        return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
    const auto *key = reinterpret_cast<KBDLLHOOKSTRUCT *>(lparam);
    if (!key)
        return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
    const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
    const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;

    const VoiceInputConfig config = GetConfiguredVoiceInput();
    const HoldShortcut active_before = g_active_hold_shortcut.load();

    if (key->vkCode == VK_LCONTROL || key->vkCode == VK_RCONTROL)
    {
        auto &state = key->vkCode == VK_LCONTROL ? g_lctrl_pressed : g_rctrl_pressed;
        if (down)
            state = true;
        else if (up)
            state = false;
    }
    else if (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN)
    {
        auto &state = key->vkCode == VK_LWIN ? g_lwin_pressed : g_rwin_pressed;
        if (down)
            state = true;
        else if (up)
            state = false;
    }
    else if (key->vkCode == VK_RMENU)
    {
        if (down)
            g_ralt_pressed = true;
        else if (up)
            g_ralt_pressed = false;
    }

    if (!g_ime_active)
    {
        // A shortcut key-down already consumed while this TIP was active must
        // keep its matching key-up consumed, even if the user switches IMEs.
        const bool suppress_ralt = key->vkCode == VK_RMENU && g_suppress_ralt_until_up;
        const bool suppress_win = (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN) && g_suppress_win_until_up;
        if (up && key->vkCode == VK_RMENU)
            g_suppress_ralt_until_up = false;
        if (up && (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN))
            g_suppress_win_until_up = false;
        if (suppress_ralt || suppress_win)
            return 1;
        return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
    }

    if (key->vkCode == VK_F9 && config.hotkey_ctrl_f9)
    {
        if (down && !g_f9_pressed.exchange(true) && IsCtrlPressed())
        {
            g_ctrl_f9_consumed = true;
            PostMessageW(g_message_window, kToggleRecordingMessage, 0, 0);
            return 1;
        }
        if (up)
        {
            g_f9_pressed = false;
            if (g_ctrl_f9_consumed.exchange(false))
                return 1;
        }
    }
    else if (key->vkCode == VK_F9 && up)
    {
        g_f9_pressed = false;
        g_ctrl_f9_consumed = false;
    }

    if (active_before == HoldShortcut::None && down)
    {
        if (config.hotkey_rctrl_ralt && key->vkCode == VK_RMENU && g_rctrl_pressed)
            ActivateHoldShortcut(HoldShortcut::RCtrlRAlt);
        else if (config.hotkey_ctrl_win && (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN) && IsCtrlPressed())
            ActivateHoldShortcut(HoldShortcut::CtrlWin);
        else if (config.hotkey_ralt && key->vkCode == VK_RMENU)
            ActivateHoldShortcut(HoldShortcut::RAlt);
    }
    else if (active_before != HoldShortcut::None && up && !IsHoldShortcutPressed(active_before))
    {
        g_active_hold_shortcut = HoldShortcut::None;
        if (!g_ralt_lock_mode)
            PostMessageW(g_message_window, kStopRecordingMessage, 0, 0);
    }

    const HoldShortcut active_now = g_active_hold_shortcut.load();
    if (key->vkCode == VK_SPACE && active_now != HoldShortcut::None && config.hotkey_hold_space_lock)
    {
        if (down && !g_ralt_lock_mode)
            PostMessageW(g_message_window, kLockRecordingMessage, 0, 0);
        return 1;
    }
    if (key->vkCode == VK_ESCAPE && g_recording)
    {
        if (down)
            PostMessageW(g_message_window, kCancelRecordingMessage, 0, 0);
        return 1;
    }

    // 组合键按书写顺序触发（先 Ctrl，后 Win/RAlt），只屏蔽第二个键的完整
    // 按下/抬起周期。这样既不会打开开始菜单或触发 Alt 行为，也不会制造粘键。
    const bool suppress_ralt = key->vkCode == VK_RMENU && g_suppress_ralt_until_up;
    const bool suppress_win = (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN) && g_suppress_win_until_up;
    if (up && key->vkCode == VK_RMENU)
        g_suppress_ralt_until_up = false;
    if (up && (key->vkCode == VK_LWIN || key->vkCode == VK_RWIN))
        g_suppress_win_until_up = false;
    if (suppress_ralt || suppress_win)
        return 1;
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
}

size_t WriteResponse(char *data, size_t size, size_t count, void *user)
{
    constexpr size_t limit = 1024 * 1024;
    if (size && count > (std::numeric_limits<size_t>::max)() / size)
        return 0;
    const size_t length = size * count;
    auto &response = *static_cast<std::string *>(user);
    if (length > limit - response.size())
        return 0;
    try
    {
        response.append(data, length);
    }
    catch (...)
    {
        return 0;
    }
    return length;
}

size_t WriteHeader(char *data, size_t size, size_t count, void *user)
{
    const size_t n = size * count;
    std::string line(data, n);
    for (char &ch : line)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    const char prefix[] = "x-siliconcloud-trace-id:";
    const auto pos = line.find(prefix);
    if (pos != std::string::npos)
    {
        std::string value = line.substr(pos + sizeof(prefix) - 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
            value.pop_back();
        *static_cast<std::string *>(user) = std::move(value);
    }
    return n;
}

bool IsSiliconFlowAsr(const VoiceInputConfig &config)
{
    std::string provider = config.asr_provider;
    for (char &ch : provider)
    {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return provider == "siliconflow";
}

struct RecognitionResult
{
    std::string text;
    std::string error;
};

std::string JsonErrorMessage(const std::string &body)
{
    try
    {
        const auto json = nlohmann::json::parse(body);
        if (json.contains("error"))
        {
            const auto &error = json["error"];
            if (error.is_string())
                return error.get<std::string>();
            if (error.is_object())
            {
                if (error.contains("message") && error["message"].is_string())
                    return error["message"].get<std::string>();
            }
        }
        if (json.contains("message") && json["message"].is_string())
        {
            std::string message = json["message"].get<std::string>();
            if (json.contains("code") && !json["code"].is_null())
                message += "（code " + json["code"].dump() + "）";
            if (json.contains("data") && json["data"].is_string() && !json["data"].get<std::string>().empty())
                message += " " + json["data"].get<std::string>();
            return message;
        }
    }
    catch (...)
    {
    }
    if (body.size() > 240)
        return body.substr(0, 240) + "...";
    return body;
}

RecognitionResult Recognize(const std::vector<float> &samples, const VoiceInputConfig &config)
{
    const std::string asr_token = VoiceInput::ResolveAsrToken(config);
    if (asr_token.empty() || config.asr_endpoint.empty())
        return {{}, "ASR Token 或接口地址为空。"};
    const std::string model_name = VoiceInput::ResolveAsrModel(config);
    if (model_name.empty())
        return {{}, "ASR 模型名为空。"};
    const bool siliconflow = IsSiliconFlowAsr(config);
    metasequoia::voice::MultipartRequest payload;
    try
    {
        payload = VoiceInput::BuildBatchTranscription(samples, config);
    }
    catch (const metasequoia::voice::VoiceError &)
    {
        return {{}, "录音数据无效或超过 20 MiB 上传限制。"};
    }
    const int attempts = siliconflow ? 2 : 1;
    std::string response;
    std::string trace_id;
    char curl_error[CURL_ERROR_SIZE] = {};
    CURLcode result = CURLE_OK;
    long http_status = 0;
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        if (attempt > 0)
            Sleep(400);
        response.clear();
        trace_id.clear();
        curl_error[0] = 0;
        CURL *curl = curl_easy_init();
        if (!curl)
            return {{}, "无法初始化网络请求。"};
        curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + asr_token).c_str());
        // Some gateways 500 on libcurl's default Expect: 100-continue.
        headers = curl_slist_append(headers, "Expect:");
        headers = curl_slist_append(headers, ("Content-Type: " + payload.content_type).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, config.asr_endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.body.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &trace_id);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        result = curl_easy_perform(curl);
        http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (result == CURLE_OK && http_status < 500)
            break;
    }
    if (result != CURLE_OK)
    {
        const char *detail = curl_error[0] ? curl_error : curl_easy_strerror(result);
        return {{}, std::string("语音识别请求失败：") + detail};
    }
    if (http_status < 200 || http_status >= 300)
    {
        std::string detail = JsonErrorMessage(response);
        if (detail.empty())
            detail = "HTTP " + std::to_string(http_status);
        if (http_status >= 500 && siliconflow)
        {
            detail += "。这是硅基流动服务端内部错误，模型名 " + model_name + " 本身是官方支持的。";
            if (!trace_id.empty())
                detail += " 追踪 ID：" + trace_id + "。";
        }
        return {{}, "语音识别失败：" + detail};
    }
    try
    {
        return {metasequoia::voice::parse_transcription(response), {}};
    }
    catch (...)
    {
        return {{}, "语音识别返回了无法解析的结果。"};
    }
}

bool ShouldPolish(const std::string &text, const VoiceInputConfig &config)
{
    return config.polish_text && !text.empty() && !VoiceInput::ResolvePolishToken(config).empty() &&
           !config.polish_endpoint.empty() && !VoiceInput::ResolvePolishModel(config).empty();
}

std::string Polish(const std::string &text, const VoiceInputConfig &config)
{
    if (!ShouldPolish(text, config))
        return text;
    const std::string polish_token = VoiceInput::ResolvePolishToken(config);
    std::string payload;
    try
    {
        payload = VoiceInput::BuildBatchPolish(text, config);
    }
    catch (...)
    {
        return text;
    }
    CURL *curl = curl_easy_init();
    if (!curl)
        return text;
    std::string response;
    curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + polish_token).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, config.polish_endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    // Polishing is optional: if it cannot finish promptly, abort the request and
    // let the caller commit the original ASR text returned below.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kPolishTimeoutMs);
    const CURLcode result = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || http_status < 200 || http_status >= 300)
        return text;
    try
    {
        return metasequoia::voice::parse_polished_text(response);
    }
    catch (...)
    {
        return text;
    }
}

void SendTextViaSendInput(const std::wstring &text)
{
    for (wchar_t ch : text)
    {
        INPUT input[2]{};
        input[0].type = INPUT_KEYBOARD;
        input[0].ki.wScan = ch;
        input[0].ki.dwFlags = KEYEVENTF_UNICODE;
        input[1] = input[0];
        input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(2, input, sizeof(INPUT));
    }
}

bool CopyUnicodeTextToClipboard(const std::wstring &text)
{
    if (!OpenClipboard(nullptr))
    {
        return false;
    }
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
    {
        CloseClipboard();
        return false;
    }
    void *destination = GlobalLock(memory);
    if (!destination)
    {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory))
    {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

void SendTextViaCtrlV(const std::wstring &text)
{
    if (!CopyUnicodeTextToClipboard(text))
    {
        SendTextViaSendInput(text);
        return;
    }
    // Give the focused app a brief moment to observe the new clipboard data.
    Sleep(30);
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
}

bool ShouldStreamInlinePreedit(const VoiceInputConfig &config)
{
    return config.stream_inline_preedit && VoiceInput::IsDoubaoAsrProvider(config.asr_provider) &&
           config.commit_mode == "tsf";
}

bool SendVoiceComposition(UINT msg_type, const std::wstring &text)
{
    const PipeClientActivation active = GetActivePipeClient();
    if (active.client_id == 0)
    {
        return false;
    }
    if (msg_type == Global::DataFromServerMsgTypeToTsfWorkerThread::CancelVoiceComposition)
    {
        return SendToTsfWorkerThreadClientViaNamedpipe(active.client_id, active.epoch, msg_type, L"");
    }
    return SendVoiceCompositionToTsfWorker(active.client_id, active.epoch, msg_type, text);
}

void PushInlinePreedit(const std::wstring &text)
{
    if (!g_stream_inline_this_session.load() || !g_recording.load())
    {
        return;
    }
    if (text.empty() || text == g_last_inline_preedit)
    {
        return;
    }
    if (SendVoiceComposition(Global::DataFromServerMsgTypeToTsfWorkerThread::UpdateVoiceComposition, text))
    {
        g_last_inline_preedit = text;
    }
}

void CancelInlinePreedit()
{
    if (!g_stream_inline_this_session.load() && g_last_inline_preedit.empty())
    {
        return;
    }
    SendVoiceComposition(Global::DataFromServerMsgTypeToTsfWorkerThread::CancelVoiceComposition, L"");
    g_last_inline_preedit.clear();
    g_stream_inline_this_session.store(false);
}

bool SendTextViaTsf(const std::wstring &text)
{
    if (text.empty())
    {
        return true;
    }
    const PipeClientActivation active = GetActivePipeClient();
    if (active.client_id == 0)
    {
        return false;
    }
    constexpr size_t maxCharsPerPacket =
        (sizeof(((FanyImeNamedpipeDataToTsfWorkerThread *)nullptr)->data) / sizeof(wchar_t)) - 1;
    size_t offset = 0;
    while (offset < text.size())
    {
        const size_t chunkLen = (std::min)(maxCharsPerPacket, text.size() - offset);
        if (!SendToTsfWorkerThreadClientViaNamedpipe(active.client_id, active.epoch,
                                                     Global::DataFromServerMsgTypeToTsfWorkerThread::InsertText,
                                                     text.substr(offset, chunkLen)))
        {
            return false;
        }
        offset += chunkLen;
    }
    return true;
}

void CommitRecognizedText(const std::string &utf8, const VoiceInputConfig &config, bool stream_inline)
{
    std::lock_guard<std::mutex> send_lock(g_send_mutex);
    const std::wstring text = string_to_wstring(utf8);
    if (text.empty())
    {
        return;
    }

    if (stream_inline)
    {
        if (SendVoiceComposition(Global::DataFromServerMsgTypeToTsfWorkerThread::CommitVoiceComposition, text))
        {
            g_last_inline_preedit.clear();
            return;
        }
        SendVoiceComposition(Global::DataFromServerMsgTypeToTsfWorkerThread::CancelVoiceComposition, L"");
        g_last_inline_preedit.clear();
        SendTextViaSendInput(text);
        return;
    }

    if (config.commit_mode == "ctrl_v")
    {
        SendTextViaCtrlV(text);
        return;
    }
    if (config.commit_mode == "sendinput")
    {
        SendTextViaSendInput(text);
        return;
    }

    // Default / "tsf": prefer TIP insert; fall back to SendInput when no active client.
    if (!SendTextViaTsf(text))
    {
        SendTextViaSendInput(text);
    }
}

void AudioCallback(const float *samples, std::size_t frames)
{
    if (!samples || !g_recording)
        return;
    double sum = 0.0;
    // Widen before squaring. float * float is evaluated at float precision and only then converted,
    // which throws away the accuracy the double accumulator exists to keep.
    for (std::size_t i = 0; i < frames; ++i)
        sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    const float rms = frames ? static_cast<float>(std::sqrt(sum / frames)) : 0.0f;
    // Compress the visual dynamic range so quiet speech still produces a clear waveform,
    // while keeping low-level room/microphone noise close to rest.
    constexpr float noise_floor = 0.004f;
    const float speech_level = (std::max)(0.0f, rms - noise_floor);
    const float normalized = (std::min)(1.0f, speech_level * 14.0f);
    g_overlay.set_input_level(std::pow(normalized, 0.55f));
    g_recorded_frames += frames;
    if (g_doubao_asr)
    {
        g_doubao_asr->PushFloatSamples(samples, frames);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_samples.insert(g_samples.end(), samples, samples + frames);
}

void AbortPendingStart(const char *reason)
{
    (void)reason;
    g_starting = false;
    g_abort_start = false;
    ++g_voice_session;
    g_stream_inline_this_session.store(false);
    g_last_inline_preedit.clear();
    if (g_doubao_asr)
    {
        g_doubao_asr->Cancel();
        g_doubao_asr.reset();
    }
}

bool StartRecording()
{
    if (g_recording)
        return true;
    if (!g_ime_active)
        return false;
    if (g_abort_start.exchange(false))
        return false;
    const VoiceInputConfig config = GetConfiguredVoiceInput();
    if (!config.enabled)
        return false;
    const bool use_doubao = VoiceInput::IsDoubaoAsrProvider(config.asr_provider);
    const std::string asr_token = VoiceInput::ResolveAsrToken(config);
    if (asr_token.empty())
    {
        MessageBoxW(nullptr, L"请先在设置的“语音输入”分区填写当前 ASR 提供商的 API Token。", L"水杉 IME",
                    MB_OK | MB_ICONINFORMATION);
        return false;
    }
    g_starting = true;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_samples.clear();
    }
    g_recorded_frames = 0;
    std::uint64_t voice_session = 0;
    {
        std::lock_guard<std::mutex> send_lock(g_send_mutex);
        voice_session = ++g_voice_session;
        g_dismissed_overlay_session = 0;
        g_last_inline_preedit.clear();
        g_stream_inline_this_session.store(use_doubao && ShouldStreamInlinePreedit(config));
    }
    g_overlay.set_show_transcript(!g_stream_inline_this_session.load());
    g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
    g_overlay.set_actions_visible(false);
    g_overlay.set_transcript(L"");
    if (use_doubao)
    {
        g_doubao_asr = std::make_unique<DoubaoAsrClient>(
            config.asr_endpoint, VoiceInput::UsesDoubaoLegacyAuth(config), config.asr_app_key, asr_token,
            config.asr_resource_id, config.doubao_enable_itn, config.doubao_enable_punc, config.doubao_enable_ddc,
            config.doubao_boosting_table_id, [voice_session](const std::string &text) {
                if (g_voice_session != voice_session)
                    return;
                const std::wstring wide = string_to_wstring(text);
                if (!g_stream_inline_this_session.load())
                    g_overlay.set_transcript(wide);
                std::lock_guard<std::mutex> send_lock(g_send_mutex);
                if (g_voice_session != voice_session)
                    return;
                PushInlinePreedit(wide);
            });
        if (!g_doubao_asr->Start())
        {
            ++g_voice_session;
            g_stream_inline_this_session.store(false);
            g_last_inline_preedit.clear();
            g_doubao_asr.reset();
            MessageBoxW(nullptr, L"无法启动豆包流式语音识别。请检查 config.toml。", L"水杉 IME", MB_OK | MB_ICONERROR);
            g_starting = false;
            return false;
        }
    }
    if (g_abort_start.exchange(false))
    {
        AbortPendingStart("aborted before capture");
        return false;
    }
    if (!g_capture.start(AudioCallback))
    {
        AbortPendingStart("capture start failed");
        MessageBoxW(nullptr, L"无法启动麦克风。", L"水杉 IME", MB_OK | MB_ICONERROR);
        return false;
    }
    if (g_abort_start.exchange(false))
    {
        g_capture.stop();
        AbortPendingStart("aborted after capture start");
        return false;
    }
    g_recording = true;
    g_starting = false;
    g_overlay.set_input_level(0.0f);
    g_overlay.set_listening(true);
    g_overlay.show();
    if (config.start_sound)
        g_cue_player.play_start();
    MuteSystemAudioIfEnabled(config);
    return true;
}

void StopRecording()
{
    if (!g_recording)
        return;
    g_capture.stop();
    if (g_capture.callback_failed())
    {
        CancelRecording();
        MessageBoxW(nullptr, L"录音中断，请重试。", L"水杉 IME", MB_OK | MB_ICONERROR);
        return;
    }
    const VoiceInputConfig config = GetConfiguredVoiceInput();
    g_recording = false;
    g_ralt_lock_mode = false;
    g_overlay.set_listening(false);
    g_overlay.set_input_level(0.0f);
    RestoreSystemAudioIfMuted();
    if (config.end_sound)
        g_cue_player.play_end();
    auto doubao_asr = std::move(g_doubao_asr);
    const bool batch_recognition = !doubao_asr;
    const bool stream_inline = g_stream_inline_this_session.load();
    const std::uint64_t voice_session = g_voice_session.load();
    if (!stream_inline)
    {
        g_overlay.set_compact_status(batch_recognition ? WaveOverlay::CompactStatus::Recognizing
                                                       : WaveOverlay::CompactStatus::None);
        g_overlay.set_show_transcript(true);
        g_overlay.set_transcript(batch_recognition ? L"" : L"正在识别…");
        g_overlay.set_actions_visible(batch_recognition);
        g_overlay.show();
    }
    else
    {
        g_overlay.set_compact_status(WaveOverlay::CompactStatus::Recognizing);
        g_overlay.set_actions_visible(true);
        g_overlay.show();
    }
    const std::size_t recorded_frames = g_recorded_frames;
    std::vector<float> samples;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        samples.swap(g_samples);
    }
    if (recorded_frames < kSampleRate / 4)
    {
        {
            std::lock_guard<std::mutex> send_lock(g_send_mutex);
            ++g_voice_session;
            CancelInlinePreedit();
        }
        g_overlay.hide();
        g_overlay.set_actions_visible(false);
        g_overlay.set_transcript(L"");
        if (doubao_asr)
        {
            g_network_tasks.emplace_back(
                std::async(std::launch::async, [client = std::move(doubao_asr)]() mutable { client->Cancel(); }));
        }
        return;
    }
    g_network_tasks.erase(std::remove_if(g_network_tasks.begin(), g_network_tasks.end(),
                                         [](std::future<void> &task) {
                                             return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                                         }),
                          g_network_tasks.end());
    g_network_tasks.emplace_back(
        std::async(std::launch::async, [samples = std::move(samples), client = std::move(doubao_asr), config,
                                        batch_recognition, stream_inline, voice_session]() mutable {
            RecognitionResult recognition;
            if (client)
            {
                recognition.text = client->Finish();
                recognition.error = client->LastError();
            }
            else
                recognition = Recognize(samples, config);
            if (!batch_recognition && !stream_inline && g_voice_session == voice_session)
            {
                if (!recognition.error.empty())
                    g_overlay.set_transcript(string_to_wstring(recognition.error));
                else if (!recognition.text.empty())
                    g_overlay.set_transcript(string_to_wstring(recognition.text));
            }
            const bool polishing = recognition.error.empty() && ShouldPolish(recognition.text, config);
            if (polishing && g_voice_session == voice_session)
            {
                g_overlay.set_compact_status(WaveOverlay::CompactStatus::Processing);
                g_overlay.set_actions_visible(true);
                if (g_dismissed_overlay_session != voice_session)
                    g_overlay.show();
            }
            else if (batch_recognition && g_voice_session == voice_session)
            {
                g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
                g_overlay.set_actions_visible(false);
                g_overlay.hide();
            }
            if (!recognition.error.empty())
            {
                MessageBoxW(nullptr, string_to_wstring(recognition.error).c_str(), L"水杉 IME", MB_OK | MB_ICONERROR);
            }
            const std::string text = recognition.error.empty() ? Polish(recognition.text, config) : std::string();
            if (!batch_recognition && !polishing && !stream_inline && g_voice_session == voice_session && !text.empty())
            {
                g_overlay.set_transcript(string_to_wstring(text));
            }
            if (!text.empty() && g_voice_session == voice_session)
                CommitRecognizedText(text, config, stream_inline);
            if (!polishing && stream_inline && g_voice_session == voice_session)
            {
                g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
                g_overlay.set_actions_visible(false);
                g_overlay.hide();
            }
            if (polishing && g_voice_session == voice_session)
            {
                g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
                g_overlay.set_actions_visible(false);
                g_overlay.hide();
                g_overlay.set_transcript(L"");
            }
            if (text.empty() && stream_inline && g_voice_session == voice_session)
            {
                std::lock_guard<std::mutex> send_lock(g_send_mutex);
                CancelInlinePreedit();
            }
            // Keep the finalized result visible briefly without delaying the commit.
            Sleep(recognition.error.empty() ? 160 : 1200);
            if (g_voice_session == voice_session && !g_recording)
            {
                g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
                g_overlay.set_actions_visible(false);
                g_overlay.hide();
                g_overlay.set_transcript(L"");
            }
        }));
}

void CancelRecording()
{
    if (!g_recording)
    {
        {
            std::lock_guard<std::mutex> send_lock(g_send_mutex);
            ++g_voice_session;
            CancelInlinePreedit();
        }
        g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
        g_overlay.set_actions_visible(false);
        g_overlay.hide();
        g_overlay.set_transcript(L"");
        return;
    }
    const VoiceInputConfig config = GetConfiguredVoiceInput();
    g_recording = false;
    {
        std::lock_guard<std::mutex> send_lock(g_send_mutex);
        ++g_voice_session;
        CancelInlinePreedit();
    }
    g_capture.stop();
    g_ralt_lock_mode = false;
    g_overlay.set_listening(false);
    g_overlay.set_input_level(0.0f);
    g_overlay.set_compact_status(WaveOverlay::CompactStatus::None);
    g_overlay.set_actions_visible(false);
    g_overlay.hide();
    g_overlay.set_transcript(L"");
    RestoreSystemAudioIfMuted();
    if (config.end_sound)
        g_cue_player.play_end();
    auto doubao_asr = std::move(g_doubao_asr);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_samples.clear();
    if (doubao_asr)
    {
        g_network_tasks.emplace_back(
            std::async(std::launch::async, [client = std::move(doubao_asr)]() mutable { client->Cancel(); }));
    }
}
} // namespace

bool VoiceInput::Initialize()
{
    if (g_initialized)
        return true;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!g_overlay.init(instance, [](WaveOverlay::Action action) {
            if (action == WaveOverlay::Action::Cancel)
            {
                if (!g_recording)
                    ++g_voice_session;
                if (g_message_window)
                    PostMessageW(g_message_window, kCancelRecordingMessage, 0, 0);
                return;
            }
            if (g_recording)
            {
                if (g_message_window)
                    PostMessageW(g_message_window, kStopRecordingMessage, 0, 0);
                return;
            }
            g_dismissed_overlay_session = g_voice_session.load();
            g_overlay.set_actions_visible(false);
            g_overlay.hide();
        }))
        return false;
    g_cue_player.init(ResolveCuePath(L"start.mp3"), ResolveCuePath(L"end.mp3"));
    RestoreOtherSystemAudio();

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = VoiceMessageWindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kMessageWindowClass;
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    g_message_window =
        CreateWindowExW(0, kMessageWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!g_message_window)
        return false;
    g_control_thread = std::thread(ControlLoop);
    if (ShouldInstallKeyboardHook(GetConfiguredVoiceInput()))
    {
        g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, instance, 0);
        if (!g_keyboard_hook)
            return false;
    }
    g_initialized = true;
    return true;
}

void VoiceInput::RefreshKeyboardHook()
{
    if (!g_initialized)
        return;

    const VoiceInputConfig config = GetConfiguredVoiceInput();
    const bool release_ralt = g_suppress_ralt_until_up;
    const bool stop_recording = g_active_hold_shortcut != HoldShortcut::None || (!config.enabled && g_recording);

    if (g_keyboard_hook)
    {
        UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = nullptr;
    }
    if (stop_recording)
        EnqueueControlCommand(ControlCommand::Stop);
    ResetKeyboardShortcutState();
    g_ralt_lock_mode = false;
    if (release_ralt)
        ForceReleaseRAlt();

    if (ShouldInstallKeyboardHook(config))
    {
        g_keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandleW(nullptr), 0);
    }
}

void VoiceInput::SetImeActive(bool active)
{
    const bool was_active = g_ime_active.exchange(active);
    if (active || !was_active)
        return;

    g_active_hold_shortcut = HoldShortcut::None;
    g_ralt_lock_mode = false;
    if (g_recording)
        EnqueueControlCommand(ControlCommand::Cancel);
}

void VoiceInput::ToggleRecording()
{
    EnqueueControlCommand(ControlCommand::Toggle);
}

void VoiceInput::StartRecording()
{
    EnqueueControlCommand(ControlCommand::Start);
}

void VoiceInput::StopRecording()
{
    EnqueueControlCommand(ControlCommand::Stop);
}

void VoiceInput::CancelRecording()
{
    EnqueueControlCommand(ControlCommand::Cancel);
}

bool VoiceInput::IsRecording()
{
    return g_recording;
}

void VoiceInput::Shutdown()
{
    const bool release_ralt = g_suppress_ralt_until_up;
    if (g_keyboard_hook)
    {
        UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = nullptr;
    }
    if (g_recording)
        EnqueueControlCommand(ControlCommand::Stop);
    EnqueueControlCommand(ControlCommand::Exit);
    if (g_control_thread.joinable())
        g_control_thread.join();
    RestoreOtherSystemAudio();
    g_muted_system_audio = false;
    for (auto &task : g_network_tasks)
        task.wait();
    g_network_tasks.clear();
    if (g_message_window)
    {
        DestroyWindow(g_message_window);
        g_message_window = nullptr;
    }
    g_cue_player.shutdown();
    g_overlay.shutdown();
    ResetKeyboardShortcutState();
    g_ralt_lock_mode = false;
    if (release_ralt)
        ForceReleaseRAlt();
    if (g_initialized)
        curl_global_cleanup();
    g_initialized = false;
}
