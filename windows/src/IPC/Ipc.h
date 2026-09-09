#pragma once

#include "KeyEventSendResult.h"
#include "VoiceCompositionPipe.h"
#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../../../vendor/MetasequoiaImeEngine/contracts/windows_ipc.h"

int InitIpc();
int InitNamedpipe();
int ConnectToAllNamedpipe();
int ConnectToTsfNamedpipe();
int CloseIpc();
int CloseNamedpipe();
void ResetNamedpipeReplyState();
HANDLE GetToTsfWorkerThreadNamedpipe();
void BindNamedpipeFocusState(
    _In_ const void *owner, _In_opt_ bool *focusResetPending, _In_opt_ bool *activationRequired,
    _In_opt_ std::atomic<uint64_t> *expectedWorkerFocusToken, _In_opt_ std::atomic<bool> *localSessionResetPending,
    _In_opt_ std::atomic<UINT> *localSessionResetToken, _In_opt_ std::atomic<bool> *workerCommitReady,
    _In_opt_ std::atomic<uint64_t> *acknowledgedWorkerFocusToken, _In_opt_ std::atomic<HANDLE> *workerPipeHandle,
    _In_opt_ std::atomic<UINT> *workerPipeGeneration);
void UnbindNamedpipeFocusState(_In_ const void *owner);
bool IsNamedpipeFocusStateOwner(_In_ const void *owner);
UINT BeginNamedpipeLocalSessionReset();
void InvalidateNamedpipeWorkerGeneration();
void MarkNamedpipeFocusLost();
void RequireNamedpipeFocusActivation();
void MarkNamedpipeSessionDirty();
bool MarkNamedpipeSessionDirtyForOwner(_In_ const void *owner);
bool EnsureNamedpipeFocusSessionActivated();
bool SupportsCharacterSetShortcut();
bool FlushNamedpipeFocusSessionReset();
bool FlushNamedpipeImeDeactivation(uint64_t focusToken = 0);

//
// For shared memory
//
int WriteDataToSharedMemory(           //
    UINT keycode,                      // VkCode
    WCHAR wch,                         // Unicode character converted from vkcode
    UINT modifiers_down,               //
    const int point[2],                //
    int pinyin_length,                 //
    const std::wstring &pinyin_string, //
    UINT write_flag                    //
);
KeyEventSendResult SendKeyEventToUIProcess(_Out_opt_ uint64_t *requestId = nullptr);
void DebugTsfKeyLatency(_In_z_ const wchar_t *stage, uint64_t requestId, double elapsedMs, HRESULT result);
void DebugTsfIssue47(_In_z_ const wchar_t *stage, uint64_t requestId, UINT code, WCHAR wch, UINT category,
                     UINT function, int eaten, BOOL composing, size_t virtualKeyLength, HRESULT result,
                     uint64_t correlationToken = 0);
void QueueTsfDiagnosticLog(const std::wstring &line);
int SendHideCandidateWndEventToUIProcess();
int SendShowCandidateWndEventToUIProcess();
int SendMoveCandidateWndEventToUIProcess();
int SendLangbarRightClickEventToUIProcess(const RECT *prcArea);
int SendIMEActivationEventToUIProcessViaNamedPipe();
int SendIMEDeactivationEventToUIProcessViaNamedPipe();
int SendClientActivatedEventToServerViaNamedPipe(uint64_t focusToken);
int SendClientDeactivatedEventToServerViaNamedPipe(uint64_t focusToken = 0);
int SendClientSuspendedEventToServerViaNamedPipe();
int SendIMEStatusSnapshotToUIProcessViaNamedPipe(bool kbdIsOpen, bool fullwidthIsOpen, bool puncIsOpen,
                                                 bool assertsFocusOwnership = false);
int SendIMEStatusEventToUIProcessViaNamedPipe(bool kbdIsOpen, bool fullwidthIsOpen, bool puncIsOpen);
int SendIMESwitchEventToUIProcessViaNamedPipe(UINT uImeStatus);
int SendPuncSwitchEventToUIProcessViaNamedPipe(BOOL isPunc);
int SendDoubleSingleByteSwitchEventToUIProcessViaNamedPipe(BOOL isDoubleSingleByte);

bool SendToAuxNamedpipe(const std::wstring &pipeData, bool waitForAcknowledgement = false);

