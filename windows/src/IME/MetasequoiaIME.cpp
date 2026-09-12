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

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace
{
constexpr UINT REFRESH_LANG_BAR_THEME_DELAY_MS = 150;
constexpr UINT CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS = 50;
constexpr UINT CONNECT_NAMEDPIPE_MAX_RETRY_INTERVAL_MS = 2000;
constexpr UINT IPC_FAILURES_BEFORE_SERVER_LAUNCH = 6;
constexpr UINT SERVER_LAUNCH_RECONNECT_DELAY_MS = 500;
constexpr wchar_t SERVER_MUTEX_NAME[] = L"Local\\MetasequoiaImeServer_SingleInstance";
constexpr wchar_t SERVER_LAUNCH_MUTEX_NAME[] = L"Local\\MetasequoiaImeServer.Launch";
constexpr wchar_t INSTALL_REGISTRY_KEY[] = L"Software\\Metasequoia\\MetasequoiaIME";
constexpr wchar_t SERVER_PATH_REGISTRY_VALUE[] = L"ServerPath";
std::atomic<UINT> nextWindowMessageToken{0};
std::atomic<bool> serverLaunchInFlight{false};

bool IsServerAlreadyRunning()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, SERVER_MUTEX_NAME);
    if (mutex)
    {
        CloseHandle(mutex);
        return true;
    }

    // uiAccess can put the Server at a different integrity level. Access
    // denied still proves that the named mutex exists.
    return GetLastError() == ERROR_ACCESS_DENIED;
}

std::wstring ReadServerPath()
{
    wchar_t path[32768]{};
    DWORD bytes = sizeof(path);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, INSTALL_REGISTRY_KEY, SERVER_PATH_REGISTRY_VALUE,
                                        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, path, &bytes);
    if (status != ERROR_SUCCESS || path[0] == L'\0')
    {
        return {};
    }
    return path;
}

bool LaunchServerIfNeeded()
{
    if (IsServerAlreadyRunning())
    {
        return true;
    }

    HANDLE launchMutex = CreateMutexW(nullptr, TRUE, SERVER_LAUNCH_MUTEX_NAME);
    if (!launchMutex)
    {
        // Another integrity level may own the launch gate.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(launchMutex);
        return true;
    }

    const std::wstring serverPath = ReadServerPath();
    if (serverPath.empty() || GetFileAttributesW(serverPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (Global::TsfDiagnosticLogEnabled.load(std::memory_order_relaxed))
        {
            QueueTsfDiagnosticLog(L"[msime]: Server path is unavailable; cannot revive Server.");
        }
        ReleaseMutex(launchMutex);
        CloseHandle(launchMutex);
        return false;
    }

    const size_t separator = serverPath.find_last_of(L"\\/");
    const std::wstring workingDirectory =
        separator == std::wstring::npos ? std::wstring{} : serverPath.substr(0, separator);
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", serverPath.c_str(), nullptr,
                      workingDirectory.empty() ? nullptr : workingDirectory.c_str(), SW_SHOWNOACTIVATE);
    const bool launched = reinterpret_cast<INT_PTR>(result) > 32;
    ReleaseMutex(launchMutex);
    CloseHandle(launchMutex);
    return launched;
}

DWORD WINAPI ServerLaunchThreadProc(LPVOID parameter)
{
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LaunchServerIfNeeded();
    if (SUCCEEDED(comInit))
    {
        CoUninitialize();
    }
    serverLaunchInFlight.store(false, std::memory_order_release);
    // The reference taken in RequestServerLaunch keeps the TIP mapped even if
    // the host deactivates it while the shell is still resolving the Server.
    // FreeLibraryAndExitThread is __declspec(noreturn); a trailing `return 0;`
    // here is dead code (C4702), not a missing-return guard.
    FreeLibraryAndExitThread(static_cast<HMODULE>(parameter), 0);
}

// Starting the uiAccess Server goes through the shell, which can block for
// seconds. Every caller here is on the host's TSF/UI thread, where a stall
// freezes the application's message pump and makes cross-apartment calls into
// this TIP (ITfLangBarItemButton::GetIcon among them) fail. Hand the launch to
// a throwaway thread and let the reconnect timer discover the Server.
//
// Returns true when the Server is running or a launch is under way.
bool RequestServerLaunch()
{
    if (IsServerAlreadyRunning())
    {
        return true;
    }
    if (serverLaunchInFlight.exchange(true, std::memory_order_acq_rel))
    {
        return true;
    }

    HMODULE selfModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ServerLaunchThreadProc),
                            &selfModule))
    {
        serverLaunchInFlight.store(false, std::memory_order_release);
        return false;
    }

    const HANDLE launchThread = CreateThread(nullptr, 0, ServerLaunchThreadProc, selfModule, 0, nullptr);
    if (!launchThread)
    {
        FreeLibrary(selfModule);
        serverLaunchInFlight.store(false, std::memory_order_release);
        return false;
    }
    CloseHandle(launchThread);
    return true;
}

UINT NextWindowMessageToken()
{
    UINT token = 0;
    do
    {
        token = nextWindowMessageToken.fetch_add(1, std::memory_order_relaxed) + 1;
    } while (token == 0);
    return token;
}

void SendCurrentImeStatusSnapshot(CMetasequoiaIME *pIME, bool assertsFocusOwnership = false)
{
    if (!Global::g_connected || !pIME || !pIME->GetCompositionProcessorEngine() || !pIME->_GetThreadMgr())
    {
        // Distinct from the drop inside the pipe layer: the compartments are
        // never even read here, so no value was available to resend later.
        return;
    }

    if (assertsFocusOwnership)
    {
        // The claim makes the Server re-route to this client, so it must be
        // backed by the one authority that cannot be held by two threads at
        // once. Document focus alone is not enough: an app may call SetFocus
        // on a background thread manager.
        BOOL hasThreadFocus = FALSE;
        if (FAILED(pIME->_GetThreadMgr()->IsThreadFocus(&hasThreadFocus)) || !hasThreadFocus)
        {
            return;
        }
    }

    CCompositionProcessorEngine *engine = pIME->GetCompositionProcessorEngine();
    SendIMEStatusSnapshotToUIProcessViaNamedPipe(
        engine->GetIMEMode(pIME->_GetThreadMgr(), pIME->_GetClientId()),
        engine->GetDoubleSingleByteMode(pIME->_GetThreadMgr(), pIME->_GetClientId()),
        engine->GetPunctuationMode(pIME->_GetThreadMgr(), pIME->_GetClientId()), assertsFocusOwnership);
}

class CPunctuationCommitEditSession : public CEditSessionBase
{
  public:
    CPunctuationCommitEditSession(CMetasequoiaIME *pTextService, ITfContext *pContext, UINT code, WCHAR wch,
                                  uint64_t requestId, LARGE_INTEGER requestStartQpc, std::wstring prefetchedText,
                                  uint64_t focusToken, uint64_t compositionEpoch, uint64_t deferredReplayToken)
        : CEditSessionBase(pTextService, pContext), _code(code), _wch(wch), _requestId(requestId),
          _requestStartQpc(requestStartQpc), _prefetchedText(std::move(prefetchedText)), _focusToken(focusToken),
          _compositionEpoch(compositionEpoch), _deferredReplayToken(deferredReplayToken)
    {
        if (_requestStartQpc.QuadPart == 0)
        {
            QueryPerformanceCounter(&_requestStartQpc);
        }
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        struct Completion
        {
            CMetasequoiaIME *textService;
            uint64_t token;
            bool applied = false;
            ~Completion()
            {
                if (textService && token != 0)
                {
                    if (applied)
                    {
                        textService->_CompleteDeferredKeyReplay(token);
                    }
                    else
                    {
                        textService->_RetryDeferredKeyReplay(token);
                    }
                }
            }
        } completion{_pTextService, _deferredReplayToken, false};

        if (!_pTextService->_IsFocusSessionCurrent(_focusToken, _pContext) ||
            !_pTextService->_IsCompositionEpochCurrent(_compositionEpoch))
        {
            return S_FALSE;
        }
        HRESULT hr =
            _pTextService->_HandleCompositionPunctuation(ec, _pContext, _code, _wch, _requestId, _prefetchedText);
        completion.applied = hr == S_OK;
        return hr;
    }

  private:
    UINT _code;
    WCHAR _wch;
    uint64_t _requestId;
    LARGE_INTEGER _requestStartQpc;
    std::wstring _prefetchedText;
    uint64_t _focusToken;
    uint64_t _compositionEpoch;
    uint64_t _deferredReplayToken;
};

class CDeferredApplicationTextEditSession : public CEditSessionBase
{
  public:
    CDeferredApplicationTextEditSession(CMetasequoiaIME *pTextService, ITfContext *pContext, WCHAR wch,
                                        uint64_t focusToken, uint64_t focusGeneration, uint64_t replayToken)
        : CEditSessionBase(pTextService, pContext), _wch(wch), _focusToken(focusToken),
          _focusGeneration(focusGeneration), _replayToken(replayToken)
    {
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        const bool replayCurrent =
            _pTextService->_IsDeferredKeyReplayCurrent(_replayToken, _focusGeneration, _pContext);
        const bool fallbackActive = _pTextService->_IsServerUnavailableFallbackActive();
        const bool focusCurrent = _pTextService->_IsFocusSessionCurrent(_focusToken, _pContext);
        if (!replayCurrent || (!fallbackActive && !focusCurrent))
        {
            _pTextService->_RetryDeferredKeyReplay(_replayToken);
            return S_FALSE;
        }

        TF_SELECTION selection = {};
        ULONG fetched = 0;
        bool textApplied = false;
        HRESULT hr = _pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (SUCCEEDED(hr) && fetched == 1 && selection.range)
        {
            hr = selection.range->SetText(ec, 0, &_wch, 1);
            if (SUCCEEDED(hr))
            {
                // SetText is the irreversible operation.  A subsequent
                // selection-placement failure must not retry the character,
                // otherwise the application receives it twice.
                textApplied = true;
                selection.range->Collapse(ec, TF_ANCHOR_END);
                hr = _pContext->SetSelection(ec, 1, &selection);
            }
            selection.range->Release();
        }
        else if (SUCCEEDED(hr))
        {
            hr = E_FAIL;
        }

        if (textApplied)
        {
            _pTextService->_CompleteDeferredKeyReplay(_replayToken);
        }
        else
        {
            _pTextService->_RetryDeferredKeyReplay(_replayToken);
        }
        return hr;
    }

