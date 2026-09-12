#include "event_listener.h"
#include <Windows.h>
#include <debugapi.h>
#include <ioapiset.h>
#include <namedpipeapi.h>
#include <string>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <iterator>
#include <utility>
#include <thread>
#include <unordered_map>
#include "Ipc.h"
#include "ipc/candidate_selection_policy.h"
#include "ipc/async_request_origin.h"
#include "ipc/candidate_ui_owner.h"
#include "ipc/candidate_text_policy.h"
#include "ipc/focus_session_policy.h"
#include "ipc/input_key_policy.h"
#include "engine/contracts/ipc_negotiation.h"
#include "defines/defines.h"
#include "ipc.h"
#include "defines/globals.h"
#include "utils/common_utils.h"
#include <boost/range/iterator_range_core.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string.hpp>
#include "fmt/xchar.h"
#include <utf8.h>
#include "global/globals.h"
#include "engine/common/helpcode_utils.h"
#include "engine/japanese/romaji_converter.h"
#include "engine/quanpin/quanpin_query.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "ipc/event_listener.h"
#include "utils/ime_utils.h"
#include "cloud/cloud_ime.h"
#include "cloud/cloud_translation.h"
#include "cloud/translation_gloss.h"
#include "ai/ai_assistant.h"
#include "english/english_ime.h"
#include "config/ime_config.h"
#include "window/window_hook.h"
#include "conversion/chinese_converter.h"
#include "session/session_factory.h"
#include "engine/local_modes/quick_phrase_query.h"
#include "engine/local_modes/unicode_query.h"
#include "engine/local_modes/date_time_query.h"
#include "engine/local_modes/emoji_query.h"
#include "engine/local_modes/kaomoji_query.h"
#include "engine/local_modes/jianpin_query.h"
#include "engine/local_modes/number_query.h"
#include "engine/shuangpin/shuangpin_profile.h"
#include "emoji/emoji_ime.h"
#include "kaomoji/kaomoji_ime.h"
#include "log/candidate_diag_log.h"
#include "log/ftb_diag_log.h"
#include "voice-input/voice_input_service.h"
#include <cwchar>

// IPC logging is compiled out. The macros still have to *mention* their arguments, otherwise every
// parameter of the log helpers below is unreferenced (C4100). sizeof keeps the arguments in an
// unevaluated context, so nothing is computed and no side effect runs — only the name is used.
template <typename... Args> int FanyIpcDiscardLogArgs(const Args &...);
#define FANY_IPC_LOG_RAW(message) ((void)sizeof(FanyIpcDiscardLogArgs(message)))
#define FANY_IPC_LOGW(message) ((void)sizeof(FanyIpcDiscardLogArgs(message)))
#define FANY_IPC_LOGF(...) ((void)sizeof(FanyIpcDiscardLogArgs(__VA_ARGS__)))

namespace
{
// The engine's local mode queries take a resolved ShuangpinProfile and default it to Xiaohe. The
// modules this file used to call resolved the *configured* scheme instead, so every call site here
// has to pass this explicitly: letting the default through would silently decode J mode, emoji and
// kaomoji as Xiaohe for anyone on Ziranma, Shoudao or Microsoft shuangpin.
const ShuangpinProfile &ConfiguredShuangpinProfile()
{
    return GetShuangpinProfile(GetConfiguredShuangpinSchema());
}

std::string BuildCurrentCandidatePage();
void PrepareCandidateTranslationRequest();
bool g_quick_phrase_triggered = false;
bool g_unicode_mode_triggered = false;
bool g_number_mode_triggered = false;
bool g_date_time_mode_triggered = false;
bool g_emoji_mode_triggered = false;
bool g_kaomoji_mode_triggered = false;
bool g_jianpin_mode_triggered = false;
bool g_y_mode_triggered = false;
bool g_r_mode_triggered = false;
std::shared_ptr<IInputSession> g_r_mode_original_session;
bool g_english_input_mode = false;
// Sticky UILess for the active Main-pipe client. When set, never raise the
// WebView2 candidate HWND — hosts (games) draw via ITfUIElementSink instead.
bool g_activate_uiless = false;
bool g_session_uiless = false;
std::unordered_map<std::string, std::string> g_candidate_translation_glosses;
std::string g_candidate_translation_signature;

std::shared_ptr<IInputSession> PersistentInputSession()
{
    return g_r_mode_original_session ? g_r_mode_original_session : g_inputSession;
}

std::string TranslationIdentity(const EnglishIme::TranslationQuery &query)
{
    return GetConfiguredTencentTmt().target_language + ":" +
           (query.direction == EnglishIme::TranslationDirection::EnglishToChinese ? "e:" : "z:") + query.key;
}

bool BuildTranslationQuery(const WordItem &item, EnglishIme::TranslationQuery &query)
{
    // Dictionary keys remain simplified even when the visible candidate is
    // converted to traditional Chinese at render/commit time.
    const std::string visible = item.word;
    if (visible.empty())
        return false;
    if (item.source == CandidateSource::Emoji || item.source == CandidateSource::Kaomoji)
        return false;
    if (item.source == CandidateSource::EnglishDictionary && !item.pinyin.empty())
    {
        query = {item.pinyin, EnglishIme::TranslationDirection::EnglishToChinese};
        return true;
    }

    bool has_ascii_letter = false;
    bool english = true;
    std::string normalized;
    normalized.reserve(visible.size());
    for (const unsigned char ch : visible)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            normalized.push_back(static_cast<char>(ch + ('a' - 'A')));
            has_ascii_letter = true;
        }
        else if (ch >= 'a' && ch <= 'z')
        {
            normalized.push_back(static_cast<char>(ch));
            has_ascii_letter = true;
        }
        else if (ch == ' ' || ch == '-' || ch == '\'')
        {
            normalized.push_back(static_cast<char>(ch));
        }
        else
        {
            english = false;
            break;
        }
    }
    if (english && has_ascii_letter)
    {
        query = {std::move(normalized), EnglishIme::TranslationDirection::EnglishToChinese};
        return true;
    }
    if (HelpcodeUtils::count_han_chars(visible) > 0)
    {
        query = {visible, EnglishIme::TranslationDirection::ChineseToEnglish};
        return true;
    }
    return false;
}

bool IsUiLessMode()
{
    return g_activate_uiless || g_session_uiless;
}

void ApplyUiLessFromPacket(const FanyImeNamedpipeData &pipe_data)
{
    const bool wasUiLess = IsUiLessMode();
    if (pipe_data.event_type == FanyImePipeEventType::ClientActivated)
    {
        g_activate_uiless = (pipe_data.keycode != 0);
        g_session_uiless = g_activate_uiless;
    }
    else if (FanyImePipeEventType::IsRouteDeactivation(pipe_data.event_type))
    {
        g_activate_uiless = false;
        g_session_uiless = false;
    }
    else if (pipe_data.event_type == FanyImePipeEventType::KeyEvent ||
             pipe_data.event_type == FanyImePipeEventType::ShowCandidateWnd ||
             pipe_data.event_type == FanyImePipeEventType::MoveCandidateWnd ||
             pipe_data.event_type == FanyImePipeEventType::HideCandidateWnd)
    {
        g_session_uiless = g_activate_uiless || ((pipe_data.modifiers_down & FanyImePipeFlags::UiLess) != 0);
    }

    if (!wasUiLess && IsUiLessMode())
    {
        g_candidate_translation_signature.clear();
        g_candidate_translation_glosses.clear();
        EnglishIme::ClearTranslations();
        // A prior non-UILess session may have left the WebView2 candidate HWND
        // visible; hide it immediately when the host takes over drawing.
        ::is_global_wnd_cand_shown = false;
        if (::global_hwnd && IsWindow(::global_hwnd))
        {
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
        }
    }
}

// A task that waited at least this long was delivered behind a stalled worker,
// so the state it describes may already be superseded. Normal typing never gets
// near it: queue waits stay under a few milliseconds unless something blocks the
// task thread.
constexpr ULONGLONG kCandidateHideBacklogMs = 24;

void RequestShowCandidateWindow()
{
    if (IsUiLessMode() || !::global_hwnd)
    {
        CAND_DIAG_LOGF(L"show request skipped uiless={} hwnd_present={}", IsUiLessMode(), ::global_hwnd != nullptr);
        return;
    }
    bool expected = false;
    if (!g_candidate_show_msg_pending.compare_exchange_strong(expected, true))
    {
        CAND_DIAG_LOGF(L"show request coalesced raw_units={} candidate_count={}",
                       GlobalIme::composition.raw_input_with_cases.size(), Global::candidate_ui.items.size());
        return;
    }
    CAND_DIAG_LOGF(L"show request posted raw_units={} candidate_count={}",
                   GlobalIme::composition.raw_input_with_cases.size(), Global::candidate_ui.items.size());
    if (!PostMessage(::global_hwnd, WM_SHOW_MAIN_WINDOW, 0, 0))
    {
        g_candidate_show_msg_pending.store(false);
    }
}

bool IsHexChar(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

bool IsQuickPhraseInput(const std::string &raw)
{
    return g_quick_phrase_triggered && raw.size() > 1 && raw.front() == 'K' &&
           std::all_of(raw.begin() + 1, raw.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; });
}

bool IsQuickPhraseCompositionActive(const std::string &raw)
{
    return g_quick_phrase_triggered && !raw.empty() && raw.front() == 'K';
}

bool IsUnicodeCompositionActive(const std::string &raw)
{
    if (!g_unicode_mode_triggered || raw.empty() || raw.front() != 'U')
        return false;
    size_t index = 1;
    if (index < raw.size() && raw[index] == '+')
        ++index;
    return std::all_of(raw.begin() + static_cast<std::ptrdiff_t>(index), raw.end(),
                       [](unsigned char ch) { return IsHexChar(ch); });
}

bool IsUnicodeInput(const std::string &raw)
{
    if (!IsUnicodeCompositionActive(raw) || raw.size() <= 1)
        return false;
    size_t index = 1;
    if (raw[index] == '+')
        ++index;
    return index < raw.size();
}

// The prefix is V after Shift+V, or v when quanpin let a bare v start the mode.
bool IsNumberCompositionActive(const std::string &raw)
{
    return g_number_mode_triggered && !raw.empty() && (raw.front() == 'V' || raw.front() == 'v') &&
           (raw.size() == 1 || metasequoia::local_modes::is_number_text(raw.substr(1)));
}

bool IsNumberInput(const std::string &raw)
{
    return IsNumberCompositionActive(raw) && raw.size() > 1;
}

// U-mode and V-mode both own bare digits as composition input; Shift+1..9
// selects candidates in either.
bool IsDigitCompositionActive(const std::string &raw)
{
    return IsUnicodeCompositionActive(raw) || IsNumberCompositionActive(raw);
}

// V-mode accepts one decimal point once at least one digit has been typed.
bool IsNumberPointKey(const std::string &raw, UINT keycode, WCHAR wch)
{
    return IsNumberCompositionActive(raw) && keycode == VK_OEM_PERIOD && wch == L'.' && raw.size() > 1 &&
           raw.find('.') == std::string::npos;
}

bool IsDateTimeCompositionActive(const std::string &raw)
{
    return g_date_time_mode_triggered && !raw.empty() && raw.front() == 'T' &&
           std::all_of(raw.begin() + 1, raw.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; });
}

bool IsDateTimeInput(const std::string &raw)
{
    if (!IsDateTimeCompositionActive(raw) || raw.size() <= 1)
        return false;
    return metasequoia::local_modes::is_date_time_keyword(raw.substr(1));
}

bool IsEmojiCompositionActive(const std::string &raw)
{
    return g_emoji_mode_triggered && !raw.empty() && raw.front() == 'E' &&
           std::all_of(raw.begin() + 1, raw.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '\'';
           });
}

bool IsEmojiInput(const std::string &raw)
{
    return IsEmojiCompositionActive(raw) && raw.size() > 1;
}

bool IsKaomojiCompositionActive(const std::string &raw)
{
    return g_kaomoji_mode_triggered && !raw.empty() && raw.front() == 'M' &&
           std::all_of(raw.begin() + 1, raw.end(), [](unsigned char ch) {
               return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '\'';
           });
}

bool IsKaomojiInput(const std::string &raw)
{
    return IsKaomojiCompositionActive(raw) && raw.size() > 1;
}

bool IsJianpinCompositionActive(const std::string &raw)
{
    return g_jianpin_mode_triggered && !raw.empty() && raw.front() == 'J';
}

bool IsJianpinInput(const std::string &raw)
{
    return IsJianpinCompositionActive(raw) && raw.size() > 1 &&
           std::all_of(raw.begin() + 1, raw.end(),
                       [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); });
}

bool IsYModeCompositionActive(const std::string &raw)
{
    return g_y_mode_triggered && !raw.empty() && raw.front() == 'Y' &&
           std::all_of(raw.begin() + 1, raw.end(),
                       [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); });
}

bool IsYModeInput(const std::string &raw)
{
    return IsYModeCompositionActive(raw) && raw.size() > 1;
}

bool IsShiftLetterSpecialModeTriggered()
{
    return g_quick_phrase_triggered || g_unicode_mode_triggered || g_number_mode_triggered ||
           g_date_time_mode_triggered || g_emoji_mode_triggered || g_kaomoji_mode_triggered ||
           g_jianpin_mode_triggered || g_y_mode_triggered || g_r_mode_triggered;
}

void ClearSpecialModeTriggers()
{
    g_quick_phrase_triggered = false;
    g_unicode_mode_triggered = false;
    g_number_mode_triggered = false;
    g_date_time_mode_triggered = false;
    g_emoji_mode_triggered = false;
    g_kaomoji_mode_triggered = false;
    g_jianpin_mode_triggered = false;
    g_y_mode_triggered = false;
    g_r_mode_triggered = false;
}

// True whenever a K/U/V/T/E/M/J/Y special-mode composition is in progress, even when the
// typed text is not yet a complete keyword/hex/number sequence. Such input must never
// be interpreted as normal pinyin.
bool IsSpecialModeCompositionActive(const std::string &raw)
{
    return IsQuickPhraseCompositionActive(raw) || IsUnicodeCompositionActive(raw) || IsNumberCompositionActive(raw) ||
           IsDateTimeCompositionActive(raw) || IsEmojiCompositionActive(raw) || IsKaomojiCompositionActive(raw) ||
           IsJianpinCompositionActive(raw) || IsYModeCompositionActive(raw);
}

constexpr auto kPipeHelloTimeout = std::chrono::seconds(2);

class ScopedPipeClientHandler
{
  public:
    explicit ScopedPipeClientHandler(uint64_t handler_id) : handler_id_(handler_id)
    {
    }

    ~ScopedPipeClientHandler()
    {
        EndPipeClientHandler(handler_id_);
    }

    ScopedPipeClientHandler(const ScopedPipeClientHandler &) = delete;
    ScopedPipeClientHandler &operator=(const ScopedPipeClientHandler &) = delete;

  private:
    uint64_t handler_id_ = 0;
};

bool SetPipeWaitMode(HANDLE pipe, bool wait)
{
    DWORD mode = PIPE_READMODE_MESSAGE | (wait ? PIPE_WAIT : PIPE_NOWAIT);
    return SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) != FALSE;
}

bool ReadExactPipeMessageUntil(HANDLE pipe, void *destination, DWORD destination_size,
                               std::chrono::steady_clock::time_point deadline, DWORD &bytes_read)
{
    bytes_read = 0;
    while (pipe_running && std::chrono::steady_clock::now() < deadline)
    {
        const BOOL result = ReadFile(pipe, destination, destination_size, &bytes_read, nullptr);
        if (result)
        {
            return bytes_read == destination_size;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_NO_DATA)
        {
            return false;
        }
        Sleep(2);
    }

    SetLastError(pipe_running ? ERROR_SEM_TIMEOUT : ERROR_OPERATION_ABORTED);
    return false;
}

using AsyncRequestOrigin = FanyImeIpc::AsyncRequestOrigin;

std::mutex g_async_request_mutex;
uint64_t g_cloud_generation = 0;
uint64_t g_english_generation = 0;
uint64_t g_emoji_generation = 0;
uint64_t g_kaomoji_generation = 0;
uint64_t g_ai_generation = 0;
AsyncRequestOrigin g_cloud_request_origin;
AsyncRequestOrigin g_english_request_origin;
AsyncRequestOrigin g_emoji_request_origin;
AsyncRequestOrigin g_kaomoji_request_origin;
AsyncRequestOrigin g_ai_request_origin;
std::string g_ai_context;
std::mutex g_status_snapshot_mutex;
int g_latest_status_snapshot = -1;
bool g_latest_english_input_mode = false;
// Global CN/EN authority for input.ime_mode_scope = "global".
// -1 until first StatusSnapshot or lazy seed from default_ime_mode.
int g_authoritative_cn_mode = -1;
uint64_t g_last_status_snapshot_client_id = 0;
// The toolbar is global but the mode is per TSF client, so an activation would
// otherwise keep displaying the outgoing client's mode until the incoming one
// happens to send its first snapshot. Entries are dropped when the client's
// main pipe unregisters.
std::unordered_map<uint64_t, int> g_client_status_snapshots;
// After switching away from this IME, the next StatusSnapshot must restore the
// configured default mode even when the same process/thread client_id reconnects.
bool g_force_global_ime_sync = false;
HWND g_status_snapshot_window = nullptr;
std::mutex g_candidate_ui_owner_mutex;
FanyImeIpc::CandidateUiOwnerState g_candidate_ui_owner;

void PublishCandidateUiOwner(uint64_t client_id, uint64_t activation_epoch)
{
    std::lock_guard lock(g_candidate_ui_owner_mutex);
    g_candidate_ui_owner.publish(client_id, activation_epoch);
}

void ClearCandidateUiOwner()
{
    std::lock_guard lock(g_candidate_ui_owner_mutex);
    g_candidate_ui_owner.clear();
}

FanyImeIpc::CandidateUiOwner SnapshotCandidateUiOwner()
{
    std::lock_guard lock(g_candidate_ui_owner_mutex);
    return g_candidate_ui_owner.snapshot();
}

bool CandidateUiOwnerIsCurrent(const FanyImeIpc::CandidateUiOwner &owner)
{
    std::lock_guard lock(g_candidate_ui_owner_mutex);
    return g_candidate_ui_owner.matches(owner);
}

void PublishStatusSnapshotValue(int packed_state)
{
    std::lock_guard lock(g_status_snapshot_mutex);
    g_latest_status_snapshot = packed_state;
    const bool has_window = g_status_snapshot_window && IsWindow(g_status_snapshot_window);
    if (has_window)
    {
        PostMessage(g_status_snapshot_window, UPDATE_FTB_STATUS, packed_state, 0);
    }
}

void PublishEnglishInputModeValue(bool enabled)
{
    std::lock_guard lock(g_status_snapshot_mutex);
    g_latest_english_input_mode = enabled;
    const bool has_window = g_status_snapshot_window && IsWindow(g_status_snapshot_window);
    if (has_window)
    {
        PostMessage(g_status_snapshot_window, UPDATE_FTB_ENGLISH_INPUT_MODE, enabled ? 1 : 0, 0);
    }
}

void SetEnglishInputMode(bool enabled)
{
    if (g_english_input_mode == enabled)
    {
        return;
    }
    g_english_input_mode = enabled;
    PublishEnglishInputModeValue(enabled);
}

void RememberClientStatusSnapshot(uint64_t client_id, int packed_state)
{
    if (client_id == 0)
    {
        return;
    }
    std::lock_guard lock(g_status_snapshot_mutex);
    g_client_status_snapshots[client_id] = packed_state;
}

void ForgetClientStatusSnapshot(uint64_t client_id)
{
    std::lock_guard lock(g_status_snapshot_mutex);
    g_client_status_snapshots.erase(client_id);
}

// Returns -1 when this client has never reported a mode.
int RecallClientStatusSnapshot(uint64_t client_id)
{
    std::lock_guard lock(g_status_snapshot_mutex);
    const auto it = g_client_status_snapshots.find(client_id);
    return it == g_client_status_snapshots.end() ? -1 : it->second;
}

