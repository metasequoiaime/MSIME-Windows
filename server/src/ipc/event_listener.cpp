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
#include "ipc/candidate_render_sync.h"
#include "ipc/candidate_selection_policy.h"
#include "ipc/async_request_origin.h"
#include "ipc/candidate_ui_owner.h"
#include "ipc/candidate_text_policy.h"
#include "ipc/candidate_translation_policy.h"
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
#include "window/caret_state_indicator.h"
#include "window/caret_state_indicator_policy.h"
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
#include "engine/shuangpin/shuangpin_profile.h"
#include "emoji/emoji_ime.h"
#include "kaomoji/kaomoji_ime.h"
#include "log/candidate_diag_log.h"
#include "log/ftb_diag_log.h"
#include "statistics/stats_pipe.h"
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

// 副候选框：Ctrl+Enter 在高亮候选有多条译义时，把候选框整个换成那几条译义，让空格/
// 数字键像选普通候选一样选一条上屏。输入串一个字都没动，所以退出这个子模式时把原来的
// items / 页码 / 高亮位原样放回去就行，不需要重新查词。
bool g_translation_candidates_active = false;
std::vector<WordItem> g_translation_saved_items;
int g_translation_saved_page_index = 0;
int g_translation_saved_selected_index = 0;

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
        Global::candidate_window_rendered_visible.store(false, std::memory_order_relaxed);
        if (::global_hwnd && IsWindow(::global_hwnd))
        {
            PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
        }
        // Same for a caret badge shown just before the host went UILess.
        if (const HWND caretState = ::global_hwnd_caret_state)
        {
            PostMessage(caretState, WM_HIDE_CARET_STATE, 0, 0);
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

// Drop every published candidate page and hide the candidate window without
// touching the composition session. The creating-word state can survive with no
// pinyin left after a segment Backspace, and that state must not be reset just
// because there is nothing left to offer as candidates.
void HideCandidateWindowAndDropItems()
{
    // Drop published candidates before any in-flight FineTuneWindow callback can
    // re-inflate an empty-preedit + stale-candidate view.
    Global::CandidateString.clear();
    Global::ClearCandidatePageSnapshot();
    Global::candidate_ui.set_items({});
    // Clear the shown flag first so async callbacks refuse to resurrect the
    // window, then post the actual hide message.
    ::is_global_wnd_cand_shown = false;
    Global::candidate_window_rendered_visible.store(false, std::memory_order_relaxed);
    if (::global_hwnd && IsWindow(::global_hwnd))
    {
        PostMessage(::global_hwnd, WM_HIDE_MAIN_WINDOW, 0, 0);
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
    return g_quick_phrase_triggered || g_unicode_mode_triggered || g_date_time_mode_triggered ||
           g_emoji_mode_triggered || g_kaomoji_mode_triggered || g_jianpin_mode_triggered || g_y_mode_triggered ||
           g_r_mode_triggered;
}

void ClearSpecialModeTriggers()
{
    g_quick_phrase_triggered = false;
    g_unicode_mode_triggered = false;
    g_date_time_mode_triggered = false;
    g_emoji_mode_triggered = false;
    g_kaomoji_mode_triggered = false;
    g_jianpin_mode_triggered = false;
    g_y_mode_triggered = false;
    g_r_mode_triggered = false;
}

// True whenever a K/U/T/E/M/J/Y special-mode composition is in progress, even when the
// typed text is not yet a complete keyword/hex sequence. Such input must never
// be interpreted as normal pinyin.
bool IsSpecialModeCompositionActive(const std::string &raw)
{
    return IsQuickPhraseCompositionActive(raw) || IsUnicodeCompositionActive(raw) || IsDateTimeCompositionActive(raw) ||
           IsEmojiCompositionActive(raw) || IsKaomojiCompositionActive(raw) || IsJianpinCompositionActive(raw) ||
           IsYModeCompositionActive(raw);
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

// Caret-badge events are presentation only. They never touch the toolbar or
// the status snapshot: StatusSnapshot remains the single source of mode state.
void PostCaretStateBadge(FanyImeUi::CaretStateBadge badge, int x, int y)
{
    const HWND hwnd = ::global_hwnd_caret_state;
    if (!hwnd || !FanyImeUi::IsUsableCaretAnchor(x, y))
        return;
    auto *request = new (std::nothrow) CaretStateIndicator::ShowRequest{std::move(badge), POINT{x, y}, IsUiLessMode()};
    if (request && !PostMessage(hwnd, WM_SHOW_CARET_STATE, 0, reinterpret_cast<LPARAM>(request)))
        delete request;
}

void PostCaretStatePosition(int x, int y)
{
    const HWND hwnd = ::global_hwnd_caret_state;
    // Visibility is owned by the UI thread, which ignores moves while hidden.
    if (!hwnd)
        return;
    auto *caret = new (std::nothrow) POINT{x, y};
    if (caret && !PostMessage(hwnd, WM_MOVE_CARET_STATE, 0, reinterpret_cast<LPARAM>(caret)))
        delete caret;
}

void PostHideCaretState()
{
    if (const HWND hwnd = ::global_hwnd_caret_state)
        PostMessage(hwnd, WM_HIDE_CARET_STATE, 0, 0);
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
    // 同一份上屏历史也是神经整句重排的前文（青简那边叫 InputHistory）：下一次查询会随
    // QueryRequest 下发到词典层。词典层自己按 RerankOptions::context_chars 取末尾若干字。
    if (g_inputSession)
        g_inputSession->set_rescoring_context(g_ai_context);
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

// NeedToCreateWord 带光标变体。可选第 4 字段（offset into remaining_raw）必须以
// CompositionRestore 协商为前提：旧 DLL 的解析器把第 2 个 '\t' 之后的尾部整个当
// display_preedit，未协商时追加会污染 inline preedit（AC8），此时帧与旧 Server 的
// plain builder 字节一致。协商侧前缀选词结算后光标归后缀首（0），必须显式携带，
// 否则 DLL 按省略语义把光标镜到末尾。串尾造词流 caret 恒在末尾，不带字段。
std::wstring BuildCreateWordPipePayloadWithCaret(bool client_supports_restore,
                                                 const std::string &remaining_raw_input_with_cases,
                                                 const std::string &current_word)
{
    std::wstring payload = BuildCreateWordPipePayload(remaining_raw_input_with_cases, current_word);
    if (FanyImeIpc::ShouldCreateWordFrameCarryCaret(client_supports_restore, GlobalIme::composition.caret_position,
                                                    remaining_raw_input_with_cases.size()))
    {
        payload += L'\t' + std::to_wstring(GlobalIme::composition.caret_position);
    }
    return payload;
}

// 日语模式由配置项决定，和 R 模式（中文里临时切日语）无关：TSF 侧只能看到配置，
// 两侧必须用同一个判据，否则按键分类会不一致、预编辑会错位。
bool IsJapaneseInputMode()
{
    return GetConfiguredInputMode() == "japanese";
}

// 日语模式下 '-' 不翻页，而是长音符（ー）的输入键。空编码时也要起头组合，
// 候选框第一项是长音符 ー、第二项是普通连字符 '-'（见日语候选提供者）。
bool IsJapaneseLongVowelKey(UINT keycode, WCHAR wch)
{
    return keycode == VK_OEM_MINUS && wch == L'-' && IsJapaneseInputMode() && g_inputSession != nullptr;
}

// 日语模式下 '-' '=' 一律不当翻页键用。
bool IsJapaneseDisabledPagingKey(UINT keycode)
{
    return (keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS) && IsJapaneseInputMode();
}

bool IsCommitWithHighlightedCandidatePunctuationInCandidateMode(UINT keycode, WCHAR wch)
{
    if (keycode == VK_TAB)
    {
        return false;
    }
    if ((keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS) && !IsJapaneseDisabledPagingKey(keycode))
    {
        return false;
    }
    // 日语模式下 '-' 走长音符输入，不能当作上屏标点。
    if (IsJapaneseLongVowelKey(keycode, wch))
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
        L'_',  // 日语模式禁用 -/= 翻页后，这两个字符退回标点上屏。
        L'=',  //
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
        if (IsUnicodeCompositionActive(raw))
        {
            // U-mode: bare digits compose hex; Shift+1..9 selects candidates.
            const bool shift_only = (Global::ModifiersDown & 0b00000111u) == 0b00000001u;
            return shift_only && keycode >= '1' && keycode <= '9';
        }
        return true;
    }
    return false;
}

bool IsPagingKey(UINT keycode)
{
    if (IsJapaneseDisabledPagingKey(keycode))
    {
        return false;
    }
    return keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS || keycode == VK_TAB || keycode == VK_PRIOR ||
           keycode == VK_NEXT || keycode == VK_LEFT || keycode == VK_RIGHT || keycode == VK_UP || keycode == VK_DOWN ||
           ((keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD) && GetConfiguredPagingCommaPeriodEnabled()) ||
           ((keycode == VK_OEM_4 || keycode == VK_OEM_6) && GetConfiguredPagingBracketsEnabled());
}

bool IsCandidateNavigationKey(UINT keycode)
{
    if (IsJapaneseDisabledPagingKey(keycode))
    {
        return false;
    }
    return keycode == VK_OEM_MINUS || keycode == VK_OEM_PLUS || keycode == VK_OEM_COMMA || keycode == VK_OEM_PERIOD ||
           keycode == VK_OEM_4 || keycode == VK_OEM_6 || keycode == VK_TAB || keycode == VK_PRIOR ||
           keycode == VK_NEXT || keycode == VK_UP || keycode == VK_DOWN;
}

bool ApplyCompositionEditKey(UINT keycode, WCHAR wch, UINT modifiers_down, bool client_supports_restore,
                             bool &composition_restored)
{
    composition_restored = false;
    std::string raw = g_inputSession->get_pinyin_sequence_with_cases();
    auto &composition = GlobalIme::composition;
    if (composition.raw_input_with_cases != raw && composition.caret_position == 0 && !raw.empty())
    {
        composition.caret_position = raw.size();
    }
    composition.caret_position = (std::min)(composition.caret_position, raw.size());

    // R2/R10：光标前缀重算总门控（与 Ctrl+Backspace / Ctrl+方向同一谓词族）。未协商、
    // UILess、专用英文、特殊模式组合一律维持整串转换，光标只是显示层插入点。
    const bool caret_resegmentation = FanyImeIpc::ShouldResegmentCompositionByCaret(
        client_supports_restore, IsUiLessMode(), g_english_input_mode, IsSpecialModeCompositionActive(raw));
    // 箭头与 Ctrl+方向路径不改 raw、没有 pending 序列，喂完光标重解一次即可。串尾
    // 也必须显式喂：引擎 caret_ 只在 set_pinyin_sequence 触发的 apply_pending_sequence
    // 里复位，箭头路径绕过它——串尾不喂 nullopt（与 set_caret(size) 在量化边界上等
    // 价）会残留上一次前缀激活的 caret_，候选停在旧前缀上、空格结算走错前缀路径（R7）。
    const auto resegment_by_caret = [&]() {
        if (!caret_resegmentation)
        {
            return;
        }
        g_inputSession->set_caret(composition.caret_position < raw.size()
                                      ? std::optional<std::size_t>(composition.caret_position)
                                      : std::nullopt);
        g_inputSession->recompute_candidates();
    };

    // Ctrl+Left / Ctrl+Right jump the caret by one segmentation unit instead of
    // one character, consuming the same engine boundaries Ctrl+Backspace
    // deletes. The Server owns the unit model, so it moves the authoritative
    // caret and answers with CompositionRestored; TSF only applies that caret.
    // Everything unnegotiated, UILess or unit-less keeps the single-character
    // move below, so both sides agree on when the jump happens.
    const bool segment_caret = FanyImeIpc::IsSegmentCaretKey(keycode, modifiers_down);
    const bool segment_caret_supported = segment_caret && client_supports_restore && !IsUiLessMode() &&
                                         !g_english_input_mode && !IsSpecialModeCompositionActive(raw);
    if (keycode == VK_LEFT || keycode == VK_RIGHT)
    {
        if (segment_caret_supported)
        {
            const std::vector<std::size_t> boundaries = g_inputSession->segment_raw_boundaries();
            if (!boundaries.empty())
            {
                composition.caret_position =
                    keycode == VK_LEFT ? FanyImeIpc::PreviousSegmentBoundary(boundaries, composition.caret_position)
                                       : FanyImeIpc::NextSegmentBoundary(boundaries, composition.caret_position);
                composition_restored = true;
                resegment_by_caret();
                return true;
            }
        }
        if (keycode == VK_LEFT)
        {
            if (composition.caret_position > 0)
            {
                --composition.caret_position;
            }
        }
        else if (composition.caret_position < raw.size())
        {
            ++composition.caret_position;
        }
        resegment_by_caret();
        return true;
    }

    // A spelling emptied by a segment Backspace below keeps the creating-word
    // state alive with the word alone (PRD R3), so the empty-raw cleanup has to
    // know this key produced that state on purpose.
    bool keep_creating_word_after_empty_raw = false;

    if (keycode == VK_BACK)
    {
        // Ctrl+Backspace deletes one segmentation unit (one character's pinyin)
        // instead of one character. The boundaries are the engine's, so TSF
        // cannot mirror the deletion: it rebuilds from the CompositionRestored
        // reply, and everything unnegotiated or unit-less falls back to the
        // ordinary single-character behavior right below.
        const bool segment_backspace = FanyImeIpc::IsSegmentBackspaceKey(keycode, modifiers_down);
        const bool segment_supported = segment_backspace && client_supports_restore && !IsUiLessMode() &&
                                       !g_english_input_mode && !IsSpecialModeCompositionActive(raw);
        if (segment_supported && FanyImeIpc::ShouldDropCreatingWordSegment(
                                     composition.creating_word.active, IsUiLessMode(), client_supports_restore,
                                     composition.caret_position, composition.selection_history.size()))
        {
            // R3: nothing is left before the caret, so the key removes the last
            // selected segment itself. Its spelling is discarded -- unlike the
            // retraction below the user asked to delete the segment, not to
            // edit its pinyin again -- and the raw stays empty.
            composition_restored = composition.drop_last_selection();
            keep_creating_word_after_empty_raw = composition_restored && composition.creating_word.active;
        }
        else if (segment_supported)
        {
            const std::vector<std::size_t> boundaries = g_inputSession->segment_raw_boundaries();
            const std::size_t start = FanyImeIpc::PreviousSegmentBoundary(boundaries, composition.caret_position);
            if (start < composition.caret_position)
            {
                raw.erase(start, composition.caret_position - start);
                composition.caret_position = start;
                FanyImeIpc::DropDanglingSegmentDelimiter(raw, start);
                composition_restored = true;
                // Emptying the raw does not end the word: the accumulated
                // segments stay on screen and the next Ctrl+Backspace drops one
                // of them (R3).
                keep_creating_word_after_empty_raw =
                    raw.empty() && FanyImeIpc::ShouldKeepCreatingWordAfterRawEmptied(
                                       composition.creating_word.active, IsUiLessMode(), client_supports_restore,
                                       composition.selection_history.size());
            }
        }

        if (!composition_restored &&
            FanyImeIpc::ShouldRetreatCreatingWordSelection(
                composition.creating_word.active, IsUiLessMode(), client_supports_restore, raw.size(),
                composition.selection_history.size(), composition.last_selection_raw_edited()))
        {
            // This Backspace must not also delete the character: the retraction
            // removes the segment and restores its raw spelling instead. The
            // restore is state only -- the engine sequence, its candidates and
            // the restored-caret prefix are applied exactly once by the tail
            // below, so nothing here may rebuild them; a helper that did cost
            // a second full candidate query on every retraction.
            composition_restored = composition.restore_last_selection();
            if (composition_restored)
            {
                // The retraction already replaced the raw, the word and the
                // caret; the local copy must follow it so the tail below
                // re-applies the restored sequence with its caret prefix
                // recompute instead of clobbering it with the stale raw.
                raw = composition.raw_input_with_cases;
            }
        }
        else if (!composition_restored && composition.caret_position > 0)
        {
            raw.erase(composition.caret_position - 1, 1);
            --composition.caret_position;
            // Deleting the last raw character must not take the accumulated word
            // down with it: the composition stays alive showing the selected
            // segments alone -- the same R3 state a segment Backspace produces --
            // and the reply below tells the client to keep composing instead of
            // cancelling. The next Backspace then retracts the newest selection
            // from that empty raw (the empty-raw override of the edit lock)
            // rather than discarding everything the user picked.
            keep_creating_word_after_empty_raw =
                raw.empty() && FanyImeIpc::ShouldKeepCreatingWordAfterRawEmptied(
                                   composition.creating_word.active, IsUiLessMode(), client_supports_restore,
                                   composition.selection_history.size());
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
        else if (IsJapaneseLongVowelKey(keycode, wch))
        {
            input = '-';
        }
        else if (IsUnicodeCompositionActive(raw) && keycode >= '0' && keycode <= '9')
        {
            input = static_cast<char>(keycode);
        }
        else if (IsUnicodeCompositionActive(raw) && keycode == VK_OEM_PLUS && wch == L'+' && raw == "U")
        {
            input = '+';
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
        // Typing locks the newest selection (Rime's selected_before_editing):
        // Backspace must keep deleting these fresh characters instead of
        // retracting the selection out from under them. Caret moves never reach
        // here and deliberately do not lock.
        composition.note_raw_inserted();
    }

    if (raw.empty() && !keep_creating_word_after_empty_raw)
    {
        // Without a kept state, TSF cancels the whole composition as soon as the
        // last remaining character is gone, so the accumulated word and the
        // snapshots a later Backspace could retract from must not survive here:
        // they would let a fresh pinyin composition retract a segment of the
        // previous one. Both Backspaces that legitimately empty the raw keep them
        // on purpose instead: the segment one through R3, the plain one because
        // its reply tells the client to keep composing with the word alone.
        composition.clear_creating_word();
        composition.selection_history.clear();
    }

    g_inputSession->set_pinyin_sequence(raw);
    g_inputSession->set_pinyin_sequence_with_cases(raw);
    if (caret_resegmentation && composition.caret_position < raw.size())
    {
        // apply_pending_sequence() 会复位引擎光标：先让新 raw 生效，再喂光标做前缀
        // 重解（R2/R6）。caret 在串尾时不进这里，上一次 recompute 就是现状整串解码
        // （R7 零回归）。
        g_inputSession->recompute_candidates();
        g_inputSession->set_caret(composition.caret_position);
        g_inputSession->recompute_candidates();
    }
    else
    {
        g_inputSession->recompute_candidates();
    }
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
    // 副候选框里装的是译文，不是这次输入的候选：助记码、云/AI 角标和「右侧译文」都不适用，
    // 而且 g_candidate_translation_glosses 还留着原候选的译文，照常查会把译文再标注一遍。
    const bool translation_page = g_translation_candidates_active;
    const bool show_helpcodes =
        !translation_page && ((current_scheme == SchemeType::Shuangpin && GetConfiguredShuangpinHelpcodeEnabled() &&
                               GetConfiguredShowShuangpinHelpcodeInCandidateWindow()) ||
                              (current_scheme == SchemeType::Quanpin && GetConfiguredQuanpinHelpcodeEnabled() &&
                               GetConfiguredShowQuanpinHelpcodeInCandidateWindow()));

    // 组页时读一次徽标配置，循环内不再逐条查
    const bool show_fixed_badge = GetConfiguredCandidateFixedBadge();
    const std::string fixed_badge_style = GetConfiguredCandidateFixedBadgeStyle();

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
        // 整句来源标签与设置页名称保持一致，方便同时比较四个来源。两家选中同一句时只剩一行，
        // 标签归先保留下来的来源；开启去重补位后，其余来源会改为显示自己的下一条不同结果。
        else if (item.source == CandidateSource::Generated)
            view.badge = " 〔Trigram〕";
        else if (item.source == CandidateSource::Fallback)
            view.badge = " 〔Unigram〕";
        else if (item.source == CandidateSource::NeuralDesktop)
            view.badge = " 〔神经D〕";
        else if (item.source == CandidateSource::NeuralKeyboard)
            view.badge = " 〔神经K〕";
        view.fixed_position = item.fixed_position > 0;
        ApplyFixedPositionBadge(view, show_fixed_badge, fixed_badge_style);
        EnglishIme::TranslationQuery translation_query;
        if (!translation_page && BuildTranslationQuery(item, translation_query))
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
    if (g_translation_candidates_active)
    {
        // 译文页不再查译文。已经取出的 glosses 也不能清，退出子模式后原候选还要用。
        return;
    }
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
    // Stamp before publishing so the UI thread can echo back exactly which page it painted.
    snapshot->generation = ++Global::candidate_page_generation;
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
    case FanyImePipeEventType::HideCaretState:
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
    WakePipeListener(FANY_IME_VOICE_CONTROL_NAMED_PIPE);
    // The statistics listener blocks in ConnectNamedPipe on a PIPE_WAIT
    // instance just like the others, but its name carries the session id.
    // Without this wake a Server that never saw a statistics client would hang
    // in stats_pipe_listener.join() forever.
    WakePipeListener(MsimeStats::BuildStatsPipeName(MsimeStats::CurrentStatsSessionId()).c_str());
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
    HideCaretState,
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
    UiPageUp,
    UiPageDown,
    ReloadInputSession,
    EnsureInputSessionMatchesConfig,
    ApplyCandidatePageSize,
    RefreshCandidatePage,
    ResetInputSessionCache,
    ExitEnglishInputMode,
    // 神经整句重排在后台线程里算完了，候选顺序需要就地更新一次。
    ApplyRescoredOrder,
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
    int page_steps = 0;
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

// Pulls the next batch of candidates out of the session without disturbing
// where the user currently is: set_items resets the page and the selection, so
// both are restored afterwards. Returns false when nothing more was loaded.
bool ExpandCandidatesKeepingPagePosition()
{
    auto &ui = Global::candidate_ui;
    // 译文页是一份固定的列表，session 里再多的候选也不属于它。
    if (g_translation_candidates_active ||
        IsSpecialModeCompositionActive(GlobalIme::composition.raw_input_with_cases) || !g_inputSession ||
        !g_inputSession->expand_initial_candidates())
    {
        return false;
    }
    const int current_page = ui.page_index;
    const int current_selection = ui.selected_index_in_page;
    auto expanded = g_inputSession->get_candidates();
    user_dictionary::apply_fixed_positions(
        user_dictionary::default_user_db_path(), CurrentRankingContextKey(), expanded, true,
        [](const std::string &key, const std::string &value) { return g_inputSession->find_candidate(key, value); },
        g_inputSession->has_active_helpcode());
    ui.set_items(std::move(expanded));
    ui.page_index = current_page;
    ui.selected_index_in_page = current_selection;
    return true;
}

enum class PageMoveResult
{
    Unchanged,
    // An expansion filled out the current page; the page index did not move.
    CurrentPageRefilled,
    Moved,
};

// Moves one page in `offset`'s direction, expanding the candidate list first
// when the move would run off the end. Anything other than Unchanged needs a
// UI refresh.
PageMoveResult MoveCandidatePage(int offset)
{
    auto &ui = Global::candidate_ui;
    if (offset > 0 && ui.is_next_page_partial_last_page())
    {
        // Populate the last partial page before entering it, so the first
        // display of that page is already full.
        ExpandCandidatesKeepingPagePosition();
    }
    else if (offset > 0 && !ui.has_next_page())
    {
        const bool current_page_was_full = ui.is_current_page_full();
        if (ExpandCandidatesKeepingPagePosition() && !current_page_was_full)
        {
            // Newly loaded items first fill the unused slots on the current last
            // page. Refresh that page instead of skipping those items by
            // advancing immediately.
            return PageMoveResult::CurrentPageRefilled;
        }
    }
    if (offset < 0 ? ui.has_prev_page() : ui.has_next_page())
    {
        ui.page_index += offset;
        return PageMoveResult::Moved;
    }
    return PageMoveResult::Unchanged;
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

// 顶字推送后的 HideCandidate 抑制标记：CommitCandidateAndContinue 会让 DLL 提交文本并
// 结束旧组合，TSF 随之发来 HideCandidateWnd；若 HideCandidate 处理器照常 ClearState，
// 会把服务端刚重建好的余码组合（如「数据」顶字后剩下的 x）抹掉，用户后续按键从空组合
// 开始组词——这正是「顶字后 x 没进组词」的根因。所有值都只在 worker 线程读写。
// 顶字与四码唯一自动上屏都会推送，每次推送成功记一笔、每个 HideCandidate 消费一笔；
// 不能在下一个按键时撤销：快打时下一个字母常常先于 DLL 应用推送到达服务端，那时撤销
// 标记，随后到来的 HideCandidateWnd 就会把余码清掉。推送被 DLL 丢弃（焦点/组合纪元已变）
// 时不会有对应的 HideCandidateWnd，所以另设一个时限，过期的记账不再压制真正的清理。
constexpr ULONGLONG kTopCommitHideSuppressMs = 1000;
uint64_t g_topCommitRemainderClient = 0;
uint64_t g_topCommitRemainderEpoch = 0;
uint32_t g_topCommitPendingHides = 0;
ULONGLONG g_topCommitLastPushMs = 0;

void NoteTopCommitPushed(uint64_t client_id, uint64_t activation_epoch)
{
    if (client_id != g_topCommitRemainderClient || activation_epoch != g_topCommitRemainderEpoch)
    {
        g_topCommitPendingHides = 0;
    }
    g_topCommitRemainderClient = client_id;
    g_topCommitRemainderEpoch = activation_epoch;
    ++g_topCommitPendingHides;
    g_topCommitLastPushMs = GetTickCount64();
}

// 消费一笔推送记账；返回这个 HideCandidate 是否对应一次仍在时限内的顶字/自动上屏推送。
bool ConsumeTopCommitPendingHide(uint64_t client_id, uint64_t activation_epoch)
{
    if (g_topCommitPendingHides == 0 || client_id != g_topCommitRemainderClient ||
        activation_epoch != g_topCommitRemainderEpoch)
    {
        return false;
    }
    --g_topCommitPendingHides;
    return GetTickCount64() - g_topCommitLastPushMs <= kTopCommitHideSuppressMs;
}

void PrepareCandidateList(uint64_t client_id, uint64_t activation_epoch);
void HandleImeKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id);
void ClearState();
void ProcessSelectionKey(UINT keycode, uint64_t client_id, uint64_t activation_epoch, int forced_index_in_page = -1);
void WaitForCandidateRenderSync(UINT keycode);
void ApplyCloudCandidate(const std::string &candidate, const std::string &pinyin, uint64_t generation,
                         const std::optional<metasequoia::OnlineQuery> &query);
void ApplyRescoredOrder();
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
            // 这条丢弃无其他日志；排查丢键时先看这里（2026-09 曾疑似顶字丢键，探针证实
            // 该路径并未触发，日志留作以后定位任务消失的入口）。
            CAND_DIAG_LOGF(L"task stale-dropped type={} client={} task_epoch={} current_epoch={}",
                           static_cast<int>(task.type), task.client_id, task.activation_epoch,
                           GetActivePipeClient().epoch);
            continue;
        }

        const bool candidateUiAction =
            task.type == TaskType::UiCommitCandidate || task.type == TaskType::UiPinCandidate ||
            task.type == TaskType::UiDeleteCandidate || task.type == TaskType::UiFixCandidatePosition ||
            task.type == TaskType::UiClearCandidatePosition || task.type == TaskType::UiPageUp ||
            task.type == TaskType::UiPageDown;
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
            if (FanyImeIpc::IsCaretPrefixEmpty(g_inputSession->prefix_end(),
                                               g_inputSession->get_pinyin_sequence_with_cases().size()))
            {
                // R4：光标前缀为空时 DLL 的 Show 事件不得唤醒一个空候选窗。
                HideCandidateWindowAndDropItems();
                break;
            }
            PrepareCandidateList(task.client_id, task.activation_epoch);
            RequestShowCandidateWindow();
            break;
        }

        case TaskType::HideCandidate: {
            ::ReadDataFromNamedPipe(0b100000);
            const ULONGLONG queue_elapsed_ms = task.enqueued_at_ms == 0 ? 0 : GetTickCount64() - task.enqueued_at_ms;
            CAND_DIAG_LOGF(L"task HideCandidate client={} epoch={} request={} queued_ms={}", task.client_id,
                           task.activation_epoch, task.pipe_data.request_id, queue_elapsed_ms);
            // 顶字/自动上屏推送引发的 TSF HideCandidateWnd：余码组合还活着，绝不能 ClearState，
            // 否则用户刚敲下的那个字母就从服务端组合里消失了。无论组合是否为空都消费一笔记账，
            // 组合为空（用户没有抢敲）时照常走下面的清理。
            const bool pushed_hide = ConsumeTopCommitPendingHide(task.client_id, task.activation_epoch);
            const bool top_commit_remainder_alive = pushed_hide && !g_inputSession->get_pinyin_sequence().empty();
            if (top_commit_remainder_alive)
            {
                CAND_DIAG_LOGF(L"task HideCandidate suppressed (top-commit remainder alive) client={} epoch={}",
                               task.client_id, task.activation_epoch);
                RequestShowCandidateWindow();
                break;
            }
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

        case TaskType::HideCaretState:
            PostHideCaretState();
            break;

        case TaskType::MoveCandidate: {
            static int cnt = 0;
            ::ReadDataFromNamedPipe(0b001000);
            CAND_DIAG_LOGF(L"task MoveCandidate client={} epoch={} caret=({},{})", task.client_id,
                           task.activation_epoch, Global::Point[0], Global::Point[1]);
            PostCaretStatePosition(Global::Point[0], Global::Point[1]);
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
            // 顶字后第 4 码丢失的定位探针：DLL 侧 keydown-sent 已确认发出，若这里没打出来，
            // 说明任务根本没进队列（reader 未收到/未入队）；打出来了但组合没变，才是 HandleImeKey 内部问题。
            CAND_DIAG_LOGF(L"task ImeKeyEvent dispatch request={} keycode=0x{:X} wch=U+{:04X} epoch={}",
                           task.pipe_data.request_id, task.pipe_data.keycode, static_cast<unsigned>(task.pipe_data.wch),
                           task.activation_epoch);
            HandleImeKey(task.client_id, task.activation_epoch, task.pipe_data.request_id);
            break;
        }

        case TaskType::LangbarRightClick: {
            ::ReadDataFromNamedPipe(0b001101);
            PostMessage(::global_hwnd_menu, WM_LANGBAR_RIGHTCLICK, 0, 0);
            break;
        }

        // Clients send the three switch events only after negotiating
        // CaretStateIndicator; they are badge requests, not state updates.
        case TaskType::IMESwitch: {
            const bool capsLockEdge = task.pipe_data.wch == VK_CAPITAL;
            const bool imeEnabled = task.pipe_data.keycode != 0;
            const auto capsLockSnapshot =
                FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(task.pipe_data.modifiers_down);
            const bool capsLockEnabled =
                capsLockSnapshot.has_value() ? *capsLockSnapshot : GetServerCapsLockState() != 0;
            const bool japaneseMode = GetConfiguredInputMode() == "japanese";
            if (FanyImeUi::ShouldShowInputModeEvent(capsLockEdge, capsLockEnabled, imeEnabled, japaneseMode))
            {
                PostCaretStateBadge(FanyImeUi::InputModeBadge(imeEnabled, japaneseMode, capsLockEnabled),
                                    task.pipe_data.point[0], task.pipe_data.point[1]);
            }
            break;
        }

        case TaskType::PuncSwitch: {
            PostCaretStateBadge(FanyImeUi::PunctuationBadge(task.pipe_data.keycode != 0, task.pipe_data.wch != 0,
                                                            GetConfiguredInputMode() == "japanese"),
                                task.pipe_data.point[0], task.pipe_data.point[1]);
            break;
        }

        case TaskType::DoubleSingleByteSwitch: {
            PostCaretStateBadge(
                FanyImeUi::SingleStateBadge(FanyImeUi::CaretStateKind::Width, task.pipe_data.keycode != 0),
                task.pipe_data.point[0], task.pipe_data.point[1]);
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
            PostHideCaretState();
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
            PostHideCaretState();
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
            PostHideCaretState();
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

        case TaskType::UiPageUp:
        case TaskType::UiPageDown: {
            const int offset = (task.type == TaskType::UiPageDown) ? 1 : -1;
            // A coalesced burst of wheel notches replays as several page moves
            // and a single refresh at the end, so a fast scroll costs one redraw
            // rather than one per notch.
            bool refresh = false;
            for (int step = 0; step < task.page_steps; ++step)
            {
                const PageMoveResult moved = MoveCandidatePage(offset);
                if (moved == PageMoveResult::Unchanged)
                {
                    break;
                }
                refresh = true;
                if (moved == PageMoveResult::Moved)
                {
                    // Mouse paging restarts the highlight at the top of the new
                    // page; the pointer, unlike the arrow keys, carries no
                    // notion of which row the user was on.
                    Global::candidate_ui.selected_index_in_page = 0;
                }
            }
            if (refresh)
            {
                RefreshCandidatePageUi(true);
            }
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

        case TaskType::ApplyRescoredOrder: {
            ApplyRescoredOrder();
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

// 后台线程算完了一批整句重排。它不知道自己算的是不是当前这次输入——那要在任务线程上、拿着
// g_inputSession 才判断得了——所以这里只负责把「去看一眼」排进队列，判断留给 ApplyRescoredOrder。
void EnqueueRescoredCandidates()
{
    {
        std::lock_guard lock(queueMutex);
        Task task;
        task.type = TaskType::ApplyRescoredOrder;
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
    else if (action == CandidateUiAction::PageUp || action == CandidateUiAction::PageDown)
    {
        // Paging has no candidate index; it goes through EnqueueCandidateUiPaging.
        return;
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

void EnqueueCandidateUiPaging(CandidateUiAction action, int steps)
{
    if (!pipe_running || steps <= 0)
    {
        return;
    }
    if (action != CandidateUiAction::PageUp && action != CandidateUiAction::PageDown)
    {
        return;
    }

    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner)
    {
        return;
    }

    const TaskType type = action == CandidateUiAction::PageUp ? TaskType::UiPageUp : TaskType::UiPageDown;
    {
        std::lock_guard lock(queueMutex);
        // One notch of the wheel is one step, and every step can hit the engine
        // and the user dictionary. Folding a burst into the task already waiting
        // for the same page keeps a fast scroll from queueing an unbounded run.
        if (!taskQueue.empty() && taskQueue.back().type == type && taskQueue.back().client_id == owner.client_id &&
            taskQueue.back().activation_epoch == owner.activation_epoch)
        {
            taskQueue.back().page_steps += steps;
        }
        else
        {
            Task task;
            task.type = type;
            task.client_id = owner.client_id;
            task.activation_epoch = owner.activation_epoch;
            task.page_steps = steps;
            taskQueue.push(std::move(task));
        }
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
            // 与 worker 侧的 dispatch 探针配对：reader 收到键包即记，两边对照可把丢键
            // 精确到「reader 未收到」还是「worker 未派发」。request_id 是 DLL 侧分配的，
            // 可直接与 [msime][issue47] 的 keydown-sent request 对齐。
            CAND_DIAG_LOGF(L"main-pipe KeyEvent received request={} keycode=0x{:X} wch=U+{:04X}", pipeData.request_id,
                           pipeData.keycode, static_cast<unsigned>(pipeData.wch));
            EnqueueTask(TaskType::ImeKeyEvent, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::HideCandidateWnd: {
            EnqueueTask(TaskType::HideCandidate, pipeData, activation.epoch);
            break;
        }

        case FanyImePipeEventType::HideCaretState: {
            EnqueueTask(TaskType::HideCaretState, pipeData, activation.epoch);
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
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationSpaceConvertChanged,
                GetConfiguredSmartPunctuationSpaceConvertEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectDigitChanged,
                GetConfiguredSmartPunctuationDirectDigitEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectLetterChanged,
                GetConfiguredSmartPunctuationDirectLetterEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationRepeatToChineseChanged,
                GetConfiguredSmartPunctuationRepeatToChineseEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged,
                GetConfiguredPairedPunctuationEnabled() ? L"1" : L"0");
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::MicrosoftShuangpinChanged,
                GetConfiguredShuangpinSchema() == "microsoft" ? L"1" : L"0");
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
            SendToTsfWorkerThreadClientViaNamedpipe(
                hello.client_id, Global::DataFromServerMsgTypeToTsfWorkerThread::StatisticsEnabledChanged,
                GetConfiguredStatisticsEnabled() ? L"1" : L"0");
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
        // A K/U/T/E/M/J/Y special-mode prefix that is not yet a complete input (e.g.
        // "K", "U", "U+", "Tw", "Txin", "E", "M", "J", "Y"): do not translate it into
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

    // R4：光标前缀为空时不造「整串假候选」——前缀为空就该没有候选（候选窗由调用方
    // 收起）。caret 未设置时 prefix_end 等于串长，此分支永不触发，现状零差异。
    if (items.empty() && !g_english_input_mode &&
        !FanyImeIpc::IsCaretPrefixEmpty(g_inputSession->prefix_end(),
                                        g_inputSession->get_pinyin_sequence_with_cases().size()))
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
    // 译文页占用着同一份 items，异步候选必须等它退出再合并，否则会把译文冲掉。
    if (FindCloudRequestOrigin(pinyin, generation).client_id == 0 || !g_inputSession || g_translation_candidates_active)
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

// 候选列表当初是按词格的静态顺序发出去的，因为打分那会儿还没算完。现在算完了：把引擎的候选缓存
// 丢掉重查一次，这一次 quanpin::make_neural_reranker 能在结果表里查到顺序，整句就落到它该在的位
// 置上。重查本身不碰模型，走的还是词格那条快路。
//
// 后台线程算的可能已经是上一次输入的了（用户没停手），所以这里不认「哪一批」，只看重查出来的词
// 序有没有真的变：没变就一个字节都不动 UI。这既挡掉了过期结果，也挡掉了模型弃权的情况。
void ApplyRescoredOrder()
{
    if (!g_inputSession || g_translation_candidates_active)
        return;
    // 造词界面和译文页各自占着 items，重排不该去动它们。
    if (GlobalIme::composition.creating_word.active)
        return;
    if (Global::candidate_ui.items.empty())
        return;
    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner || !IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
        return;

    const std::vector<WordItem> before = g_inputSession->get_candidates();
    // 云/AI 候选只活在 series cache 里，reset_cache 会把它们一起清掉，而它们的请求早已回来、不会
    // 再发一次——不补回来，重排一落地它们就从列表里消失了。重查不推进在线请求的 generation，
    // 所以用当前的 online_query 原样回填即可；回填时若同一句已被整句候选占了，引擎自己会拒绝。
    std::vector<WordItem> online_items;
    std::copy_if(before.begin(), before.end(), std::back_inserter(online_items), [](const WordItem &item) {
        return item.source == CandidateSource::CloudSuggestion || item.source == CandidateSource::AiSuggestion;
    });
    g_inputSession->reset_cache();
    g_inputSession->recompute_candidates();
    if (!online_items.empty())
    {
        if (const auto query = g_inputSession->online_query())
        {
            for (const WordItem &item : online_items)
                g_inputSession->apply_online_candidate(*query, item.word, item.source);
        }
    }
    const std::vector<WordItem> &after = g_inputSession->get_candidates();
    if (after.size() == before.size() &&
        std::equal(before.begin(), before.end(), after.begin(),
                   [](const WordItem &a, const WordItem &b) { return a.word == b.word; }))
        return;

    PrepareCandidateList(owner.client_id, owner.activation_epoch);
    RequestShowCandidateWindow();
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
        GlobalIme::composition.creating_word.active || current_identity != identity || g_translation_candidates_active)
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
        expected_input != input || GlobalIme::composition.creating_word.active || g_translation_candidates_active)
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
        IsUiLessMode() || g_candidate_translation_signature.empty() || g_translation_candidates_active ||
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
        g_inputSession == nullptr || g_translation_candidates_active ||
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
        g_inputSession == nullptr || g_translation_candidates_active ||
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

// 把候选框换回 Ctrl+Enter 之前的那一屏。译文页期间输入串一个字都没动，session 里的候选
// 还是原来那批，所以这里直接把存下来的 items / 页码 / 高亮位放回去即可；随后这颗按键继续
// 走它本来的流程，就像译文页从来没出现过一样。
void ExitTranslationCandidateMode()
{
    if (!g_translation_candidates_active)
    {
        return;
    }
    g_translation_candidates_active = false;
    auto &ui = Global::candidate_ui;
    ui.set_items(std::move(g_translation_saved_items));
    g_translation_saved_items.clear();
    ui.page_index = g_translation_saved_page_index;
    ui.selected_index_in_page = g_translation_saved_selected_index;
    g_translation_saved_page_index = 0;
    g_translation_saved_selected_index = 0;
    RefreshCandidatePageUi(false);
}

// Ctrl+Enter：上屏高亮候选右边的那条译文（副候选）。只有一条译义就直接上屏；有多条时把
// 候选框整个换成这几条译义，空格/数字键照常选一条上屏（见 ProcessSelectionKey 的译文分支）。
void HandleTranslationCommitKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id)
{
    // 拿不到译文时回 NavigationIgnored：这颗键已经被 TSF 吃掉了，必须给一条回复，
    // 而且这条回复既不上屏也不给 wch 补标点。
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::NavigationIgnored;
    const bool japanese = g_inputSession && g_inputSession->current_scheme_type() == SchemeType::JapaneseRomaji;
    if (g_translation_candidates_active || IsUiLessMode() || japanese || !GetConfiguredCandidateTranslationsEnabled() ||
        Global::candidate_ui.items.empty())
    {
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }

    // 和数字/空格选词一样，先等画面追上已发布的那一页，否则取到的是用户没看见的那条译文。
    WaitForCandidateRenderSync(VK_RETURN);
    EnsureCandidatePageReady();

    auto &ui = Global::candidate_ui;
    const size_t index = static_cast<size_t>((std::max)(0, ui.selected_index_in_page));
    if (index >= ui.page_glosses.size())
    {
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }
    const auto senses = FanyImeIpc::SplitTranslationGloss(wstring_to_string(ui.page_glosses[index]));
    if (senses.empty())
    {
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }

    if (senses.size() == 1)
    {
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::CommitExactText;
        ui.selected_text = string_to_wstring(CandidateTextForOutput(senses.front()));
        // Normal/CommitExactText 的回复由 SendCurrentDataToClient 负责收尾（ClearState）。
        SendCurrentDataToClient(client_id, activation_epoch, request_id);
        return;
    }

    g_translation_saved_items = ui.items;
    g_translation_saved_page_index = ui.page_index;
    g_translation_saved_selected_index = ui.selected_index_in_page;
    std::vector<WordItem> translation_items;
    translation_items.reserve(senses.size());
    for (const auto &sense : senses)
    {
        translation_items.emplace_back(std::string{}, sense, 0, CandidateSource::Fallback);
    }
    ui.set_items(std::move(translation_items));
    g_translation_candidates_active = true;
    RefreshCandidatePageUi(true);
    SendCurrentDataToClient(client_id, activation_epoch, request_id);
}

// 真顶字与自动上屏的推送负载："<消费字符数>\t<上屏文本>"。TSF 拿这个数字裁自己的
// 组合缓冲，所以服务端看到的是四码、用户已抢敲第五个字母时，第五个字母不会被旧快照覆盖。
// 消费数就是五笔完整码的字母数（engine/schemes/wubi_scheme.h 的 kMaxCodeLength）。
constexpr std::size_t kWubiCompleteCodeLength = 4;
std::wstring BuildWubiCommitAndContinuePayload(const std::wstring &text)
{
    return std::to_wstring(kWubiCompleteCodeLength) + L"\t" + text;
}

// Bring the candidate page in step with the CompositionRestored frame just sent.
// TSF applies that payload without touching its candidate presenter, so the page
// on screen has to follow here -- and that covers every frame the reply branch
// sends, not just the retraction: the unit Backspace deletion shortens the raw
// and the Ctrl+arrow unit jump moves the caret prefix, and both would otherwise
// leave the pre-key page (and its selection) on screen.
//   - the raw is gone: nothing to show. The selected segments may still be on
//     screen (a segment Backspace emptied the raw but kept the word), where TSF
//     deliberately leaves its presenter alone because ending it would send
//     HideCandidateWnd and reset this very composition; and a Backspace that
//     ended the word must never fall through to a rebuild that would publish the
//     empty-input fallback candidate;
//   - the caret prefix is empty: hide too (R4, the same rule as the ShowCandidate
//     task and the caret-arrow path);
//   - otherwise rebuild from the engine -- the old page holds the pre-frame
//     items -- and re-highlight the restored pick.
void PublishRestoredCompositionCandidates(uint64_t client_id, uint64_t activation_epoch)
{
    // One-shot: consume it even when this outcome hides instead of rebuilding, so
    // a position recorded on a page that no longer exists cannot leak into a
    // later frame.
    const GlobalIme::RestoredSelectionHighlight restored_highlight =
        GlobalIme::composition.take_restored_selection_highlight();
    if (GlobalIme::composition.raw_input_with_cases.empty())
    {
        HideCandidateWindowAndDropItems();
        return;
    }
    if (FanyImeIpc::IsCaretPrefixEmpty(g_inputSession->prefix_end(),
                                       g_inputSession->get_pinyin_sequence_with_cases().size()))
    {
        HideCandidateWindowAndDropItems();
        return;
    }

    PrepareCandidateList(client_id, activation_epoch);
    if (restored_highlight.absolute_index >= 0)
    {
        // A retraction re-highlights the item the user had picked on the rebuilt
        // page: no frequency update ran during the creating word, so a page
        // rebuilt for the same prefix still holds the same items in the same
        // order. A page for another prefix (the suffix was edited between the
        // pick and the retraction) does not, and the recorded position would land
        // on an unrelated candidate -- apply it only after the prefixes match.
        const std::string rebuilt_page_prefix = FanyImeIpc::NormalizeCandidatePagePrefix(
            g_inputSession->get_pinyin_sequence_with_cases(), g_inputSession->prefix_end());
        auto &ui = Global::candidate_ui;
        if (rebuilt_page_prefix == restored_highlight.page_prefix && ui.item_total_count > 0 && ui.page_size > 0)
        {
            const int position = std::min(restored_highlight.absolute_index, ui.item_total_count - 1);
            ui.page_index = position / ui.page_size;
            ui.selected_index_in_page = position % ui.page_size;
            RefreshCandidatePageUi(false);
        }
    }
    RequestShowCandidateWindow();
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
        {
            const std::string previous = GetConfiguredCharacterSet();
            const std::string next = previous == "traditional" ? "simplified" : "traditional";
            // The packet's point[] is this key's badge anchor, not the
            // candidate anchor: read it locally and leave Global::Point alone.
            // Legacy clients leave the struct-default point there instead.
            if (SetConfiguredCharacterSet(next) && GetConfiguredCharacterSet() != previous &&
                ClientNegotiatedCaretStateIndicator(client_id))
            {
                PostCaretStateBadge(FanyImeUi::SingleStateBadge(FanyImeUi::CaretStateKind::CharacterSet,
                                                                GetConfiguredCharacterSet() == "traditional"),
                                    namedpipeData.point[0], namedpipeData.point[1]);
            }
        }
        return;
    }

    if (FanyImeIpc::IsEnglishModeToggleKey(Global::Keycode, Global::ModifiersDown))
    {
        SetEnglishInputMode(!g_english_input_mode);
        ClearState();
        return;
    }

    if (FanyImeIpc::IsTranslationCommitKey(Global::Keycode, Global::ModifiersDown))
    {
        HandleTranslationCommitKey(client_id, activation_epoch, request_id);
        return;
    }
    // 译文页只认选词和翻页/移动高亮。其它任何键都先把候选框换回原来那一屏，然后照常处理，
    // 所以退格、字母、回车、标点在译文页上的表现和没按过 Ctrl+Enter 时完全一致。
    if (g_translation_candidates_active)
    {
        // Shift 放行是给 Shift+Tab 上一页留的；Ctrl/Alt 组合一律退出。
        const bool stays_on_translation_page =
            (Global::ModifiersDown & 0b00000110u) == 0 &&
            (IsSelectionKey(Global::Keycode) || IsCandidateNavigationKey(Global::Keycode));
        if (!stays_on_translation_page)
        {
            ExitTranslationCandidateMode();
        }
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
    // 顶字要的是「插入之前」的原始串长度与光标位置：ApplyCompositionEditKey 会把第五个字母插进
    // 本地 raw 并把光标推到 5，之后再问就分不清「用户又敲了一个字母」和「本来就停在别处」。引擎
    // 随后会把 raw 裁回四码，这个快照是唯一能区分两者的地方（raw_length_before_key == 4 且光标
    // 在末尾 = 用户正在往后打，不是回来改码）。
    const std::size_t raw_length_before_key = input_before_key.size();
    const std::size_t caret_before_key = GlobalIme::composition.caret_position;
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

    const bool unicode_composition_active = IsUnicodeCompositionActive(input_before_key);
    const bool is_paging_key = IsPagingKey(Global::Keycode);
    const bool is_manual_pinyin_separator = IsManualPinyinSeparatorKey(Global::Keycode, Global::Wch);
    const bool is_microsoft_shuangpin_ing_key =
        IsMicrosoftShuangpinIngKey(Global::Keycode, Global::Wch, input_before_key);
    // 日语模式下 '-' 是长音符输入键，既不翻页也不做词转字。
    const bool is_japanese_long_vowel = IsJapaneseLongVowelKey(Global::Keycode, Global::Wch);
    const int word_character_direction =
        FanyImeIpc::WordToCharacterDirection(Global::Keycode, Global::Wch, Global::ModifiersDown,
                                             GetConfiguredWordToCharacterEnabled() && !is_japanese_long_vowel,
                                             GetConfiguredWordToCharacterKeys() == "minus_equal");
    const bool is_commit_with_highlighted_candidate_punctuation =
        word_character_direction != 0 ||
        (!is_manual_pinyin_separator && !is_microsoft_shuangpin_ing_key &&
         IsCommitWithHighlightedCandidatePunctuationInCandidateMode(Global::Keycode, Global::Wch));
    const bool is_selection_key = IsSelectionKey(Global::Keycode);
    const bool is_unicode_shift_digit_selection =
        unicode_composition_active && shift_only && Global::Keycode >= '1' && Global::Keycode <= '9';
    const bool is_unicode_hex_digit = unicode_composition_active && !is_unicode_shift_digit_selection &&
                                      Global::Keycode >= '0' && Global::Keycode <= '9';
    const bool is_unicode_plus = unicode_composition_active && Global::Keycode == VK_OEM_PLUS && Global::Wch == L'+';
    const bool is_composition_edit_key = Global::Keycode == VK_LEFT || Global::Keycode == VK_RIGHT ||
                                         Global::Keycode == VK_BACK || Global::Keycode == VK_DELETE ||
                                         (Global::Keycode >= 'A' && Global::Keycode <= 'Z') ||
                                         is_manual_pinyin_separator || is_microsoft_shuangpin_ing_key ||
                                         is_unicode_hex_digit || is_unicode_plus || is_japanese_long_vowel;
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
    bool composition_restored = false;
    // The client arms its Backspace reply hold from its own creating-word mirror,
    // which mirrors the state before this key. Capture that shape here so the
    // reply below can still answer a Backspace that ends the word (the post-key
    // shape is gone by then, and the hold would otherwise burn its full timeout).
    bool retreat_backspace_shape_before_key = false;
    // 前缀重算让所有编辑键（含字母/Delete）都需要协商结果；段操作（Ctrl+Backspace /
    // Ctrl+方向）的键位与修饰键条件仍由各自的 chord 判定把守，这里放宽键位限制不影
    // 响它们。
    const bool client_supports_restore = is_composition_edit_key && ClientNegotiatedCompositionRestore(client_id);
    const bool r_mode_prefix_backspace = g_r_mode_triggered && Global::Keycode == VK_BACK && input_before_key.empty();
    if (r_mode_prefix_backspace)
    {
        ClearState();
    }
    else if (is_composition_edit_key && !r_mode_trigger_key)
    {
        retreat_backspace_shape_before_key =
            Global::Keycode == VK_BACK &&
            FanyImeIpc::HasRetreatBackspaceShape(GlobalIme::composition.creating_word.active, IsUiLessMode(),
                                                 client_supports_restore);
        ApplyCompositionEditKey(Global::Keycode, Global::Wch, Global::ModifiersDown, client_supports_restore,
                                composition_restored);
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

    // 五笔四码唯一自动上屏：敲满四码且码表只给一个候选时，直接走与空格完全相同的提交路径，
    // 用户不必再按一次空格。判定只发生在字母键插入之后（上面的 ApplyCompositionEditKey）：
    // 退格、方向键、composition_restored 等路径都不会到这里，所以「打满第四键就上屏」只有
    // 这一个入口。这是无条件行为，不读配置。
    const bool letter_key = Global::Keycode >= 'A' && Global::Keycode <= 'Z';
    if (!g_english_input_mode && letter_key &&
        FanyImeIpc::ShouldAutoCommitCompleteWubiCode(g_inputSession->wubi_unique_four_code(),
                                                     GlobalIme::composition.creating_word.active))
    {
        // 候选页是异步发布的：此刻 ui.items / ui.page_words 可能还停在第 3 码那一拍，而提交
        // 路径读的正是这两份数据。先按当前组合同步重建一次，否则会把上一拍的候选上屏。
        // forced_index_in_page = 0 让结算不进入渲染等待（与鼠标点击同类），自动上屏的语义
        // 是「这个码只有一个候选」，必须显式取 0 而不是跟随页内选择。
        PrepareCandidateList(client_id, activation_epoch);
        Global::candidate_ui.select_first_on_page();
        ProcessSelectionKey(VK_SPACE, client_id, activation_epoch, /*forced_index_in_page=*/0);
        if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::Normal)
        {
            // 真上屏只能靠 worker 管道推送：字母键在默认 raw 预编辑样式下不读请求-回复管道，
            // 回一帧 Normal 既不会上屏，还会被 TSF 当成「不属于本次请求」的帧缓存起来，
            // 而本函数返回前 Server 已经清掉组合，两边就此分叉。推送携带消费的 4 个字符，
            // TSF 裁自己的缓冲；快打时用户已多敲的字母因此不会被旧快照覆盖。
            if (SendToTsfWorkerThreadClientViaNamedpipe(
                    client_id, activation_epoch,
                    Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidateAndContinue,
                    BuildWubiCommitAndContinuePayload(Global::candidate_ui.selected_text)))
            {
                ClearState();
                // 推送同样会引来 HideCandidateWnd；用户若已抢敲下一个字母，那时服务端组合
                // 就是这个字母，不能被这次 Hide 清掉。
                NoteTopCommitPushed(client_id, activation_epoch);
            }
        }
        // UILess 与 pinyin 预编辑样式会为字母键等一帧回复（上限 50ms）：给它们一帧免得空等；
        // raw 样式不读回复，塞一帧反而变成死帧。
        if (IsUiLessMode() || GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
        {
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
        }
        return;
    }

    // 真顶字：完整四码（不论是否唯一）之后再敲一个字母时，先上屏该码的首选候选，再把这个字母
    // 留作下一次组合的开头——用户已经在打下一个字，字母绝不能丢。它不看自动上屏开关：开关
    // 只决定「唯一码要不要多敲一键才上屏」，不决定丢不丢输入。判定复用同一份引擎事实，
    // 但不要求唯一；上屏取候选 0（首选），不进入 30ms 渲染等待。
    if (!g_english_input_mode && letter_key && raw_length_before_key == kWubiCompleteCodeLength &&
        caret_before_key == raw_length_before_key &&
        FanyImeIpc::ShouldCommitCompleteWubiCodeOnNextKey(g_inputSession->wubi_four_code_is_complete(),
                                                          /*key_is_letter=*/true, /*caret_at_end=*/true,
                                                          GlobalIme::composition.creating_word.active))
    {
        PrepareCandidateList(client_id, activation_epoch);
        Global::candidate_ui.select_first_on_page();
        ProcessSelectionKey(VK_SPACE, client_id, activation_epoch, /*forced_index_in_page=*/0);
        const std::wstring committed_text = Global::candidate_ui.selected_text;

        // 用刚敲下的这个字母重建服务端组合。ProcessSelectionKey 已经把引擎与组合清空，这里
        // 把字母写回去；引擎此刻的 raw 仍是被裁回的四码，所以必须显式设置而不是继续追加。
        // 大小写照 ApplyCompositionEditKey 的同一套规则取，保持 preedit 与用户敲键一致。
        char next_char = static_cast<char>(Global::Keycode + ('a' - 'A'));
        if (Global::Wch >= L'A' && Global::Wch <= L'Z')
        {
            next_char = static_cast<char>(Global::Wch);
        }
        else if (Global::Wch >= L'a' && Global::Wch <= L'z')
        {
            next_char = static_cast<char>(Global::Wch);
        }
        const std::string next_raw(1, next_char);
        GlobalIme::composition.clear_creating_word();
        GlobalIme::composition.selection_history.clear();
        g_inputSession->set_pinyin_sequence(next_raw);
        g_inputSession->set_pinyin_sequence_with_cases(next_raw);
        g_inputSession->recompute_candidates();
        GlobalIme::composition.raw_input_with_cases = g_inputSession->get_pinyin_sequence_with_cases();
        GlobalIme::composition.segmented_pinyin = g_inputSession->get_pinyin_segmentation_with_cases();
        GlobalIme::composition.caret_position = GlobalIme::composition.raw_input_with_cases.size();
        PrepareCandidateList(client_id, activation_epoch);
        // 组合被提交时 TSF 会送 HideCandidateWnd 把候选窗藏起来；顶字重建的新组合必须
        // 显式把窗口再请出来，否则后续整词的候选（xyyf 的统计）用户永远看不到。
        RequestShowCandidateWindow();

        // 推送与用户下一个按键是两条独立路径：TSF 裁的是它自己那一刻的缓冲，所以「服务端说
        // 消费 4 个、TSF 手里已经有 5 个」时，第 5 个自然留下来继续组词。服务端的组合也正好
        // 是同一批多出来的字母，两边都从同一条按键流派生，不会错位。不要 ClearState：重建的
        // 组合正是下一次按键要用的状态。
        if (Global::MsgTypeToTsf == Global::DataFromServerMsgType::Normal)
        {
            // 推送会让 DLL 结束旧组合，TSF 随之发来 HideCandidateWnd；标记本客户端的余码
            // 组合仍然存活，HideCandidate 处理器据此跳过 ClearState。推送失败就不会有这次 Hide，
            // 也就不记账。
            if (SendToTsfWorkerThreadClientViaNamedpipe(
                    client_id, activation_epoch,
                    Global::DataFromServerMsgTypeToTsfWorkerThread::CommitCandidateAndContinue,
                    BuildWubiCommitAndContinuePayload(committed_text)))
            {
                NoteTopCommitPushed(client_id, activation_epoch);
            }
        }
        // UILess 与 pinyin 预编辑样式会为字母键等一帧回复（上限 50ms）。这里绝不能回 Normal
        // （SendCurrentDataToClient 会 ClearState，把刚重建的组合再清掉），只能回渲染帧。
        if (IsUiLessMode())
        {
            SendUiLessCompositionToClient(client_id, activation_epoch, request_id);
        }
        else if (GlobalSettings::getTsfPreeditStyle() == GlobalSettings::TsfPreeditStyle::Pinyin)
        {
            Global::MsgTypeToTsf = Global::DataFromServerMsgType::Preedit;
            Global::candidate_ui.selected_text = GetPreedit();
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
        }
        return;
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

    // The shape the key leaves behind: a Backspace that keeps the creating word
    // alive still owes the client's hold a frame even when nothing was restored
    // (a plain deletion behind the edit lock, or a no-op).
    const bool retreat_backspace_shape_after_key =
        Global::Keycode == VK_BACK && FanyImeIpc::HasRetreatBackspaceShape(GlobalIme::composition.creating_word.active,
                                                                           IsUiLessMode(), client_supports_restore);

    //
    // 普通的拼音字符，发送 preedit 到 TSF 端
    //
    if (FanyImeIpc::ShouldSendCompositionReply(Global::Keycode >= 'A' && Global::Keycode <= 'Z',
                                               is_manual_pinyin_separator, is_microsoft_shuangpin_ing_key,
                                               is_unicode_hex_digit, is_unicode_plus, is_japanese_long_vowel))
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
    else if (Global::Keycode == VK_BACK || Global::Keycode == VK_DELETE || composition_restored)
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
        else if (FanyImeIpc::ShouldAnswerRetreatBackspace(composition_restored, retreat_backspace_shape_before_key,
                                                          retreat_backspace_shape_after_key))
        {
            // Unlike an ordinary deletion, the retraction and the unit caret
            // jump are not mirrored by TSF on its own: for a deletion TSF
            // rebuilds its keystroke buffer from this payload, and for a jump
            // it applies the caret field. It must therefore be sent in both
            // preedit styles, and the trailing caret field pins the
            // authoritative caret.
            //
            // Every Backspace the client may be holding for gets this frame --
            // retreat, plain character deletion behind the edit lock, or a
            // no-op behind an empty history: the DLL arms its hold from the
            // creating-word mirror it saw before the key (word_for_creating_word),
            // and without this frame the default raw preedit style would send no
            // reply at all, burning the hold's full 50 ms timeout. The pre-key
            // shape keeps that promise for the Backspace that deletes the last
            // raw character and ends the word: the post-key shape is gone by
            // then, while the client is still holding. The payload then
            // describes the state the key left behind -- restored, unchanged, or
            // one character shorter when the caret deletion in
            // ApplyCompositionEditKey ran -- which is what the hold applies in
            // every outcome. The candidate page follows it below.
            Global::MsgTypeToTsf = Global::DataFromServerMsgType::CompositionRestored;
            Global::candidate_ui.selected_text = BuildCreateWordPipePayload(GlobalIme::composition.raw_input_with_cases,
                                                                            GlobalIme::composition.creating_word.word) +
                                                 L'\t' + std::to_wstring(GlobalIme::composition.caret_position);
            SendCurrentDataToClient(client_id, activation_epoch, request_id);
            PublishRestoredCompositionCandidates(client_id, activation_epoch);
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
    /* VK_SPACE, Digits (U-mode: Shift+1..9) */
    if (Global::Keycode == VK_SPACE || is_unicode_shift_digit_selection ||
        (!IsUnicodeCompositionActive(GlobalIme::composition.raw_input_with_cases) && Global::Keycode > '0' &&
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
            // R2/R4：前缀重算生效时光标移动会改变候选内容——前缀为空则收起候选窗，
            // 其余（含移回串尾）一律从引擎重读重建页面。移回串尾时引擎已按整串重算，
            // 但页面 items 还是旧前缀候选，只刷新页面会让那批旧候选参与结算（真机回归：
            // ni'hao'ya 右移回串尾后空格只上屏「你好」+「ya」）。整串解码（未启用或未协商）
            // 维持只刷新页面的现状（R7/AC8 零差异）。
            const std::string caret_raw = g_inputSession->get_pinyin_sequence_with_cases();
            const std::size_t prefix_end = g_inputSession->prefix_end();
            const bool caret_resegmentation = FanyImeIpc::ShouldResegmentCompositionByCaret(
                client_supports_restore, IsUiLessMode(), g_english_input_mode,
                IsSpecialModeCompositionActive(caret_raw));
            switch (FanyImeIpc::ResolveCaretArrowCandidatePublish(caret_resegmentation, prefix_end, caret_raw.size()))
            {
            case FanyImeIpc::CaretArrowCandidatePublish::Hide:
                HideCandidateWindowAndDropItems();
                break;
            case FanyImeIpc::CaretArrowCandidatePublish::RebuildFromEngine:
                // 窗口可能因之前的前缀为空状态被收起（单音节后缀从 caret=0 右移两次），
                // 必须显式请求显示；PrepareCandidateList 末尾自带 RefreshCandidatePageUi(false)。
                PrepareCandidateList(client_id, activation_epoch);
                RequestShowCandidateWindow();
                break;
            case FanyImeIpc::CaretArrowCandidatePublish::RefreshPageOnly:
                RefreshCandidatePageUi(true);
                break;
            }
        }
    }
    else if (IsCandidateNavigationKey(Global::Keycode) && !is_unicode_plus)
    {
        auto &ui = Global::candidate_ui;
        UINT result = Global::DataFromServerMsgType::NavigationIgnored;
        bool refresh = false;

        const auto move_page = [&](int offset, UINT response_type) {
            result = response_type;
            // Keyboard paging keeps the in-page selection where it is; the wheel
            // path in WorkerThread is the one that restarts it at the top.
            if (MoveCandidatePage(offset) != PageMoveResult::Unchanged)
            {
                refresh = true;
            }
        };
        const auto move_selection = [&](int offset, UINT response_type) {
            result = response_type;
            if (offset > 0 && (ui.is_selection_at_last_candidate() ||
                               (ui.is_selection_at_current_page_end() && ui.is_next_page_partial_last_page())))
            {
                ExpandCandidatesKeepingPagePosition();
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
    g_translation_candidates_active = false;
    g_translation_saved_items.clear();
    g_translation_saved_page_index = 0;
    g_translation_saved_selected_index = 0;
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
    HideCandidateWindowAndDropItems();
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

// A digit/space selection settles against the live page_words, while the user is looking at the
// asynchronously painted snapshot. Pin-frequency reorders the page after every commit, so a
// keystroke that lands between publish and paint would commit a candidate the user never saw.
// Poll (no lock, no event) until the UI echoes back the generation it painted, with a hard bound so
// a wedged UI thread cannot hang input: on timeout the selection continues with the current page and
// the miss is recorded in the diagnostic log. Runs on the IPC worker thread only.
void WaitForCandidateRenderSync(UINT keycode)
{
    const auto renderLagsPublished = []() {
        return FanyImeIpc::ShouldWaitForCandidateRender(
            Global::rendered_candidate_page_generation.load(std::memory_order_acquire),
            Global::candidate_page_generation.load(std::memory_order_acquire), IsUiLessMode(),
            Global::candidate_window_rendered_visible.load(std::memory_order_acquire));
    };
    if (!renderLagsPublished())
    {
        return;
    }

    const ULONGLONG startedTick = GetTickCount64();
    while (renderLagsPublished())
    {
        const ULONGLONG waitedMs = GetTickCount64() - startedTick;
        if (waitedMs >= static_cast<ULONGLONG>(FanyImeIpc::kCandidateSelectionRenderWaitMaxMs))
        {
            CAND_DIAG_LOGF(L"candidate-select-render-timeout keycode={} current={} rendered={}", keycode,
                           Global::candidate_page_generation.load(std::memory_order_acquire),
                           Global::rendered_candidate_page_generation.load(std::memory_order_acquire));
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CAND_DIAG_LOGF(L"candidate-select-render-synced keycode={} waited_ms={}", keycode, GetTickCount64() - startedTick);
}

void ProcessSelectionKey(UINT keycode, uint64_t client_id, uint64_t activation_epoch, int forced_index_in_page)
{
    /* 先清理一下状态 */
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;

    static bool isNeedUpdateWeight = false;
    isNeedUpdateWeight = false;

    if (g_translation_candidates_active)
    {
        // 译文页：选中的就是要上屏的完整文本，既不调频也不造词，更不能把译文当候选词学进库。
        if (forced_index_in_page < 0 && (keycode == VK_SPACE || (keycode >= '1' && keycode <= '9')))
        {
            WaitForCandidateRenderSync(keycode);
        }
        EnsureCandidatePageReady();
        auto &ui = Global::candidate_ui;
        const int index = forced_index_in_page >= 0
                              ? forced_index_in_page
                              : (keycode == VK_SPACE ? ui.selected_index_in_page : static_cast<int>(keycode - '1'));
        if (index < 0 || static_cast<size_t>(index) >= ui.page_words.size())
        {
            // 这一页没有这个序号：什么都不上屏，译文页原样留着。走 SELECT_BY_NUMBER 的
            // TSF 路径只认 Normal / OutofRange 两种回复，这里必须是后者。
            Global::MsgTypeToTsf = Global::DataFromServerMsgType::OutofRange;
            return;
        }
        // 同理，上屏必须回 Normal：_HandleCandidateFinalize 只在 Normal 时写入文本，
        // 而且它写的就是 candidate_string 本身，不会再补标点。
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;
        ui.selected_text = ui.page_words[index];
        return;
    }

    // Keyboard selection must match the painted page. Mouse clicks carry an explicit index
    // (forced_index_in_page >= 0) and keep the old behavior: waiting cannot restore the intent of a
    // click that targeted a candidate of a page that is no longer on screen.
    if (forced_index_in_page < 0 && (keycode == VK_SPACE || (keycode >= '1' && keycode <= '9')))
    {
        WaitForCandidateRenderSync(keycode);
    }

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
            // 因此造好的词必须在这里落库，否则前缀 + 整句只上屏、学不到。没有前缀时
            // 整句自己就是要落库的那条词：它在词库里没有行，下面的调频改不到它。
            // 拼音两段都是 canonical quanpin（creating_word.pinyin 由
            // append_canonical_pinyin 累积，lattice 候选的 canonical_pinyin 是整句 key），
            // 直接按 '\'' 拼接即可；音节数与汉字数是否匹配由
            // create_word_from_canonical_pinyin 自行校验，不匹配时安全地拒绝入库。
            if (FanyImeIpc::ShouldStoreEarlyReturnPhrase(
                    curWordItem.source, GlobalIme::composition.creating_word.active,
                    GlobalIme::composition.creating_word.pinyin, curWordItem.canonical_pinyin))
            {
                const std::string &prefix_pinyin = GlobalIme::composition.creating_word.pinyin;
                // 前缀为空时不能带上那个分隔符，'na'yi'tiao 这种前导撇号会让整条读音作废。
                const std::string stored_pinyin = prefix_pinyin.empty()
                                                      ? curWordItem.canonical_pinyin
                                                      : prefix_pinyin + "'" + curWordItem.canonical_pinyin;
                // 这里异步处理，不然有可能会阻塞住 TSF 端读取 pipe 导致超时
                EnqueueStoreUserPhraseTask(stored_pinyin, GlobalIme::composition.creating_word.word + curWord,
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
        bool cloudCommittedPinyinIsCanonical = false;
        std::string aiCommittedPinyin;
        bool aiCommittedPinyinIsCanonical = false;
        if (curWordItem.source == CandidateSource::CloudSuggestion)
        {
            if (!curWordItem.canonical_pinyin.empty())
            {
                cloudCommittedPinyin = curWordItem.canonical_pinyin;
                cloudCommittedPinyinIsCanonical = true;
            }
            else if (g_inputSession->is_all_complete_pure_pinyin())
            {
                // 与 AI 联想同一个坑：committed_pinyin 走的是 normalized_input，丢掉了音节
                // 边界，create_word 会用贪心的 "correction" 切分重新断句，qi'e'huan 落成
                // qie'huan，音节数与字数对不上、do_validate 静默失败而不入库。整串是完整
                // 拼音时改用带撇号的 normalized_segmentation 作 canonical 键，按用户实际断句
                // 落库。简拼 / 带 helpcode 等非完整拼音的云候选仍走原来的 committed_pinyin
                // 路径（见下面的 else），避免回归。
                cloudCommittedPinyin = g_inputSession->get_pinyin_segmentation();
                cloudCommittedPinyinIsCanonical = true;
            }
            else
            {
                cloudCommittedPinyin = g_inputSession->get_cloud_query_state().committed_pinyin;
            }
        }
        if (curWordItem.source == CandidateSource::AiSuggestion)
        {
            if (g_inputSession->is_all_complete_pure_pinyin())
            {
                if (!curWordItem.canonical_pinyin.empty())
                {
                    aiCommittedPinyin = curWordItem.canonical_pinyin;
                    aiCommittedPinyinIsCanonical = true;
                }
                else
                {
                    // AI 候选自己不带 canonical_pinyin。committed_pinyin 走的是
                    // normalized_input，那串已经去掉了音节边界（见 quanpin_scheme 里
                    // 只往 normalized_input 里塞非撇号字符），create_word 会用贪心的
                    // "correction" 切分重新断句：qi'e'huan 落成 qie'huan，音节数与字数
                    // 对不上，do_validate 直接判失败、静默不入库，于是用户再打同样的音
                    // 时这条 AI 联想仍然要靠现场猜。这里改用带撇号的 normalized_segmentation
                    // （即屏幕上的分段，is_all_complete_pure_pinyin 已保证它整串是完整音节）
                    // 作 canonical 键，create_word_from_canonical_pinyin 按撇号 split、不再
                    // 重新贪心断句，用户实际选的那条读音就能正确落库。
                    aiCommittedPinyin = g_inputSession->get_pinyin_segmentation();
                    aiCommittedPinyinIsCanonical = true;
                }
            }
            isNeedUpdateWeight = false;
        }
        // 这次选择之前是否已经在造词。下面的造词收尾会清掉这个标志，之后就问不出来了。
        const bool was_creating_word = GlobalIme::composition.creating_word.active;
        // R5 前缀选词：候选来自光标前缀时，advance 消耗的正是前缀本身，剩余 raw 就是
        // 后缀。必须在 advance 之前判定（advance 会缩短 raw）；结算把会话光标复位为
        // 「后缀整串转换」（nullopt），组合态光标归后缀首，之后由编辑键按新光标重新
        // 接管前缀语义。该状态只可能由门控内的编辑键产生（未协商/UILess 的光标从不
        // 进会话），因此无需重复门控。云/英文/表情等特殊候选在上方提前返回，不会进入
        // 这里。
        // 用户眼前这一页的身份也取在 advance 之前：撤销重建的页面只有前缀一致时才
        // 装着同一批候选，记录的位置才有意义。
        const std::string selection_page_prefix = FanyImeIpc::NormalizeCandidatePagePrefix(
            g_inputSession->get_pinyin_sequence_with_cases(), g_inputSession->prefix_end());
        const bool caret_prefix_selection =
            g_inputSession->prefix_end() < g_inputSession->get_pinyin_sequence_with_cases().size();
        auto selection_transition =
            g_inputSession->advance_composition_after_selection(curWordPinyin, curWord, curWordItem.canonical_pinyin);
        if (caret_prefix_selection)
        {
            g_inputSession->set_caret(std::nullopt);
            g_inputSession->recompute_candidates();
            GlobalIme::composition.caret_position = 0;
        }
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
            // Snapshot the state the user is leaving before this selection
            // overwrites it. The engine's current raw cannot serve as the
            // snapshot: it still contains the remaining suffix, which the user
            // may delete before asking to retract this segment. The picked
            // candidate position travels with it, together with the page prefix
            // it was recorded on, so a retraction can put that item back under
            // the highlight.
            GlobalIme::composition.push_selection_snapshot(selection_transition.consumed_raw_input_with_cases,
                                                           Global::candidate_ui.page_index * page_size + index,
                                                           selection_page_prefix);
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
            Global::candidate_ui.selected_text = BuildCreateWordPipePayloadWithCaret(
                ClientNegotiatedCompositionRestore(client_id), g_inputSession->get_pinyin_sequence_with_cases(),
                GlobalIme::composition.creating_word.word);
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

        // Google 解码器那条整句（Fallback）不在上面提前返回的名单里，走的是这条普通路径，
        // 但它和词格整句一样是猜出来的：词库里没有它那一行，下面 isNeedUpdateWeight 要改的
        // 行根本不存在。所以它独立上屏时也要落库。接在造词前缀后面的那种由上面的造词收尾
        // 负责（creating_word_progress.can_store），这里不重复存。
        if (!isNeedCreateWord && !was_creating_word &&
            FanyImeIpc::ShouldStoreStandaloneSentence(curWordItem.source, curWordItem.canonical_pinyin))
        {
            EnqueueStoreUserPhraseTask(curWordItem.canonical_pinyin, curWord, /*pinyin_is_canonical=*/true);
        }

        // 看看云联想出来的词是否需要被插入到数据库
        if (curWordItem.source == CandidateSource::CloudSuggestion && !cloudCommittedPinyin.empty())
        {
            EnqueueStoreUserPhraseTask(cloudCommittedPinyin, curWord, cloudCommittedPinyinIsCanonical);
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
            // The composition is over; stale snapshots must not survive into the
            // next one where they could restore an unrelated spelling.
            GlobalIme::composition.selection_history.clear();
            ClearSpecialModeTriggers();
        }
        else
        {
            // 组合继续时同步剩余 raw。HandleImeKey 在选词之前就写过它，不同步的话下一次
            // 编辑键里「raw 变了且 caret==0 → 光标移到串尾」的判定会误触发：前缀选词后
            // caret 已按造词帧的 caret 字段归 0，DLL 光标停在后缀首，Server 却跳到串尾，
            // 之后的插入与移动两侧分叉。
            GlobalIme::composition.raw_input_with_cases = g_inputSession->get_pinyin_sequence_with_cases();
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
        // With the raw spelling gone, the only composition text left is the
        // accumulated word, and TSF's empty-buffer finalize commits exactly that
        // text. So an out-of-range selection here really ends the composition:
        // drop the creating-word state instead of leaving one that the next key
        // would resurrect as a duplicate preedit prefix.
        if (GlobalIme::composition.creating_word.active && g_inputSession->get_pinyin_sequence_with_cases().empty())
        {
            ClearState();
        }
    }
}

} // namespace FanyNamedPipe
