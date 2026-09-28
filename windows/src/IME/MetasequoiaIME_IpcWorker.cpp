// Worker pipe side: IpcWorkerThread, which validates and dispatches Server frames, and the
// token tables that hand its commits, voice snapshots and compartment switches to the TSF thread.

#include "Private.h"
#include "Globals.h"
#include "MetasequoiaIME.h"
#include "CandidateListUIPresenter.h"
#include "CompositionProcessorEngine.h"
#include "Compartment.h"
#include "define.h"
#include <debugapi.h>
#include <namedpipeapi.h>
#include <winnt.h>
#include <winuser.h>
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "FanyLog.h"
#include "Ipc.h"
#include "CommonUtils.h"
#include "Global/FanyDefines.h"
#include "Utils/FanyUtils.h"
#include "../Utils/PerfTimer.h"
#include "MetasequoiaIMEInternal.h"

using namespace metasequoia_ime_detail;

bool CMetasequoiaIME::_PostServerCandidateCommit(_In_z_ const WCHAR *candidateText)
{
    return _PostServerTextDelivery(WM_CommitCandidate, candidateText);
}

bool CMetasequoiaIME::_PostServerCandidateCommitAndContinue(_In_z_ const WCHAR *payload)
{
    return _PostServerTextDelivery(WM_CommitCandidateAndContinue, payload);
}

bool CMetasequoiaIME::_PostServerInsertText(_In_z_ const WCHAR *text)
{
    return _PostServerTextDelivery(WM_InsertText, text);
}

void CMetasequoiaIME::_ResetVoiceCompositionAssemble()
{
    _voiceCompositionAssemble.clear();
    _voiceCompositionAssembleMsg = 0;
    _voiceCompositionAssembleGeneration = 0;
    _voiceCompositionAssembleActive = false;
}

void CMetasequoiaIME::_AssembleVoiceCompositionFrame(const FanyImeNamedpipeDataToTsfWorkerThread &buf)
{
    const FanyImeVoiceCompositionPipe::Frame frame = FanyImeVoiceCompositionPipe::ParseFrame(buf.data);
    if (!frame.valid)
    {
        _ResetVoiceCompositionAssemble();
        return;
    }

    if (frame.first)
    {
        _voiceCompositionAssemble.clear();
        _voiceCompositionAssembleMsg = buf.msg_type;
        _voiceCompositionAssembleGeneration = frame.generation;
        _voiceCompositionAssembleActive = true;
    }
    else if (!_voiceCompositionAssembleActive || frame.generation != _voiceCompositionAssembleGeneration ||
             buf.msg_type != _voiceCompositionAssembleMsg)
    {
        _ResetVoiceCompositionAssemble();
        return;
    }

    if (_voiceCompositionAssemble.size() + frame.chunk.size() > FanyImeVoiceCompositionPipe::kMaxSnapshotChars)
    {
        _ResetVoiceCompositionAssemble();
        return;
    }
    _voiceCompositionAssemble.append(frame.chunk);

    if (!frame.last)
    {
        return;
    }

    const UINT windowMessage = buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitVoiceComposition
                                   ? WM_CommitVoiceComposition
                                   : WM_UpdateVoiceComposition;
    const std::wstring snapshot = std::move(_voiceCompositionAssemble);
    _ResetVoiceCompositionAssemble();
    if (_workerCommitReady.load(std::memory_order_acquire))
    {
        _PostServerTextDelivery(windowMessage, snapshot.c_str());
    }
}