int EnsureAuthoritativeCnMode()
{
    if (g_authoritative_cn_mode < 0)
    {
        ReloadImeConfigIfChanged();
        g_authoritative_cn_mode = GetConfiguredDefaultImeMode() == "english" ? 0 : 1;
    }
    return g_authoritative_cn_mode;
}

void UpdateCloudInput(const std::string &input, uint64_t client_id = 0, uint64_t activation_epoch = 0)
{
    std::lock_guard lock(g_async_request_mutex);
    const std::string effective_input = GetConfiguredCloudCandidatesEnabled() ? input : std::string{};
    const bool japanese = g_inputSession && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji;
    CloudIme::OnInputChanged(effective_input, japanese);
    ++g_cloud_generation;
    g_cloud_request_origin = effective_input.empty()
                                 ? AsyncRequestOrigin{}
                                 : AsyncRequestOrigin{client_id, activation_epoch, g_cloud_generation, effective_input};
    if (!effective_input.empty() && g_inputSession)
        g_cloud_request_origin.engine_query = g_inputSession->online_query();
}

void UpdateEnglishInput(const std::string &input, uint64_t client_id = 0, uint64_t activation_epoch = 0,
                        bool dedicated_mode = false)
{
    std::lock_guard lock(g_async_request_mutex);
    const size_t mixed_min_prefix =
        dedicated_mode ? size_t{1} : static_cast<size_t>(GetConfiguredEnglishMixedInputMinChars());
    EnglishIme::OnInputChanged(input, dedicated_mode, mixed_min_prefix);
    ++g_english_generation;
    g_english_request_origin = input.empty()
                                   ? AsyncRequestOrigin{}
                                   : AsyncRequestOrigin{client_id, activation_epoch, g_english_generation, input};
}

void UpdateEmojiInput(const std::string &input, uint64_t client_id = 0, uint64_t activation_epoch = 0)
{
    std::lock_guard lock(g_async_request_mutex);
    EmojiIme::OnInputChanged(input, g_inputSession ? g_inputSession->current_scheme_type() : SchemeType::Quanpin);
    ++g_emoji_generation;
    g_emoji_request_origin = input.empty() ? AsyncRequestOrigin{}
                                           : AsyncRequestOrigin{client_id, activation_epoch, g_emoji_generation, input};
}

void UpdateKaomojiInput(const std::string &input, uint64_t client_id = 0, uint64_t activation_epoch = 0)
{
    std::lock_guard lock(g_async_request_mutex);
    KaomojiIme::OnInputChanged(input, g_inputSession ? g_inputSession->current_scheme_type() : SchemeType::Quanpin);
    ++g_kaomoji_generation;
    g_kaomoji_request_origin = input.empty()
                                   ? AsyncRequestOrigin{}
                                   : AsyncRequestOrigin{client_id, activation_epoch, g_kaomoji_generation, input};
}

std::vector<std::string> SplitPinyin(const std::string &segmentation)
{
    std::vector<std::string> result;
    boost::split(result, segmentation, boost::is_any_of("' "), boost::token_compress_on);
    result.erase(std::remove_if(result.begin(), result.end(), [](const std::string &item) { return item.empty(); }),
                 result.end());
    return result;
}

void UpdateAiInput(const std::string &identity, uint64_t client_id = 0, uint64_t activation_epoch = 0)
{
    std::lock_guard lock(g_async_request_mutex);
    const AiAssistantConfig config = GetConfiguredAiAssistant();
    const bool usable = config.enabled && g_inputSession &&
                        (g_inputSession->current_scheme_type() == SchemeType::Quanpin ||
                         g_inputSession->current_scheme_type() == SchemeType::Shuangpin) &&
                        g_inputSession->is_all_complete_pure_pinyin() && !g_inputSession->has_active_helpcode() &&
                        !identity.empty();
    (void)0;
    AiAssistant::Request request;
    if (usable)
    {
        request.pinyin_segments = SplitPinyin(g_inputSession->get_pinyin_segmentation());
        request.context = g_ai_context;
        request.identity = identity;
        request.config = config;
    }
    AiAssistant::OnInputChanged(std::move(request));
    ++g_ai_generation;
    g_ai_request_origin =
        usable ? AsyncRequestOrigin{client_id, activation_epoch, g_ai_generation, identity} : AsyncRequestOrigin{};
    if (usable)
        g_ai_request_origin.engine_query = g_inputSession->online_query();
}

AsyncRequestOrigin FindCloudRequestOrigin(const std::string &input, uint64_t generation)
{
    std::lock_guard lock(g_async_request_mutex);
    if (g_cloud_request_origin.matches(input, generation))
    {
        return g_cloud_request_origin;
    }
    return {};
}

AsyncRequestOrigin FindEnglishRequestOrigin(const std::string &input, uint64_t generation)
{
    std::lock_guard lock(g_async_request_mutex);
    if (g_english_request_origin.generation == generation && g_english_request_origin.input == input)
    {
        return g_english_request_origin;
    }
    return {};
}

AsyncRequestOrigin FindEmojiRequestOrigin(const std::string &input, uint64_t generation)
{
    std::lock_guard lock(g_async_request_mutex);
    if (g_emoji_request_origin.generation == generation && g_emoji_request_origin.input == input)
    {
        return g_emoji_request_origin;
    }
    return {};
}

AsyncRequestOrigin FindKaomojiRequestOrigin(const std::string &input, uint64_t generation)
{
    std::lock_guard lock(g_async_request_mutex);
    if (g_kaomoji_request_origin.generation == generation && g_kaomoji_request_origin.input == input)
    {
        return g_kaomoji_request_origin;
    }
    return {};
}

AsyncRequestOrigin FindAiRequestOrigin(const std::string &input, uint64_t generation)
{
    std::lock_guard lock(g_async_request_mutex);
    if (g_ai_request_origin.matches(input, generation))
        return g_ai_request_origin;
    return {};
}

std::string CandidateTextForOutput(const std::string &text)
{
    if (g_inputSession && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji)
        return text;
    return GetConfiguredCharacterSet() == "traditional" ? ChineseConverter::ToTraditional(text) : text;
}

// 每次提交都把上屏文本追加进 AI 联想的上下文，并按 UTF-8 边界裁剪到 1024 字节。
// 造词过程中每选中一段都会各自调用一次，因此这里只追加本次提交的那一段。
void AppendAiContext(const std::string &committed_word)
{
    g_ai_context += CandidateTextForOutput(committed_word);
    if (g_ai_context.size() > 1024)
    {
        size_t cut = g_ai_context.size() - 1024;
        while (cut < g_ai_context.size() && (static_cast<unsigned char>(g_ai_context[cut]) & 0xC0) == 0x80)
            ++cut;
        g_ai_context.erase(0, cut);
    }
}

std::wstring BuildCreateWordPipePayload(const std::string &remaining_raw_input_with_cases,
                                        const std::string &current_word)
{
    // remaining_raw \t committed_word \t display_preedit
    // display_preedit matches the candidate-window preedit (汉字 + 剩余分词).
    // Legacy TSF only reads the first two fields.
    const std::wstring remaining = string_to_wstring(remaining_raw_input_with_cases);
    const std::wstring word = string_to_wstring(CandidateTextForOutput(current_word));
    const std::wstring preedit = word + string_to_wstring(GlobalIme::composition.segmented_pinyin);
    return remaining + L'\t' + word + L'\t' + preedit;
}

bool IsCommitWithHighlightedCandidatePunctuationInCandidateMode(UINT keycode, WCHAR wch)
{
    if (keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS || keycode == VK_TAB)
    {
        return false;
    }
    const bool has_active_composition = g_inputSession != nullptr && !g_inputSession->get_pinyin_sequence().empty();
    if ((keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD) && GetConfiguredPagingCommaPeriodEnabled() &&
        has_active_composition)
    {
        return false;
    }
    if ((keycode == VK_OEM_4 || keycode == VK_OEM_6) && GetConfiguredPagingBracketsEnabled() && has_active_composition)
    {
        return false;
    }

    static const std::unordered_set<WCHAR> kCommitWithHighlightedCandidatePunctuation = {
        L'`',  //
        L'!',  //
        L'@',  //
        L'#',  //
        L'$',  //
        L'%',  //
        L'^',  //
        L'&',  //
        L'*',  //
        L'-',  // Numpad arithmetic keys are not candidate paging keys.
        L'+',  //
        L'(',  //
        L')',  //
        L'[',  //
        L']',  //
        L'\\', //
        L'/',  //
        L';',  //
        L':',  //
        L'\'', //
        L'"',  //
        L',',  //
        L'<',  //
        L'.',  //
        L'>',  //
        L'?'   //
    };
    return kCommitWithHighlightedCandidatePunctuation.find(wch) != kCommitWithHighlightedCandidatePunctuation.end();
}

bool IsManualPinyinSeparatorKey(UINT keycode, WCHAR wch)
{
    return keycode == VK_OEM_7 && wch == L'\'' && g_inputSession != nullptr &&
           g_inputSession->current_scheme_type() != SchemeType::Wubi && !g_inputSession->get_pinyin_sequence().empty();
}

bool IsMicrosoftShuangpinIngKey(UINT keycode, WCHAR wch, const std::string &raw_input)
{
    if (keycode != VK_OEM_1 || wch != L';' || GetConfiguredShuangpinSchema() != "microsoft" ||
        g_inputSession == nullptr || g_inputSession->current_scheme_type() != SchemeType::Shuangpin)
    {
        return false;
    }

    const size_t caret = (std::min)(GlobalIme::composition.caret_position, raw_input.size());
    const size_t separator = caret == 0 ? std::string::npos : raw_input.rfind('\'', caret - 1);
    const size_t chunk_start = separator == std::string::npos ? 0 : separator + 1;
    return (caret - chunk_start) % 2 == 1;
}

bool IsSelectionKey(UINT keycode)
{
    if (keycode == VK_SPACE)
        return true;
    if (keycode >= '0' && keycode <= '9')
    {
        const std::string raw = g_inputSession ? g_inputSession->get_pinyin_sequence_with_cases() : std::string{};
        if (IsDigitCompositionActive(raw))
        {
            // U-mode / V-mode: bare digits compose; Shift+1..9 selects candidates.
            const bool shift_only = (Global::ModifiersDown & 0b00000111u) == 0b00000001u;
            return shift_only && keycode >= '1' && keycode <= '9';
        }
        return true;
    }
    return false;
}

bool IsPagingKey(UINT keycode)
{
    return keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS || keycode == VK_TAB || keycode == VK_PRIOR ||
           keycode == VK_NEXT || keycode == VK_LEFT || keycode == VK_RIGHT || keycode == VK_UP || keycode == VK_DOWN ||
           ((keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD) && GetConfiguredPagingCommaPeriodEnabled()) ||
           ((keycode == VK_OEM_4 || keycode == VK_OEM_6) && GetConfiguredPagingBracketsEnabled());
}

bool IsCandidateNavigationKey(UINT keycode)
{
    return keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS || keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD ||
           keycode == VK_OEM_4 || keycode == VK_OEM_6 || keycode == VK_TAB || keycode == VK_PRIOR ||
           keycode == VK_NEXT || keycode == VK_UP || keycode == VK_DOWN;
}

bool ApplyCompositionEditKey(UINT keycode, WCHAR wch)
{
    std::string raw = g_inputSession->get_pinyin_sequence_with_cases();
    auto &composition = GlobalIme::composition;
    if (composition.raw_input_with_cases != raw && composition.caret_position == 0 && !raw.empty())
    {
        composition.caret_position = raw.size();
    }
    composition.caret_position = (std::min)(composition.caret_position, raw.size());

    if (keycode == VK_LEFT)
    {
        if (composition.caret_position > 0)
        {
            --composition.caret_position;
        }
        return true;
    }
    if (keycode == VK_RIGHT)
    {
        if (composition.caret_position < raw.size())
        {
            ++composition.caret_position;
        }
        return true;
    }

    if (keycode == VK_BACK)
    {
        if (composition.caret_position > 0)
        {
            raw.erase(composition.caret_position - 1, 1);
            --composition.caret_position;
        }
    }
    else if (keycode == VK_DELETE)
    {
        if (composition.caret_position < raw.size())
        {
            raw.erase(composition.caret_position, 1);
        }
    }
    else
    {
        char input = 0;
        if (keycode >= 'A' && keycode <= 'Z')
        {
            input = wch >= L'A' && wch <= L'Z' || wch >= L'a' && wch <= L'z' ? static_cast<char>(wch)
                                                                             : static_cast<char>(keycode + ('a' - 'A'));
        }
        else if (keycode == VK_OEM_7 && wch == L'\'')
        {
            input = '\'';
        }
        else if (keycode == VK_OEM_1 && wch == L';' && GetConfiguredShuangpinSchema() == "microsoft" &&
                 g_inputSession->current_scheme_type() == SchemeType::Shuangpin)
        {
            input = ';';
        }
        else if (IsDigitCompositionActive(raw) && keycode >= '0' && keycode <= '9')
        {
            input = static_cast<char>(keycode);
        }
        else if (IsUnicodeCompositionActive(raw) && keycode == VK_OEM_PLUS && wch == L'+' && raw == "U")
        {
            input = '+';
        }
        else if (IsNumberPointKey(raw, keycode, wch))
        {
            input = '.';
        }
        else
        {
            return false;
        }
        if (input == '\'' && ((composition.caret_position > 0 && raw[composition.caret_position - 1] == '\'') ||
                              (composition.caret_position < raw.size() && raw[composition.caret_position] == '\'')))
        {
            return true;
        }
        raw.insert(raw.begin() + static_cast<std::ptrdiff_t>(composition.caret_position), input);
        ++composition.caret_position;
    }

    g_inputSession->set_pinyin_sequence(raw);
    g_inputSession->set_pinyin_sequence_with_cases(raw);
    g_inputSession->recompute_candidates();
    composition.raw_input_with_cases = raw;
    return true;
}

void EnsureCandidatePageReady()
{
    if (!Global::candidate_ui.page_words.empty())
    {
        return;
    }
    if (Global::candidate_ui.items.empty())
    {
        return;
    }
    BuildCurrentCandidatePage();
}

std::wstring BuildUiLessCandidatePageW()
{
    EnsureCandidatePageReady();
    auto &ui = Global::candidate_ui;
    if (ui.page_words.empty() && !ui.items.empty())
    {
        BuildCurrentCandidatePage();
    }
    std::wstring page;
    for (size_t i = 0; i < ui.page_words.size(); ++i)
    {
        if (i != 0)
        {
            page += L',';
        }
        page += ui.page_words[i];
    }
    return page;
}

std::string BuildCurrentCandidatePage()
{
    auto &ui = Global::candidate_ui;
    ui.clear_page();
    const SchemeType current_scheme = g_inputSession->current_scheme_type();
    const bool uppercase_all_helpcodes = current_scheme == SchemeType::Quanpin;
    const bool show_helpcodes = (current_scheme == SchemeType::Shuangpin && GetConfiguredShuangpinHelpcodeEnabled() &&
                                 GetConfiguredShowShuangpinHelpcodeInCandidateWindow()) ||
                                (current_scheme == SchemeType::Quanpin && GetConfiguredQuanpinHelpcodeEnabled() &&
                                 GetConfiguredShowQuanpinHelpcodeInCandidateWindow());

    const int start = ui.current_page_start();
    const int loop = ui.current_page_count();

    int maxCount = 0;
    std::string candidate_string;
    for (int i = 0; i < loop; i++)
    {
        const auto &item = ui.items[start + i];
        const std::string word = CandidateTextForOutput(item.word);

        CandidateViewItem view;
        view.text = word;
        if (!item.corrected_from.empty())
        {
            // Correction-sourced candidates carry a light visible marker (PRD R5/AC7).
            // Only the display text is touched: commits, word frequency updates and
            // pinned-position lookups read item.word / page_words and must never see
            // the marker suffix.
            view.text += "*";
        }
        if (item.source == CandidateSource::Generated && show_helpcodes)
        {
            // Generated whole-sentence candidates carry the raw spelling in
            // item.pinyin.  When helpcodes are enabled, do not expose that
            // full preedit; sentence annotations must use the normal
            // first/last-character helpcode rule instead.  If the helpcode
            // cannot be computed, leave the annotation empty rather than
            // falling back to the raw preedit.
            view.annotation = g_inputSession->get_helpcode_annotation(item.word, uppercase_all_helpcodes);
        }
        if (show_helpcodes && item.source != CandidateSource::EnglishDictionary &&
            item.source != CandidateSource::QuickPhrase && item.source != CandidateSource::Emoji &&
            item.source != CandidateSource::Kaomoji && item.source != CandidateSource::Generated)
            view.annotation = g_inputSession->get_helpcode_annotation(item.word, uppercase_all_helpcodes);
        if (item.source == CandidateSource::CloudSuggestion)
            view.badge = " ☁️";
        else if (item.source == CandidateSource::AiSuggestion)
            view.badge = " 🤖";
        view.fixed_position = item.fixed_position > 0;
        EnglishIme::TranslationQuery translation_query;
        if (BuildTranslationQuery(item, translation_query))
        {
            const auto gloss = g_candidate_translation_glosses.find(TranslationIdentity(translation_query));
            if (gloss != g_candidate_translation_glosses.end())
                view.translation = gloss->second;
        }
        const std::string visible = view.text + view.annotation + view.badge;
        const int display_length = static_cast<int>(utf8::distance(visible.begin(), visible.end()));
        candidate_string += CandidateViewHtml(view);
        ui.page_glosses.push_back(string_to_wstring(view.translation));
        ui.page_views.push_back(std::move(view));
        maxCount = (std::max)(maxCount, display_length);
        ui.page_words.push_back(string_to_wstring(word));
        if (i < loop - 1)
        {
            candidate_string += ",";
        }
    }

    if (maxCount > 2)
    {
        ui.cur_page_max_word_len = maxCount;
    }
    ui.cur_page_item_cnt = loop;
    if (!ui.page_words.empty())
    {
        ui.selected_index_in_page = std::clamp(ui.selected_index_in_page, 0, loop - 1);
        ui.selected_text = ui.page_words[ui.selected_index_in_page];
    }
    return candidate_string;
}

void PrepareCandidateTranslationRequest()
{
    const bool japanese = g_inputSession && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji;
    const bool enabled = GetConfiguredCandidateTranslationsEnabled() && !IsUiLessMode() && !japanese;
    auto &ui = Global::candidate_ui;
    if (!enabled || ui.items.empty())
    {
        if (!g_candidate_translation_signature.empty() || !g_candidate_translation_glosses.empty())
        {
            g_candidate_translation_signature.clear();
            g_candidate_translation_glosses.clear();
            EnglishIme::ClearTranslations();
            CloudTranslation::Clear();
        }
        return;
    }

    std::vector<EnglishIme::TranslationQuery> queries;
    std::string signature;
    const int start = ui.current_page_start();
    const int count = ui.current_page_count();
    for (int i = 0; i < count; ++i)
    {
        EnglishIme::TranslationQuery query;
        if (!BuildTranslationQuery(ui.items[start + i], query))
            continue;
        const std::string identity = TranslationIdentity(query);
        signature += std::to_string(identity.size()) + ":" + identity;
        if (std::none_of(queries.begin(), queries.end(), [&](const auto &existing) {
                return existing.key == query.key && existing.direction == query.direction;
            }))
            queries.push_back(std::move(query));
    }

    if (signature == g_candidate_translation_signature)
        return;
    g_candidate_translation_signature = std::move(signature);
    g_candidate_translation_glosses.clear();
    CloudTranslation::Clear();
    EnglishIme::RequestTranslations(std::move(queries), GetConfiguredTencentTmt().target_language == "en");
}

// Copy the page the worker just finished into an immutable snapshot for the UI thread. This is the single publish
// point: set_items() and clear_page() are only intermediate steps of a rebuild, so publishing there would hand the UI a
// half-built page.
void PublishBuiltCandidatePage(const std::wstring &candidate_string)
{
    const auto &ui = Global::candidate_ui;
    auto snapshot = std::make_shared<Global::CandidatePageSnapshot>();
    snapshot->page_views = ui.page_views;
    snapshot->page_words = ui.page_words;
    snapshot->candidate_string = candidate_string;
    snapshot->selected_index_in_page = ui.selected_index_in_page;
    snapshot->page_count = ui.current_page_count();
    snapshot->page_item_count = ui.cur_page_item_cnt;
    Global::PublishCandidatePageSnapshot(std::move(snapshot));
}

