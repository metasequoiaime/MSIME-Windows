#pragma once

// Single wire contract for Windows TSF and Server. This header has no runtime
// dependency on the engine or the Windows SDK. UTF-16 and alignment are explicit
// so the same ABI assertions can run on every platform and on x86/x64 Windows.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ipc_protocol_limits.h"
#ifdef _WIN32
using FanyImeWireChar = wchar_t;
#else
using FanyImeWireChar = char16_t;
#endif

inline const wchar_t *FANY_IME_SHARED_MEMORY = L"Local\\FanyImeSharedMemory";
inline const int BUFFER_SIZE = 4096;

inline const wchar_t *FANY_IME_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeNamedPipe";
inline const wchar_t *FANY_IME_TO_TSF_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeToTsfNamedPipe";
inline const wchar_t *FANY_IME_TO_TSF_WORKER_THREAD_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeToTsfWorkerThreadNamedPipe";
inline const wchar_t *FANY_IME_AUX_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeAuxNamedPipe";
inline const wchar_t *FANY_IME_TSF_DIAGNOSTIC_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeTsfDiagnosticNamedPipe";
// Dedicated Tauri-to-Server voice lifecycle channel. It never carries TSF
// frames and cannot be confused with the legacy Aux endpoint.
inline const wchar_t *FANY_IME_VOICE_CONTROL_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeVoiceControlNamedPipe";
inline constexpr uint32_t FANY_IME_TSF_DIAGNOSTIC_MAGIC = 0x474F4C54; // "TLOG"
inline constexpr uint32_t FANY_IME_TSF_DIAGNOSTIC_VERSION = 1;
// DLL -> Server local input statistics counters. A separate channel from the
// diagnostic log: independent switch gate and fixed binary records instead of
// log text. No payload of this channel ever contains typed text.
inline const wchar_t *FANY_IME_STATS_NAMED_PIPE = L"\\\\.\\pipe\\FanyImeStatsNamedPipe";
inline constexpr uint32_t FANY_IME_STATS_MAGIC = 0x54415453; // "STAT"
inline constexpr uint32_t FANY_IME_STATS_VERSION = 1;
inline constexpr size_t FANY_IME_TSF_DIAGNOSTIC_MAX_FRAME_BYTES = 16 * 1024;
inline constexpr size_t FANY_IME_STATS_MAX_FRAME_BYTES = 16 * 1024;
inline constexpr uint64_t FANY_IME_UNSOLICITED_REQUEST_ID = 0;
inline constexpr uint64_t FANY_IME_NO_REQUEST_ID = UINT64_MAX;

inline const std::vector<std::wstring> FANY_IME_EVENT_ARRAY = {
    L"FanyImeKeyEvent",           // Event sent to UI process to notify time to update UI by new pinyin_string
    L"FanyHideCandidateWndEvent", // Event sent to UI process to notify time to hide candidate window
    L"FanyShowCandidateWndEvent", // Event sent to UI process to notify time to show candidate window
    L"FanyMoveCandidateWndEvent", // Event sent to UI process to notify time to move candidate window
};

//
// modifiers:
//   0: non
//   1: shift
//   2: control
//   3: alt
//   4: win
//   5: to be supplemented
//
struct FanyImeSharedMemoryData
{
    std::uint32_t keycode;
    FanyImeWireChar wch;
    std::uint32_t modifiers_down = 0;
    int point[2] = {100, 100};
    int pinyin_length = 0;
    FanyImeWireChar pinyin_string[128];
    FanyImeWireChar candidate_string[1024];
    FanyImeWireChar selected_candiate_string[128];
};

//
// For uwp/metro apps, here we do not need candidate_string and selected_candiate_string,
// just let server process to handle them
//
// event_type
//   0: FanyImeKeyEvent
//   1: FanyHideCandidateWndEvent
//   2: FanyShowCandidateWndEvent
//   3: FanyMoveCandidateWndEvent
//   4: FanyLangbarRightClickEvent (legacy Main enum; tip now sends via Aux)
//
struct alignas(8) FanyImeNamedpipeData
{
    std::uint32_t event_type;
    uint64_t client_id = 0;
    uint64_t request_id = 0;
    std::uint32_t keycode;
    FanyImeWireChar wch;
    std::uint32_t modifiers_down = 0;
    int point[2] = {100, 100};
    int pinyin_length = 0;
    FanyImeWireChar pinyin_string[128];
};