void CMetasequoiaIME::_DispatchUnsolicitedVoiceText(WPARAM wParam, KEYSTROKE_FUNCTION function)
{
    WorkerCandidateCommit request;
    if (!_TakeServerCandidateCommit(static_cast<UINT>(wParam), request))
    {
        return;
    }
    if (!_IsFocusSessionCurrent(request.focusToken) || !_IsCompositionEpochCurrent(request.compositionEpoch))
    {
        return;
    }
    ITfDocumentMgr *pDocMgrFocus = nullptr;
    ITfContext *pContext = nullptr;
    if (SUCCEEDED(_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
    {
        if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
        {
            _KEYSTROKE_STATE KeystrokeState;
            KeystrokeState.Category = CATEGORY_COMPOSING;
            KeystrokeState.Function = function;
            _InvokeKeyHandler(pContext, 0, 0, 0, KeystrokeState, FANY_IME_UNSOLICITED_REQUEST_ID,
                              std::move(request.text), 0, request.compositionEpoch, request.focusToken);
            pContext->Release();
        }
        pDocMgrFocus->Release();
    }
}

bool CMetasequoiaIME::_PostServerTextDelivery(UINT windowMessage, _In_z_ const WCHAR *text)
{
    constexpr size_t maxPendingServerCommits = 64;
    if (!_workerCommitReady.load(std::memory_order_acquire) ||
        _localSessionResetPending.load(std::memory_order_acquire) ||
        _acknowledgedWorkerFocusToken.load(std::memory_order_acquire) == 0)
    {
        return false;
    }
    const HWND ownerWindow = _msgWndHandle;
    if (!ownerWindow || !IsWindow(ownerWindow))
    {
        return false;
    }

    UINT token = 0;
    const uint64_t focusToken = _expectedWorkerFocusToken.load(std::memory_order_acquire);
    const uint64_t compositionEpoch = _CaptureCompositionEpoch();
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        if (!_workerCommitReady.load(std::memory_order_relaxed) ||
            _localSessionResetPending.load(std::memory_order_relaxed) || focusToken == 0 ||
            _expectedWorkerFocusToken.load(std::memory_order_relaxed) != focusToken ||
            _acknowledgedWorkerFocusToken.load(std::memory_order_relaxed) != focusToken ||
            _pendingServerCommitMessages.size() >= maxPendingServerCommits)
        {
            return false;
        }
        do
        {
            token = NextWindowMessageToken();
        } while (token == 0 || _pendingServerCommitMessages.count(token) != 0);
        _pendingServerCommitMessages.emplace(token,
                                             WorkerCandidateCommit{text ? text : L"", focusToken, compositionEpoch});
    }

    if (!PostMessage(ownerWindow, windowMessage, static_cast<WPARAM>(token), 0))
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        _pendingServerCommitMessages.erase(token);
        // The delivery was lost locally; the pipes are fine. Resync the
        // composition on the owner thread instead of rotating the transport.
        PostMessage(ownerWindow, WM_IpcSessionDirty, 0, IPC_SESSION_DIRTY_RESYNC);
        return false;
    }
    return true;
}

bool CMetasequoiaIME::_TakeServerCandidateCommit(UINT token, _Out_ WorkerCandidateCommit &request)
{
    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    const auto entry = _pendingServerCommitMessages.find(token);
    if (entry == _pendingServerCommitMessages.end())
    {
        return false;
    }
    request = std::move(entry->second);
    _pendingServerCommitMessages.erase(entry);
    return true;
}

bool CMetasequoiaIME::_PostWorkerCompartmentSwitch(UINT messageType, uint64_t focusToken)
{
    constexpr size_t maxPendingSwitches = 64;
    if (messageType < Global::DataToTsfWorkerThreadMsgType::SwitchToEnglish ||
        messageType > Global::DataToTsfWorkerThreadMsgType::SwitchToHalfwidth || focusToken == 0 ||
        !_workerCommitReady.load(std::memory_order_acquire) ||
        _localSessionResetPending.load(std::memory_order_acquire) ||
        _expectedWorkerFocusToken.load(std::memory_order_acquire) != focusToken ||
        _acknowledgedWorkerFocusToken.load(std::memory_order_acquire) != focusToken)
    {
        return false;
    }

    const HWND ownerWindow = _msgWndHandle;
    if (!ownerWindow || !IsWindow(ownerWindow))
    {
        return false;
    }

    UINT token = 0;
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        if (!_workerCommitReady.load(std::memory_order_relaxed) ||
            _localSessionResetPending.load(std::memory_order_relaxed) ||
            _expectedWorkerFocusToken.load(std::memory_order_relaxed) != focusToken ||
            _acknowledgedWorkerFocusToken.load(std::memory_order_relaxed) != focusToken ||
            _pendingWorkerSwitchMessages.size() >= maxPendingSwitches)
        {
            return false;
        }
        do
        {
            token = NextWindowMessageToken();
        } while (token == 0 || _pendingWorkerSwitchMessages.count(token) != 0);
        _pendingWorkerSwitchMessages.emplace(
            token, WorkerCompartmentSwitch{messageType, focusToken, _CaptureCompositionEpoch()});
    }

    if (!PostMessage(ownerWindow, WM_CheckGlobalCompartment, static_cast<WPARAM>(token), 0))
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        _pendingWorkerSwitchMessages.erase(token);
        return false;
    }
    return true;
}

bool CMetasequoiaIME::_TakeWorkerCompartmentSwitch(UINT token, _Out_ WorkerCompartmentSwitch &request)
{
    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    const auto entry = _pendingWorkerSwitchMessages.find(token);
    if (entry == _pendingWorkerSwitchMessages.end())
    {
        return false;
    }
    request = entry->second;
    _pendingWorkerSwitchMessages.erase(entry);
    return true;
}