void RefreshCandidatePageUi(bool show_window)
{
    PrepareCandidateTranslationRequest();
    const std::string candidate_string = BuildCurrentCandidatePage();
    // Host-drawn UI wants plain words (Microsoft IME style), not helpcodes.
    const std::wstring published = IsUiLessMode() ? BuildUiLessCandidatePageW() : string_to_wstring(candidate_string);
    ::WriteDataToSharedMemory(published, true);
    PublishBuiltCandidatePage(published);
    CAND_DIAG_LOGF(L"candidate UI refreshed show={} uiless={} items={} page_words={} selected={} page={} "
                   L"serialized_units={}",
                   show_window, IsUiLessMode(), Global::candidate_ui.items.size(),
                   Global::candidate_ui.page_words.size(), Global::candidate_ui.selected_index_in_page,
                   Global::candidate_ui.page_index, candidate_string.size());
    if (show_window)
    {
        RequestShowCandidateWindow();
    }
}

void LogPipeConnectResult(const wchar_t *pipe_name, BOOL connected)
{
    const DWORD gle = connected ? ERROR_SUCCESS : GetLastError();
    CAND_DIAG_LOGF(L"pipe connect name={} connected={} gle={}", pipe_name, connected != FALSE, gle);
    if (connected)
    {
        FANY_IPC_LOGF(L"[msime]: [ipc] {} connected", pipe_name);
    }
    else
    {
        FANY_IPC_LOGF(L"[msime]: [ipc] {} ConnectNamedPipe returned false: gle={}", pipe_name, gle);
    }
}

void LogPipeReadFailure(const wchar_t *pipe_name, DWORD bytes_read)
{
    const DWORD gle = GetLastError();
    FANY_IPC_LOGF(L"[msime]: [ipc] {} ReadFile failed or returned empty: gle={}, bytes_read={}", pipe_name, gle,
                  bytes_read);
    CAND_DIAG_LOGF(L"pipe read failure name={} gle={} bytes={}", pipe_name, gle, bytes_read);
}

void LogPipeDisconnect(const wchar_t *pipe_name)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] {} disconnected", pipe_name);
    CAND_DIAG_LOGF(L"pipe disconnected name={}", pipe_name);
}

void LogPipeEvent(const wchar_t *pipe_name, UINT event_type, UINT keycode, WCHAR wch, UINT modifiers_down)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] {} event: type={}, keycode={}, wch={}, modifiers={}", pipe_name, event_type, keycode,
                  static_cast<unsigned int>(wch), modifiers_down);
}

void LogClientLifecycle(const wchar_t *phase, uint64_t client_id, UINT event_type)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] client lifecycle: phase={}, client_id={}, event_type={}", phase, client_id,
                  event_type);
}

void LogClientRouting(uint64_t client_id, UINT event_type, bool is_active)
{
    FANY_IPC_LOGF(L"[msime]: [ipc] client routing: client_id={}, event_type={}, is_active={}", client_id, event_type,
                  is_active);
}

bool IsImplicitActivationEvent(UINT event_type)
{
    // A plain StatusSnapshot or compartment notification can be a delayed
    // background callback and must never steal routing from the focused TSF
    // client. A real key, and a FocusRestored the tip only emits after
    // ITfThreadMgr::IsThreadFocus confirmed ownership, are unambiguous.
    return event_type == FanyImePipeEventType::KeyEvent || event_type == FanyImePipeEventType::FocusRestored;
}

void SendFocusSessionReady(const PipeClientActivation &activation)
{
    if (!FanyImeIpc::CanSendFocusSessionReady(activation.client_id, activation.epoch, activation.focus_token))
    {
        return;
    }

    // This packet is an ordered focus-session fence on the same worker
    // endpoint used for candidate commits. The activation request id is a TSF
    // focus token and is echoed verbatim; unlike the Server-only epoch, it lets
    // TSF reject a buffered marker from an older focus session.
    SendToTsfWorkerThreadClientViaNamedpipe(activation.client_id, activation.epoch,
                                            Global::DataFromServerMsgTypeToTsfWorkerThread::FocusSessionReady,
                                            std::to_wstring(activation.focus_token));
}

void SendInputModeState(const PipeClientActivation &activation)
{
    if (activation.client_id == 0 || activation.epoch == 0)
    {
        return;
    }

    // Input candidates use the Server's current session/config, whereas the
    // TSF language-bar icon is cached inside every host process.  Re-send the
    // authoritative mode at focus/activation boundaries so a background host
    // that missed the original broadcast cannot retain a stale Japanese or
    // Chinese icon indefinitely.
    SendToTsfWorkerThreadClientViaNamedpipe(activation.client_id, activation.epoch,
                                            Global::DataFromServerMsgTypeToTsfWorkerThread::InputModeChanged,
                                            GetConfiguredInputMode() == "japanese" ? L"1" : L"0");
}

bool IsKnownMainPipeEvent(UINT event_type)
{
    switch (event_type)
    {
    case FanyImePipeEventType::KeyEvent:
    case FanyImePipeEventType::HideCandidateWnd:
    case FanyImePipeEventType::ShowCandidateWnd:
    case FanyImePipeEventType::MoveCandidateWnd:
    // LangbarRightClick is Aux-only (session-less UI); do not accept on Main.
    case FanyImePipeEventType::IMESwitch:
    case FanyImePipeEventType::PuncSwitch:
    case FanyImePipeEventType::DoubleSingleByteSwitch:
    case FanyImePipeEventType::ClientHello:
    case FanyImePipeEventType::ClientActivated:
    case FanyImePipeEventType::ClientDeactivated:
    case FanyImePipeEventType::StatusSnapshot:
    case FanyImePipeEventType::ClientSuspended:
    case FanyImePipeEventType::FocusRestored:
        return true;
    default:
        return false;
    }
}

bool IsValidMainPipeFrame(const FanyImeNamedpipeData &pipe_data)
{
    if (!IsKnownMainPipeEvent(pipe_data.event_type) || pipe_data.pinyin_length < 0 ||
        pipe_data.pinyin_length >= static_cast<int>(std::size(pipe_data.pinyin_string)) ||
        pipe_data.pinyin_string[std::size(pipe_data.pinyin_string) - 1] != L'\0' ||
        pipe_data.pinyin_string[pipe_data.pinyin_length] != L'\0')
    {
        return false;
    }

    if ((pipe_data.event_type == FanyImePipeEventType::StatusSnapshot ||
         pipe_data.event_type == FanyImePipeEventType::FocusRestored) &&
        (pipe_data.keycode > 1 || pipe_data.modifiers_down > 1 || pipe_data.pinyin_length > 1))
    {
        return false;
    }
    if (pipe_data.event_type == FanyImePipeEventType::KeyEvent && pipe_data.request_id == 0)
    {
        return false;
    }
    if (pipe_data.event_type == FanyImePipeEventType::ClientActivated && pipe_data.request_id == 0)
    {
        // FocusSessionReady can never acknowledge token zero. Reject the
        // activation instead of creating a server epoch that TSF cannot fence.
        return false;
    }
    return true;
}

bool WaitForPipeClient(HANDLE pipe)
{
    BOOL connected = ConnectNamedPipe(pipe, NULL);
    if (connected)
    {
        return true;
    }
    return GetLastError() == ERROR_PIPE_CONNECTED;
}

bool PipeClientIdMatchesConnectedProcess(HANDLE pipe, uint64_t client_id)
{
    ULONG client_process_id = 0;
    if (!GetNamedPipeClientProcessId(pipe, &client_process_id))
    {
        // Best effort for older/exceptional hosts. When Windows provides the
        // process identity, however, never accept a spoofed routing id.
        return true;
    }
    return static_cast<DWORD>(client_id >> 32) == static_cast<DWORD>(client_process_id);
}

bool ReadPipeHello(HANDLE pipe, UINT expected_pipe_role, FanyImePipeHello &hello)
{
    if (!SetPipeWaitMode(pipe, false))
    {
        return false;
    }
    DWORD bytesRead = 0;
    const bool readResult = ReadExactPipeMessageUntil(pipe, &hello, sizeof(hello),
                                                      std::chrono::steady_clock::now() + kPipeHelloTimeout, bytesRead);
    return readResult && pipe_running && hello.client_id != 0 && hello.pipe_role == expected_pipe_role &&
           PipeClientIdMatchesConnectedProcess(pipe, hello.client_id);
}

void WakePipeListener(const wchar_t *pipe_name)
{
    for (int retry = 0; retry < 20; ++retry)
    {
        HANDLE wake_pipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake_pipe && wake_pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(wake_pipe);
            return;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY)
        {
            return;
        }
        WaitNamedPipeW(pipe_name, 10);
    }
}

void WakeNamedPipeListenersForShutdown()
{
    WakePipeListener(FANY_IME_NAMED_PIPE);
    WakePipeListener(FANY_IME_TO_TSF_NAMED_PIPE);
    WakePipeListener(FANY_IME_TO_TSF_WORKER_THREAD_NAMED_PIPE);
    WakePipeListener(FANY_IME_AUX_NAMED_PIPE);
    WakePipeListener(FANY_IME_TSF_DIAGNOSTIC_NAMED_PIPE);
}

// The pipe server accepts clients before the candidate window exists, so an
// activation can arrive with nowhere to deliver it. Held here until the window
// creation path can replay it.
std::atomic_bool g_deferred_client_activation{false};
} // namespace

namespace FanyNamedPipe
{
void ReplayDeferredClientActivation()
{
    if (!::global_hwnd)
    {
        return;
    }
    if (!g_deferred_client_activation.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    FTB_DIAG_LOGF(L"replaying client activation deferred until candidate window existed");
    PostMessage(::global_hwnd, WM_IMEACTIVATE, 0, 0);
    PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
}

void CancelCloudCandidateRequest()
{
    UpdateCloudInput("");
}

enum class TaskType
{
    ShowCandidate,
    HideCandidate,
    MoveCandidate,
    ImeKeyEvent,
    LangbarRightClick,
    IMESwitch,
    PuncSwitch,
    DoubleSingleByteSwitch,
    ApplyCloudCandidate,
    ApplyAiCandidate,
    ApplyEnglishCandidates,
    ApplyCandidateTranslations,
    ApplyEmojiCandidates,
    ApplyKaomojiCandidates,
    StoreUserPhrase,
    LearnEnteredEnglishWord,
    PinCandidate,
    ClientActivated,
    ClientDeactivated,
    ClientSuspended,
    StatusSnapshot,
    UiCommitCandidate,
    UiPinCandidate,
    UiDeleteCandidate,
    UiFixCandidatePosition,
    UiClearCandidatePosition,
    ReloadInputSession,
    EnsureInputSessionMatchesConfig,
    ApplyCandidatePageSize,
    RefreshCandidatePage,
    ResetInputSessionCache,
    ExitEnglishInputMode,
};

struct Task
{
    TaskType type;
    bool has_pipe_data = false;
    FanyImeNamedpipeData pipe_data = {};
    uint64_t client_id = 0;
    uint64_t activation_epoch = 0;
    ULONGLONG enqueued_at_ms = 0;
    std::string cloud_candidate;
    std::string cloud_pinyin;
    uint64_t cloud_generation = 0;
    std::string ai_candidate;
    std::string ai_identity;
    uint64_t ai_generation = 0;
    std::optional<metasequoia::OnlineQuery> online_query;
    std::vector<WordItem> english_candidates;
    std::string english_input;
    uint64_t english_generation = 0;
    std::vector<EnglishIme::TranslationResult> translation_results;
    uint64_t translation_generation = 0;
    bool translation_merge = false;
    std::vector<WordItem> emoji_candidates;
    std::string emoji_input;
    uint64_t emoji_generation = 0;
    std::vector<WordItem> kaomoji_candidates;
    std::string kaomoji_input;
    uint64_t kaomoji_generation = 0;
    std::string session_pinyin;
    std::string session_word;
    bool session_pinyin_is_canonical = false;
    int candidate_one_based_index = 0;
    int fixed_position = 0;
};

struct ScopedServerKeyLatency
{
    uint64_t client_id;
    uint64_t activation_epoch;
    uint64_t request_id;
    ULONGLONG started_at_ms = GetTickCount64();

    ~ScopedServerKeyLatency()
    {
        const ULONGLONG elapsed_ms = GetTickCount64() - started_at_ms;
        if (elapsed_ms >= 8)
        {
            DIAG_LOGF(L"[key-latency] side=server stage=handle request={} client={} epoch={} elapsed_ms={}", request_id,
                      client_id, activation_epoch, elapsed_ms);
        }
    }
};

std::string CurrentRankingContextKey()
{
    if (g_inputSession)
    {
        const std::string raw = g_inputSession->get_pinyin_sequence_with_cases();
        if (IsJianpinCompositionActive(raw) && raw.size() > 1)
            return metasequoia::local_modes::jianpin_ranking_context(
                raw.substr(1), g_inputSession->current_scheme_type(), ConfiguredShuangpinProfile());
    }
    std::string converted = g_inputSession->get_quanpin();
    if (converted.empty())
        converted = g_inputSession->get_pinyin_segmentation();
    if (g_inputSession->get_pinyin_sequence().size() == 1)
        return converted;
    std::string plain = converted;
    plain.erase(std::remove(plain.begin(), plain.end(), '\''), plain.end());
    const auto cuts = quanpin::cut_pinyin_by_mode(plain, "correction");
    return cuts.empty() ? converted : quanpin::join_segments(cuts.front());
}

std::string EnglishRankingContextKey()
{
    std::string key = g_inputSession->get_pinyin_sequence_with_cases();
    if (IsYModeInput(key))
        key = key.substr(1);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return "english:" + key;
}

std::string CandidateDatabaseKey(const WordItem &item, const std::string &context_key)
{
    if (!item.canonical_pinyin.empty())
        return item.canonical_pinyin;
    if (g_inputSession->get_pinyin_sequence().size() == 1)
        return item.pinyin;
    auto segments = quanpin::split_segments(context_key);
    const size_t han_count = HelpcodeUtils::count_han_chars(item.word);
    if (segments.empty() || han_count == 0)
        return item.pinyin;
    if (segments.size() > han_count)
        segments.resize(han_count);
    return quanpin::join_segments(segments);
}

bool IsWubiRankingScheme()
{
    return g_inputSession && g_inputSession->current_scheme_type() == SchemeType::Wubi;
}

std::pair<std::string, std::string> RankingKeysForCandidate(const WordItem &item)
{
    if (IsWubiRankingScheme())
    {
        const std::string key =
            item.pinyin.empty() && g_inputSession ? g_inputSession->get_pinyin_sequence() : item.pinyin;
        return {key, item.pinyin};
    }
    const std::string context_key = CurrentRankingContextKey();
    return {context_key, CandidateDatabaseKey(item, context_key)};
}

std::queue<Task> taskQueue;
std::mutex queueMutex;

void PrepareCandidateList(uint64_t client_id, uint64_t activation_epoch);
void HandleImeKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id);
void ClearState();
void ProcessSelectionKey(UINT keycode, uint64_t client_id, uint64_t activation_epoch, int forced_index_in_page = -1);
void ApplyCloudCandidate(const std::string &candidate, const std::string &pinyin, uint64_t generation,
                         const std::optional<metasequoia::OnlineQuery> &query);
void ApplyAiCandidate(const std::string &candidate, const std::string &identity, uint64_t generation,
                      const std::optional<metasequoia::OnlineQuery> &query);
void ApplyEnglishCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation);
void ApplyCandidateTranslations(std::vector<EnglishIme::TranslationResult> results, uint64_t generation, bool merge);
void ApplyEmojiCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation);
void ApplyKaomojiCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation);
void EnqueueStoreUserPhraseTask(const std::string &pinyin, const std::string &word, bool pinyin_is_canonical = false);
void EnqueuePinCandidateTask(const std::string &pinyin, const std::string &word);
bool ResolveCandidateItem(int one_based_index, WordItem &item);
bool SendCurrentDataToClient(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id);
void MainPipeClientThread(HANDLE clientPipe, uint64_t handlerId);
void RegisteredPipeMonitorThread(HANDLE clientPipe, UINT pipeRole, uint64_t handlerId);