namespace FanyImePipeEventType
{
constexpr std::uint32_t KeyEvent = 0;
constexpr std::uint32_t HideCandidateWnd = 1;
constexpr std::uint32_t ShowCandidateWnd = 2;
constexpr std::uint32_t MoveCandidateWnd = 3;
constexpr std::uint32_t LangbarRightClick = 4; // Aux text protocol; not accepted on Main
constexpr std::uint32_t ClientHello = 10;
constexpr std::uint32_t ClientActivated = 11;
constexpr std::uint32_t ClientDeactivated = 12; // terminal TIP switch; toolbar hidden
constexpr std::uint32_t StatusSnapshot = 13;    // wch: CapsLock 1/0
constexpr std::uint32_t ClientSuspended = 14;   // temporary focus route reset; toolbar kept
// Same payload as StatusSnapshot, but additionally asserts that this client
// owns thread focus right now. Sent when document focus returns to a session
// the Server may have re-routed to another client meanwhile; without the
// ownership claim the Server would discard it as an inactive-client event.
constexpr std::uint32_t FocusRestored = 15;
constexpr std::uint32_t IMESwitch = 7;
constexpr std::uint32_t PuncSwitch = 8;
constexpr std::uint32_t DoubleSingleByteSwitch = 9;
constexpr bool IsRouteDeactivation(std::uint32_t event_type)
{
    return event_type == ClientDeactivated || event_type == ClientSuspended;
}
constexpr bool IsTerminalDeactivation(std::uint32_t event_type)
{
    return event_type == ClientDeactivated;
}
} // namespace FanyImePipeEventType

// OR'd into modifiers_down on Main-pipe packets. Server strips it before any
// key-modifier policy runs. Used so UILess hosts (games) never get an HWND.
namespace FanyImePipeFlags
{
constexpr std::uint32_t UiLess = 0x80000000u;
} // namespace FanyImePipeFlags

namespace FanyImePipeRole
{
constexpr std::uint32_t Main = 0;
constexpr std::uint32_t ToTsf = 1;
constexpr std::uint32_t ToTsfWorkerThread = 2;
} // namespace FanyImePipeRole

// Tauri-to-Server voice lifecycle commands. Consumers must authenticate the
// client_id and activation epoch with the registered pipe lease; generation
// is a non-zero session value used to reject stale cancellation requests.
namespace FanyImeVoiceControl
{
constexpr std::uint32_t Start = 1;
constexpr std::uint32_t Stop = 2;
constexpr std::uint32_t Cancel = 3;
constexpr std::size_t MaxMessageChars = 96;
} // namespace FanyImeVoiceControl

struct alignas(8) FanyImePipeHello
{
    uint64_t client_id = 0;
    std::uint32_t pipe_role = 0;
};

//
// Data received from server end
//
// msg_type is a FanyImeReplyType opcode (declared below); candidate_string
// carries the reply's UTF-16 payload, which is empty for most opcodes.
//
struct alignas(8) FanyImeNamedpipeDataToTsf
{
    std::uint32_t msg_type;
    uint64_t request_id = 0;
    FanyImeWireChar candidate_string[FanyImePipeLimits::CandidateTextCapacity];
};

//
// Data sent to tsf worker thread
//
// msg_type
//   0: IME switch to EN
//   1: IME switch to CN
//
// data
//   Not used now.
//
//
struct FanyImeNamedpipeDataToTsfWorkerThread
{
    std::uint32_t msg_type;
    FanyImeWireChar data[FanyImePipeLimits::CandidateTextCapacity];
};

struct FanyImeTsfDiagnosticBatchHeader
{
    uint32_t magic = FANY_IME_TSF_DIAGNOSTIC_MAGIC;
    uint32_t version = FANY_IME_TSF_DIAGNOSTIC_VERSION;
    uint32_t header_size = 28;
    uint32_t payload_bytes = 0;
    uint32_t record_count = 0;
    uint32_t dropped_count = 0;
    uint32_t source_process_id = 0;
};

struct FanyImeStatsBatchHeader
{
    uint32_t magic = FANY_IME_STATS_MAGIC;
    uint32_t version = FANY_IME_STATS_VERSION;
    uint32_t header_size = 28;
    uint32_t payload_bytes = 0;
    uint32_t record_count = 0;
    uint32_t dropped_count = 0;
    uint32_t source_process_id = 0;
};