  private:
    WCHAR _wch;
    uint64_t _focusToken;
    uint64_t _focusGeneration;
    uint64_t _replayToken;
};

} // namespace

//+---------------------------------------------------------------------------
//
// CreateInstance
//
//----------------------------------------------------------------------------

/* static */
HRESULT CMetasequoiaIME::CreateInstance(_In_ IUnknown *pUnkOuter, REFIID riid, _Outptr_ void **ppvObj)
{
    CMetasequoiaIME *pMetasequoiaIME = nullptr;
    HRESULT hr = S_OK;

    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (nullptr != pUnkOuter)
    {
        return CLASS_E_NOAGGREGATION;
    }

    pMetasequoiaIME = new (std::nothrow) CMetasequoiaIME();
    if (pMetasequoiaIME == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    hr = pMetasequoiaIME->QueryInterface(riid, ppvObj);

    pMetasequoiaIME->Release();

    return hr;
}

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CMetasequoiaIME::CMetasequoiaIME()
{
    DllAddRef();

    _pThreadMgr = nullptr;

    _threadMgrEventSinkCookie = TF_INVALID_COOKIE;

    _pTextEditSinkContext = nullptr;
    _textEditSinkCookie = TF_INVALID_COOKIE;

    _activeLanguageProfileNotifySinkCookie = TF_INVALID_COOKIE;

    _dwThreadFocusSinkCookie = TF_INVALID_COOKIE;

    _pComposition = nullptr;

    _pCompositionProcessorEngine = nullptr;

    _candidateMode = CANDIDATE_NONE;
    _pCandidateListUIPresenter = nullptr;
    _isCandidateWithWildcard = FALSE;

    _pDocMgrLastFocused = nullptr;

    _pSIPIMEOnOffCompartment = nullptr;
    _dwSIPIMEOnOffCompartmentSinkCookie = 0;
    _msgWndHandle = nullptr;
    _themeRegKey = nullptr;
    _themeRegEvent = nullptr;
    _pThemeWatcherThread = nullptr;
    _stopThemeWatcher = false;

    _pContext = nullptr;

    _refCount = 1;
    _pITfFnSearchCandidateProvider = nullptr;

    _msgWndHandle = nullptr;
    _pIpcThread = nullptr;
    _hToTsfWorkerThreadPipe.store(nullptr);
    _workerPipeGeneration.store(0);
    _ipcStopEvent = nullptr;
    _shouldStopIpcThread = false;
    _ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
    _ipcConsecutiveFailures = 0;
    _workerCommitReady.store(false);
    _expectedWorkerFocusToken.store(0);
    _acknowledgedWorkerFocusToken.store(0);
    _compositionEpoch.store(1);
    _voiceCompositionAssemble.clear();
    _voiceCompositionAssembleMsg = 0;
    _voiceCompositionAssembleGeneration = 0;
    _voiceCompositionAssembleActive = false;
    _voiceCompositionActive = false;
    _localSessionResetPending.store(false);
    _localSessionResetToken.store(0);
    _localResetEditSessionQueued = false;
    _queuedLocalResetToken = 0;
    _focusResetPending = false;
    _activationRequired = false;
    _focusLostToWindowsTextInputHost = false;
    _focusLossDeferPending = false;
    _hasPendingServerCandidate = false;
    _pendingServerCandidateMsgType = Global::DataFromServerMsgType::OutofRange;
    _hasDeferredKeyInFlight = false;
    _deferredKeyReplayToken = 0;
    _nextDeferredKeyReplayToken = 0;
    _deferredKeyProjectionValid = false;
    _deferredProjectedImeOpen = false;
    _deferredProjectedPunctuationOpen = false;
    _deferredProjectedDoubleSingleByteOpen = false;
    _deferredProjectedInputLength = 0;
    _deferredProjectedRawInput.clear();
    _deferredProjectedCaret = 0;
    _deferredProjectedCandidateActive = false;
    _deferredProjectedUnicodeMode = false;
    _deferredKeyFocusGeneration = 1;
    _deferredKeyDrainPosted = false;
    _serverUnavailableFallbackActive = false;
    _shiftHotkeyArmed = false;
    _ctrlHotkeyArmed = false;
    _modifierHotkeyExpire = {};
    _bareShiftHook = nullptr;
    _bareShiftDownMask = 0;
    _bareShiftArmed = false;
    _bareShiftSequence = 0;
    _bareShiftHandledSequence = 0;
    _bareShiftFocusGeneration = 0;
    _bareShiftExpireTick = 0;
}

thread_local CMetasequoiaIME *CMetasequoiaIME::_bareShiftHookOwner = nullptr;

//+---------------------------------------------------------------------------
//
// dtor
//
//----------------------------------------------------------------------------

CMetasequoiaIME::~CMetasequoiaIME()
{
    _UninitBareShiftKeyboardHook();
    _ClearDeferredKeyDowns();
    // Deactivate normally owns the unbind.  Keep this owner-aware fallback for
    // partial activation failures without allowing a delayed old service
    // destructor to clear a newer service's TLS bindings.
    UnbindNamedpipeFocusState(this);
    if (_pCandidateListUIPresenter)
    {
        delete _pCandidateListUIPresenter;
        _pCandidateListUIPresenter = nullptr;
    }
    _DrainPendingCandidatePresenterCleanup();
    DllRelease();

    /* 处理线程的清理 */
    if (_pIpcThread)
    {
        _shouldStopIpcThread.store(true);
        if (_ipcStopEvent)
        {
            SetEvent(_ipcStopEvent);
        }
        HANDLE workerPipe = _hToTsfWorkerThreadPipe.load();
        if (workerPipe && workerPipe != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(workerPipe, nullptr);
        }
        if (_pIpcThread->joinable())
        {
            _pIpcThread->join();
        }
        delete _pIpcThread;
        _pIpcThread = nullptr;
    }
    if (_ipcStopEvent)
    {
        CloseHandle(_ipcStopEvent);
        _ipcStopEvent = nullptr;
    }
}

HRESULT CMetasequoiaIME::_RequestDeferredApplicationTextEditSession(_In_ ITfContext *pContext, WCHAR wch,
                                                                    uint64_t expectedFocusToken,
                                                                    uint64_t expectedFocusGeneration,
                                                                    uint64_t deferredReplayToken)
{
    if (pContext == nullptr || wch == L'\0' || deferredReplayToken == 0)
    {
        _CompleteDeferredKeyReplay(deferredReplayToken);
        return E_INVALIDARG;
    }

    CDeferredApplicationTextEditSession *editSession = new (std::nothrow) CDeferredApplicationTextEditSession(
        this, pContext, wch, expectedFocusToken, expectedFocusGeneration, deferredReplayToken);
    if (editSession == nullptr)
    {
        _RetryDeferredKeyReplay(deferredReplayToken);
        return E_OUTOFMEMORY;
    }

    HRESULT editSessionHr = E_FAIL;
    const HRESULT requestHr =
        pContext->RequestEditSession(_tfClientId, editSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &editSessionHr);
    editSession->Release();
    if (FAILED(requestHr) || FAILED(editSessionHr))
    {
        _RetryDeferredKeyReplay(deferredReplayToken);
    }
    return FAILED(requestHr) ? requestHr : editSessionHr;
}

HRESULT CMetasequoiaIME::_RequestDirectPunctuationEditSession(_In_ ITfContext *pContext, UINT code, WCHAR wch,
                                                              uint64_t requestId, std::wstring prefetchedText,
                                                              uint64_t expectedFocusToken,
                                                              uint64_t expectedCompositionEpoch,
                                                              uint64_t deferredReplayToken)
{
    if (pContext == nullptr)
    {
        return E_INVALIDARG;
    }

    LARGE_INTEGER requestStartQpc = {};
    QueryPerformanceCounter(&requestStartQpc);

    CPunctuationCommitEditSession *pEditSession = new (std::nothrow) CPunctuationCommitEditSession(
        this, pContext, code, wch, requestId, requestStartQpc, std::move(prefetchedText),
        expectedFocusToken != 0 ? expectedFocusToken : _CaptureFocusSessionToken(),
        expectedCompositionEpoch != 0 ? expectedCompositionEpoch : _CaptureCompositionEpoch(), deferredReplayToken);
    if (pEditSession == nullptr)
    {
        _RetryDeferredKeyReplay(deferredReplayToken);
        return E_OUTOFMEMORY;
    }

    HRESULT editSessionHr = E_FAIL;
    PerfTimer requestTimer;
    HRESULT requestHr =
        pContext->RequestEditSession(_tfClientId, pEditSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &editSessionHr);
    pEditSession->Release();
    if (FAILED(requestHr) || FAILED(editSessionHr))
    {
        _RetryDeferredKeyReplay(deferredReplayToken);
    }
    return FAILED(requestHr) ? requestHr : editSessionHr;
}

void CMetasequoiaIME::_QueuePendingServerCandidate(UINT msgType, _In_z_ const WCHAR *pCandidateString)
{
    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    _hasPendingServerCandidate = true;
    _pendingServerCandidateMsgType = msgType;
    _pendingServerCandidateString = pCandidateString ? pCandidateString : L"";
}

bool CMetasequoiaIME::_TakePendingServerCandidate(_Out_ UINT *pMsgType, _Out_ std::wstring *pCandidateString)
{
    if (pMsgType == nullptr || pCandidateString == nullptr)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    if (!_hasPendingServerCandidate)
    {
        return false;
    }

    *pMsgType = _pendingServerCandidateMsgType;
    pCandidateString->swap(_pendingServerCandidateString);
    _hasPendingServerCandidate = false;
    _pendingServerCandidateMsgType = Global::DataFromServerMsgType::OutofRange;
    return true;
}

bool CMetasequoiaIME::_PostAsyncKeyRequest(UINT message, UINT code, WCHAR wch, uint64_t requestId,
                                           std::wstring prefetchedText, uint64_t expectedFocusToken,
                                           uint64_t expectedCompositionEpoch, uint64_t deferredReplayToken)
{
    switch (message)
    {
    case WM_AsyncServerCandidateKey:
    case WM_AsyncFinalizeCandidate:
    case WM_AsyncPunctuationCommit:
    case WM_AsyncNumberCandidateCommit:
        break;
    default:
        _RetryDeferredKeyReplay(deferredReplayToken);
        return false;
    }

    constexpr size_t maxPendingAsyncKeys = 64;
    const uint64_t focusToken = expectedFocusToken != 0 ? expectedFocusToken : _CaptureFocusSessionToken();
    const uint64_t compositionEpoch =
        expectedCompositionEpoch != 0 ? expectedCompositionEpoch : _CaptureCompositionEpoch();
    const HWND ownerWindow = _msgWndHandle;
    if (focusToken == 0 || !ownerWindow || !IsWindow(ownerWindow))
    {
        if (deferredReplayToken != 0)
        {
            _RetryDeferredKeyReplay(deferredReplayToken);
        }
        else
        {
            MarkNamedpipeSessionDirtyForOwner(this);
        }
        return false;
    }

    UINT token = 0;
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        if (_pendingAsyncKeyMessages.size() < maxPendingAsyncKeys)
        {
            do
            {
                token = NextWindowMessageToken();
            } while (token == 0 || _pendingAsyncKeyMessages.count(token) != 0);
            _pendingAsyncKeyMessages.emplace(token, AsyncKeyRequest{message, code, wch, requestId, focusToken,
                                                                    compositionEpoch, deferredReplayToken,
                                                                    std::move(prefetchedText)});
        }
    }
    if (token == 0)
    {
        if (deferredReplayToken != 0)
        {
            _RetryDeferredKeyReplay(deferredReplayToken);
        }
        else
        {
            MarkNamedpipeSessionDirtyForOwner(this);
        }
        return false;
    }
    if (!PostMessage(ownerWindow, message, static_cast<WPARAM>(token), 0))
    {
        {
            std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
            _pendingAsyncKeyMessages.erase(token);
        }
        if (deferredReplayToken != 0)
        {
            _RetryDeferredKeyReplay(deferredReplayToken);
        }
        else
        {
            MarkNamedpipeSessionDirtyForOwner(this);
        }
        return false;
    }
    return true;
}