void WorkerThread()
{
    while (pipe_running)
    {
        Task task;
        {
            std::unique_lock lock(queueMutex);
            pipe_queueCv.wait(lock, [] { return !taskQueue.empty() || !pipe_running; });
            if (!pipe_running)
                break;
            task = taskQueue.front();
            taskQueue.pop();
        }

        if (task.type == TaskType::ClientDeactivated || task.type == TaskType::ClientSuspended)
        {
            if (!IsPipeActivationCurrent(0, task.activation_epoch))
            {
                FTB_DIAG_LOGF(L"task {} epoch={} rejected as stale",
                              task.type == TaskType::ClientDeactivated ? L"ClientDeactivated" : L"ClientSuspended",
                              task.activation_epoch);
                CAND_DIAG_LOGF(L"candidate lifecycle task rejected stale type={} epoch={}",
                               task.type == TaskType::ClientDeactivated ? L"deactivated" : L"suspended",
                               task.activation_epoch);
                continue;
            }
        }
        else if (task.client_id != 0 && task.activation_epoch != 0 &&
                 !IsPipeActivationCurrent(task.client_id, task.activation_epoch))
        {
            // Every task carrying an owner is rejected after a focus/session
            // transition, including UI-originated candidate actions.
            continue;
        }

        const bool candidateUiAction =
            task.type == TaskType::UiCommitCandidate || task.type == TaskType::UiPinCandidate ||
            task.type == TaskType::UiDeleteCandidate || task.type == TaskType::UiFixCandidatePosition ||
            task.type == TaskType::UiClearCandidatePosition;
        if (candidateUiAction && !CandidateUiOwnerIsCurrent({task.client_id, task.activation_epoch}))
        {
            // The page was hidden or replaced after the click was posted.
            continue;
        }

        if (task.has_pipe_data)
        {
            namedpipeData = task.pipe_data;
            ApplyUiLessFromPacket(namedpipeData);
        }

        switch (task.type)
        {
        case TaskType::ShowCandidate: {
            static int cnt = 0;
            // A TSF ShowCandidate packet owns a complete candidate payload,
            // including the caret anchor. Read it here rather than inside
            // PrepareCandidateList: that function is also used for server-side
            // refreshes (creating-word progress and candidate context-menu
            // actions), whose current packet does not carry a valid point.
            ::ReadDataFromNamedPipe(0b111111);
            CAND_DIAG_LOGF(L"task ShowCandidate client={} epoch={} request={} caret=({},{}) input_units={}",
                           task.client_id, task.activation_epoch, task.pipe_data.request_id, Global::Point[0],
                           Global::Point[1], GlobalIme::composition.raw_input_with_cases.size());
            PrepareCandidateList(task.client_id, task.activation_epoch);
            RequestShowCandidateWindow();
            break;
        }

        case TaskType::HideCandidate: {
            ::ReadDataFromNamedPipe(0b100000);
            const ULONGLONG queue_elapsed_ms = task.enqueued_at_ms == 0 ? 0 : GetTickCount64() - task.enqueued_at_ms;
            CAND_DIAG_LOGF(L"task HideCandidate client={} epoch={} request={} queued_ms={}", task.client_id,
                           task.activation_epoch, task.pipe_data.request_id, queue_elapsed_ms);
            // Only a hide this thread delivered late can belong to a keystroke the
            // user has already typed past — that is the one worth holding briefly,
            // because a show for a later keystroke is right behind it. A hide
            // delivered on time is a real commit or focus loss and must take effect
            // now; delaying it leaves the candidate window hanging after the word is
            // already on screen, which is exactly the snap that typing loses.
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, queue_elapsed_ms >= kCandidateHideBacklogMs ? 1 : 0, 0);
            /* 清理状态 */
            ClearState();
            break;
        }

        case TaskType::MoveCandidate: {
            static int cnt = 0;
            ::ReadDataFromNamedPipe(0b001000);
            CAND_DIAG_LOGF(L"task MoveCandidate client={} epoch={} caret=({},{})", task.client_id,
                           task.activation_epoch, Global::Point[0], Global::Point[1]);
            bool expected = false;
            if (!g_candidate_move_msg_pending.compare_exchange_strong(expected, true))
            {
                CAND_DIAG_LOGF(L"move request coalesced caret=({},{})", Global::Point[0], Global::Point[1]);
                break;
            }
            if (!PostMessage(::global_hwnd, WM_MOVE_CANDIDATE_WINDOW, 0, 0))
            {
                g_candidate_move_msg_pending.store(false);
            }
            break;
        }

        case TaskType::ImeKeyEvent: {
            const ULONGLONG queue_elapsed_ms = task.enqueued_at_ms == 0 ? 0 : GetTickCount64() - task.enqueued_at_ms;
            if (queue_elapsed_ms >= 8)
            {
                DIAG_LOGF(L"[key-latency] side=server stage=queue request={} client={} epoch={} elapsed_ms={}",
                          task.pipe_data.request_id, task.client_id, task.activation_epoch, queue_elapsed_ms);
            }
            HandleImeKey(task.client_id, task.activation_epoch, task.pipe_data.request_id);
            break;
        }

        case TaskType::LangbarRightClick: {
            ::ReadDataFromNamedPipe(0b001101);
            PostMessage(::global_hwnd_menu, WM_LANGBAR_RIGHTCLICK, 0, 0);
            break;
        }

        case TaskType::IMESwitch: {
            PostMessage(::global_hwnd, WM_IMESWITCH, task.pipe_data.keycode, 0);
            break;
        }

        case TaskType::PuncSwitch: {
            PostMessage(::global_hwnd, WM_PUNCSWITCH, task.pipe_data.keycode, 0);
            break;
        }

        case TaskType::DoubleSingleByteSwitch: {
            PostMessage(::global_hwnd, WM_DOUBLESINGLEBYTESWITCH, task.pipe_data.keycode, 0);
            break;
        }

        case TaskType::ApplyCloudCandidate: {
            ApplyCloudCandidate(task.cloud_candidate, task.cloud_pinyin, task.cloud_generation, task.online_query);
            break;
        }

        case TaskType::ApplyAiCandidate: {
            ApplyAiCandidate(task.ai_candidate, task.ai_identity, task.ai_generation, task.online_query);
            break;
        }

        case TaskType::ApplyEnglishCandidates: {
            ApplyEnglishCandidates(std::move(task.english_candidates), task.english_input, task.english_generation);
            break;
        }

        case TaskType::ApplyCandidateTranslations: {
            ApplyCandidateTranslations(std::move(task.translation_results), task.translation_generation,
                                       task.translation_merge);
            break;
        }

        case TaskType::ApplyEmojiCandidates: {
            ApplyEmojiCandidates(std::move(task.emoji_candidates), task.emoji_input, task.emoji_generation);
            break;
        }

        case TaskType::ApplyKaomojiCandidates: {
            ApplyKaomojiCandidates(std::move(task.kaomoji_candidates), task.kaomoji_input, task.kaomoji_generation);
            break;
        }

        case TaskType::StoreUserPhrase: {
            const auto session = PersistentInputSession();
            if (task.session_pinyin_is_canonical)
            {
                session->store_user_phrase_from_canonical_pinyin(task.session_pinyin, task.session_word);
            }
            else
            {
                session->store_user_phrase(task.session_pinyin, task.session_word);
            }
            session->reset_cache();
            break;
        }

        case TaskType::LearnEnteredEnglishWord: {
            (void)user_dictionary::learn_entered_english_word(CommonUtils::get_ime_data_path() + "\\english.db",
                                                              user_dictionary::default_user_db_path(),
                                                              task.session_word);
            break;
        }

        case TaskType::PinCandidate: {
            const auto session = PersistentInputSession();
            session->pin_candidate(task.session_pinyin, task.session_word);
            session->reset_cache();
            break;
        }

        case TaskType::ClientActivated: {
            CAND_DIAG_LOGF(L"client activated client={} epoch={} hwnd_present={}", task.client_id,
                           task.activation_epoch, ::global_hwnd != nullptr);
            VoiceInput::SetImeActive(true);
            // Activation replaces all composition/candidate state from the
            // previous focus session. A terminal TIP activation also makes
            // the configured floating toolbar visible. Re-activation after a
            // suspension is idempotent and therefore does not flash it.
            // PostMessage to a null window is not a no-op: it delivers to this
            // pipe thread's own queue, where nothing reads it. Defer instead, so
            // an activation that races window creation is replayed rather than
            // swallowed.
            if (::global_hwnd)
            {
                PostMessage(::global_hwnd, WM_IMEACTIVATE, 0, 0);
                PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            }
            else
            {
                g_deferred_client_activation.store(true, std::memory_order_release);
            }
            ClearState();

            // Show the incoming client's own mode now rather than the outgoing
            // client's, which would otherwise stay up for the whole handoff and
            // indefinitely if this client never sends another snapshot.
            int remembered = RecallClientStatusSnapshot(task.client_id);
            if (remembered >= 0)
            {
                ReloadImeConfigIfChanged();
                if (IsConfiguredImeModeScopeGlobal())
                {
                    remembered = (EnsureAuthoritativeCnMode() << 2) | (remembered & 0x3);
                }
                PublishStatusSnapshotValue(remembered);
            }
            break;
        }

        case TaskType::ClientDeactivated: {
            CAND_DIAG_LOGF(L"client deactivated client={} epoch={}", task.client_id, task.activation_epoch);
            VoiceInput::SetImeActive(false);
            // Unlike a route-only suspension, terminal TIP deactivation means
            // the user switched to another input method. Forget the previous
            // global authority so switching back starts from default_ime_mode
            // instead of restoring the mode used before deactivation.
            g_authoritative_cn_mode = -1;
            g_force_global_ime_sync = true;
            // Supersede any activation still waiting for the window, otherwise
            // the replay would resurrect a toolbar the user just switched away
            // from. Without a window the flag is already false, so only the
            // deferred activation needs cancelling.
            g_deferred_client_activation.store(false, std::memory_order_release);
            if (::global_hwnd)
            {
                PostMessage(::global_hwnd, WM_IMEDEACTIVATE, 0, 0);
                PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            }
            ClearState();
            break;
        }

        case TaskType::ClientSuspended: {
            CAND_DIAG_LOGF(L"client suspended client={} epoch={}", task.client_id, task.activation_epoch);
            // A suspension rotates the IPC focus session while the TIP may
            // still own thread focus. It clears candidates just like terminal
            // deactivation, but never changes floating-toolbar visibility.
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            ClearState();
            break;
        }

        case TaskType::StatusSnapshot: {
            ReloadImeConfigIfChanged();
            const int cn_state = task.pipe_data.keycode != 0 ? 1 : 0;
            const int fullwidth_state = task.pipe_data.modifiers_down != 0 ? 1 : 0;
            const int punctuation_state = task.pipe_data.pinyin_length != 0 ? 1 : 0;
            int effective_cn = cn_state;
            const bool force_global_sync = g_force_global_ime_sync;
            g_force_global_ime_sync = false;

            // client_id is pid<<32|tid, so every window of one Chromium/Electron
            // host reports the same id: a change here means the focused TSF
            // thread changed, not merely the focused window.

            if (IsConfiguredImeModeScopeGlobal())
            {
                const int authoritative = EnsureAuthoritativeCnMode();
                const bool client_changed =
                    g_last_status_snapshot_client_id != 0 && task.client_id != g_last_status_snapshot_client_id;
                if (client_changed || force_global_sync)
                {
                    // Focus moved to another app: keep the unified mode. After
                    // switching back from another IME, the authority was reset
                    // on ClientDeactivated and is seeded from default_ime_mode.
                    effective_cn = authoritative;
                    if (cn_state != authoritative && task.client_id != 0)
                    {
                        // The toolbar is moved to the authority below whether or
                        // not the tip accepts this packet, so a rejected switch
                        // leaves the two indicators disagreeing.
                        SendToTsfWorkerThreadClientViaNamedpipe(
                            task.client_id, task.activation_epoch,
                            authoritative != 0 ? Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToCn
                                               : Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToEn,
                            L"");
                    }
                }
                else
                {
                    // Same client (or first report): accept as the new authority.
                    g_authoritative_cn_mode = cn_state;
                    effective_cn = cn_state;
                }
            }
            else
            {
                g_authoritative_cn_mode = cn_state;
            }

            if (task.client_id != 0)
            {
                g_last_status_snapshot_client_id = task.client_id;
            }

            const int caps_state = GetServerCapsLockState();
            const int packed_state =
                (caps_state << 3) | (effective_cn << 2) | (fullwidth_state << 1) | punctuation_state;
            if (FanyImeIpc::ShouldResetCompositionForImeMode(effective_cn != 0))
            {
                if (effective_cn == 0)
                    SetEnglishInputMode(false);
                PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
                ClearState();
            }
            RememberClientStatusSnapshot(task.client_id, packed_state);
            PublishStatusSnapshotValue(packed_state);
            break;
        }

        case TaskType::ExitEnglishInputMode: {
            if (g_english_input_mode)
            {
                SetEnglishInputMode(false);
                PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
                ClearState();
            }
            break;
        }

        case TaskType::UiCommitCandidate: {
            ProcessSelectionKey(0, task.client_id, task.activation_epoch, task.candidate_one_based_index - 1);
            if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::Normal)
            {
                // The worker packet is the complete edit-session-owned commit
                // for a normal UI click. Do not also leave an unsolicited
                // request-id-0 reply on the normal reverse pipe.
                if (SendToTsfWorkerThreadClientViaNamedpipe(
                        task.client_id, task.activation_epoch,
                        Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidate,
                        Global::candidate_ui.selected_text))
                {
                    ClearState();
                }
            }
            else if (SendCurrentDataToClient(task.client_id, task.activation_epoch, 0))
            {
                // NeedToCreateWord/OutOfRange require the normal reply's
                // subtype. An empty worker packet is only the ordered trigger;
                // TSF consumes (rather than discards) the id-0 reply.
                SendToTsfWorkerThreadClientViaNamedpipe(task.client_id, task.activation_epoch,
                                                        Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidate,
                                                        L"");
            }
            break;
        }

        case TaskType::UiPinCandidate:
        case TaskType::UiDeleteCandidate:
        case TaskType::UiFixCandidatePosition:
        case TaskType::UiClearCandidatePosition: {
            WordItem item;
            if (!ResolveCandidateItem(task.candidate_one_based_index, item) ||
                item.source == CandidateSource::QuickPhrase || item.source == CandidateSource::Emoji ||
                item.source == CandidateSource::Kaomoji || item.source == CandidateSource::Generated)
            {
                break;
            }

            const bool english_candidate = item.source == CandidateSource::EnglishDictionary;
            const auto ranking_keys = RankingKeysForCandidate(item);
            const std::string context_key = english_candidate ? EnglishRankingContextKey() : ranking_keys.first;
            const std::string entry_key = english_candidate ? item.pinyin : ranking_keys.second;

            if (task.type == TaskType::UiPinCandidate)
            {
                if (english_candidate)
                    (void)user_dictionary::adjust_english_candidate_ranking(
                        CommonUtils::get_ime_data_path() + "\\english.db", user_dictionary::default_user_db_path(),
                        context_key, Global::candidate_ui.items, entry_key, item.word, "pin", 1, 1, true);
                else
                    (void)user_dictionary::adjust_candidate_ranking(
                        CommonUtils::get_ime_data_path() + "\\msime.db", user_dictionary::default_user_db_path(),
                        context_key, Global::candidate_ui.items, entry_key, item.word, "pin", 1, 1, true, nullptr,
                        IsWubiRankingScheme() ? user_dictionary::DictionaryKind::Wubi
                                              : user_dictionary::DictionaryKind::Pinyin);
            }
            else if (task.type == TaskType::UiDeleteCandidate)
            {
                if (english_candidate)
                    (void)user_dictionary::delete_english_candidate(CommonUtils::get_ime_data_path() + "\\english.db",
                                                                    user_dictionary::default_user_db_path(), entry_key,
                                                                    item.word);
                else if (utf8::distance(item.word.begin(), item.word.end()) == 1)
                {
                    break;
                }
                else
                {
                    // Pinyin candidates carry both the typed code and, when
                    // available, the canonical quanpin database key.  Delete
                    // with the canonical key so a raw shuangpin sequence is
                    // not mistaken for an equally valid quanpin spelling.
                    const std::string delete_pinyin =
                        item.canonical_pinyin.empty() ? item.pinyin : item.canonical_pinyin;
                    g_inputSession->remove_candidate(delete_pinyin, item.word);
                }
            }
            else if (task.type == TaskType::UiFixCandidatePosition)
            {
                (void)user_dictionary::set_fixed_position(user_dictionary::default_user_db_path(), context_key,
                                                          entry_key, item.word, task.fixed_position);
            }
            else
            {
                (void)user_dictionary::clear_fixed_position(user_dictionary::default_user_db_path(), context_key,
                                                            entry_key, item.word);
            }
            g_inputSession->reset_cache();
            g_inputSession->recompute_candidates();
            PrepareCandidateList(task.client_id, task.activation_epoch);
            RequestShowCandidateWindow();
            break;
        }

        case TaskType::ReloadInputSession: {
            ClearState();
            Global::candidate_ui.page_size = GetConfiguredCandidatePageSize();
            g_inputSession = CreateInputSessionFromConfig();
            Global::candidate_ui.set_items({});
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            break;
        }

        case TaskType::EnsureInputSessionMatchesConfig: {
            const SchemeType wanted = GetConfiguredActiveInputScheme();
            const bool has_session = g_inputSession != nullptr;
            const bool configured_scheme_matches = has_session && g_inputSession->current_scheme_type() == wanted;
            const bool session_is_japanese =
                has_session && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji;
            if (FanyImeIpc::InputSessionMatchesConfig(configured_scheme_matches, g_r_mode_triggered,
                                                      session_is_japanese))
            {
                break;
            }
            ClearState();
            Global::candidate_ui.page_size = GetConfiguredCandidatePageSize();
            g_inputSession = CreateInputSessionFromConfig();
            Global::candidate_ui.set_items({});
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
            break;
        }

        case TaskType::ApplyCandidatePageSize: {
            const int pageSize = GetConfiguredCandidatePageSize();
            if (Global::candidate_ui.page_size != pageSize)
            {
                Global::candidate_ui.page_size = pageSize;
                Global::candidate_ui.page_index = 0;
                Global::candidate_ui.clear_page();
                const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
                if (owner && IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
                {
                    RefreshCandidatePageUi(true);
                }
            }
            break;
        }

        case TaskType::RefreshCandidatePage: {
            if (!Global::candidate_ui.items.empty())
            {
                Global::candidate_ui.clear_page();
                const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
                if (owner && IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
                {
                    RefreshCandidatePageUi(true);
                }
            }
            break;
        }

        case TaskType::ResetInputSessionCache: {
            const auto session = PersistentInputSession();
            if (session)
            {
                session->reset_cache();
            }
            break;
        }
        }
    }

    ShutdownPipeClients();
    WakeNamedPipeListenersForShutdown();
}

void RegisterStatusSnapshotWindow(HWND toolbar_window)
{
    std::lock_guard lock(g_status_snapshot_mutex);
    g_status_snapshot_window = toolbar_window;
    if (g_latest_status_snapshot >= 0 && g_status_snapshot_window && IsWindow(g_status_snapshot_window))
    {
        PostMessage(g_status_snapshot_window, UPDATE_FTB_STATUS, g_latest_status_snapshot, 0);
    }
    if (g_status_snapshot_window && IsWindow(g_status_snapshot_window))
    {
        PostMessage(g_status_snapshot_window, UPDATE_FTB_ENGLISH_INPUT_MODE, g_latest_english_input_mode ? 1 : 0, 0);
    }
}