//
// For named pipe
//
int WriteDataToNamedPipe(              //
    UINT keycode,                      //
    WCHAR wch,                         //
    UINT modifiers_down,               //
    const int point[2],                //
    int pinyin_length,                 //
    const std::wstring &pinyin_string, //
    UINT write_flag                    //
);
KeyEventSendResult SendKeyEventToUIProcessViaNamedPipe(_Out_opt_ uint64_t *requestId = nullptr);
int SendHideCandidateWndEventToUIProcessViaNamedPipe();
int SendShowCandidateWndEventToUIProcessViaNamedPipe();
int SendMoveCandidateWndEventToUIProcessViaNamedPipe();
int SendLangbarRightClickEventToUIProcessViaNamedPipe(const RECT *prcArea);
void ClearNamedpipeDataIfExists(bool force = false);
// Best-effort read of the Server-published current candidate page (comma-
// separated). Used in UILess mode so ITfCandidateListUIElement::GetString can
// return real candidates after PrepareCandidateList has written shared memory.
bool TryReadCandidatePageFromSharedMemory(_Out_ std::wstring *candidatePage);
struct FanyImeNamedpipeDataToTsf *TryReadDataFromServerPipeWithTimeout(uint64_t expectedRequestId);
// When abortTransportOnTimeout is false, a missed reply leaves the pipe up and
// returns a non-TransportUnavailable empty frame for the caller to fall back.
struct FanyImeNamedpipeDataToTsf *TryReadDataFromServerPipeWithTimeout(uint64_t expectedRequestId,
                                                                       bool abortTransportOnTimeout);
struct FanyImeNamedpipeDataToTsf *ReadDataFromServerViaNamedPipe(uint64_t expectedRequestId);

//
// Modifiers:
//     0b00000001: Shift
//     0b00000010: Control
//     0b00000100: Alt
// TODO: Make it able to denote explicit modifiers, e.g. LShift, RShift, we could use left keys
//
namespace Global
{
inline thread_local UINT Keycode = 0;
inline thread_local WCHAR wch = L'\0';
inline thread_local UINT ModifiersDown = 0;
inline thread_local int Point[2] = {100, 100};
inline thread_local int PinyinLength = 0;
inline thread_local std::wstring PinyinString = L"";

// TF_TMF_UIELEMENTENABLEDONLY at ActivateEx, and/or BeginUIElement pbShow=FALSE.
inline thread_local bool HostUiLessMode = false;
inline thread_local bool CandidateUiLessMode = false;
inline bool IsUiLessMode()
{
    return HostUiLessMode || CandidateUiLessMode;
}

inline thread_local int firefox_like_cnt = 0; // Apps like firefox, e.g. firefox, zen...
inline thread_local std::wstring current_process_name = L"";

inline thread_local wchar_t app_name[512] = {0};

namespace DataFromServerMsgType = FanyImeReplyType;

namespace DataToTsfWorkerThreadMsgType = FanyImeWorkerReplyType;

namespace PunctuationLock
{
constexpr int Follow = 0;
constexpr int AlwaysChinese = 1;
constexpr int AlwaysEnglish = 2;
} // namespace PunctuationLock

inline std::atomic<int> PunctuationLockMode{PunctuationLock::Follow};

inline BOOL ResolvePunctuationOpen(BOOL followImeOpen)
{
    switch (PunctuationLockMode.load(std::memory_order_relaxed))
    {
    case PunctuationLock::AlwaysChinese:
        return TRUE;
    case PunctuationLock::AlwaysEnglish:
        return FALSE;
    default:
        return followImeOpen;
    }
}

inline bool IsPunctuationLocked()
{
    return PunctuationLockMode.load(std::memory_order_relaxed) != PunctuationLock::Follow;
}

inline std::atomic_bool PagingCommaPeriodEnabled{false};
// Default on: matches rime-ice digit_separators behavior until Server syncs.
inline std::atomic_bool SmartPunctuationEnabled{true};
// Default on until the Server sends the persisted setting.
inline std::atomic_bool SmartPunctuationRepeatToChineseEnabled{true};
// Default on until the Server sends the persisted setting.
inline std::atomic_bool PairedPunctuationEnabled{true};
inline std::atomic_bool MicrosoftShuangpinEnabled{false};
inline std::atomic_bool JapaneseInputModeEnabled{false};
inline std::atomic_bool CapsLockEnabled{false};
inline std::atomic_bool TsfDiagnosticLogEnabled{false};
inline thread_local bool g_connected = false;

} // namespace Global