bool CMetasequoiaIME::_PostServerCandidateCommit(_In_z_ const WCHAR *candidateText)
{
    return _PostServerTextDelivery(WM_CommitCandidate, candidateText);
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
        PostMessage(ownerWindow, WM_IpcSessionDirty, 0, 0);
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

bool CMetasequoiaIME::_TakeAsyncKeyRequest(UINT message, UINT token, _Out_ AsyncKeyRequest &request)
{
    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    const auto entry = _pendingAsyncKeyMessages.find(token);
    if (entry == _pendingAsyncKeyMessages.end() || entry->second.message != message)
    {
        return false;
    }
    request = std::move(entry->second);
    _pendingAsyncKeyMessages.erase(entry);
    return true;
}

void CMetasequoiaIME::_ClearAsyncKeyRequests()
{
    std::vector<uint64_t> deferredReplayTokens;
    {
        std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
        for (const auto &entry : _pendingAsyncKeyMessages)
        {
            if (entry.second.deferredReplayToken != 0)
            {
                deferredReplayTokens.push_back(entry.second.deferredReplayToken);
            }
        }
        _pendingAsyncKeyMessages.clear();
    }
    for (uint64_t replayToken : deferredReplayTokens)
    {
        _RetryDeferredKeyReplay(replayToken);
    }
}

void CMetasequoiaIME::_ClearPendingIpcRequests()
{
    _ClearAsyncKeyRequests();
    ResetNamedpipeReplyState();

    std::lock_guard<std::mutex> lock(_pendingCommitCandidateMutex);
    _pendingServerCommitMessages.clear();
    _pendingWorkerSwitchMessages.clear();
    _pendingServerCandidateString.clear();
    _hasPendingServerCandidate = false;
    _pendingServerCandidateMsgType = Global::DataFromServerMsgType::OutofRange;
}

uint64_t CMetasequoiaIME::_CaptureFocusSessionToken() const
{
    return _expectedWorkerFocusToken.load(std::memory_order_acquire);
}

bool CMetasequoiaIME::_IsFocusSessionCurrent(uint64_t focusToken, _In_opt_ ITfContext *expectedContext) const
{
    if (focusToken == 0 || !Global::g_connected ||
        _expectedWorkerFocusToken.load(std::memory_order_acquire) != focusToken ||
        _acknowledgedWorkerFocusToken.load(std::memory_order_acquire) != focusToken ||
        !_workerCommitReady.load(std::memory_order_acquire) ||
        _localSessionResetPending.load(std::memory_order_acquire))
    {
        return false;
    }
    if (expectedContext == nullptr)
    {
        return true;
    }
    if (_pThreadMgr == nullptr)
    {
        return false;
    }

    ITfDocumentMgr *documentMgr = nullptr;
    ITfContext *topContext = nullptr;
    bool matches = false;
    if (SUCCEEDED(_pThreadMgr->GetFocus(&documentMgr)) && documentMgr)
    {
        if (SUCCEEDED(documentMgr->GetTop(&topContext)) && topContext)
        {
            matches = topContext == expectedContext;
            topContext->Release();
        }
        documentMgr->Release();
    }
    return matches;
}

uint64_t CMetasequoiaIME::_CaptureCompositionEpoch() const
{
    return _compositionEpoch.load(std::memory_order_acquire);
}

bool CMetasequoiaIME::_IsCompositionEpochCurrent(uint64_t compositionEpoch) const
{
    return compositionEpoch != 0 && _compositionEpoch.load(std::memory_order_acquire) == compositionEpoch;
}

bool CMetasequoiaIME::_IsCompositionCurrent(_In_opt_ ITfComposition *expectedComposition) const
{
    return expectedComposition != nullptr && _pComposition == expectedComposition;
}

bool CMetasequoiaIME::_IsLocalSessionResetCurrent(UINT resetToken) const
{
    return resetToken != 0 && _localResetEditSessionQueued && _queuedLocalResetToken == resetToken &&
           _localSessionResetPending.load(std::memory_order_acquire) &&
           _localSessionResetToken.load(std::memory_order_acquire) == resetToken;
}

void CMetasequoiaIME::_CompleteLocalSessionReset(UINT resetToken)
{
    if (!_localResetEditSessionQueued || _queuedLocalResetToken != resetToken)
    {
        return;
    }
    _localResetEditSessionQueued = false;
    _queuedLocalResetToken = 0;

    const UINT currentToken = _localSessionResetToken.load(std::memory_order_acquire);
    if (_localSessionResetPending.load(std::memory_order_acquire) && currentToken != resetToken)
    {
        // A focus loss superseded an already-granted dirty reset.  The old
        // cancel has finished; process the exact newer token before reopening
        // the key path.
        _RequestLocalSessionReset(nullptr, currentToken);
        return;
    }
    if (currentToken == resetToken && !_IsComposing() && _pCandidateListUIPresenter == nullptr)
    {
        _localSessionResetPending.store(false, std::memory_order_release);
    }
    else if (currentToken == resetToken && _localSessionResetPending.load(std::memory_order_acquire))
    {
        // A granted edit session can still fail inside the key-state handler.
        // Do not reopen the transport while the old local composition or
        // presenter survives; retire this queue slot and retry the exact token.
        if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
        {
            _ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
            SetTimer(_msgWndHandle, TIMER_CONNECT_ALL_NAMEDPIPE, _ipcReconnectDelayMs, nullptr);
        }
        return;
    }
    if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
    {
        PostMessage(_msgWndHandle, WM_IpcReconnect, 0, 0);
        _ScheduleDeferredKeyDownDrain();
    }
}

void CMetasequoiaIME::_RequestLocalSessionReset(_In_opt_ ITfContext *preferredContext, UINT resetToken)
{
    if (resetToken == 0 || _localSessionResetToken.load(std::memory_order_acquire) != resetToken ||
        !_localSessionResetPending.load(std::memory_order_acquire))
    {
        return;
    }
    _workerCommitReady.store(false, std::memory_order_release);
    _ClearPendingIpcRequests();

    if (_localResetEditSessionQueued)
    {
        return;
    }

    const bool needsCancel = _IsComposing() || _pCandidateListUIPresenter != nullptr;
    if (!needsCancel)
    {
        if (_localSessionResetToken.load(std::memory_order_acquire) == resetToken)
        {
            _localSessionResetPending.store(false, std::memory_order_release);
        }
        if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
        {
            PostMessage(_msgWndHandle, WM_IpcReconnect, 0, 0);
            _ScheduleDeferredKeyDownDrain();
        }
        return;
    }

    ITfContext *resetContext = preferredContext;
    if (resetContext)
    {
        resetContext->AddRef();
    }
    else if (_pContext)
    {
        resetContext = _pContext;
        resetContext->AddRef();
    }
    else if (_pThreadMgr)
    {
        ITfDocumentMgr *documentMgr = nullptr;
        if (SUCCEEDED(_pThreadMgr->GetFocus(&documentMgr)) && documentMgr)
        {
            documentMgr->GetTop(&resetContext);
            documentMgr->Release();
        }
    }

    if (resetContext)
    {
        _KEYSTROKE_STATE keyState = {};
        keyState.Category = CATEGORY_COMPOSING;
        keyState.Function = FUNCTION_CANCEL;
        _localResetEditSessionQueued = true;
        _queuedLocalResetToken = resetToken;
        const HRESULT resetRequestHr =
            _InvokeKeyHandler(resetContext, 0, L'\0', 0, keyState, FANY_IME_NO_REQUEST_ID, {}, resetToken);
        if (FAILED(resetRequestHr) && _localResetEditSessionQueued && _queuedLocalResetToken == resetToken)
        {
            // The edit session was not accepted and therefore cannot retire
            // its queue slot. Keep the exact reset gate closed and retry from
            // the reconnect timer; reopening IPC here could bind the old
            // local composition to a new server focus token.
            _localResetEditSessionQueued = false;
            _queuedLocalResetToken = 0;
            if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
            {
                _ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
                SetTimer(_msgWndHandle, TIMER_CONNECT_ALL_NAMEDPIPE, _ipcReconnectDelayMs, nullptr);
            }
        }
        resetContext->Release();
        return;
    }

    // No context remains capable of granting an edit session. At least tear
    // down the local presenter so a stale candidate cannot commit later.
    _DeleteCandidateList(TRUE, nullptr);
    if (_localSessionResetToken.load(std::memory_order_acquire) == resetToken)
    {
        _localSessionResetPending.store(false, std::memory_order_release);
    }
    if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
    {
        PostMessage(_msgWndHandle, WM_IpcReconnect, 0, 0);
        _ScheduleDeferredKeyDownDrain();
    }
}

void CMetasequoiaIME::_ScheduleCandidatePresenterCleanup(_In_ CCandidateListUIPresenter *pPresenter)
{
    if (pPresenter == nullptr)
    {
        return;
    }

    pPresenter->_PrepareForAsyncCleanup();
    _pendingCandidatePresenterCleanup.push_back(pPresenter);

    if (_msgWndHandle)
    {
        PostMessage(_msgWndHandle, WM_CleanupCandidatePresenter, 0, 0);
    }
}

void CMetasequoiaIME::_DrainPendingCandidatePresenterCleanup()
{
    PerfTimer timer;
    size_t cleanedCount = 0;
    while (!_pendingCandidatePresenterCleanup.empty())
    {
        CCandidateListUIPresenter *pPresenter = _pendingCandidatePresenterCleanup.front();
        _pendingCandidatePresenterCleanup.pop_front();
        if (pPresenter)
        {
            PerfTimer deleteTimer;
            delete pPresenter;
            ++cleanedCount;
        }
    }
}

//+---------------------------------------------------------------------------
//
// QueryInterface
//
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfTextInputProcessor))
    {
        *ppvObj = (ITfTextInputProcessor *)this;
    }
    else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    {
        *ppvObj = (ITfTextInputProcessorEx *)this;
    }
    else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    {
        *ppvObj = (ITfThreadMgrEventSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfTextEditSink))
    {
        *ppvObj = (ITfTextEditSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    {
        *ppvObj = (ITfKeyEventSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfActiveLanguageProfileNotifySink))
    {
        *ppvObj = (ITfActiveLanguageProfileNotifySink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfCompositionSink))
    {
        *ppvObj = (ITfCompositionSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider))
    {
        *ppvObj = (ITfDisplayAttributeProvider *)this;
    }
    else if (IsEqualIID(riid, IID_ITfThreadFocusSink))
    {
        *ppvObj = (ITfThreadFocusSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfFunctionProvider))
    {
        *ppvObj = (ITfFunctionProvider *)this;
    }
    else if (IsEqualIID(riid, IID_ITfFunction))
    {
        *ppvObj = (ITfFunction *)this;
    }
    else if (IsEqualIID(riid, IID_ITfFnGetPreferredTouchKeyboardLayout))
    {
        *ppvObj = (ITfFnGetPreferredTouchKeyboardLayout *)this;
    }

    if (*ppvObj)
    {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

//+---------------------------------------------------------------------------
//
// AddRef
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CMetasequoiaIME::AddRef()
{
    return ++_refCount;
}

//+---------------------------------------------------------------------------
//
// Release
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CMetasequoiaIME::Release()
{
    LONG cr = --_refCount;

    assert(_refCount >= 0);

    if (_refCount == 0)
    {
        delete this;
    }

    return cr;
}

//+---------------------------------------------------------------------------
//
// ITfTextInputProcessorEx::ActivateEx
//
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::ActivateEx(ITfThreadMgr *pThreadMgr, TfClientId tfClientId, DWORD dwFlags)
{
    _pThreadMgr = pThreadMgr;
    _pThreadMgr->AddRef();

    _tfClientId = tfClientId;
    _dwActivateFlags = dwFlags;
    Global::HostUiLessMode = _IsUiLessMode() ? true : false;
    Global::CandidateUiLessMode = false;
    // Match Weasel's activation-time recovery: switching to this TIP is an
    // explicit user request, so revive a missing Server immediately. Merely
    // focusing another text box still uses reconnect-only behavior.
    _WakeServerIfNeeded();
    _focusResetPending = false;
    _activationRequired = false;
    _focusLossDeferPending = false;
    _workerCommitReady.store(false, std::memory_order_release);
    _acknowledgedWorkerFocusToken.store(0, std::memory_order_release);
    _localSessionResetPending.store(false, std::memory_order_release);
    _localResetEditSessionQueued = false;
    _queuedLocalResetToken = 0;
    BindNamedpipeFocusState(this, &_focusResetPending, &_activationRequired, &_expectedWorkerFocusToken,
                            &_localSessionResetPending, &_localSessionResetToken, &_workerCommitReady,
                            &_acknowledgedWorkerFocusToken, &_hToTsfWorkerThreadPipe, &_workerPipeGeneration);

    /*
    std::wstring processName = FanyUtils::GetCurrentProcessName();
    if (Global::VSCodeSeries.find(processName) != Global::VSCodeSeries.end())
    {
        Global::IsVSCodeLike = true;
    }
    */
    // Set up IPC(named pipe)
    // InitIpc();
    // TODO: 去掉共享内存，只保留命名管道
    // InitNamedpipe();

    Global::current_process_name = GetCurrentProcessName();

    if (!_InitThreadMgrEventSink())
    {
        goto ExitError;
    }

    // Register generic window class for message-only window
    {
        WNDCLASSEX wcex = {};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.lpfnWndProc = CMetasequoiaIME_WindowProc;
        wcex.hInstance = Global::dllInstanceHandle;
        wcex.lpszClassName = L"MetasequoiaIMEWorkerWnd";
        wcex.cbWndExtra = sizeof(LONG_PTR);
        RegisterClassEx(&wcex);
    }

    _msgWndHandle = CreateWindowEx( //
        0,                          //
        L"MetasequoiaIMEWorkerWnd", //
        L"MetasequoiaIMEWorkerWnd", //
        0, 0, 0, 0, 0,              //
        HWND_MESSAGE,               //
        nullptr,                    //
        Global::dllInstanceHandle,  //
        this);
    if (!_msgWndHandle)
    {
        goto ExitError;
    }
    SetWindowLongPtr(_msgWndHandle, GWLP_USERDATA, (LONG_PTR)this);
    Global::msgWndHandle = _msgWndHandle;
    _StartThemeRegistryWatcher();

    BOOL hasThreadFocus = FALSE;
    const HRESULT threadFocusResult = _pThreadMgr->IsThreadFocus(&hasThreadFocus);
    // Preserve the historical eager-connect behavior if a host cannot report
    // focus. Otherwise an already-focused activation could remain offline
    // forever because no later focus callback is guaranteed.
    Global::g_connected = SUCCEEDED(threadFocusResult) ? hasThreadFocus != FALSE : true;
    if (Global::g_connected)
    {
        RequireNamedpipeFocusActivation();
    }

    /* 创建 IPC 线程 */
    _ipcStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!_ipcStopEvent)
    {
        goto ExitError;
    }
    _shouldStopIpcThread.store(false);
    _pIpcThread = new std::thread(IpcWorkerThread, this);

    if (Global::g_connected)
    {
        PostMessage(_msgWndHandle, WM_ConnectNamedpipe, 0, 0);
    }

    ITfDocumentMgr *pDocMgrFocus = nullptr;
    if (SUCCEEDED(_pThreadMgr->GetFocus(&pDocMgrFocus)) && (pDocMgrFocus != nullptr))
    {
        _InitTextEditSink(pDocMgrFocus);
        pDocMgrFocus->Release();
    }

    if (!_InitKeyEventSink())
    {
        goto ExitError;
    }

    if (!_InitActiveLanguageProfileNotifySink())
    {
        goto ExitError;
    }

    if (!_InitThreadFocusSink())
    {
        goto ExitError;
    }

    if (!_InitDisplayAttributeGuidAtom())
    {
        goto ExitError;
    }

    if (!_InitFunctionProviderSink())
    {
        goto ExitError;
    }

    if (!_AddTextProcessorEngine())
    {
        goto ExitError;
    }

    Global::CapsLockEnabled.store((GetKeyState(VK_CAPITAL) & 0x0001) != 0, std::memory_order_relaxed);

    // Apply configured default CN/EN whenever switching back to this IME
    _pCompositionProcessorEngine->InitializeMetasequoiaIMECompartment(pThreadMgr, tfClientId);
    _InitBareShiftKeyboardHook();

    // The first connect timer can run before the engine exists. Re-arm after
    // compartment initialization so an already-focused activation always
    // replays both ownership and a complete status snapshot.
    if (Global::g_connected)
    {
        PostMessage(_msgWndHandle, WM_ThreadFocus, 0, 0);
    }

    // TSF lifecycle is published on the epoch-checked Main pipe. The Server
    // uses terminal activation/deactivation to reconcile floating-toolbar
    // visibility and status snapshots to update its displayed mode.

    return S_OK;

ExitError:
    Deactivate();
    return E_FAIL;
}

//+---------------------------------------------------------------------------
//
// ITfTextInputProcessorEx::Deactivate
//
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::Deactivate()
{
    _UninitBareShiftKeyboardHook();
    Global::HostUiLessMode = false;
    Global::CandidateUiLessMode = false;
    // Send this synchronously before destroying the message window. OnKill can
    // queue the same lifecycle event, but that queued message may never run
    // during a rapid TIP deactivation; the server treats duplicates as idempotent.
    const uint64_t deactivatedFocusToken = _expectedWorkerFocusToken.load(std::memory_order_acquire);
    Global::g_connected = false;
    _workerCommitReady.store(false, std::memory_order_release);
    BeginNamedpipeLocalSessionReset(); // invalidate queued dirty-reset messages
    _localSessionResetPending.store(false, std::memory_order_release);
    _localResetEditSessionQueued = false;
    _queuedLocalResetToken = 0;
    _ClearDeferredKeyDowns();
    MarkNamedpipeFocusLost();
    FlushNamedpipeImeDeactivation(deactivatedFocusToken);
    _ClearPendingIpcRequests();

    // 注销此输入法时，向 server 端发送一个注销的消息
    // 注销不要给消息窗口发送消息，而是直接在这里处理
    // SendIMEDeactivationEventToUIProcessViaNamedPipe();

    // ClientDeactivated closes the Server-side input session and hides the
    // toolbar because the TIP itself is going away. A temporary thread-focus
    // loss uses ClientSuspended instead and deliberately keeps it visible.

    /* 清理 IPC 线程 */
    _shouldStopIpcThread.store(true);
    if (_ipcStopEvent)
    {
        SetEvent(_ipcStopEvent);
    }
    HANDLE workerPipe = _hToTsfWorkerThreadPipe.load();
    if (workerPipe && workerPipe != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(workerPipe, nullptr);
    }
    if (_pIpcThread && _pIpcThread->joinable())
    {
        _pIpcThread->join();
        delete _pIpcThread;
        _pIpcThread = nullptr;
    }
    _hToTsfWorkerThreadPipe.store(nullptr);
    InvalidateNamedpipeWorkerGeneration();
    // The worker no longer has an outstanding OVERLAPPED operation, so the
    // UI thread can now close all three handles safely.
    CloseIpc();
    if (_ipcStopEvent)
    {
        CloseHandle(_ipcStopEvent);
        _ipcStopEvent = nullptr;
    }
    // Stop callbacks from the previously focused top context before tearing
    // down the engine and thread manager. Context stack changes can otherwise
    // re-enter a half-deactivated service.
    _InitTextEditSink(nullptr);
    // TODO: 去掉共享内存，只保留命名管道
    // CloseNamedpipe();

    ITfContext *pContext = _pContext;
    if (_pContext)
    {
        pContext->AddRef();
        _EndComposition(_pContext, _pComposition, true);
    }

    if (_pCandidateListUIPresenter)
    {
        delete _pCandidateListUIPresenter;
        _pCandidateListUIPresenter = nullptr;

        _candidateMode = CANDIDATE_NONE;
        _isCandidateWithWildcard = FALSE;
    }

    if (pContext)
    {
        pContext->Release();
    }

    _DrainPendingCandidatePresenterCleanup();

    if (_pCompositionProcessorEngine)
    {
        delete _pCompositionProcessorEngine;
        _pCompositionProcessorEngine = nullptr;
    }

    _UninitFunctionProviderSink();

    _UninitThreadFocusSink();

    _UninitActiveLanguageProfileNotifySink();

    _UninitKeyEventSink();

    _UninitThreadMgrEventSink();

    CCompartment CompartmentKeyboardOpen(_pThreadMgr, _tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    CompartmentKeyboardOpen._ClearCompartment();

    CCompartment CompartmentDoubleSingleByte(_pThreadMgr, _tfClientId,
                                             Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    CompartmentDoubleSingleByte._ClearCompartment();

    CCompartment CompartmentPunctuation(_pThreadMgr, _tfClientId, Global::MetasequoiaIMEGuidCompartmentPunctuation);
    CompartmentPunctuation._ClearCompartment();

    if (_pThreadMgr != nullptr)
    {
        _pThreadMgr->Release();
        _pThreadMgr = nullptr;
    }

    _tfClientId = TF_CLIENTID_NULL;

    if (_pDocMgrLastFocused)
    {
        _pDocMgrLastFocused->Release();
        _pDocMgrLastFocused = nullptr;
    }

    /* 清理消息窗口 */
    _StopThemeRegistryWatcher();
    if (_msgWndHandle)
    {
        KillTimer(_msgWndHandle, TIMER_CONNECT_ALL_NAMEDPIPE);
        KillTimer(_msgWndHandle, TIMER_CONNECT_TO_TSF_NAMEDPIPE);
        KillTimer(_msgWndHandle, TIMER_REFRESH_LANG_BAR_THEME);
        KillTimer(_msgWndHandle, TIMER_DEFERRED_FOCUS_LOSS);
        KillTimer(_msgWndHandle, TIMER_FOCUS_STATUS_RESEND);
        DestroyWindow(_msgWndHandle);
        if (Global::msgWndHandle == _msgWndHandle)
        {
            Global::msgWndHandle = nullptr;
        }
        _msgWndHandle = nullptr;
    }
    UnregisterClass(L"MetasequoiaIMEWorkerWnd", Global::dllInstanceHandle);

    UnbindNamedpipeFocusState(this);
    _focusResetPending = false;
    _activationRequired = false;
    _focusLossDeferPending = false;

    return S_OK;
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
            if (pIME->_ipcStopEvent && WaitForSingleObject(pIME->_ipcStopEvent, 50) == WAIT_OBJECT_0)
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
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PairedPunctuationChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::MicrosoftShuangpinChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::InputModeChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CapsLockChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::TsfDiagnosticLogChanged ||
             buf.msg_type == Global::DataToTsfWorkerThreadMsgType::LowercaseNumberModeChanged))
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
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PairedPunctuationChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::MicrosoftShuangpinChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::InputModeChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CapsLockChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::TsfDiagnosticLogChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PunctuationLockChanged ||
                buf.msg_type == Global::DataToTsfWorkerThreadMsgType::LowercaseNumberModeChanged ||
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
            }
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::CommitCurCandidate)
        {
            if (pIME->_workerCommitReady.load(std::memory_order_acquire))
            {
                pIME->_PostServerCandidateCommit(buf.data);
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
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::PairedPunctuationChanged)
        {
            Global::PairedPunctuationEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::MicrosoftShuangpinChanged)
        {
            Global::MicrosoftShuangpinEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::LowercaseNumberModeChanged)
        {
            Global::LowercaseNumberModeEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
        }
        else if (buf.msg_type == Global::DataToTsfWorkerThreadMsgType::InputModeChanged)
        {
            Global::JapaneseInputModeEnabled.store(buf.data[0] == L'1', std::memory_order_relaxed);
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

//+---------------------------------------------------------------------------
//
// Theme registry watcher / language bar icon refresh
//
//----------------------------------------------------------------------------

void CMetasequoiaIME::_RefreshLanguageBarThemeIcons()
{
    CCompositionProcessorEngine *pEngine = GetCompositionProcessorEngine();
    if (pEngine)
    {
        pEngine->RefreshLanguageBarIcons();
    }
}

void CMetasequoiaIME::_RequestLanguageBarCapsIconRefresh()
{
    if (_msgWndHandle && IsWindow(_msgWndHandle))
    {
        PostMessage(_msgWndHandle, WM_RefreshLanguageBarTheme, 1, 0);
    }
}

void CMetasequoiaIME::_StartThemeRegistryWatcher()
{
    if (_pThemeWatcherThread)
    {
        return;
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_NOTIFY, &_themeRegKey) != ERROR_SUCCESS)
    {
        return;
    }

    _themeRegEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!_themeRegEvent)
    {
        RegCloseKey(_themeRegKey);
        _themeRegKey = nullptr;
        return;
    }

    if (RegNotifyChangeKeyValue(_themeRegKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, _themeRegEvent, TRUE) != ERROR_SUCCESS)
    {
        CloseHandle(_themeRegEvent);
        _themeRegEvent = nullptr;
        RegCloseKey(_themeRegKey);
        _themeRegKey = nullptr;
        return;
    }

    _stopThemeWatcher.store(false);
    _pThemeWatcherThread = new (std::nothrow) std::thread([this]() {
        while (!_stopThemeWatcher.load(std::memory_order_acquire))
        {
            const DWORD wait = WaitForSingleObject(_themeRegEvent, INFINITE);
            if (_stopThemeWatcher.load(std::memory_order_acquire))
            {
                break;
            }
            if (wait != WAIT_OBJECT_0)
            {
                continue;
            }
            if (_stopThemeWatcher.load(std::memory_order_acquire))
            {
                break;
            }

            ResetEvent(_themeRegEvent);
            RegNotifyChangeKeyValue(_themeRegKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, _themeRegEvent, TRUE);

            const HWND ownerWindow = _msgWndHandle;
            if (ownerWindow && IsWindow(ownerWindow))
            {
                PostMessage(ownerWindow, WM_RefreshLanguageBarTheme, 0, 0);
            }
        }
    });

    if (!_pThemeWatcherThread)
    {
        CloseHandle(_themeRegEvent);
        _themeRegEvent = nullptr;
        RegCloseKey(_themeRegKey);
        _themeRegKey = nullptr;
    }
}

void CMetasequoiaIME::_StopThemeRegistryWatcher()
{
    _stopThemeWatcher.store(true, std::memory_order_release);
    if (_themeRegEvent)
    {
        SetEvent(_themeRegEvent);
    }

    if (_pThemeWatcherThread)
    {
        if (_pThemeWatcherThread->joinable())
        {
            _pThemeWatcherThread->join();
        }
        delete _pThemeWatcherThread;
        _pThemeWatcherThread = nullptr;
    }

    if (_themeRegEvent)
    {
        CloseHandle(_themeRegEvent);
        _themeRegEvent = nullptr;
    }

    if (_themeRegKey)
    {
        RegCloseKey(_themeRegKey);
        _themeRegKey = nullptr;
    }

    _stopThemeWatcher.store(false, std::memory_order_release);
}

//+---------------------------------------------------------------------------
//
// _WakeServerIfNeeded
//
//----------------------------------------------------------------------------
void CMetasequoiaIME::_WakeServerIfNeeded()
{
    if (_IsSecureMode() || _IsComLess() || IsServerAlreadyRunning())
        return;
    if (!RequestServerLaunch())
        return;

    _ipcConsecutiveFailures = 0;
    if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
    {
        _ipcReconnectDelayMs = SERVER_LAUNCH_RECONNECT_DELAY_MS;
        SetTimer(_msgWndHandle, TIMER_CONNECT_ALL_NAMEDPIPE, _ipcReconnectDelayMs, nullptr);
    }
}

//+---------------------------------------------------------------------------
//
// _NoteKeyEventIpcFailure
//
//----------------------------------------------------------------------------
void CMetasequoiaIME::_NoteKeyEventIpcFailure()
{
    // Only a real keystroke counts as evidence that the user wants the Server
    // back. The background reconnect timer must never revive it on its own,
    // otherwise merely focusing a text box resurrects a deliberately killed
    // Server.
    if (++_ipcConsecutiveFailures < IPC_FAILURES_BEFORE_SERVER_LAUNCH)
    {
        return;
    }

    _ipcConsecutiveFailures = 0;
    if (IsServerAlreadyRunning())
    {
        // A live Server that has merely lost its focus session is recovered by
        // the ordinary reconnect timer. Do not disturb that schedule.
        return;
    }
    if (_IsSecureMode() || _IsComLess() || !RequestServerLaunch())
    {
        return;
    }
    if (Global::g_connected && _msgWndHandle && IsWindow(_msgWndHandle))
    {
        _ipcReconnectDelayMs = SERVER_LAUNCH_RECONNECT_DELAY_MS;
        SetTimer(_msgWndHandle, TIMER_CONNECT_ALL_NAMEDPIPE, _ipcReconnectDelayMs, nullptr);
    }
}

//+---------------------------------------------------------------------------
//
// CMetasequoiaIME_WindowProc
//
//----------------------------------------------------------------------------
LRESULT CALLBACK CMetasequoiaIME_WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    CMetasequoiaIME *pIME = (CMetasequoiaIME *)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (!pIME)
    {
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    switch (message)
    {
    case WM_BareShiftRelease:
        pIME->_HandleHookedBareShiftRelease(static_cast<UINT>(wParam));
        return 0;

    case WM_CheckGlobalCompartment: {
        CMetasequoiaIME::WorkerCompartmentSwitch request;
        if (!pIME->_TakeWorkerCompartmentSwitch(static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken))
        {
            // The Server already moved the toolbar to the value it asked for,
            // so a rejection here leaves the two indicators disagreeing.
            break;
        }

        if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToEnglish)
        {
            if (pIME->_IsComposing() || pIME->_pCandidateListUIPresenter != nullptr)
            {
                ITfDocumentMgr *documentMgr = nullptr;
                ITfContext *context = nullptr;
                if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&documentMgr)) && documentMgr)
                {
                    if (SUCCEEDED(documentMgr->GetTop(&context)) && context)
                    {
                        _KEYSTROKE_STATE keyState = {};
                        keyState.Category = CATEGORY_COMPOSING;
                        keyState.Function = FUNCTION_TOGGLE_IME_MODE;
                        if (FAILED(pIME->_InvokeKeyHandler(context, 0, L'\0', 0, keyState, FANY_IME_NO_REQUEST_ID, {},
                                                           0, request.compositionEpoch, request.focusToken)))
                        {
                            MarkNamedpipeSessionDirtyForOwner(pIME);
                        }
                        context->Release();
                    }
                    documentMgr->Release();
                }
            }
            pIME->GetCompositionProcessorEngine()->SetIMEMode(pIME->_GetThreadMgr(), pIME->_GetClientId(), FALSE);
        }
        else if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToChinese)
        {
            pIME->GetCompositionProcessorEngine()->SetIMEMode(pIME->_GetThreadMgr(), pIME->_GetClientId(), TRUE);
        }
        else if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToPuncEn)
        {
            pIME->GetCompositionProcessorEngine()->SetPunctuationMode(pIME->_GetThreadMgr(), pIME->_GetClientId(),
                                                                      FALSE);
        }
        else if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToPuncCn)
        {
            pIME->GetCompositionProcessorEngine()->SetPunctuationMode(pIME->_GetThreadMgr(), pIME->_GetClientId(),
                                                                      TRUE);
        }
        else if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToFullwidth)
        {
            pIME->GetCompositionProcessorEngine()->SetDoubleSingleByteMode(pIME->_GetThreadMgr(), pIME->_GetClientId(),
                                                                           TRUE);
        }
        else if (request.messageType == Global::DataToTsfWorkerThreadMsgType::SwitchToHalfwidth)
        {
            pIME->GetCompositionProcessorEngine()->SetDoubleSingleByteMode(pIME->_GetThreadMgr(), pIME->_GetClientId(),
                                                                           FALSE);
        }
        // Read all three compartments after applying the exact worker command.
        // Sending a complete ordered snapshot prevents two queued status WMs
        // from each observing/overwriting a different partial state.
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_ConnectNamedpipe: {
        if (Global::g_connected)
        {
            PostMessage(hWnd, WM_IpcReconnect, 0, 0);
        }
        break;
    }
    case WM_DisconnectNamedpipe: {
        if (Global::g_connected)
        {
            // A newer OnSetThreadFocus already superseded this queued kill.
            // Never let a stale focus-loss message deactivate the recovered
            // client (the Win+. round-trip can produce exactly this ordering).
            break;
        }
        KillTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE);
        KillTimer(hWnd, TIMER_CONNECT_TO_TSF_NAMEDPIPE);
        FlushNamedpipeFocusSessionReset();
        pIME->_ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
        pIME->_ipcConsecutiveFailures = 0;
        break;
    }
    case WM_ConnectToTsfNamedpipe: {
        SetTimer(hWnd, TIMER_CONNECT_TO_TSF_NAMEDPIPE, CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS, nullptr);
        break;
    }
    case WM_IpcWorkerDisconnected: {
        // The worker posts this only after its OVERLAPPED read has completed,
        // so it is now safe for the UI thread to close and recreate all pipes.
        const HANDLE disconnectedPipe = reinterpret_cast<HANDLE>(wParam);
        const UINT disconnectedGeneration = static_cast<UINT>(lParam);
        if (!disconnectedPipe || disconnectedGeneration == 0 ||
            disconnectedGeneration != pIME->_workerPipeGeneration.load(std::memory_order_acquire) ||
            disconnectedPipe != GetToTsfWorkerThreadNamedpipe() ||
            pIME->_hToTsfWorkerThreadPipe.load(std::memory_order_acquire) != nullptr)
        {
            // A delayed notification from an older registration must not tear
            // down handles opened by a newer reconnect attempt.
            break;
        }
        pIME->_workerCommitReady.store(false, std::memory_order_release);
        // Preserve the visible raw composition before the transport reset
        // cancels the local TSF text store.  If a key arrived after the reader
        // thread closed Ready but before this message ran, its queued item
        // remains behind this checkpoint.
        pIME->_ArmDeferredRecoveryForTransport(pIME->_pContext);
        MarkNamedpipeSessionDirtyForOwner(pIME);
        CloseNamedpipe();
        if (Global::g_connected)
        {
            pIME->_ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
            SetTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE, pIME->_ipcReconnectDelayMs, nullptr);
        }
        break;
    }
    case WM_IpcSessionDirty: {
        const UINT resetToken = static_cast<UINT>(wParam);
        if (resetToken == 0)
        {
            // Worker-thread callbacks cannot access the UI thread's TLS IPC
            // binding.  Let the owner thread allocate and post an exact reset
            // token here.
            MarkNamedpipeSessionDirtyForOwner(pIME);
        }
        else
        {
            pIME->_RequestLocalSessionReset(nullptr, resetToken);
        }
        break;
    }
    case WM_IpcReconnect: {
        // Main and reply-pipe failures invalidate only their own TLS handle.
        // Preserve a healthy worker read and let ConnectToAllNamedpipe reopen
        // just the missing channel(s).
        if (Global::g_connected)
        {
            const bool resetPending = pIME->_localSessionResetPending.load(std::memory_order_acquire);
            const bool activated = !resetPending && EnsureNamedpipeFocusSessionActivated();
            if (activated)
            {
                KillTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE);
                pIME->_ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
                pIME->_ipcConsecutiveFailures = 0;
                pIME->_TryLeaveServerUnavailableFallback();
                SendCurrentImeStatusSnapshot(pIME);
                pIME->_ScheduleDeferredKeyDownDrain();
                break;
            }
            pIME->_ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
            SetTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE, pIME->_ipcReconnectDelayMs, nullptr);
        }
        break;
    }
    case WM_TIMER: {
        if (wParam == TIMER_DEFERRED_FOCUS_LOSS)
        {
            KillTimer(hWnd, TIMER_DEFERRED_FOCUS_LOSS);
            pIME->_focusLossDeferPending = false;
            if (Global::g_connected)
            {
                // The document focus did not return within the deferral
                // window, so this is a real focus loss (app switch), not a
                // transient in-app route change (Excel cell-edit entry).
                MarkNamedpipeFocusLost();
                Global::g_connected = false;
                PostMessage(hWnd, WM_DisconnectNamedpipe, 0, 0);
            }
            break;
        }
        if (wParam == TIMER_FOCUS_STATUS_RESEND)
        {
            KillTimer(hWnd, TIMER_FOCUS_STATUS_RESEND);
            SendCurrentImeStatusSnapshot(pIME, true);
            break;
        }
        if (wParam == TIMER_CONNECT_ALL_NAMEDPIPE)
        {
            // 如果用户已经切换走了，就不用继续重试
            if (!Global::g_connected)
            {
                KillTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE);
                pIME->_ipcConsecutiveFailures = 0;
                break;
            }

            if (pIME->_localSessionResetPending.load(std::memory_order_acquire))
            {
                const UINT resetToken = pIME->_localSessionResetToken.load(std::memory_order_acquire);
                pIME->_RequestLocalSessionReset(nullptr, resetToken);
                if (pIME->_localSessionResetPending.load(std::memory_order_acquire))
                {
                    pIME->_ipcReconnectDelayMs =
                        min(pIME->_ipcReconnectDelayMs * 2, CONNECT_NAMEDPIPE_MAX_RETRY_INTERVAL_MS);
                    SetTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE, pIME->_ipcReconnectDelayMs, nullptr);
                    break;
                }
            }

            if (EnsureNamedpipeFocusSessionActivated())
            {
                KillTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE);
                pIME->_ipcReconnectDelayMs = CONNECT_NAMEDPIPE_RETRY_INTERVAL_MS;
                pIME->_ipcConsecutiveFailures = 0;
                SendCurrentImeStatusSnapshot(pIME);
                pIME->_ScheduleDeferredKeyDownDrain();
                break;
            }
            // Keep retrying while this TSF thread owns UI focus. The server
            // can be restarted independently of the host application.
            pIME->_ipcReconnectDelayMs = min(pIME->_ipcReconnectDelayMs * 2, CONNECT_NAMEDPIPE_MAX_RETRY_INTERVAL_MS);
            SetTimer(hWnd, TIMER_CONNECT_ALL_NAMEDPIPE, pIME->_ipcReconnectDelayMs, nullptr);
            break;
        }

        if (wParam == TIMER_CONNECT_TO_TSF_NAMEDPIPE)
        {
            if (ConnectToTsfNamedpipe())
            {
                KillTimer(hWnd, TIMER_CONNECT_TO_TSF_NAMEDPIPE);
                break;
            }
            break;
        }

        if (wParam == TIMER_REFRESH_LANG_BAR_THEME)
        {
            KillTimer(hWnd, TIMER_REFRESH_LANG_BAR_THEME);
            pIME->_RefreshLanguageBarThemeIcons();
            break;
        }
        break;
    }
    case WM_IMEActivation: {
        // Retained only for message-number compatibility.  Main-pipe lifecycle
        // messages no longer control floating-toolbar visibility.
        break;
    }
    case WM_ThreadFocus: {
        // Some Chromium/Electron window switches surface only as thread-focus
        // changes. Refresh this thread's cached TSF language-bar item even when
        // there was no document-focus transition.
        pIME->_RefreshLanguageBarThemeIcons();
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_DrainDeferredKeyDown: {
        pIME->_DrainOneDeferredKeyDown();
        break;
    }
    case WM_UpdateIMEStatus: {
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_UpdateDoubleSingleByte: {
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_UpdatePuncMode: {
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_CommitCandidate: {
        CMetasequoiaIME::WorkerCandidateCommit request;
        if (!pIME->_TakeServerCandidateCommit(static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            break;
        }
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;

        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                _KEYSTROKE_STATE KeystrokeState;
                KeystrokeState.Category = CATEGORY_CANDIDATE;
                KeystrokeState.Function = FUNCTION_FINALIZE_CANDIDATELIST;
                pIME->_InvokeKeyHandler(pContext, 0, 0, 0, KeystrokeState, FANY_IME_UNSOLICITED_REQUEST_ID,
                                        std::move(request.text), 0, request.compositionEpoch, request.focusToken);
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        break;
    }
    case WM_InsertText: {
        CMetasequoiaIME::WorkerCandidateCommit request;
        if (!pIME->_TakeServerCandidateCommit(static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            break;
        }
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;

        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                _KEYSTROKE_STATE KeystrokeState;
                KeystrokeState.Category = CATEGORY_COMPOSING;
                KeystrokeState.Function = FUNCTION_INSERT_TEXT;
                pIME->_InvokeKeyHandler(pContext, 0, 0, 0, KeystrokeState, FANY_IME_UNSOLICITED_REQUEST_ID,
                                        std::move(request.text), 0, request.compositionEpoch, request.focusToken);
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        break;
    }
    case WM_UpdateVoiceComposition:
        pIME->_DispatchUnsolicitedVoiceText(wParam, FUNCTION_UPDATE_VOICE_COMPOSITION);
        break;
    case WM_CommitVoiceComposition:
        pIME->_DispatchUnsolicitedVoiceText(wParam, FUNCTION_COMMIT_VOICE_COMPOSITION);
        break;
    case WM_CancelVoiceComposition:
        pIME->_DispatchUnsolicitedVoiceText(wParam, FUNCTION_CANCEL_VOICE_COMPOSITION);
        break;
    case WM_AsyncFinalizeCandidate: {
        CMetasequoiaIME::AsyncKeyRequest request;
        if (!pIME->_TakeAsyncKeyRequest(WM_AsyncFinalizeCandidate, static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            pIME->_RetryDeferredKeyReplay(request.deferredReplayToken);
            break;
        }
        PerfTimer timer;
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;

        bool handedOffReplay = false;
        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                _KEYSTROKE_STATE KeystrokeState;
                KeystrokeState.Category = CATEGORY_CANDIDATE;
                KeystrokeState.Function = FUNCTION_FINALIZE_CANDIDATELIST;
                pIME->_InvokeKeyHandler(pContext, request.code, request.wch, 0, KeystrokeState, request.requestId, {},
                                        0, request.compositionEpoch, request.focusToken, request.deferredReplayToken);
                handedOffReplay = true;
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        if (!handedOffReplay)
        {
            pIME->_RetryDeferredKeyReplay(request.deferredReplayToken);
        }
        break;
    }
    case WM_AsyncPunctuationCommit: {
        CMetasequoiaIME::AsyncKeyRequest request;
        if (!pIME->_TakeAsyncKeyRequest(WM_AsyncPunctuationCommit, static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            pIME->_RetryDeferredKeyReplay(request.deferredReplayToken);
            break;
        }
        PerfTimer timer;
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;
        const UINT code = request.code;
        const WCHAR wch = request.wch;

        bool handedOffReplay = false;
        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                const bool useDirectPunctuationSession =
                    pIME->_candidateMode == CANDIDATE_NONE && !pIME->_IsComposing();
                if (useDirectPunctuationSession)
                {
                    pIME->_RequestDirectPunctuationEditSession(pContext, code, wch, request.requestId,
                                                               std::move(request.prefetchedText), request.focusToken,
                                                               request.compositionEpoch, request.deferredReplayToken);
                    handedOffReplay = true;
                }
                else
                {
                    _KEYSTROKE_STATE KeystrokeState;
                    KeystrokeState.Category = CATEGORY_COMPOSING;
                    KeystrokeState.Function = FUNCTION_PUNCTUATION;
                    pIME->_InvokeKeyHandler(pContext, code, wch, 0, KeystrokeState, request.requestId,
                                            std::move(request.prefetchedText), 0, request.compositionEpoch,
                                            request.focusToken, request.deferredReplayToken);
                    handedOffReplay = true;
                }
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        if (!handedOffReplay)
        {
            pIME->_RetryDeferredKeyReplay(request.deferredReplayToken);
        }
        break;
    }
    case WM_AsyncServerCandidateKey: {
        CMetasequoiaIME::AsyncKeyRequest request;
        if (!pIME->_TakeAsyncKeyRequest(WM_AsyncServerCandidateKey, static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            pIME->_RetryDeferredKeyReplay(request.deferredReplayToken);
            break;
        }
        const UINT code = request.code;
        const WCHAR wch = request.wch;
        FanyImeNamedpipeDataToTsf *receivedData = TryReadDataFromServerPipeWithTimeout(request.requestId);
        if (receivedData->msg_type == Global::DataFromServerMsgType::TransportUnavailable)
        {
            // Keep the existing composition intact. A transport failure is
            // not text and must never be committed to the application.
            if (request.deferredReplayToken != 0)
            {
                pIME->_RetryDeferredKeyReplay(request.deferredReplayToken);
            }
            else
            {
                MarkNamedpipeSessionDirtyForOwner(pIME);
            }
            break;
        }

        if (receivedData->msg_type == Global::DataFromServerMsgType::CommitExactText)
        {
            pIME->_PostAsyncKeyRequest(WM_AsyncPunctuationCommit, code, wch, request.requestId,
                                       receivedData->candidate_string, request.focusToken, request.compositionEpoch,
                                       request.deferredReplayToken);
        }
        else if (receivedData->msg_type == Global::DataFromServerMsgType::Normal)
        {
            if (wch == 0)
            {
                pIME->_CompleteDeferredKeyReplay(request.deferredReplayToken);
                break;
            }
            std::wstring commitText = receivedData->candidate_string;
            if (code == VK_DECIMAL)
            {
                commitText.append(L".");
            }
            else if (pIME->_pCompositionProcessorEngine)
            {
                const WCHAR preceding = commitText.empty() ? 0 : commitText.back();
                commitText.append(pIME->_ResolveSmartPunctuation(wch, preceding));
            }
            pIME->_PostAsyncKeyRequest(WM_AsyncPunctuationCommit, code, wch, request.requestId, std::move(commitText),
                                       request.focusToken, request.compositionEpoch, request.deferredReplayToken);
        }
        else
        {
            pIME->_CompleteDeferredKeyReplay(request.deferredReplayToken);
        }
        // Navigation responses are acknowledgements only. The Server owns and
        // refreshes candidate paging/selection state, so TSF must not apply the
        // same movement to its presenter a second time.
        break;
    }
    case WM_AsyncNumberCandidateCommit: {
        CMetasequoiaIME::AsyncKeyRequest request;
        if (!pIME->_TakeAsyncKeyRequest(WM_AsyncNumberCandidateCommit, static_cast<UINT>(wParam), request))
        {
            break;
        }
        if (!pIME->_IsFocusSessionCurrent(request.focusToken) ||
            !pIME->_IsCompositionEpochCurrent(request.compositionEpoch))
        {
            pIME->_RetryDeferredKeyReplay(request.deferredReplayToken);
            break;
        }
        PerfTimer timer;
        ITfDocumentMgr *pDocMgrFocus = nullptr;
        ITfContext *pContext = nullptr;

        bool handedOffReplay = false;
        if (SUCCEEDED(pIME->_GetThreadMgr()->GetFocus(&pDocMgrFocus)) && pDocMgrFocus)
        {
            if (SUCCEEDED(pDocMgrFocus->GetTop(&pContext)) && pContext)
            {
                _KEYSTROKE_STATE KeystrokeState;
                KeystrokeState.Category = CATEGORY_CANDIDATE;
                KeystrokeState.Function = FUNCTION_SELECT_BY_NUMBER;
                pIME->_InvokeKeyHandler(pContext, request.code, request.wch, 0, KeystrokeState, request.requestId, {},
                                        0, request.compositionEpoch, request.focusToken, request.deferredReplayToken);
                handedOffReplay = true;
                pContext->Release();
            }
            pDocMgrFocus->Release();
        }
        if (!handedOffReplay)
        {
            pIME->_RetryDeferredKeyReplay(request.deferredReplayToken);
        }
        break;
    }
    case WM_CleanupCandidatePresenter: {
        pIME->_DrainPendingCandidatePresenterCleanup();
        break;
    }
    case WM_RefreshLanguageBarTheme: {
        if (wParam != 0)
        {
            pIME->_RefreshLanguageBarThemeIcons();
            break;
        }
        SetTimer(hWnd, TIMER_REFRESH_LANG_BAR_THEME, REFRESH_LANG_BAR_THEME_DELAY_MS, nullptr);
        break;
    }
    case WM_ApplyPunctuationLock: {
        CCompositionProcessorEngine *engine = pIME->GetCompositionProcessorEngine();
        if (engine && Global::IsPunctuationLocked())
        {
            engine->SetPunctuationMode(pIME->_GetThreadMgr(), pIME->_GetClientId(), TRUE);
        }
        SendCurrentImeStatusSnapshot(pIME);
        break;
    }
    case WM_PairedPunctuationMoveLeft: {
        const uint64_t focusToken = static_cast<uint64_t>(static_cast<uint32_t>(wParam)) |
                                    (static_cast<uint64_t>(static_cast<uint32_t>(lParam)) << 32);
        if (focusToken == 0 || !pIME->_IsFocusSessionCurrent(focusToken))
        {
            break;
        }

        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_LEFT;
        inputs[1] = inputs[0];
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

        SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));
        break;
    }
    case WM_ReplaceRepeatedSmartPunctuation: {
        const uint64_t focusToken = static_cast<uint64_t>(static_cast<uint32_t>(wParam)) |
                                    (static_cast<uint64_t>(static_cast<uint32_t>(lParam)) << 32);
        const WCHAR replacement = pIME->_pendingSmartPunctuationReplacement;
        const bool requestCurrent = replacement != 0 && focusToken != 0 &&
                                    focusToken == pIME->_pendingSmartPunctuationFocusToken &&
                                    Global::SmartPunctuationRepeatToChineseEnabled.load(std::memory_order_relaxed) &&
                                    pIME->_IsFocusSessionCurrent(focusToken) &&
                                    GetForegroundWindow() == pIME->_pendingSmartPunctuationForegroundWindow &&
                                    GetTickCount64() <= pIME->_pendingSmartPunctuationDeadline;

        pIME->_pendingSmartPunctuationReplacement = 0;
        pIME->_pendingSmartPunctuationFocusToken = 0;
        pIME->_pendingSmartPunctuationForegroundWindow = nullptr;
        pIME->_pendingSmartPunctuationDeadline = 0;
        if (!requestCurrent)
        {
            break;
        }

        INPUT inputs[4] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_BACK;
        inputs[0].ki.dwExtraInfo = SMART_PUNCTUATION_SENDINPUT_EXTRA_INFO;
        inputs[1] = inputs[0];
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wScan = replacement;
        inputs[2].ki.dwFlags = KEYEVENTF_UNICODE;
        inputs[2].ki.dwExtraInfo = SMART_PUNCTUATION_SENDINPUT_EXTRA_INFO;
        inputs[3] = inputs[2];
        inputs[3].ki.dwFlags |= KEYEVENTF_KEYUP;

        if (SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT)) == ARRAYSIZE(inputs))
        {
            pIME->_smartPunctuationShadowChar = replacement;
            pIME->_smartPunctuationShadowValid = true;
        }
        else
        {
            pIME->_InvalidateSmartPunctuationShadow();
        }
        break;
    }
    case WM_SETTINGCHANGE: {
        // Registry may not be flushed yet when the broadcast arrives.
        if (lParam && _wcsicmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0)
        {
            SetTimer(hWnd, TIMER_REFRESH_LANG_BAR_THEME, REFRESH_LANG_BAR_THEME_DELAY_MS, nullptr);
        }
        break;
    }
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

//+---------------------------------------------------------------------------
//
// ITfFunctionProvider::GetType
//
//----------------------------------------------------------------------------
HRESULT CMetasequoiaIME::GetType(__RPC__out GUID *pguid)
{
    HRESULT hr = E_INVALIDARG;
    if (pguid)
    {
        *pguid = Global::MetasequoiaIMECLSID;
        hr = S_OK;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFunctionProvider::::GetDescription
//
//----------------------------------------------------------------------------
HRESULT CMetasequoiaIME::GetDescription(__RPC__deref_out_opt BSTR *pbstrDesc)
{
    HRESULT hr = E_INVALIDARG;
    if (pbstrDesc != nullptr)
    {
        *pbstrDesc = nullptr;
        hr = E_NOTIMPL;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFunctionProvider::::GetFunction
//
//----------------------------------------------------------------------------
HRESULT CMetasequoiaIME::GetFunction(__RPC__in REFGUID rguid, __RPC__in REFIID riid,
                                     __RPC__deref_out_opt IUnknown **ppunk)
{
    HRESULT hr = E_NOINTERFACE;

    if ((IsEqualGUID(rguid, GUID_NULL)) && (IsEqualGUID(riid, __uuidof(ITfFnSearchCandidateProvider))))
    {
        if (_pITfFnSearchCandidateProvider != nullptr)
        {
            hr = _pITfFnSearchCandidateProvider->QueryInterface(riid, (void **)ppunk);
        }
    }
    else if (IsEqualGUID(rguid, GUID_NULL))
    {
        hr = QueryInterface(riid, (void **)ppunk);
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFunction::GetDisplayName
//
//----------------------------------------------------------------------------
HRESULT CMetasequoiaIME::GetDisplayName(_Out_ BSTR *pbstrDisplayName)
{
    HRESULT hr = E_INVALIDARG;
    if (pbstrDisplayName != nullptr)
    {
        *pbstrDisplayName = nullptr;
        hr = E_NOTIMPL;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFnGetPreferredTouchKeyboardLayout::GetLayout
// The tkblayout will be Optimized layout.
//----------------------------------------------------------------------------
HRESULT CMetasequoiaIME::GetLayout(_Out_ TKBLayoutType *ptkblayoutType, _Out_ WORD *pwPreferredLayoutId)
{
    HRESULT hr = E_INVALIDARG;
    if ((ptkblayoutType != nullptr) && (pwPreferredLayoutId != nullptr))
    {
        *ptkblayoutType = TKBLT_OPTIMIZED;
        *pwPreferredLayoutId = TKBL_OPT_SIMPLIFIED_CHINESE_PINYIN;
        hr = S_OK;
    }
    return hr;
}