void EnqueueTask(TaskType type, const FanyImeNamedpipeData &pipeData, uint64_t activation_epoch)
{
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = type;
        task.has_pipe_data = true;
        task.pipe_data = pipeData;
        task.client_id = pipeData.client_id;
        task.activation_epoch = activation_epoch;
        task.enqueued_at_ms = GetTickCount64();
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueCloudCandidate(const std::string &candidate, const std::string &pinyin, uint64_t generation)
{
    const AsyncRequestOrigin origin = FindCloudRequestOrigin(pinyin, generation);
    if (origin.client_id == 0 || origin.activation_epoch == 0)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyCloudCandidate;
        task.cloud_candidate = candidate;
        task.cloud_pinyin = pinyin;
        task.cloud_generation = generation;
        task.online_query = origin.engine_query;
        task.client_id = origin.client_id;
        task.activation_epoch = origin.activation_epoch;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueAiCandidate(const std::string &candidate, const std::string &identity, uint64_t generation)
{
    const AsyncRequestOrigin origin = FindAiRequestOrigin(identity, generation);
    if (origin.client_id == 0 || origin.activation_epoch == 0)
        return;
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyAiCandidate;
        task.ai_candidate = candidate;
        task.ai_identity = identity;
        task.ai_generation = generation;
        task.online_query = origin.engine_query;
        task.client_id = origin.client_id;
        task.activation_epoch = origin.activation_epoch;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueEnglishCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)
{
    const AsyncRequestOrigin origin = FindEnglishRequestOrigin(input, generation);
    if (origin.client_id == 0 || origin.activation_epoch == 0)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyEnglishCandidates;
        task.english_candidates = std::move(candidates);
        task.english_input = input;
        task.english_generation = generation;
        task.client_id = origin.client_id;
        task.activation_epoch = origin.activation_epoch;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueCandidateTranslations(std::vector<EnglishIme::TranslationResult> results, uint64_t generation, bool merge)
{
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyCandidateTranslations;
        task.translation_results = std::move(results);
        task.translation_generation = generation;
        task.translation_merge = merge;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueEmojiCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)
{
    const AsyncRequestOrigin origin = FindEmojiRequestOrigin(input, generation);
    if (origin.client_id == 0 || origin.activation_epoch == 0)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyEmojiCandidates;
        task.emoji_candidates = std::move(candidates);
        task.emoji_input = input;
        task.emoji_generation = generation;
        task.client_id = origin.client_id;
        task.activation_epoch = origin.activation_epoch;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueKaomojiCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)
{
    const AsyncRequestOrigin origin = FindKaomojiRequestOrigin(input, generation);
    if (origin.client_id == 0 || origin.activation_epoch == 0)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyKaomojiCandidates;
        task.kaomoji_candidates = std::move(candidates);
        task.kaomoji_input = input;
        task.kaomoji_generation = generation;
        task.client_id = origin.client_id;
        task.activation_epoch = origin.activation_epoch;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueStoreUserPhraseTask(const std::string &pinyin, const std::string &word, bool pinyin_is_canonical)
{
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::StoreUserPhrase;
        task.session_pinyin = pinyin;
        task.session_word = word;
        task.session_pinyin_is_canonical = pinyin_is_canonical;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueLearnEnteredEnglishWordTask(const std::string &word)
{
    if (word.empty())
        return;
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::LearnEnteredEnglishWord;
        task.session_word = word;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueuePinCandidateTask(const std::string &pinyin, const std::string &word)
{
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::PinCandidate;
        task.session_pinyin = pinyin;
        task.session_word = word;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueuePipeSessionInvalidatedTask(uint64_t client_id, uint64_t invalidation_epoch)
{
    if (!pipe_running || client_id == 0 || invalidation_epoch == 0)
    {
        return;
    }

    FanyImeNamedpipeData disconnectData = {};
    disconnectData.event_type = FanyImePipeEventType::ClientSuspended;
    disconnectData.client_id = client_id;
    EnqueueTask(TaskType::ClientSuspended, disconnectData, invalidation_epoch);
}

void EnqueueCandidateUiAction(CandidateUiAction action, int one_based_index, int fixed_position)
{
    if (!pipe_running || one_based_index <= 0 || one_based_index > 10)
    {
        return;
    }

    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner)
    {
        return;
    }

    TaskType type = TaskType::UiCommitCandidate;
    if (action == CandidateUiAction::Pin)
    {
        type = TaskType::UiPinCandidate;
    }
    else if (action == CandidateUiAction::Delete)
    {
        type = TaskType::UiDeleteCandidate;
    }
    else if (action == CandidateUiAction::FixPosition)
    {
        if (fixed_position < 1 || fixed_position > 5)
            return;
        type = TaskType::UiFixCandidatePosition;
    }
    else if (action == CandidateUiAction::ClearPosition)
    {
        type = TaskType::UiClearCandidatePosition;
    }

    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = type;
        task.client_id = owner.client_id;
        task.activation_epoch = owner.activation_epoch;
        task.candidate_one_based_index = one_based_index;
        task.fixed_position = fixed_position;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueReloadInputSessionTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ReloadInputSession;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueEnsureInputSessionMatchesConfigTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::EnsureInputSessionMatchesConfig;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueApplyCandidatePageSizeTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyCandidatePageSize;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueRefreshCandidatePageTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::RefreshCandidatePage;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueResetInputSessionCacheTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ResetInputSessionCache;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

void EnqueueExitEnglishInputModeTask()
{
    if (!pipe_running)
    {
        return;
    }
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ExitEnglishInputMode;
        taskQueue.push(std::move(task));
    }
    pipe_queueCv.notify_one();
}

bool SendCurrentDataToClient(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id)
{
    const UINT msg_type = Global::MsgTypeToTsf;
    FANY_IPC_LOGF(L"[msime]: [ipc] send-current-data: msg_type={}, text={}", msg_type,
                  ::Global::candidate_ui.selected_text);
    const ULONGLONG send_started_at_ms = GetTickCount64();
    const bool sent = SendToTsfClientViaNamedpipe(client_id, activation_epoch, msg_type, request_id,
                                                  ::Global::candidate_ui.selected_text);
    const ULONGLONG send_elapsed_ms = GetTickCount64() - send_started_at_ms;
    if (!sent || send_elapsed_ms >= 8)
    {
        DIAG_LOGF(L"[key-latency] side=server stage=reply-send request={} client={} epoch={} elapsed_ms={} sent={}",
                  request_id, client_id, activation_epoch, send_elapsed_ms, sent);
    }
    if (sent &&
        (msg_type == Global::DataFromServerMsgType::Normal ||
         msg_type == Global::DataFromServerMsgType::CommitExactText) &&
        IsPipeActivationCurrent(client_id, activation_epoch))
    {
        ClearState();
    }
    return sent;
}

bool SendUiLessCompositionToClient(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id)
{
    std::wstring preedit;
    if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
    {
        preedit = GetPreedit();
    }
    const std::wstring page = BuildUiLessCandidatePageW();
    ::WriteDataToSharedMemory(page, true);
    auto &ui = Global::candidate_ui;
    const int selection = ui.page_words.empty()
                              ? 0
                              : std::clamp(ui.selected_index_in_page, 0, static_cast<int>(ui.page_words.size()) - 1);
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::UiLessComposition;
    Global::candidate_ui.selected_text = preedit + L'\t' + page + L'\t' + std::to_wstring(selection);
    return SendCurrentDataToClient(client_id, activation_epoch, request_id);
}

void EventListenerLoopThread()
{
    HANDLE listeningPipe = hPipe;
    hPipe = INVALID_HANDLE_VALUE;

    while (pipe_running)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
        {
            listeningPipe = CreateMainNamedPipeInstance();
            if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
            {
                FANY_IPC_LOGF(L"[msime]: [ipc] failed to create next main-pipe instance: gle={}", GetLastError());
                Sleep(50);
                continue;
            }
        }

        BOOL connected = WaitForPipeClient(listeningPipe);
        LogPipeConnectResult(L"main-pipe", connected);
        if (connected)
        {
            if (!pipe_running)
            {
                DisconnectNamedPipe(listeningPipe);
                CloseHandle(listeningPipe);
                listeningPipe = INVALID_HANDLE_VALUE;
                break;
            }
            HANDLE clientPipe = listeningPipe;
            listeningPipe = CreateMainNamedPipeInstance();
            const uint64_t handlerId = BeginPipeClientHandler(clientPipe);
            if (handlerId == 0)
            {
                DisconnectNamedPipe(clientPipe);
                CloseHandle(clientPipe);
            }
            else
            {
                try
                {
                    std::thread(MainPipeClientThread, clientPipe, handlerId).detach();
                }
                catch (...)
                {
                    EndPipeClientHandler(handlerId);
                    DisconnectNamedPipe(clientPipe);
                    CloseHandle(clientPipe);
                }
            }
        }
        else
        {
            CloseHandle(listeningPipe);
            listeningPipe = INVALID_HANDLE_VALUE;
        }
    }

    if (listeningPipe && listeningPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(listeningPipe);
    }
}

void MainPipeClientThread(HANDLE clientPipe, uint64_t handlerId)
{
    ScopedPipeClientHandler handler(handlerId);
    uint64_t clientId = 0;
    uint64_t mainRegistrationId = 0;
    bool helloReceived = false;
    if (!SetPipeWaitMode(clientPipe, false))
    {
        DisconnectNamedPipe(clientPipe);
        CloseHandle(clientPipe);
        return;
    }
    while (pipe_running)
    {
        FanyImeNamedpipeData pipeData = {};
        DWORD bytesRead = 0;
        const BOOL readResult =
            helloReceived ? ReadFile(clientPipe, &pipeData, sizeof(pipeData), &bytesRead, nullptr)
                          : ReadExactPipeMessageUntil(clientPipe, &pipeData, sizeof(pipeData),
                                                      std::chrono::steady_clock::now() + kPipeHelloTimeout, bytesRead);
        if (!readResult || bytesRead != sizeof(pipeData))
        {
            LogPipeReadFailure(L"main-pipe", bytesRead);
            break;
        }
        if (!pipe_running)
        {
            break;
        }
        if (!IsValidMainPipeFrame(pipeData))
        {
            FANY_IPC_LOGF(L"[msime]: [ipc] rejected malformed main-pipe frame: type={}, client_id={}, pinyin_length={}",
                          pipeData.event_type, pipeData.client_id, pipeData.pinyin_length);
            break;
        }

        if (!helloReceived)
        {
            if (pipeData.event_type != FanyImePipeEventType::ClientHello || pipeData.client_id == 0 ||
                !PipeClientIdMatchesConnectedProcess(clientPipe, pipeData.client_id))
            {
                FANY_IPC_LOGF(L"[msime]: [ipc] rejected main pipe without a valid hello: type={}, client_id={}",
                              pipeData.event_type, pipeData.client_id);
                break;
            }
            clientId = pipeData.client_id;
            LogClientLifecycle(L"hello", clientId, pipeData.event_type);
            mainRegistrationId = RegisterMainPipeClient(clientId, clientPipe);
            if (mainRegistrationId == 0)
            {
                break;
            }
            if (!NegotiateMainPipeClient(pipeData, mainRegistrationId))
            {
                break;
            }
            if (!pipe_running || !SetPipeWaitMode(clientPipe, true))
            {
                break;
            }
            helloReceived = true;
            continue;
        }

        if (pipeData.client_id != clientId)
        {
            FANY_IPC_LOGF(L"[msime]: [ipc] rejected client-id change on main pipe: pinned={}, received={}", clientId,
                          pipeData.client_id);
            break;
        }
        if (!IsPipeClientRegistrationCurrent(clientId, FanyImePipeRole::Main, mainRegistrationId))
        {
            break;
        }
        if (pipeData.event_type == FanyImePipeEventType::ClientHello)
        {
            // A repeated hello from the same pinned connection is harmless.
            continue;
        }
        if (pipeData.event_type == FanyImePipeEventType::ClientActivated)
        {
            LogClientLifecycle(L"activated", clientId, pipeData.event_type);
            const PipeClientActivation activation =
                ActivatePipeClient(clientId, mainRegistrationId, true, pipeData.request_id, true);
            // client_id 0 means the reverse pipes never reported ready inside the
            // 100ms budget, so the whole packet is about to be discarded.
            SendFocusSessionReady(activation);
            SendInputModeState(activation);
            if (activation.changed)
            {
                EnqueueTask(TaskType::ClientActivated, pipeData, activation.epoch);
            }
            continue;
        }
        if (FanyImePipeEventType::IsRouteDeactivation(pipeData.event_type))
        {
            const bool terminalDeactivation = FanyImePipeEventType::IsTerminalDeactivation(pipeData.event_type);
            LogClientLifecycle(terminalDeactivation ? L"deactivated" : L"suspended", clientId, pipeData.event_type);
            uint64_t deactivationEpoch = DeactivatePipeClient(clientId, mainRegistrationId);
            if (terminalDeactivation && deactivationEpoch == 0)
            {
                // ClientSuspended may already have put routing into the
                // inactive state. Preserve exact terminal cleanup for that
                // owner; a subsequent activation makes this task stale.
                deactivationEpoch = ResolvePipeClientTerminalDeactivationEpoch(clientId);
            }
            if (deactivationEpoch != 0)
            {
                EnqueueTask(terminalDeactivation ? TaskType::ClientDeactivated : TaskType::ClientSuspended, pipeData,
                            deactivationEpoch);
            }
            continue;
        }

        PipeClientActivation activation = GetActivePipeClient();
        if (IsImplicitActivationEvent(pipeData.event_type))
        {
            activation = ActivatePipeClient(clientId, mainRegistrationId, false);
            // A real key can be the first observable foreground signal after
            // Win+. returns, before the TSF reconnect timer has replayed its
            // explicit activation. FocusRestored plays the same role when
            // document focus returns to a client that never lost its session
            // and therefore never re-activates. Fence the worker stream before
            // enqueueing the corresponding task. Repeated markers for one
            // epoch are intentional and harmless.
            SendFocusSessionReady(activation);
            if (pipeData.event_type == FanyImePipeEventType::FocusRestored)
            {
                SendInputModeState(activation);
            }
            if (activation.changed)
            {
                EnqueueTask(TaskType::ClientActivated, pipeData, activation.epoch);
            }
        }

        const bool isActiveClient =
            activation.client_id == clientId && activation.epoch != 0 && IsActivePipeClient(clientId, activation.epoch);
        LogClientRouting(clientId, pipeData.event_type, isActiveClient);
        if (!isActiveClient)
        {
            FANY_IPC_LOGF(L"[msime]: [ipc] ignored inactive main-pipe event: client_id={}, type={}", clientId,
                          pipeData.event_type);
            CAND_DIAG_LOGF(L"main-pipe event ignored inactive client={} type={} request={}", clientId,
                           pipeData.event_type, pipeData.request_id);
            continue;
        }

        LogPipeEvent(L"main-pipe", pipeData.event_type, pipeData.keycode, pipeData.wch, pipeData.modifiers_down);
        switch (pipeData.event_type)
        {
        case FanyImePipeEventType::KeyEvent: {
            EnqueueTask(TaskType::ImeKeyEvent, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::HideCandidateWnd: {
            EnqueueTask(TaskType::HideCandidate, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::ShowCandidateWnd: {
            EnqueueTask(TaskType::ShowCandidate, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::MoveCandidateWnd: {
            EnqueueTask(TaskType::MoveCandidate, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::IMESwitch: {
            EnqueueTask(TaskType::IMESwitch, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::PuncSwitch: {
            EnqueueTask(TaskType::PuncSwitch, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::DoubleSingleByteSwitch: {
            EnqueueTask(TaskType::DoubleSingleByteSwitch, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::StatusSnapshot:
        case FanyImePipeEventType::FocusRestored: {
            // The ownership claim was already consumed above; the payload is
            // identical, so both feed the same toolbar update.
            EnqueueTask(TaskType::StatusSnapshot, pipeData, activation.epoch);
            // A plain StatusSnapshot is also emitted by OnSetThreadFocus in
            // hosts that do not produce a document-focus transition.  Replying
            // here makes that path converge too.  FocusRestored was already
            // answered above, so avoid a duplicate worker frame.
            if (pipeData.event_type == FanyImePipeEventType::StatusSnapshot)
            {
                SendInputModeState(activation);
            }
            break;
        }
        }
    }

    const PipeClientUnregisterResult unregisterResult =
        UnregisterPipeClientHandle(clientId, FanyImePipeRole::Main, clientPipe, mainRegistrationId);
    if (unregisterResult.removed)
    {
        ForgetClientStatusSnapshot(clientId);
    }
    uint64_t disconnectEpoch = unregisterResult.deactivation_epoch;
    if (disconnectEpoch == 0 && unregisterResult.removed)
    {
        // A process can disconnect its Main pipe after it suspended the route.
        // Reuse only that owner's inactive epoch; a replacement Main that has
        // already activated makes this terminal cleanup stale.
        disconnectEpoch = ResolvePipeClientTerminalDeactivationEpoch(clientId);
    }
    if (disconnectEpoch != 0)
    {
        FanyImeNamedpipeData disconnectData = {};
        disconnectData.event_type = FanyImePipeEventType::ClientSuspended;
        disconnectData.client_id = clientId;
        EnqueueTask(TaskType::ClientSuspended, disconnectData, disconnectEpoch);
    }
    LogPipeDisconnect(L"main-pipe");
    DisconnectNamedPipe(clientPipe);
    CloseHandle(clientPipe);
}

void ToTsfPipeEventListenerLoopThread()
{
    HANDLE listeningPipe = hToTsfPipe;
    hToTsfPipe = INVALID_HANDLE_VALUE;
    while (pipe_running)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
        {
            listeningPipe = CreateToTsfNamedPipeInstance();
            if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
            {
                FANY_IPC_LOGF(L"[msime]: [ipc] failed to create next to-tsf-pipe instance: gle={}", GetLastError());
                Sleep(50);
                continue;
            }
        }

        BOOL connected = WaitForPipeClient(listeningPipe);
#ifdef FANY_DEBUG
        (void)0;
#endif
        LogPipeConnectResult(L"to-tsf-pipe", connected);
        if (connected)
        {
            if (!pipe_running)
            {
                DisconnectNamedPipe(listeningPipe);
                CloseHandle(listeningPipe);
                listeningPipe = INVALID_HANDLE_VALUE;
                break;
            }
            HANDLE clientPipe = listeningPipe;
            listeningPipe = CreateToTsfNamedPipeInstance();
            const uint64_t handlerId = BeginPipeClientHandler(clientPipe);
            if (handlerId == 0)
            {
                DisconnectNamedPipe(clientPipe);
                CloseHandle(clientPipe);
            }
            else
            {
                try
                {
                    std::thread(RegisteredPipeMonitorThread, clientPipe, FanyImePipeRole::ToTsf, handlerId).detach();
                }
                catch (...)
                {
                    EndPipeClientHandler(handlerId);
                    DisconnectNamedPipe(clientPipe);
                    CloseHandle(clientPipe);
                }
            }
        }
        else
        {
            CloseHandle(listeningPipe);
            listeningPipe = INVALID_HANDLE_VALUE;
        }
    }

    if (listeningPipe && listeningPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(listeningPipe);
    }
}

void ToTsfWorkerThreadPipeEventListenerLoopThread()
{
    HANDLE listeningPipe = hToTsfWorkerThreadPipe;
    hToTsfWorkerThreadPipe = INVALID_HANDLE_VALUE;
    while (pipe_running)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
        {
            listeningPipe = CreateToTsfWorkerThreadNamedPipeInstance();
            if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
            {
                FANY_IPC_LOGF(L"[msime]: [ipc] failed to create next to-tsf-worker-pipe instance: gle={}",
                              GetLastError());
                Sleep(50);
                continue;
            }
        }

        BOOL connected = WaitForPipeClient(listeningPipe);
        LogPipeConnectResult(L"to-tsf-worker-pipe", connected);
        if (connected)
        {
#ifdef FANY_DEBUG
            (void)0;
#endif
            if (!pipe_running)
            {
                DisconnectNamedPipe(listeningPipe);
                CloseHandle(listeningPipe);
                listeningPipe = INVALID_HANDLE_VALUE;
                break;
            }
            HANDLE clientPipe = listeningPipe;
            listeningPipe = CreateToTsfWorkerThreadNamedPipeInstance();
            const uint64_t handlerId = BeginPipeClientHandler(clientPipe);
            if (handlerId == 0)
            {
                DisconnectNamedPipe(clientPipe);
                CloseHandle(clientPipe);
            }
            else
            {
                try
                {
                    std::thread(RegisteredPipeMonitorThread, clientPipe, FanyImePipeRole::ToTsfWorkerThread, handlerId)
                        .detach();
                }
                catch (...)
                {
                    EndPipeClientHandler(handlerId);
                    DisconnectNamedPipe(clientPipe);
                    CloseHandle(clientPipe);
                }
            }
        }
        else
        {
            CloseHandle(listeningPipe);
            listeningPipe = INVALID_HANDLE_VALUE;
        }
    }

    if (listeningPipe && listeningPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(listeningPipe);
    }
}

void RegisteredPipeMonitorThread(HANDLE clientPipe, UINT pipeRole, uint64_t handlerId)
{
    ScopedPipeClientHandler handler(handlerId);
    FanyImePipeHello hello = {};
    if (!ReadPipeHello(clientPipe, pipeRole, hello))
    {
        LogPipeReadFailure(pipeRole == FanyImePipeRole::ToTsf ? L"to-tsf-pipe" : L"to-tsf-worker-pipe", 0);
        DisconnectNamedPipe(clientPipe);
        CloseHandle(clientPipe);
        return;
    }

    HANDLE monitorPipe = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), clientPipe, GetCurrentProcess(), &monitorPipe, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        monitorPipe = INVALID_HANDLE_VALUE;
    }

    uint64_t registrationId = 0;
    if (pipeRole == FanyImePipeRole::ToTsf)
    {
        registrationId = RegisterToTsfPipeClient(hello.client_id, clientPipe);
    }
    else if (pipeRole == FanyImePipeRole::ToTsfWorkerThread)
    {
        registrationId = RegisterToTsfWorkerThreadPipeClient(hello.client_id, clientPipe);
        if (registrationId != 0)
        {
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::PagingCommaPeriodChanged,
                FormatPagingCommaPeriodWorkerPayload());
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationChanged,
                GetConfiguredSmartPunctuationEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationRepeatToChineseChanged,
                GetConfiguredSmartPunctuationRepeatToChineseEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged,
                GetConfiguredPairedPunctuationEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::MicrosoftShuangpinChanged,
                GetConfiguredShuangpinSchema() == "microsoft" ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::LowercaseNumberModeChanged,
                FormatLowercaseNumberModeWorkerPayload());
            SendToTsfWorkerThreadClientViaNamedpipe(hello.client_id,
                                                    Global::DataFromServerMsgTypeToTsfWorkerThread::InputModeChanged,
                                                    GetConfiguredInputMode() == "japanese" ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(hello.client_id,
                                                    Global::DataFromServerMsgTypeToTsfWorkerThread::CapsLockChanged,
                                                    GetServerCapsLockState() != 0 ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::TsfDiagnosticLogChanged,
                GetConfiguredTsfDiagnosticLogEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::PunctuationLockChanged,
                FormatPunctuationLockWorkerPayload());
        }
    }

    if (registrationId == 0)
    {
        if (monitorPipe && monitorPipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(monitorPipe);
        }
        DisconnectNamedPipe(clientPipe);
        CloseHandle(clientPipe);
        return;
    }

    if (!monitorPipe || monitorPipe == INVALID_HANDLE_VALUE)
    {
        const PipeClientUnregisterResult result =
            UnregisterPipeClientHandle(hello.client_id, pipeRole, clientPipe, registrationId);
        EnqueuePipeSessionInvalidatedTask(hello.client_id, result.deactivation_epoch);
        return;
    }

    const wchar_t *pipeName = pipeRole == FanyImePipeRole::ToTsf ? L"to-tsf-pipe" : L"to-tsf-worker-pipe";
    while (pipe_running && IsPipeClientRegistrationCurrent(hello.client_id, pipeRole, registrationId))
    {
        DWORD bytesAvailable = 0;
        if (!PeekNamedPipe(monitorPipe, nullptr, 0, nullptr, &bytesAvailable, nullptr))
        {
            LogPipeReadFailure(pipeName, 0);
            break;
        }
        Sleep(20);
    }

    const PipeClientUnregisterResult result =
        UnregisterPipeClientHandle(hello.client_id, pipeRole, clientPipe, registrationId);
    EnqueuePipeSessionInvalidatedTask(hello.client_id, result.deactivation_epoch);
    LogPipeDisconnect(pipeName);
    CloseHandle(monitorPipe);
}

void AuxPipeEventListenerLoopThread()
{
    HANDLE listeningPipe = hAuxPipe;
    hAuxPipe = INVALID_HANDLE_VALUE;
    while (pipe_running)
    {
        if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
        {
            listeningPipe = CreateAuxNamedPipeInstance();
            if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
            {
                Sleep(50);
                continue;
            }
        }

        const BOOL connected = WaitForPipeClient(listeningPipe);
        LogPipeConnectResult(L"aux-pipe", connected);
        if (connected)
        {
            if (!pipe_running)
            {
                DisconnectNamedPipe(listeningPipe);
                break;
            }

            wchar_t buffer[128] = {0};
            DWORD bytesRead = 0;
            BOOL readResult = FALSE;
            DWORD pipeMode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
            if (SetNamedPipeHandleState(listeningPipe, &pipeMode, nullptr, nullptr))
            {
                // Polling a NOWAIT server handle keeps shutdown bounded even if a client connects and never sends its
                // auxiliary message. The deadline is what frees the single Aux instance in that case: without it the
                // poll spins forever, the loop never returns to ConnectNamedPipe, and no later client
                // (LangbarRightClick, TerminalDeactivation) is ever accepted.
                const auto deadline = std::chrono::steady_clock::now() + kPipeHelloTimeout;
                while (pipe_running && std::chrono::steady_clock::now() < deadline)
                {
                    readResult = ReadFile(listeningPipe, buffer, sizeof(buffer), &bytesRead, nullptr);
                    if (readResult || GetLastError() != ERROR_NO_DATA)
                    {
                        break;
                    }
                    Sleep(1);
                }
            }
            if (!readResult || bytesRead == 0) // Disconnected or error
            {
                LogPipeReadFailure(L"aux-pipe", bytesRead);
            }
            else
            {
                std::wstring message(buffer, bytesRead / sizeof(wchar_t));
                FANY_IPC_LOGF(L"[msime]: [ipc] aux-pipe message: {}", message);

                // Aux normally carries session-less UI notifications. Terminal
                // deactivation is the one lifecycle exception: it is a bounded,
                // token-checked fallback for a failed Main-pipe teardown write.
                int left = 0;
                int top = 0;
                int right = 0;
                int bottom = 0;
                if (swscanf_s(message.c_str(), L"LangbarRightClick|%d|%d|%d|%d", &left, &top, &right, &bottom) == 4)
                {
                    FanyImeNamedpipeData pipeData = {};
                    pipeData.event_type = FanyImePipeEventType::LangbarRightClick;
                    pipeData.point[0] = left;
                    pipeData.point[1] = top;
                    pipeData.keycode = static_cast<UINT>(right);
                    pipeData.modifiers_down = static_cast<UINT>(bottom);
                    // client_id/epoch stay 0 so WorkerThread skips active-client
                    // gating and never activates a suspended TIP for a menu click.
                    EnqueueTask(TaskType::LangbarRightClick, pipeData, 0);
                }
                else if (message == L"RestartServer")
                {
                    // Settings runs in its own process, so the restart it offers for
                    // backend changes has to travel the same cross-integrity Aux path
                    // the config notifications use. The Watchdog relaunches us.
                    RestartServerProcess();
                }
                else if (message == L"ConfigChanged" || message == L"InputSchemeChanged" ||
                         message == L"CandidateSkinRefresh")
                {
                    const UINT configMessage =
                        message == L"InputSchemeChanged" ? WM_APPLY_IME_INPUT_SCHEME : WM_APPLY_IME_CONFIG;
                    const WPARAM configWParam = message == L"CandidateSkinRefresh" ? 1 : 0;
                    const HWND candidateWindow = ::global_hwnd;
                    if (candidateWindow && IsWindow(candidateWindow))
                    {
                        PostMessageW(candidateWindow, configMessage, configWParam, 0);
                    }
                    else
                    {
                        // The next startup load reads the already-persisted
                        // config. Explicit invalidation also prevents an early
                        // cached timestamp from suppressing that convergence.
                        InvalidateImeConfigWriteTime();
                    }
                }
                else
                {
                    unsigned long long clientId = 0;
                    unsigned long long focusToken = 0;
                    if (swscanf_s(message.c_str(), L"TerminalDeactivation|%llu|%llu", &clientId, &focusToken) == 2 &&
                        PipeClientIdMatchesConnectedProcess(listeningPipe, static_cast<uint64_t>(clientId)))
                    {
                        const uint64_t deactivationEpoch = DeactivatePipeClientByFocusToken(
                            static_cast<uint64_t>(clientId), static_cast<uint64_t>(focusToken));
                        if (deactivationEpoch != 0)
                        {
                            FanyImeNamedpipeData pipeData = {};
                            pipeData.event_type = FanyImePipeEventType::ClientDeactivated;
                            pipeData.client_id = static_cast<uint64_t>(clientId);
                            EnqueueTask(TaskType::ClientDeactivated, pipeData, deactivationEpoch);

                            constexpr wchar_t acknowledgement[] = L"OK";
                            DWORD bytesWritten = 0;
                            WriteFile(listeningPipe, acknowledgement, sizeof(acknowledgement) - sizeof(wchar_t),
                                      &bytesWritten, nullptr);
                        }
                    }
                }
            }
        }
        else
        {
            if (pipe_running)
            {
                Sleep(10);
            }
        }
        LogPipeDisconnect(L"aux-pipe");
        DisconnectNamedPipe(listeningPipe);
        CloseHandle(listeningPipe);
        listeningPipe = INVALID_HANDLE_VALUE;
    }
    if (listeningPipe && listeningPipe != INVALID_HANDLE_VALUE)
    {
        DisconnectNamedPipe(listeningPipe);
        CloseHandle(listeningPipe);
    }
}

void TsfDiagnosticPipeEventListenerLoopThread()
{
    HANDLE listeningPipe = hTsfDiagnosticPipe;
    hTsfDiagnosticPipe = INVALID_HANDLE_VALUE;
    while (pipe_running)
    {
        if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
        {
            listeningPipe = CreateTsfDiagnosticNamedPipeInstance();
            if (!listeningPipe || listeningPipe == INVALID_HANDLE_VALUE)
            {
                Sleep(50);
                continue;
            }
        }

        const BOOL connected = WaitForPipeClient(listeningPipe);
        if (connected && pipe_running)
        {
            std::vector<unsigned char> frame(FANY_IME_TSF_DIAGNOSTIC_MAX_FRAME_BYTES);
            DWORD bytesRead = 0;
            BOOL readResult = FALSE;
            DWORD pipeMode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
            if (SetNamedPipeHandleState(listeningPipe, &pipeMode, nullptr, nullptr))
            {
                // Same bound as the Aux pipe: a client that connects without ever writing must not hold the single
                // diagnostic instance for the process lifetime.
                const auto deadline = std::chrono::steady_clock::now() + kPipeHelloTimeout;
                while (pipe_running && std::chrono::steady_clock::now() < deadline)
                {
                    readResult =
                        ReadFile(listeningPipe, frame.data(), static_cast<DWORD>(frame.size()), &bytesRead, nullptr);
                    if (readResult || GetLastError() != ERROR_NO_DATA)
                    {
                        break;
                    }
                    Sleep(1);
                }
            }

            FanyImeTsfDiagnosticBatchHeader header{};
            if (readResult && bytesRead >= sizeof(header))
            {
                memcpy(&header, frame.data(), sizeof(header));
                ULONG clientProcessId = 0;
                const bool clientMatches = GetNamedPipeClientProcessId(listeningPipe, &clientProcessId) &&
                                           clientProcessId == header.source_process_id;
                const bool frameValid = clientMatches && header.magic == FANY_IME_TSF_DIAGNOSTIC_MAGIC &&
                                        header.version == FANY_IME_TSF_DIAGNOSTIC_VERSION &&
                                        header.header_size == sizeof(header) && header.record_count != 0 &&
                                        header.payload_bytes != 0 && (header.payload_bytes % sizeof(wchar_t)) == 0 &&
                                        sizeof(header) + header.payload_bytes == bytesRead;
                if (frameValid && GetConfiguredTsfDiagnosticLogEnabled())
                {
                    std::wstring payload(header.payload_bytes / sizeof(wchar_t), L'\0');
                    memcpy(payload.data(), frame.data() + sizeof(header), header.payload_bytes);
                    if (header.dropped_count != 0)
                    {
                        DiagnosticLog::Write(fmt::format(L"[tsf-log] source_pid={} dropped_records={}",
                                                         header.source_process_id, header.dropped_count));
                    }
                    size_t start = 0;
                    while (start < payload.size())
                    {
                        const size_t end = payload.find(L'\n', start);
                        const size_t length = end == std::wstring::npos ? payload.size() - start : end - start;
                        if (length != 0)
                        {
                            DiagnosticLog::Write(payload.substr(start, length));
                        }
                        if (end == std::wstring::npos)
                        {
                            break;
                        }
                        start = end + 1;
                    }
                }
            }
        }

        if (listeningPipe && listeningPipe != INVALID_HANDLE_VALUE)
        {
            DisconnectNamedPipe(listeningPipe);
            CloseHandle(listeningPipe);
            listeningPipe = INVALID_HANDLE_VALUE;
        }
    }
}

// Stopwatch for the per-keystroke candidate build. Everything this function does
// runs on the shared task thread, so a stall anywhere in it delays the hide/show
// messages queued behind it — which is what reaches the screen as flicker. The
// splits exist to tell those segments apart: the engine candidate lookup and the
// user-dictionary fixed-position pass both touch a database, and only a
// measurement says which one is paying for it.
class CandidateBuildTimer
{
  public:
    // Milliseconds since the previous split, then restarts the segment.
    double Split()
    {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
        return ms;
    }
    double TotalMs() const
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    }

  private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_ = start_;
};

void PrepareCandidateList(uint64_t client_id, uint64_t activation_epoch)
{
    CandidateBuildTimer segment;
    // Zero means the branch taken this keystroke has no such segment, not that
    // the segment was instant.
    double queryMs = 0;
    double fixedPosMs = 0;

    auto &ui = Global::candidate_ui;
    std::string pinyin = wstring_to_string(Global::PinyinString);
    const std::string current_input = g_inputSession->get_pinyin_sequence_with_cases();
    std::vector<WordItem> items;
    if (g_english_input_mode)
    {
        // Do not expose transient Chinese/raw fallback candidates while the
        // dedicated English query is in flight.
    }
    else if (IsUnicodeInput(current_input))
    {
        items = metasequoia::local_modes::query_unicode(current_input.substr(1));
    }
    else if (IsNumberInput(current_input))
    {
        items = metasequoia::local_modes::query_number(current_input.substr(1));
    }
    else if (IsQuickPhraseInput(current_input))
    {
        items = metasequoia::local_modes::query_quick_phrases(current_input.substr(1)).candidates;
    }
    else if (IsDateTimeInput(current_input))
    {
        items = metasequoia::local_modes::query_date_time(current_input.substr(1));
    }
    else if (IsEmojiInput(current_input))
    {
        items = metasequoia::local_modes::query_emoji(current_input.substr(1), g_inputSession->current_scheme_type(),
                                                      10, ConfiguredShuangpinProfile())
                    .candidates;
    }
    else if (IsKaomojiInput(current_input))
    {
        items = metasequoia::local_modes::query_kaomoji(current_input.substr(1), g_inputSession->current_scheme_type(),
                                                        10, ConfiguredShuangpinProfile())
                    .candidates;
    }
    else if (IsJianpinInput(current_input))
    {
        const int limit = current_input.size() == 2 ? 24 : 100;
        items = metasequoia::local_modes::query_jianpin(current_input.substr(1), g_inputSession->current_scheme_type(),
                                                        limit, ConfiguredShuangpinProfile())
                    .candidates;
        const std::string typed = g_inputSession->get_pinyin_sequence();
        for (auto &item : items)
            item.pinyin = typed;
        queryMs = segment.Split();
        user_dictionary::apply_fixed_positions(user_dictionary::default_user_db_path(), CurrentRankingContextKey(),
                                               items, false);
        fixedPosMs = segment.Split();
    }
    else if (IsYModeInput(current_input))
    {
        // Show the typed English immediately; dictionary completions arrive asynchronously.
        items.emplace_back("", current_input.substr(1), 0, CandidateSource::Generated);
    }
    else if (IsSpecialModeCompositionActive(current_input))
    {
        // A K/U/V/T/E/M/J/Y special-mode prefix that is not yet a complete input (e.g.
        // "K", "U", "U+", "V", "Tw", "Txin", "E", "M", "J", "Y"): do not translate it into
        // normal pinyin candidates. Leave items empty so only the raw typed text
        // shows as the fallback.
    }
    else
    {
        items = g_inputSession->get_candidates();
        queryMs = segment.Split();
        user_dictionary::apply_fixed_positions(
            user_dictionary::default_user_db_path(), CurrentRankingContextKey(), items,
            g_inputSession->get_pinyin_sequence().size() == 1,
            [](const std::string &key, const std::string &value) { return g_inputSession->find_candidate(key, value); },
            g_inputSession->has_active_helpcode());
        fixedPosMs = segment.Split();
        if (g_inputSession->get_pinyin_sequence().size() == 1 && items.size() > 24)
            items.resize(24);
    }

    if (items.empty() && !g_english_input_mode)
    {
        items.emplace_back(pinyin, pinyin, 1, CandidateSource::Fallback);
    }

    // Whatever the branch above did that the two splits did not already claim.
    const double branchMs = segment.Split();
    const size_t itemCount = items.size();

    ui.set_items(std::move(items));
    RefreshCandidatePageUi(false);
    PublishCandidateUiOwner(client_id, activation_epoch);
    const double uiMs = segment.Split();

    const SchemeType scheme = g_inputSession->current_scheme_type();
    if (g_english_input_mode)
    {
        UpdateEnglishInput(current_input, client_id, activation_epoch, true);
    }
    else if (IsYModeInput(current_input))
    {
        UpdateEnglishInput(current_input.substr(1), client_id, activation_epoch, true);
    }
    else if (!IsSpecialModeCompositionActive(current_input) && GetConfiguredEnglishCandidatesEnabled() &&
             (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin) &&
             !GlobalIme::composition.creating_word.active)
    {
        UpdateEnglishInput(current_input, client_id, activation_epoch);
    }
    else
    {
        UpdateEnglishInput("");
    }
    const double englishMs = segment.Split();

    if (!g_english_input_mode && !IsSpecialModeCompositionActive(current_input) &&
        GetConfiguredEmojiMixedInputEnabled() && (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin) &&
        !GlobalIme::composition.creating_word.active)
    {
        UpdateEmojiInput(current_input, client_id, activation_epoch);
    }
    else
    {
        UpdateEmojiInput("");
    }
    const double emojiMs = segment.Split();

    if (!g_english_input_mode && !IsSpecialModeCompositionActive(current_input) &&
        GetConfiguredKaomojiMixedInputEnabled() && (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin) &&
        !GlobalIme::composition.creating_word.active)
    {
        UpdateKaomojiInput(current_input, client_id, activation_epoch);
    }
    else
    {
        UpdateKaomojiInput("");
    }
    const double kaomojiMs = segment.Split();

    CAND_DIAG_LOGF(L"candidate build total_ms={:.1f} query_ms={:.1f} fixed_pos_ms={:.1f} branch_ms={:.1f} "
                   L"ui_ms={:.1f} english_ms={:.1f} emoji_ms={:.1f} kaomoji_ms={:.1f} items={}",
                   segment.TotalMs(), queryMs, fixedPosMs, branchMs, uiMs, englishMs, emojiMs, kaomojiMs, itemCount);
}

void ApplyCloudCandidate(const std::string &candidate, const std::string &pinyin, uint64_t generation,
                         const std::optional<metasequoia::OnlineQuery> &query)
{
    if (!GetConfiguredCloudCandidatesEnabled())
        return;
    // A callback can become stale after enqueueing, while earlier key tasks run.
    if (FindCloudRequestOrigin(pinyin, generation).client_id == 0 || !g_inputSession)
        return;

    if (candidate.empty())
        return;

    if (GlobalIme::composition.creating_word.active)
        return;

    const auto cloud_query_state = g_inputSession->get_cloud_query_state();
    if (cloud_query_state.query_text.empty() || cloud_query_state.query_text != pinyin)
        return;

    if (Global::candidate_ui.items.empty())
        return;

    if (!query)
        return;

    auto &items = Global::candidate_ui.items;
    // Same word already visible (dict / prior cloud): keep page and skip re-cache.
    if (std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == candidate; }))
    {
        return;
    }

    // The engine checks the original session and composition before caching the result.
    if (!g_inputSession->apply_online_candidate(*query, candidate, CandidateSource::CloudSuggestion))
        return;
    const auto &engine_candidates = g_inputSession->get_candidates();
    const auto accepted = std::find_if(engine_candidates.begin(), engine_candidates.end(), [&](const WordItem &item) {
        return item.word == candidate && item.source == CandidateSource::CloudSuggestion;
    });
    if (accepted == engine_candidates.end())
        return;

    // Replace any previous cloud suggestion with the new unique text.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const WordItem &item) { return item.source == CandidateSource::CloudSuggestion; }),
                items.end());

    size_t insert_index = items.size() >= 1 ? 1 : 0;
    items.insert(items.begin() + insert_index, *accepted);
    const bool preserve_single_kana_pair =
        g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji &&
        japanese::IsSingleKanaConversion(japanese::ConvertRomaji(g_inputSession->get_pinyin_sequence()));
    FanyImeIpc::NormalizeMixedCandidateOrder(items, preserve_single_kana_pair ? 2 : 1);
    Global::cloud_candidate = {true, candidate, cloud_query_state.committed_pinyin};

    Global::candidate_ui.item_total_count = static_cast<int>(items.size());
    Global::candidate_ui.page_index = 0;
    Global::candidate_ui.select_first_on_page();
    Global::candidate_ui.clear_page();
    RefreshCandidatePageUi(true);
}

void ApplyAiCandidate(const std::string &candidate, const std::string &identity, uint64_t generation,
                      const std::optional<metasequoia::OnlineQuery> &engine_query)
{
    if (!engine_query || FindAiRequestOrigin(identity, generation).client_id == 0)
        return;
    const bool enabled = GetConfiguredAiAssistant().enabled;
    const bool has_session = static_cast<bool>(g_inputSession);
    const bool non_pinyin = has_session && g_inputSession->current_scheme_type() != SchemeType::Quanpin &&
                            g_inputSession->current_scheme_type() != SchemeType::Shuangpin;
    const bool complete = has_session && g_inputSession->is_all_complete_pure_pinyin();
    const bool helpcode_active = has_session && g_inputSession->has_active_helpcode();
    const std::string current_identity = has_session ? g_inputSession->get_pinyin_segmentation() : std::string{};
    if (!enabled || candidate.empty() || !has_session || non_pinyin || !complete || helpcode_active ||
        GlobalIme::composition.creating_word.active || current_identity != identity)
    {
        (void)0;
        return;
    }
    auto &items = Global::candidate_ui.items;
    const auto query = g_inputSession->get_cloud_query_state();
    // Align with cloud: if the word is already in the list, do not erase / reinsert /
    // reset page_index / re-cache. This stops cache-hit reapply from breaking paging.
    if (std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == candidate; }))
    {
        return;
    }

    if (!g_inputSession->apply_online_candidate(*engine_query, candidate, CandidateSource::AiSuggestion))
        return;
    const auto &engine_candidates = g_inputSession->get_candidates();
    const auto accepted = std::find_if(engine_candidates.begin(), engine_candidates.end(), [&](const WordItem &item) {
        return item.word == candidate && item.source == CandidateSource::AiSuggestion;
    });
    if (accepted == engine_candidates.end())
        return;

    // Only replace prior AI rows when inserting a genuinely new suggestion text.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const WordItem &item) { return item.source == CandidateSource::AiSuggestion; }),
                items.end());
    const size_t insert_index = std::min<size_t>(2, items.size());
    items.insert(items.begin() + insert_index, *accepted);
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    (void)0;
    Global::ai_candidate = {true, candidate, query.committed_pinyin};
    Global::candidate_ui.item_total_count = static_cast<int>(items.size());
    Global::candidate_ui.page_index = 0;
    Global::candidate_ui.select_first_on_page();
    Global::candidate_ui.clear_page();
    RefreshCandidatePageUi(true);
}

void ApplyEnglishCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)
{
    const std::string session_input =
        g_inputSession != nullptr ? g_inputSession->get_pinyin_sequence_with_cases() : std::string{};
    const bool y_mode = IsYModeInput(session_input);
    const bool dedicated_mode = g_english_input_mode || y_mode;
    const std::string expected_input = y_mode ? session_input.substr(1) : session_input;
    if ((!dedicated_mode && !GetConfiguredEnglishCandidatesEnabled()) ||
        !EnglishIme::IsCurrent(input, generation, dedicated_mode) || g_inputSession == nullptr ||
        (!dedicated_mode && g_inputSession->current_scheme_type() != SchemeType::Quanpin &&
         g_inputSession->current_scheme_type() != SchemeType::Shuangpin) ||
        expected_input != input || GlobalIme::composition.creating_word.active)
    {
        return;
    }

    auto &items = Global::candidate_ui.items;
    if (dedicated_mode)
    {
        std::string context_input = input;
        std::transform(context_input.begin(), context_input.end(), context_input.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        user_dictionary::apply_fixed_positions(user_dictionary::default_user_db_path(), "english:" + context_input,
                                               candidates, false, {}, false);
        if (y_mode)
        {
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                            [&](const WordItem &item) {
                                                if (item.word.size() != input.size())
                                                    return false;
                                                for (size_t i = 0; i < input.size(); ++i)
                                                {
                                                    if (std::tolower(static_cast<unsigned char>(item.word[i])) !=
                                                        std::tolower(static_cast<unsigned char>(input[i])))
                                                        return false;
                                                }
                                                return true;
                                            }),
                             candidates.end());
            candidates.insert(candidates.begin(), WordItem("", input, 0, CandidateSource::Generated));
        }
        else if (candidates.empty() && !input.empty())
        {
            // A raw fallback is selectable, but it is not an english.db row
            // and therefore must not participate in dictionary mutations.
            candidates.emplace_back("", input, 0, CandidateSource::Generated);
        }
        items = std::move(candidates);
        Global::candidate_ui.item_total_count = static_cast<int>(items.size());
        Global::candidate_ui.page_index = 0;
        Global::candidate_ui.select_first_on_page();
        Global::candidate_ui.clear_page();
        RefreshCandidatePageUi(true);
        return;
    }
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const WordItem &item) { return item.source == CandidateSource::EnglishDictionary; }),
                items.end());

    std::vector<WordItem> unique_candidates;
    for (auto &candidate : candidates)
    {
        const bool duplicate =
            std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == candidate.word; });
        if (!duplicate)
        {
            unique_candidates.push_back(std::move(candidate));
        }
    }

    if (!unique_candidates.empty())
    {
        user_dictionary::apply_fixed_positions(user_dictionary::default_user_db_path(), EnglishRankingContextKey(),
                                               unique_candidates, false);
        const size_t insert_index = std::min<size_t>(1, items.size());
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(unique_candidates.front()));
        user_dictionary::apply_fixed_positions(user_dictionary::default_user_db_path(), CurrentRankingContextKey(),
                                               items, false, {}, g_inputSession->has_active_helpcode());
        for (size_t index = 1; index < unique_candidates.size(); ++index)
        {
            items.push_back(std::move(unique_candidates[index]));
        }
        FanyImeIpc::NormalizeMixedCandidateOrder(items);
    }

    Global::candidate_ui.item_total_count = static_cast<int>(items.size());
    Global::candidate_ui.page_index = 0;
    Global::candidate_ui.select_first_on_page();
    Global::candidate_ui.clear_page();
    RefreshCandidatePageUi(true);
}