// One aggregated composition commit. Counts are user-perceived characters (a
// UTF-16 surrogate pair counts once) and active_ms is the typing time since the
// previous commit, capped at the idle threshold. Classification stays inside the
// TSF process: only these counters cross the pipe.
struct FanyImeStatsRecord
{
    uint32_t day_key = 0; // local date, YYYYMMDD
    uint16_t hour = 0;    // 0-23
    uint16_t cjk = 0;
    uint16_t latin = 0;
    uint16_t digit = 0;
    uint16_t punct = 0;
    uint16_t other = 0;
    uint32_t active_ms = 0;
};

static_assert(sizeof(FanyImeWireChar) == 2, "The IPC ABI requires 16-bit FanyImeWireChar.");
static_assert(offsetof(FanyImeNamedpipeData, client_id) == 8);
static_assert(offsetof(FanyImeNamedpipeData, request_id) == 16);
static_assert(offsetof(FanyImeNamedpipeData, keycode) == 24);
static_assert(offsetof(FanyImeNamedpipeData, pinyin_string) == 48);
static_assert(sizeof(FanyImeNamedpipeData) == 304);
static_assert(offsetof(FanyImeNamedpipeDataToTsf, request_id) == 8);
static_assert(offsetof(FanyImeNamedpipeDataToTsf, candidate_string) == 16);
static_assert(sizeof(FanyImeNamedpipeDataToTsf) == 416);
static_assert(sizeof(FanyImePipeHello) == 16);
static_assert(sizeof(FanyImeNamedpipeDataToTsfWorkerThread) == 404);
static_assert(sizeof(FanyImeTsfDiagnosticBatchHeader) == 28);
static_assert(sizeof(FanyImeStatsBatchHeader) == 28);
static_assert(sizeof(FanyImeStatsRecord) == 20);
static_assert(offsetof(FanyImeStatsRecord, day_key) == 0);
static_assert(offsetof(FanyImeStatsRecord, hour) == 4);
static_assert(offsetof(FanyImeStatsRecord, active_ms) == 16);

namespace FanyImeReplyType
{
constexpr std::uint32_t Normal = 0;
constexpr std::uint32_t OutofRange = 1;
constexpr std::uint32_t NeedToCreateWord = 2;
constexpr std::uint32_t Preedit = 3;
constexpr std::uint32_t NavigationIgnored = 4;
constexpr std::uint32_t MoveSelectionPrevious = 5;
constexpr std::uint32_t MoveSelectionNext = 6;
constexpr std::uint32_t MovePagePrevious = 7;
constexpr std::uint32_t MovePageNext = 8;
// Reverse-pipe registration acknowledgement. This frame is consumed during
// the ToTsf pipe handshake and is never exposed as a key reply.
constexpr std::uint32_t PipeReady = 9;
// candidate_string is the complete text to commit; do not append punctuation.
constexpr std::uint32_t CommitExactText = 10;
// UILess hosts (games): candidate_string = preedit + L'\t' + cand1,cand2,...
// Plain words only — no helpcodes — so ITfCandidateListUIElement::GetString
// matches Microsoft IME behavior for host-drawn candidate UI.
constexpr std::uint32_t UiLessComposition = 11;
// Registration-only replies, consumed before a new client may send keys.
constexpr std::uint32_t ProtocolReady = 12;
constexpr std::uint32_t ProtocolMismatch = 13;
// The Server retracted or repositioned an in-progress word and restored its raw
// pinyin: this is the authoritative reply for the retraction (Backspace), the
// unit deletion (Ctrl+Backspace) and the unit caret move (Ctrl+Left /
// Ctrl+Right). candidate_string =
//   remaining_raw \t committed_word \t display_preedit [\t caret]
// caret is an optional decimal offset into remaining_raw; a 3-field payload
// means "caret at the end". NeedToCreateWord and CompositionRestored share the
// payload parser on the TSF side.
constexpr std::uint32_t CompositionRestored = 14;
// Highest opcode a live Server may send. Local-only sentinels are larger and
// must never be accepted as a Server frame.
constexpr std::uint32_t MaxKnown = CompositionRestored;
// Local-only result. It is never sent over the pipe and must never be
// interpreted as candidate text to commit.
constexpr std::uint32_t TransportUnavailable = static_cast<std::uint32_t>(-1);
} // namespace FanyImeReplyType