//+---------------------------------------------------------------------------
//
// IpcWorkerThread
// 从 Server 端接收消息
//
//----------------------------------------------------------------------------
void CMetasequoiaIME::IpcWorkerThread(CMetasequoiaIME *pIME)
{
    const auto notifyDisconnected = [pIME](HANDLE disconnectedPipe, UINT disconnectedGeneration) {
        if (disconnectedGeneration == 0 ||
            pIME->_workerPipeGeneration.load(std::memory_order_acquire) != disconnectedGeneration)
        {
            return;
        }
        // Close the first-key window immediately on the reader thread.  The UI
        // message may be delayed, fail to post, or race a replacement handle.
        pIME->_workerCommitReady.store(false, std::memory_order_release);
        pIME->_acknowledgedWorkerFocusToken.store(0, std::memory_order_release);
        pIME->_ResetVoiceCompositionAssemble();
        HANDLE expected = disconnectedPipe;
        if (pIME->_hToTsfWorkerThreadPipe.compare_exchange_strong(expected, nullptr))
        {
            const HWND ownerWindow = pIME->_msgWndHandle;
            if (ownerWindow && IsWindow(ownerWindow))
            {
                PostMessage(ownerWindow, WM_IpcWorkerDisconnected, reinterpret_cast<WPARAM>(disconnectedPipe),
                            static_cast<LPARAM>(disconnectedGeneration));
            }
        }
    };

    while (!pIME->_shouldStopIpcThread.load())
    {
        HANDLE workerPipe = pIME->_hToTsfWorkerThreadPipe.load(std::memory_order_acquire);
        const UINT workerGeneration = pIME->_workerPipeGeneration.load(std::memory_order_acquire);
        if (!workerPipe || workerPipe == INVALID_HANDLE_VALUE)
        {
            // Wake as soon as the UI thread publishes a pipe instead of on the
            // next 50 ms tick; the timeout stays as a fallback. Stop is index 0
            // so it wins when both are signalled.
            HANDLE waitHandles[] = {pIME->_ipcStopEvent, pIME->_workerPipePublishedEvent};
            const DWORD waitCount = pIME->_workerPipePublishedEvent ? 2 : 1;
            if (pIME->_ipcStopEvent && WaitForMultipleObjects(waitCount, waitHandles, FALSE, 50) == WAIT_OBJECT_0)
            {
                return;
            }
            continue;
        }

        DWORD bytesRead = 0;
        FanyImeNamedpipeDataToTsfWorkerThread buf = {};
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent)
        {
            notifyDisconnected(workerPipe, workerGeneration);
            continue;
        }

        BOOL readResult = ReadFile(workerPipe, &buf, sizeof(buf), &bytesRead, &overlapped);
        if (!readResult && GetLastError() == ERROR_IO_PENDING)
        {
            HANDLE waitHandles[] = {pIME->_ipcStopEvent, overlapped.hEvent};
            const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0)
            {
                CancelIoEx(workerPipe, &overlapped);
                GetOverlappedResult(workerPipe, &overlapped, &bytesRead, TRUE);
                CloseHandle(overlapped.hEvent);
                return;
            }
            if (waitResult == WAIT_OBJECT_0 + 1)
            {
                readResult = GetOverlappedResult(workerPipe, &overlapped, &bytesRead, FALSE);
            }
            else
            {
                // WAIT_FAILED and every unexpected result must drain the
                // pending operation before this stack OVERLAPPED is destroyed.
                CancelIoEx(workerPipe, &overlapped);
                GetOverlappedResult(workerPipe, &overlapped, &bytesRead, TRUE);
                readResult = FALSE;
            }
        }
        CloseHandle(overlapped.hEvent);

        // Transport errors tear down the pipe. Unknown future opcodes and
        // soft-invalid config payloads are ignored so protocol additions cannot
        // freeze every host process that loads this TIP.
        if (!readResult || bytesRead != sizeof(buf))
        {
            if (pIME->_shouldStopIpcThread.load())
            {
                return;
            }
            notifyDisconnected(workerPipe, workerGeneration);
            continue;
        }
        if (buf.msg_type > Global::DataToTsfWorkerThreadMsgType::MaxKnown)
        {
            continue;
        }

        bool validFrame = true;
        if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitCurCandidate ||
            buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitCandidateAndContinue ||
            buf.msg_type == Global::DataToTsfWorkerThreadMsgType::InsertText ||
            buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CancelVoiceComposition)
        {
            bool hasTerminator = false;
            for (const wchar_t ch : buf.data)
            {
                if (ch == L'\0')
                {
                    hasTerminator = true;
                    break;
                }
            }
            validFrame = hasTerminator;
        }
        if (validFrame && buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PagingCommaPeriodChanged)
        {
            // Accepted forms: "0", "1", "0|raw", "1|pinyin", "0|empty".
            // Legacy clients only inspected data[0]; keep that contract.
            bool hasTerminator = false;
            for (const wchar_t ch : buf.data)
            {
                if (ch == L'\0')
                {
                    hasTerminator = true;
                    break;
                }
            }
            const bool pagingOk = buf.data[0] == L'0' || buf.data[0] == L'1';
            if (!hasTerminator || !pagingOk)
            {
                validFrame = false;
            }
            else if (buf.data[1] == L'\0')
            {
                // Legacy "0"/"1" payload.
            }
            else if (buf.data[1] == L'|')
            {
                validFrame = GlobalSettings::isKnownTsfPreeditStyleWide(buf.data + 2);
            }
            else
            {
                validFrame = false;
            }
        }
        if (validFrame &&
            (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationRepeatToChineseChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationSpaceConvertChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationDirectDigitChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationDirectLetterChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PairedPunctuationChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::MicrosoftShuangpinChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::InputModeChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CapsLockChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::TsfDiagnosticLogChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::StatisticsEnabledChanged))
        {
            bool hasTerminator = false;
            for (const wchar_t ch : buf.data)
            {
                if (ch == L'\0')
                {
                    hasTerminator = true;
                    break;
                }
            }
            validFrame = hasTerminator && (buf.data[0] == L'0' || buf.data[0] == L'1') && buf.data[1] == L'\0';
        }
        if (validFrame && buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PunctuationLockChanged)
        {
            bool hasTerminator = false;
            for (const wchar_t ch : buf.data)
            {
                if (ch == L'\0')
                {
                    hasTerminator = true;
                    break;
                }
            }
            validFrame = hasTerminator && (buf.data[0] == L'0' || buf.data[0] == L'1' || buf.data[0] == L'2') &&
                         buf.data[1] == L'\0';
        }
        if (validFrame && (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::UpdateVoiceComposition ||
                           buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitVoiceComposition))
        {
            validFrame = FanyImeVoiceCompositionPipe::ParseFrame(buf.data).valid;
        }
        if (validFrame && buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PipeReady)
        {
            // The registration ACK is normally consumed before this handle is
            // published. Ignore a defensive duplicate only when well formed.
            validFrame = buf.data[0] == L'\0';
        }
        uint64_t focusToken = 0;
        if (validFrame && buf.msg_type == Global::DataToTsfWorkerThreadMsgType::FocusSessionReady)
        {
            bool hasTerminator = false;
            for (const wchar_t ch : buf.data)
            {
                if (ch == L'\0')
                {
                    hasTerminator = true;
                    break;
                }
            }
            wchar_t *end = nullptr;
            if (hasTerminator && buf.data[0] != L'\0')
            {
                focusToken = _wcstoui64(buf.data, &end, 10);
            }
            // Token 0 is a syntactically valid legacy/dummy activation marker,
            // but it can never satisfy the nonzero focus barrier below. Treat
            // it as a harmless stale frame instead of tearing down the healthy
            // worker pipe.
            validFrame = hasTerminator && buf.data[0] != L'\0' && end && *end == L'\0';
        }

        if (!validFrame)
        {
            if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::UpdateVoiceComposition ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitVoiceComposition ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CancelVoiceComposition)
            {
                pIME->_ResetVoiceCompositionAssemble();
            }
            // Soft config/control frames: drop without killing the worker pipe.
            if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PagingCommaPeriodChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationRepeatToChineseChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationSpaceConvertChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationDirectDigitChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationDirectLetterChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PairedPunctuationChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::MicrosoftShuangpinChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::InputModeChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CapsLockChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::TsfDiagnosticLogChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::StatisticsEnabledChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PunctuationLockChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PipeReady ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::FocusSessionReady ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::UpdateVoiceComposition ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitVoiceComposition ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CancelVoiceComposition)
            {
                continue;
            }
            if (pIME->_shouldStopIpcThread.load())
            {
                return;
            }
            notifyDisconnected(workerPipe, workerGeneration);
            continue;
        }

        const bool isVoiceSnapshot = buf.msg_type == Global::DataToTsfWorkerThreadMsgType::UpdateVoiceComposition ||
                                     buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitVoiceComposition;
        if (!isVoiceSnapshot)
        {
            pIME->_ResetVoiceCompositionAssemble();
        }

        if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::FocusSessionReady)
        {
            const uint64_t expectedToken = pIME->_expectedWorkerFocusToken.load(std::memory_order_acquire);
            if (expectedToken != 0 && focusToken == expectedToken)
            {
                pIME->_acknowledgedWorkerFocusToken.store(focusToken, std::memory_order_release);
                pIME->_workerCommitReady.store(true, std::memory_order_release);
                // Wake the UI thread waiting in EnsureNamedpipeFocusSessionActivated.
                if (pIME->_workerAckEvent)
                {
                    SetEvent(pIME->_workerAckEvent);
                }
            }
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitCurCandidate)
        {
            if (pIME->_workerCommitReady.load(std::memory_order_acquire))
            {
                pIME->_PostServerCandidateCommit(buf.data);
            }
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitCandidateAndContinue)
        {
            if (pIME->_workerCommitReady.load(std::memory_order_acquire))
            {
                pIME->_PostServerCandidateCommitAndContinue(buf.data);
            }
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::InsertText)
        {
            if (pIME->_workerCommitReady.load(std::memory_order_acquire))
            {
                pIME->_PostServerInsertText(buf.data);
            }
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::UpdateVoiceComposition ||
                 buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitVoiceComposition)
        {
            pIME->_AssembleVoiceCompositionFrame(buf);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CancelVoiceComposition)
        {
            if (pIME->_workerCommitReady.load(std::memory_order_acquire))
            {
                pIME->_PostServerTextDelivery(WM_CancelVoiceComposition, L"");
            }
        }
        else if (buf.msg_type >= Global::DataToTsfWorkerThreadMsgType::SwitchToEnglish &&
                 buf.msg_type <= Global::DataToTsfWorkerThreadMsgType::SwitchToHalfwidth)
        {
            const uint64_t expectedFocusToken = pIME->_expectedWorkerFocusToken.load(std::memory_order_acquire);
            pIME->_PostWorkerCompartmentSwitch(buf.msg_type, expectedFocusToken);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PagingCommaPeriodChanged)
        {
            const bool enabled = buf.data[0] == L'1';
            Global::PagingCommaPeriodEnabled.store(enabled, std::memory_order_relaxed);
            if (buf.data[1] == L'|')
            {
                GlobalSettings::setTsfPreeditStyleFromWide(buf.data + 2);
            }
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationChanged)
        {
            Global::SmartPunctuationEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationRepeatToChineseChanged)
        {
            Global::SmartPunctuationRepeatToChineseEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationSpaceConvertChanged)
        {
            Global::SmartPunctuationSpaceConvertEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationDirectDigitChanged)
        {
            Global::SmartPunctuationDirectDigitEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::SmartPunctuationDirectLetterChanged)
        {
            Global::SmartPunctuationDirectLetterEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PairedPunctuationChanged)
        {
            Global::PairedPunctuationEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::MicrosoftShuangpinChanged)
        {
            Global::MicrosoftShuangpinEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::InputModeChanged)
        {
            Global::JapaneseInputModeEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
            // The frame only carries the JP on/off bit. The kana-vs-romaji
            // layout choice lives in the same [input] table, so re-read it here
            // whenever the server broadcasts an input-mode/scheme change.
            Global::JapaneseKanaLayoutEnabled.store(FanyUtils::ReadConfiguredJapaneseKanaLayout() != FALSE,
                                                    std::memory_order_relaxed);
            const HWND ownerWindow = pIME->_msgWndHandle;
            if (ownerWindow && IsWindow(ownerWindow))
            {
                PostMessage(ownerWindow, WM_RefreshLanguageBarTheme, 0, 0);
            }
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::TsfDiagnosticLogChanged)
        {
            Global::TsfDiagnosticLogEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::StatisticsEnabledChanged)
        {
            Global::StatisticsEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PunctuationLockChanged)
        {
            int lock = Global::PunctuationLock::Follow;
            if (buf.data[0] == L'1')
            {
                lock = Global::PunctuationLock::AlwaysChinese;
            }
            else if (buf.data[0] == L'2')
            {
                lock = Global::PunctuationLock::AlwaysEnglish;
            }
            Global::PunctuationLockMode.store(lock, std::memory_order_relaxed);
            const HWND ownerWindow = pIME->_msgWndHandle;
            if (ownerWindow && IsWindow(ownerWindow))
            {
                PostMessage(ownerWindow, WM_ApplyPunctuationLock, 0, 0);
            }
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CapsLockChanged)
        {
            Global::CapsLockEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
            const HWND ownerWindow = pIME->_msgWndHandle;
            if (ownerWindow && IsWindow(ownerWindow))
            {
                PostMessage(ownerWindow, WM_RefreshLanguageBarTheme, 1, 0);
            }
        }
    }
}