void ApplyCandidateTranslations(std::vector<EnglishIme::TranslationResult> results, uint64_t generation, bool merge)
{
    if (!EnglishIme::IsTranslationCurrent(generation) || !GetConfiguredCandidateTranslationsEnabled() ||
        IsUiLessMode() || g_candidate_translation_signature.empty() ||
        (g_inputSession && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji))
        return;

    if (!merge)
        g_candidate_translation_glosses.clear();

    std::vector<EnglishIme::TranslationQuery> misses;
    for (auto &result : results)
    {
        std::string gloss = std::move(result.gloss);
        if (gloss.empty() && !merge)
            gloss = CloudTranslation::LookupCache(result.key, result.direction);
        if (!gloss.empty())
            g_candidate_translation_glosses[TranslationIdentity({result.key, result.direction})] = std::move(gloss);
        else if (!merge)
        {
            const bool cloud_translatable = result.direction == EnglishIme::TranslationDirection::EnglishToChinese
                                                ? CloudTranslation::IsCloudTranslatableEnglish(result.key)
                                                : CloudTranslation::IsCloudTranslatableChinese(result.key);
            if (cloud_translatable)
                misses.push_back({result.key, result.direction});
        }
    }
    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (owner && IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
        RefreshCandidatePageUi(true);
    if (!merge)
        CloudTranslation::RequestMisses(std::move(misses), generation);
}

void ApplyEmojiCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)
{
    if (!GetConfiguredEmojiMixedInputEnabled() || !EmojiIme::IsCurrent(input, generation) ||
        g_inputSession == nullptr ||
        (g_inputSession->current_scheme_type() != SchemeType::Quanpin &&
         g_inputSession->current_scheme_type() != SchemeType::Shuangpin) ||
        g_inputSession->get_pinyin_sequence_with_cases() != input || GlobalIme::composition.creating_word.active)
    {
        return;
    }

    auto &items = Global::candidate_ui.items;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const WordItem &item) { return item.source == CandidateSource::Emoji; }),
                items.end());

    std::vector<WordItem> unique_candidates;
    for (auto &candidate : candidates)
    {
        const bool duplicate =
            std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == candidate.word; });
        if (!duplicate)
        {
            unique_candidates.push_back(std::move(candidate));
        }
    }

    if (!unique_candidates.empty())
    {
        const size_t insert_index = std::min<size_t>(2, items.size());
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(unique_candidates.front()));
        for (size_t index = 1; index < unique_candidates.size(); ++index)
        {
            items.push_back(std::move(unique_candidates[index]));
        }
        FanyImeIpc::NormalizeMixedCandidateOrder(items);
    }

    Global::candidate_ui.item_total_count = static_cast<int>(items.size());
    Global::candidate_ui.page_index = 0;
    Global::candidate_ui.select_first_on_page();
    Global::candidate_ui.clear_page();
    RefreshCandidatePageUi(true);
}

void ApplyKaomojiCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)
{
    if (!GetConfiguredKaomojiMixedInputEnabled() || !KaomojiIme::IsCurrent(input, generation) ||
        g_inputSession == nullptr ||
        (g_inputSession->current_scheme_type() != SchemeType::Quanpin &&
         g_inputSession->current_scheme_type() != SchemeType::Shuangpin) ||
        g_inputSession->get_pinyin_sequence_with_cases() != input || GlobalIme::composition.creating_word.active)
    {
        return;
    }

    auto &items = Global::candidate_ui.items;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const WordItem &item) { return item.source == CandidateSource::Kaomoji; }),
                items.end());

    std::vector<WordItem> unique_candidates;
    for (auto &candidate : candidates)
    {
        const bool duplicate =
            std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == candidate.word; });
        if (!duplicate)
        {
            unique_candidates.push_back(std::move(candidate));
        }
    }

    if (!unique_candidates.empty())
    {
        const size_t insert_index = std::min<size_t>(3, items.size());
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(unique_candidates.front()));
        for (size_t index = 1; index < unique_candidates.size(); ++index)
        {
            items.push_back(std::move(unique_candidates[index]));
        }
        FanyImeIpc::NormalizeMixedCandidateOrder(items);
    }

    Global::candidate_ui.item_total_count = static_cast<int>(items.size());
    Global::candidate_ui.page_index = 0;
    Global::candidate_ui.select_first_on_page();
    Global::candidate_ui.clear_page();
    RefreshCandidatePageUi(true);
}

/**
 * @brief
 *
 * 调频、造词也都在这里处理。
 *
 */
void HandleImeKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id)
{
    const ScopedServerKeyLatency latency{client_id, activation_epoch, request_id};
    /* 先清理一下状态 */
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;
    ::ReadDataFromNamedPipe(0b000111);

    // TSF classifies VK_NUMPAD0..9 as candidate digit keys. Keep the IPC
    // contract symmetric before any selection/composition predicates run.
    Global::Keycode = FanyImeIpc::NormalizeNumpadDigitKey(Global::Keycode);

    if (FanyImeProtocol::IsCharacterSetShortcut(Global::Keycode, Global::ModifiersDown))
    {
        if (g_authoritative_cn_mode != 0 && GetConfiguredCharacterSetShortcutEnabled())
            SetConfiguredCharacterSet(GetConfiguredCharacterSet() == "traditional" ? "simplified" : "traditional");
        return;
    }

    if (FanyImeIpc::IsEnglishModeToggleKey(Global::Keycode, Global::ModifiersDown))
    {
        SetEnglishInputMode(!g_english_input_mode);
        ClearState();
        return;
    }

    if (g_r_mode_triggered && !GlobalIme::composition.raw_input_with_cases.empty() &&
        GlobalIme::composition.raw_input_with_cases.front() == 'R' && GlobalIme::composition.caret_position > 0)
    {
        // The published preedit has one extra display-only prefix. Normalize
        // the caret before every R-mode key, including paging and selection.
        --GlobalIme::composition.caret_position;
    }

    const std::string input_before_key =
        g_inputSession ? g_inputSession->get_pinyin_sequence_with_cases() : std::string{};
    const bool shift_only = (Global::ModifiersDown & 0b00000111u) == 0b00000001u;
    const bool chinese_scheme = g_inputSession && (g_inputSession->current_scheme_type() == SchemeType::Quanpin ||
                                                   g_inputSession->current_scheme_type() == SchemeType::Shuangpin);
    if (Global::Keycode == VK_RETURN && !input_before_key.empty())
    {
        std::string english_word;
        const bool shift_letter_special_mode = IsShiftLetterSpecialModeTriggered();
        if (FanyImeIpc::ShouldLearnEnteredEnglishWord(g_english_input_mode, shift_letter_special_mode, chinese_scheme,
                                                      g_inputSession->is_all_complete_pure_pinyin()))
            english_word = g_r_mode_triggered ? "R" + input_before_key : input_before_key;
        EnqueueLearnEnteredEnglishWordTask(english_word);
    }
    if (chinese_scheme && !g_english_input_mode && GetConfiguredQuickPhraseEnabled() && input_before_key.empty() &&
        Global::Keycode == 'K' && Global::Wch == L'K' && shift_only)
        g_quick_phrase_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredUnicodeModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'U' && Global::Wch == L'U' && shift_only)
        g_unicode_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredNumberModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'V' && Global::Wch == L'V' && shift_only)
        g_number_mode_triggered = true;
    // Quanpin never begins a syllable with v, so a bare v may start number mode
    // without Shift (v123). Shuangpin keeps v as an initial and needs Shift+V.
    const bool no_modifiers = (Global::ModifiersDown & 0b00000111u) == 0;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredNumberModeEnabled() && input_before_key.empty() &&
        g_inputSession->current_scheme_type() == SchemeType::Quanpin && Global::Keycode == 'V' && Global::Wch == L'v' &&
        no_modifiers)
        g_number_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredDateTimeModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'T' && Global::Wch == L'T' && shift_only)
        g_date_time_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredEmojiModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'E' && Global::Wch == L'E' && shift_only)
        g_emoji_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredKaomojiModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'M' && Global::Wch == L'M' && shift_only)
        g_kaomoji_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredJianpinModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'J' && Global::Wch == L'J' && shift_only)
        g_jianpin_mode_triggered = true;
    if (chinese_scheme && !g_english_input_mode && GetConfiguredYModeEnabled() && input_before_key.empty() &&
        Global::Keycode == 'Y' && Global::Wch == L'Y' && shift_only)
        g_y_mode_triggered = true;
    const bool r_mode_trigger_key = chinese_scheme && !g_english_input_mode && GetConfiguredRModeEnabled() &&
                                    input_before_key.empty() && Global::Keycode == 'R' && Global::Wch == L'R' &&
                                    shift_only;
    if (r_mode_trigger_key)
    {
        g_r_mode_original_session = g_inputSession;
        g_inputSession = CreateTemporaryJapaneseInputSession();
        g_r_mode_triggered = true;
    }

    if (FanyImeIpc::IsBackendIndependentCompositionResetKey(Global::Keycode))
    {
        // TSF completes/cancels the composition locally. Keep every backend in
        // lockstep, invalidate async candidates, and do not manufacture a
        // reply for this locally consumed key.
        PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
        ClearState();
        return;
    }

    const bool digit_composition_active = IsDigitCompositionActive(input_before_key);
    const bool is_paging_key = IsPagingKey(Global::Keycode);
    const bool is_manual_pinyin_separator = IsManualPinyinSeparatorKey(Global::Keycode, Global::Wch);
    const bool is_microsoft_shuangpin_ing_key =
        IsMicrosoftShuangpinIngKey(Global::Keycode, Global::Wch, input_before_key);
    const int word_character_direction = FanyImeIpc::WordToCharacterDirection(
        Global::Keycode, Global::Wch, Global::ModifiersDown, GetConfiguredWordToCharacterEnabled(),
        GetConfiguredWordToCharacterKeys() == "minus_equal");
    const bool is_commit_with_highlighted_candidate_punctuation =
        word_character_direction != 0 ||
        (!is_manual_pinyin_separator && !is_microsoft_shuangpin_ing_key &&
         IsCommitWithHighlightedCandidatePunctuationInCandidateMode(Global::Keycode, Global::Wch));
    const bool is_selection_key = IsSelectionKey(Global::Keycode);
    // U-mode and V-mode share the digit rules: bare digits compose, Shift+1..9 selects.
    const bool is_unicode_shift_digit_selection =
        digit_composition_active && shift_only && Global::Keycode >= '1' && Global::Keycode <= '9';
    const bool is_unicode_hex_digit = digit_composition_active && !is_unicode_shift_digit_selection &&
                                      Global::Keycode >= '0' && Global::Keycode <= '9';
    const bool is_unicode_plus =
        IsUnicodeCompositionActive(input_before_key) && Global::Keycode == VK_OEM_PLUS && Global::Wch == L'+';
    const bool is_number_point = IsNumberPointKey(input_before_key, Global::Keycode, Global::Wch);
    const bool is_composition_edit_key = Global::Keycode == VK_LEFT || Global::Keycode == VK_RIGHT ||
                                         Global::Keycode == VK_BACK || Global::Keycode == VK_DELETE ||
                                         (Global::Keycode >= 'A' && Global::Keycode <= 'Z') ||
                                         is_manual_pinyin_separator || is_microsoft_shuangpin_ing_key ||
                                         is_unicode_hex_digit || is_unicode_plus || is_number_point;
    const bool should_forward_key_to_session = !is_commit_with_highlighted_candidate_punctuation && !is_selection_key &&
                                               !is_paging_key && !is_composition_edit_key;

    // Punctuation needs a synchronous highlighted-candidate response on the TSF pipe.
    // Reply before cloud-query and candidate recomputation work so the TSF-side
    // timeout sentinel keeps its original meaning instead of masking latency here.
    if (is_commit_with_highlighted_candidate_punctuation)
    {
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;
        const bool has_active_composition = g_inputSession != nullptr && !g_inputSession->get_pinyin_sequence().empty();
        if (has_active_composition)
        {
            EnsureCandidatePageReady();
            auto &ui = Global::candidate_ui;
            ui.selected_text = FanyImeIpc::HighlightedCandidateText(ui.page_words, ui.selected_index_in_page);

            WordItem highlighted_item;
            if (word_character_direction != 0 && ResolveCandidateItem(ui.selected_index_in_page + 1, highlighted_item))
            {
                const auto edge = word_character_direction < 0 ? FanyImeIpc::HanCharacterEdge::First
                                                               : FanyImeIpc::HanCharacterEdge::Last;
                const auto character =
                    FanyImeIpc::ExtractHanCharacter(CandidateTextForOutput(highlighted_item.word), edge);
                if (character)
                {
                    Global::MsgTypeToTsf = Global::DataFromServerMsgType::CommitExactText;
                    ui.selected_text = string_to_wstring(*character);
                }
            }
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            ClearState();
        }
        return;
    }

    /* 先处理一下通用的按键，包括所有可能的按键，如普通的拼音字符按键、空格、Tab
     * 等等，然后再在下面处理其中的特殊的按键 */
    const bool r_mode_prefix_backspace = g_r_mode_triggered && Global::Keycode == VK_BACK && input_before_key.empty();
    if (r_mode_prefix_backspace)
    {
        ClearState();
    }
    else if (is_composition_edit_key && !r_mode_trigger_key)
    {
        ApplyCompositionEditKey(Global::Keycode, Global::Wch);
    }
    else if (should_forward_key_to_session)
    {
        g_inputSession->handle_key(Global::Keycode, Global::ModifiersDown, Global::Wch);
    }
    GlobalIme::composition.segmented_pinyin = g_inputSession->get_pinyin_segmentation_with_cases();
    GlobalIme::composition.raw_input_with_cases = g_inputSession->get_pinyin_sequence_with_cases();
    if (g_english_input_mode)
    {
        // English candidates are queried by the raw spelling. Do not expose
        // the Chinese pinyin session's syllable boundaries in the preedit.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (g_inputSession->get_pinyin_sequence_with_cases().empty() && !g_r_mode_triggered)
    {
        ClearSpecialModeTriggers();
    }
    if (!g_english_input_mode && g_r_mode_triggered)
    {
        // R is a visible mode prefix but is not part of the romaji sent to the
        // temporary Japanese engine. Keep both TSF and candidate-window preedit
        // aligned, including their caret coordinates.
        GlobalIme::composition.segmented_pinyin.insert(0, 1, 'R');
        GlobalIme::composition.raw_input_with_cases.insert(0, 1, 'R');
        ++GlobalIme::composition.caret_position;
    }
    if (!g_english_input_mode && IsUnicodeCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed U/+hex sequence.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsNumberCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed V-prefixed number.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsDateTimeCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsQuickPhraseCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed K-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsEmojiCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed E-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsKaomojiCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed M-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsJianpinCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed J-prefixed code.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    if (!g_english_input_mode && IsYModeCompositionActive(GlobalIme::composition.raw_input_with_cases))
    {
        // Keep preedit identical to the typed Y-prefixed English.
        GlobalIme::composition.segmented_pinyin = GlobalIme::composition.raw_input_with_cases;
    }
    //
    // 先判断要不要触发云联想
    // 判断依据：
    //  - 拼音序列长度是偶数
    //  - 最后一个字符不是大写字母
    //
    // Paging / selection must not bump async generations or re-apply cached
    // cloud/AI results (that previously reset page_index and re-cached duplicates).
    const bool suppress_async_lookup = is_paging_key || is_selection_key || is_unicode_shift_digit_selection;

    const auto cloud_query_state = g_inputSession->get_cloud_query_state();
    if (!g_english_input_mode && !suppress_async_lookup &&
        !IsSpecialModeCompositionActive(g_inputSession->get_pinyin_sequence_with_cases()) &&
        cloud_query_state.should_query)
    {
        UpdateCloudInput(cloud_query_state.query_text, client_id, activation_epoch);
    }

    const bool ai_eligible = !g_english_input_mode &&
                             !IsSpecialModeCompositionActive(g_inputSession->get_pinyin_sequence_with_cases()) &&
                             (g_inputSession->current_scheme_type() == SchemeType::Quanpin ||
                              g_inputSession->current_scheme_type() == SchemeType::Shuangpin) &&
                             g_inputSession->is_all_complete_pure_pinyin() && !g_inputSession->has_active_helpcode() &&
                             !GlobalIme::composition.creating_word.active;
    if (!suppress_async_lookup)
    {
        UpdateAiInput(ai_eligible ? g_inputSession->get_pinyin_segmentation() : std::string{}, client_id,
                      activation_epoch);
    }

    //
    // 普通的拼音字符，发送 preedit 到 TSF 端
    //
    if (FanyImeIpc::ShouldSendCompositionReply(Global::Keycode >= 'A' && Global::Keycode <= 'Z',
                                               is_manual_pinyin_separator, is_microsoft_shuangpin_ing_key,
                                               is_unicode_hex_digit, is_unicode_plus, is_number_point))
    {
        if (IsUiLessMode())
        {
            PrepareCandidateList(client_id, activation_epoch);
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
            {
                std::wstring preedit = GetPreedit();
                Global::MsgTypeToTsf = Global::DataFromServerMsgType::Preedit;
                Global::candidate_ui.selected_text = preedit;
                SendCurrentDataToClient(client_id, activation_epoch, request_id);
            }
        }
    }
    else if (Global::Keycode == VK_BACK || Global::Keycode == VK_DELETE)
    {
        if (IsUiLessMode())
        {
            if (g_inputSession->get_pinyin_sequence().empty())
            {
                ClearState();
                Global::MsgTypeToTsf = Global::DataFromServerMsgType::UiLessComposition;
                Global::candidate_ui.selected_text = L"\t";
                SendCurrentDataToClient(client_id, activation_epoch, request_id);
            }
            else
            {
                PrepareCandidateList(client_id, activation_epoch);
                SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
            }
        }
        else if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
        {
            if (!g_inputSession->get_pinyin_sequence().empty())
            {
                std::wstring preedit = GetPreedit();
                Global::MsgTypeToTsf = Global::DataFromServerMsgType::Preedit;
                Global::candidate_ui.selected_text = preedit;
                SendCurrentDataToClient(client_id, activation_epoch, request_id);
            }
        }
    }
    else if (IsUiLessMode() && is_composition_edit_key && Global::Keycode != VK_LEFT && Global::Keycode != VK_RIGHT &&
             Global::Keycode != VK_BACK)
    {
        PrepareCandidateList(client_id, activation_epoch);
        SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
    }

    //
    // 在以下情况下，TSF 端会请求候选字符串
    //  - 空格，会上屏第一个候选项
    //  - 数字，会上屏相应序号对应的候选项
    //
    // 空格和数字键可能会触发造词，如果数字键上屏的汉字字符串所对应的拼音比实际的拼音要短的话，
    // 那么，就可能会触发造词事件，那么，就要适时改变候选框的状态
    //
    /* VK_SPACE, Digits (U-mode / V-mode: Shift+1..9) */
    if (Global::Keycode == VK_SPACE || is_unicode_shift_digit_selection ||
        (!IsDigitCompositionActive(GlobalIme::composition.raw_input_with_cases) && Global::Keycode > '0' &&
         Global::Keycode <= '9'))
    {
        ProcessSelectionKey(Global::Keycode, client_id, activation_epoch);
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
    }
    else if (Global::Keycode == VK_LEFT || Global::Keycode == VK_RIGHT)
    {
        if (IsUiLessMode())
        {
            PrepareCandidateList(client_id, activation_epoch);
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            RefreshCandidatePageUi(true);
        }
    }
    else if (IsCandidateNavigationKey(Global::Keycode) && !is_unicode_plus && !is_number_point)
    {
        auto &ui = Global::candidate_ui;
        UINT result = Global::DataFromServerMsgType::NavigationIgnored;
        bool refresh = false;

        const auto expand_initial_candidates = [&] {
            if (!IsSpecialModeCompositionActive(GlobalIme::composition.raw_input_with_cases) &&
                g_inputSession->expand_initial_candidates())
            {
                const int current_page = ui.page_index;
                const int current_selection = ui.selected_index_in_page;
                auto expanded = g_inputSession->get_candidates();
                user_dictionary::apply_fixed_positions(
                    user_dictionary::default_user_db_path(), CurrentRankingContextKey(), expanded, true,
                    [](const std::string &key, const std::string &value) {
                        return g_inputSession->find_candidate(key, value);
                    },
                    g_inputSession->has_active_helpcode());
                ui.set_items(std::move(expanded));
                ui.page_index = current_page;
                ui.selected_index_in_page = current_selection;
                return true;
            }
            return false;
        };
        const auto move_page = [&](int offset, UINT response_type) {
            result = response_type;
            if (offset > 0 && ui.is_next_page_partial_last_page())
            {
                // Populate the last partial page before entering it, so the
                // first display of that page is already full.
                expand_initial_candidates();
            }
            else if (offset > 0 && !ui.has_next_page())
            {
                const bool current_page_was_full = ui.is_current_page_full();
                if (expand_initial_candidates() && !current_page_was_full)
                {
                    // Newly loaded items first fill the unused slots on the
                    // current last page. Refresh that page instead of skipping
                    // those items by advancing immediately.
                    refresh = true;
                    return;
                }
            }
            if (offset < 0 ? ui.has_prev_page() : ui.has_next_page())
            {
                ui.page_index += offset;
                refresh = true;
            }
        };
        const auto move_selection = [&](int offset, UINT response_type) {
            result = response_type;
            if (offset > 0 && (ui.is_selection_at_last_candidate() ||
                               (ui.is_selection_at_current_page_end() && ui.is_next_page_partial_last_page())))
            {
                expand_initial_candidates();
            }
            if (ui.move_selection(offset))
            {
                refresh = true;
            }
        };

        const bool shift_down = (Global::ModifiersDown & 0b00000001u) != 0;
        if (Global::Keycode == VK_OEM_MINUS && GetConfiguredPagingMinusEqualEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_OEM_PLUS && GetConfiguredPagingMinusEqualEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_OEM_COMMA && GetConfiguredPagingCommaPeriodEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_OEM_PERIOD && GetConfiguredPagingCommaPeriodEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_OEM_4 && GetConfiguredPagingBracketsEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_OEM_6 && GetConfiguredPagingBracketsEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_TAB && GetConfiguredPagingTabEnabled())
        {
            move_page(shift_down ? -1 : 1, shift_down ? Global::DataFromServerMsgType::MovePagePrevious
                                                      : Global::DataFromServerMsgType::MovePageNext);
        }
        else if (Global::Keycode == VK_PRIOR && GetConfiguredPagingPageUpDownEnabled())
        {
            move_page(-1, Global::DataFromServerMsgType::MovePagePrevious);
        }
        else if (Global::Keycode == VK_NEXT && GetConfiguredPagingPageUpDownEnabled())
        {
            move_page(1, Global::DataFromServerMsgType::MovePageNext);
        }
        else if (GetConfiguredCandidateArrowNavigationEnabled() &&
                 (Global::Keycode == VK_UP || Global::Keycode == VK_DOWN))
        {
            if (Global::Keycode == VK_UP)
            {
                move_selection(-1, Global::DataFromServerMsgType::MoveSelectionPrevious);
            }
            else
            {
                move_selection(1, Global::DataFromServerMsgType::MoveSelectionNext);
            }
        }

        if (IsUiLessMode())
        {
            if (refresh)
            {
                RefreshCandidatePageUi(false);
            }
            else
            {
                EnsureCandidatePageReady();
            }
            // Prefer selection index in page for host-drawn lists.
            if (!ui.page_words.empty())
            {
                ui.selected_index_in_page =
                    std::clamp(ui.selected_index_in_page, 0, static_cast<int>(ui.page_words.size()) - 1);
            }
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else
        {
            Global::MsgTypeToTsf = result;
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
            if (refresh)
            {
                RefreshCandidatePageUi(true);
            }
        }
    }
}

void ClearState()
{
    const auto r_mode_original_session = g_r_mode_original_session;
    ClearSpecialModeTriggers();
    ClearCandidateUiOwner();
    UpdateCloudInput("");
    UpdateEnglishInput("");
    g_candidate_translation_signature.clear();
    g_candidate_translation_glosses.clear();
    EnglishIme::ClearTranslations();
    CloudTranslation::Clear();
    UpdateEmojiInput("");
    UpdateKaomojiInput("");
    UpdateAiInput("");
    /* Clear dict engine state */
    g_inputSession->reset_state();
    if (r_mode_original_session)
    {
        g_inputSession = r_mode_original_session;
        g_r_mode_original_session.reset();
    }
    /* 造词的状态也要清理 */
    GlobalIme::composition.clear();
    // Drop published candidates before any in-flight FineTuneWindow callback
    // can re-inflate an empty-preedit + stale-candidate view.
    Global::CandidateString.clear();
    Global::ClearCandidatePageSnapshot();
    Global::candidate_ui.set_items({});
    // Hide synchronously from the caller's perspective: clear the shown flag
    // first so async FineTuneWindow callbacks refuse to resurrect the window,
    // then post the actual hide message (idempotent with TSF's HideCandidateWnd).
    ::is_global_wnd_cand_shown = false;
    if (::global_hwnd && IsWindow(::global_hwnd))
    {
        PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
    }
}

bool ResolveCandidateItem(int one_based_index, WordItem &item)
{
    if (!g_inputSession || one_based_index <= 0)
    {
        return false;
    }

    const auto &ui = Global::candidate_ui;
    const size_t indexInPage = static_cast<size_t>(one_based_index - 1);
    if (indexInPage >= ui.page_words.size() || ui.page_index < 0 || ui.page_size <= 0)
    {
        return false;
    }

    const size_t pageStart = static_cast<size_t>(ui.page_index) * static_cast<size_t>(ui.page_size);
    if (pageStart > ui.items.size() || indexInPage >= ui.items.size() - pageStart)
    {
        return false;
    }

    item = ui.items[pageStart + indexInPage];
    return true;
}

void ProcessSelectionKey(UINT keycode, uint64_t client_id, uint64_t activation_epoch, int forced_index_in_page)
{
    /* 先清理一下状态 */
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;

    static bool isNeedUpdateWeight = false;
    isNeedUpdateWeight = false;

    EnsureCandidatePageReady();

    const bool is_space = keycode == VK_SPACE;
    const bool is_digit_selection = keycode >= '1' && keycode <= '9';
    const bool is_direct_selection = forced_index_in_page >= 0;
    const int index = is_direct_selection
                          ? forced_index_in_page
                          : (is_space ? Global::candidate_ui.selected_index_in_page : static_cast<int>(keycode - '1'));
    WordItem curWordItem;
    const int page_size =
        Global::candidate_ui.page_size > 0 ? Global::candidate_ui.page_size : GetConfiguredCandidatePageSize();
    const bool within_page_size = !is_digit_selection || index < page_size;
    const bool is_valid_selection = within_page_size && (is_direct_selection || is_space || is_digit_selection) &&
                                    index >= 0 && static_cast<size_t>(index) < Global::candidate_ui.page_words.size() &&
                                    ResolveCandidateItem(index + 1, curWordItem);

    if (is_valid_selection)
    {
        // Capture ranking keys before reset_state()/composition advance clears the
        // input sequence. CandidateDatabaseKey() consults get_pinyin_sequence(),
        // and for single-code lists (e.g. "n") it must keep item.pinyin ("na"/"nv")
        // rather than falling back to the one-letter context key.
        const auto ranking_keys = RankingKeysForCandidate(curWordItem);
        const std::string ranking_context_key = ranking_keys.first;
        const std::string ranking_entry_key = ranking_keys.second;
        // First-page first slot is already the default commit; space/mouse/digit
        // should only learn when the user picked something else.
        const bool is_first_page_first = Global::candidate_ui.page_index == 0 && index == 0;
        isNeedUpdateWeight = !is_first_page_first;
        Global::candidate_ui.selected_text = Global::candidate_ui.page_words[index];
        std::string curWord = curWordItem.word;
        std::string curWordPinyin = curWordItem.pinyin;
        if (curWordItem.source == CandidateSource::EnglishDictionary ||
            curWordItem.source == CandidateSource::QuickPhrase || curWordItem.source == CandidateSource::Emoji ||
            curWordItem.source == CandidateSource::Kaomoji || curWordItem.source == CandidateSource::Generated)
        {
            Global::candidate_ui.selected_text =
                string_to_wstring(CandidateTextForOutput(GlobalIme::composition.creating_word.word + curWord));
            // 整句候选走的是这条提前返回的捷径，到不了下面 creating_word 的收尾逻辑，
            // 因此造好的词必须在这里落库，否则前缀 + 整句只上屏、学不到。
            // 拼音两段都是 canonical quanpin（creating_word.pinyin 由
            // append_canonical_pinyin 累积，lattice 候选的 canonical_pinyin 是整句 key），
            // 直接按 '\'' 拼接即可；音节数与汉字数是否匹配由
            // create_word_from_canonical_pinyin 自行校验，不匹配时安全地拒绝入库。
            if (FanyImeIpc::ShouldStoreEarlyReturnPhrase(
                    curWordItem.source, GlobalIme::composition.creating_word.active,
                    GlobalIme::composition.creating_word.pinyin, curWordItem.canonical_pinyin))
            {
                // 这里异步处理，不然有可能会阻塞住 TSF 端读取 pipe 导致超时
                EnqueueStoreUserPhraseTask(GlobalIme::composition.creating_word.pinyin + "'" +
                                               curWordItem.canonical_pinyin,
                                           GlobalIme::composition.creating_word.word + curWord,
                                           /*pinyin_is_canonical=*/true);
            }
            // 同理，这条捷径也到不了下面的 AI 上下文累积。造词前缀在它自己被选中的那次
            // ProcessSelectionKey 里已经追加过了，这里只补本次提交的这一段。
            AppendAiContext(curWord);
            if (curWordItem.source == CandidateSource::EnglishDictionary && isNeedUpdateWeight)
            {
                const auto &frequency = GetConfiguredFrequencyAdjustment();
                (void)user_dictionary::adjust_english_candidate_ranking(
                    CommonUtils::get_ime_data_path() + "\\english.db", user_dictionary::default_user_db_path(),
                    EnglishRankingContextKey(), Global::candidate_ui.items, curWordItem.pinyin, curWordItem.word,
                    frequency.mode, frequency.linear_step, frequency.trigger_count, false);
            }
            UpdateCloudInput("");
            UpdateEnglishInput("");
            UpdateEmojiInput("");
            UpdateKaomojiInput("");
            g_inputSession->reset_state();
            if (g_r_mode_original_session)
            {
                g_inputSession = g_r_mode_original_session;
                g_r_mode_original_session.reset();
            }
            GlobalIme::composition.clear();
            ClearSpecialModeTriggers();
            return;
        }
        std::string cloudCommittedPinyin;
        std::string aiCommittedPinyin;
        bool aiCommittedPinyinIsCanonical = false;
        if (curWordItem.source == CandidateSource::CloudSuggestion)
        {
            cloudCommittedPinyin = g_inputSession->get_cloud_query_state().committed_pinyin;
        }
        if (curWordItem.source == CandidateSource::AiSuggestion)
        {
            if (g_inputSession->is_all_complete_pure_pinyin())
            {
                aiCommittedPinyin = curWordItem.canonical_pinyin.empty()
                                        ? g_inputSession->get_cloud_query_state().committed_pinyin
                                        : curWordItem.canonical_pinyin;
                aiCommittedPinyinIsCanonical = !curWordItem.canonical_pinyin.empty();
            }
            isNeedUpdateWeight = false;
        }
        auto selection_transition =
            g_inputSession->advance_composition_after_selection(curWordPinyin, curWord, curWordItem.canonical_pinyin);
        // A cloud suggestion is an already-composed result returned for the
        // current query.  It must commit as one candidate even when the
        // returned query spelling is shorter than the raw input (for example
        // with an abbreviation or an active help-code suffix).  Treating it
        // like an ordinary partial candidate enters word-creation mode, while
        // the cloud branch below still persists the selected word.
        const bool isNeedCreateWord =
            FanyImeIpc::ShouldEnterCreatingWord(curWordItem.source, selection_transition.continues_composition);
        if (isNeedCreateWord)
        { /* 候选只消耗了输入的一部分，继续使用剩余输入造词。完整拼音和简拼均可进入。 */
            /* 打开造词开关 */
            GlobalIme::composition.creating_word.active = true;
            Global::MsgTypeToTsf = Global::DataFromServerMsgType::NeedToCreateWord;
            GlobalIme::composition.segmented_pinyin = selection_transition.current_segmentation_with_cases;

            PrepareCandidateList(client_id, activation_epoch);
        }

        // 详细处理一下造词的逻辑
        if (GlobalIme::composition.creating_word.active)
        {
            /* 造词的时候，不可以更新词频 */
            isNeedUpdateWeight = false;

            const auto creating_word_progress = g_inputSession->update_creating_word_progress(
                GlobalIme::composition.creating_word.pinyin, GlobalIme::composition.creating_word.word, curWord,
                selection_transition);
            GlobalIme::composition.creating_word.pinyin = creating_word_progress.pinyin;
            GlobalIme::composition.creating_word.word = creating_word_progress.word;
            GlobalIme::composition.creating_word.preedit = creating_word_progress.preedit;
            /* 更新一下中间态的造词时 tsf 端所需的数据 */
            Global::candidate_ui.selected_text = BuildCreateWordPipePayload(
                g_inputSession->get_pinyin_sequence_with_cases(), GlobalIme::composition.creating_word.word);
            if (creating_word_progress.completed)
            { /* 最终的造词 */
#ifdef FANY_DEBUG
                (void)0;
#endif

                /* 更新一下被选中的候选项 */
                Global::candidate_ui.selected_text =
                    string_to_wstring(CandidateTextForOutput(GlobalIme::composition.creating_word.word));

                if (creating_word_progress.can_store)
                {
                    // 这里异步处理，不然有可能会阻塞住 TSF 端读取 pipe 导致超时
                    EnqueueStoreUserPhraseTask(GlobalIme::composition.creating_word.pinyin,
                                               GlobalIme::composition.creating_word.word,
                                               /*pinyin_is_canonical=*/true);
                }

                /* 清理 */
                GlobalIme::composition.clear_creating_word();
            }
        }

        // 看看云联想出来的词是否需要被插入到数据库
        if (curWordItem.source == CandidateSource::CloudSuggestion && !cloudCommittedPinyin.empty())
        {
            EnqueueStoreUserPhraseTask(cloudCommittedPinyin, curWord);
            // 清理云联想变量状态
            Global::cloud_candidate.added = false;
            Global::cloud_candidate.word.clear();
            Global::cloud_candidate.pinyin.clear();
        }
        if (curWordItem.source == CandidateSource::AiSuggestion && !aiCommittedPinyin.empty())
        {
            EnqueueStoreUserPhraseTask(aiCommittedPinyin, curWord, aiCommittedPinyinIsCanonical);
            Global::ai_candidate = {};
        }

        AppendAiContext(curWord);

        if (!isNeedCreateWord)
        {
            g_inputSession->reset_state();
            if (g_r_mode_original_session)
            {
                g_inputSession = g_r_mode_original_session;
                g_r_mode_original_session.reset();
            }
            GlobalIme::composition.caret_position = 0;
            GlobalIme::composition.raw_input_with_cases.clear();
            ClearSpecialModeTriggers();
        }
        else
        {
            /* TODO: 这里到 main 线程的时候，可能下面的那个清理状态的操作已经执行了，因此，这里可能会导致 string
             * 越界的问题 */
            RequestShowCandidateWindow();
        }

        if (isNeedUpdateWeight)
        {
            const auto &frequency = GetConfiguredFrequencyAdjustment();
            bool ranking_changed = false;
            (void)user_dictionary::adjust_candidate_ranking(
                CommonUtils::get_ime_data_path() + "\\msime.db", user_dictionary::default_user_db_path(),
                ranking_context_key, Global::candidate_ui.items, ranking_entry_key, curWord, frequency.mode,
                frequency.linear_step, frequency.trigger_count, false, &ranking_changed,
                IsWubiRankingScheme() ? user_dictionary::DictionaryKind::Wubi
                                      : user_dictionary::DictionaryKind::Pinyin);
            if (ranking_changed)
            {
                g_inputSession->reset_cache();
            }
        }
    }
    else
    {
        Global::candidate_ui.selected_text = L"OutofRange";
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::OutofRange;
    }
}

} // namespace FanyNamedPipe