namespace FanyImeWorkerReplyType
{
constexpr std::uint32_t SwitchToEnglish = 0;
constexpr std::uint32_t SwitchToChinese = 1;
constexpr std::uint32_t SwitchToPuncEn = 2;
constexpr std::uint32_t SwitchToPuncCn = 3;
constexpr std::uint32_t SwitchToFullwidth = 4;
constexpr std::uint32_t SwitchToHalfwidth = 5;
constexpr std::uint32_t CommitCurCandidate = 6;
constexpr std::uint32_t PagingCommaPeriodChanged = 7;
constexpr std::uint32_t FocusSessionReady = 8;
// Worker-endpoint registration acknowledgement. It is consumed before the
// handle is published to IpcWorkerThread and before Main can be opened.
constexpr std::uint32_t PipeReady = 9;
// Unsolicited text insert (voice ASR, etc.). Does not finalize candidates.
constexpr std::uint32_t InsertText = 10;
// Master switch for the reversible Chinese/ASCII punctuation conversion:
// space after a committed Chinese punctuation converts it. Payload "0"/"1".
constexpr std::uint32_t SmartPunctuationChanged = 11;
// Auto-complete opening paired punctuation and leave the caret inside. Payload "0"/"1".
constexpr std::uint32_t PairedPunctuationChanged = 12;
// Whether ';' is an input key for the Microsoft shuangpin profile.
constexpr std::uint32_t MicrosoftShuangpinChanged = 13;
// Revert a converted ASCII punctuation back to Chinese when the same
// punctuation key is pressed within the revert window. Payload "0"/"1".
constexpr std::uint32_t SmartPunctuationRepeatToChineseChanged = 14;
// Streaming ASR: replace the inline composition with this full snapshot.
constexpr std::uint32_t UpdateVoiceComposition = 15;
// Streaming ASR: abort the voice composition without committing.
constexpr std::uint32_t CancelVoiceComposition = 16;
// Streaming ASR: replace the inline composition with this snapshot and commit.
constexpr std::uint32_t CommitVoiceComposition = 17;
// Payload "1" when input.mode is Japanese, otherwise "0".
constexpr std::uint32_t InputModeChanged = 18;
// Payload "1" when Caps Lock is on. Server is the source of truth.
constexpr std::uint32_t CapsLockChanged = 19;
// Enables buffered TSF diagnostics sent to Server. Payload "0"/"1".
constexpr std::uint32_t TsfDiagnosticLogChanged = 20;
// Payload "0" follow IME, "1" always Chinese punctuation, "2" always English punctuation.
constexpr std::uint32_t PunctuationLockChanged = 21;
// A space after a committed Chinese punctuation converts it to ASCII. Covers
// plain punctuation and individually-occurring paired symbols. Payload "0"/"1".
constexpr std::uint32_t SmartPunctuationSpaceConvertChanged = 22;
// 23 was the paired-symbol space conversion, removed because an auto-closed
// pair loses its caret position. The number stays unused: this branch already
// shipped test builds, so reusing it would let an old test build misinterpret
// a new frame.
// Direct ASCII punctuation output for ',' '.' ':' after ASCII digits. Payload "0"/"1".
// Opcode 24 used to cover letters and digits together. Keeping the number and narrowing
// it to digits is a semantic subset, not a reuse: a new DLL reading an old "1" frame
// still does the right thing (digits direct, letters not), while the old DLL ignores the
// new 25 because it filters everything above its MaxKnown.
constexpr std::uint32_t SmartPunctuationDirectDigitChanged = 24;
// Direct ASCII punctuation output for ',' '.' ':' after ASCII letters. Payload "0"/"1".
constexpr std::uint32_t SmartPunctuationDirectLetterChanged = 25;
// Whether local input statistics are collected. Payload "1" on / "0" off.
// The number must not be recycled if the feature is ever removed: this branch
// ships test builds, and a reused opcode would let an old build misread a new
// frame (see 23 above).
constexpr std::uint32_t StatisticsEnabledChanged = 26;
constexpr std::uint32_t MaxKnown = StatisticsEnabledChanged;
// Source compatibility for the Server's historical spellings.
constexpr std::uint32_t SwitchToEn = SwitchToEnglish;
constexpr std::uint32_t SwitchToCn = SwitchToChinese;
constexpr std::uint32_t CommitCandidate = CommitCurCandidate;
} // namespace FanyImeWorkerReplyType
