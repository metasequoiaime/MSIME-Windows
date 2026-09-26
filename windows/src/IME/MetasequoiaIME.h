#pragma once

#include "KeyHandlerEditSession.h"
#include "MetasequoiaIMEBaseStructure.h"
#include "Ipc.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CLangBarItemButton;
class CCandidateListUIPresenter;
class CCompositionProcessorEngine;

const DWORD WM_CheckGlobalCompartment = WM_USER;
const DWORD WM_ConnectNamedpipe = WM_USER + 1;
const DWORD WM_DisconnectNamedpipe = WM_USER + 2;
const DWORD WM_ConnectToTsfNamedpipe = WM_USER + 3;
const DWORD WM_IMEActivation = WM_USER + 4;
const DWORD WM_ThreadFocus = WM_USER + 5;
const DWORD WM_UpdateIMEStatus = WM_USER + 6;
const DWORD WM_UpdateDoubleSingleByte = WM_USER + 7;
const DWORD WM_UpdatePuncMode = WM_USER + 8;
const DWORD WM_CommitCandidate = WM_USER + 9;
const DWORD WM_CleanupCandidatePresenter = WM_USER + 10;
const DWORD WM_AsyncFinalizeCandidate = WM_USER + 11;
const DWORD WM_AsyncPunctuationCommit = WM_USER + 12;
const DWORD WM_AsyncNumberCandidateCommit = WM_USER + 13;
const DWORD WM_AsyncServerCandidateKey = WM_USER + 14;
const DWORD WM_IpcWorkerDisconnected = WM_USER + 15;
const DWORD WM_IpcReconnect = WM_USER + 16;
const DWORD WM_IpcSessionDirty = WM_USER + 17;
const DWORD WM_DrainDeferredKeyDown = WM_USER + 18;
const DWORD WM_InsertText = WM_USER + 19;
const DWORD WM_RefreshLanguageBarTheme = WM_USER + 20;
const DWORD WM_PairedPunctuationCaretMove = WM_USER + 21;
const DWORD WM_RewriteSmartPunctuationViaSendInput = WM_USER + 22;
const DWORD WM_BareShiftRelease = WM_USER + 23;
const DWORD WM_UpdateVoiceComposition = WM_USER + 24;
const DWORD WM_CommitVoiceComposition = WM_USER + 25;
const DWORD WM_CancelVoiceComposition = WM_USER + 26;
const DWORD WM_ApplyPunctuationLock = WM_USER + 27;
const DWORD WM_CommitCandidateAndContinue = WM_USER + 28;
constexpr ULONG_PTR SMART_PUNCTUATION_SENDINPUT_EXTRA_INFO = 0x4D535050u;
constexpr ULONG_PTR PAIRED_PUNCTUATION_SENDINPUT_EXTRA_INFO = 0x4D535051u;
// Synthetic input that this tip generates carries a marker meaning "this tip
// generated the event": the key sinks and the bare-Shift hook must pass it
// straight through, or the synthetic arrow re-enters our own direction-key
// handling instead of reaching the application. Two things are synthesized:
// the paired-punctuation caret move, and the smart-punctuation rewrite in
// hosts whose text store cannot be edited in place.
constexpr bool IsSelfGeneratedSendInputExtraInfo(ULONG_PTR extraInfo)
{
    return extraInfo == SMART_PUNCTUATION_SENDINPUT_EXTRA_INFO || extraInfo == PAIRED_PUNCTUATION_SENDINPUT_EXTRA_INFO;
}
// Payload shared by the NeedToCreateWord and CompositionRestored replies:
//   remaining_raw \t committed_word \t display_preedit [\t caret]
// The caret is an optional decimal offset into remaining_raw; omitting it means
// "caret at the end", which is what a legacy three-field payload meant.
struct CreatingWordPayload
{
    std::wstring remaining_raw;
    std::wstring word;
    std::wstring display_preedit;
    bool has_caret = false;
    size_t caret = 0;
};

inline bool ParseCreatingWordPayload(const std::wstring &data, CreatingWordPayload &payload)
{
    const size_t separator = data.find(L'\t');
    if (separator == std::wstring::npos)
    {
        return false;
    }
    payload.remaining_raw = data.substr(0, separator);
    const std::wstring rest = data.substr(separator + 1);
    const size_t second_separator = rest.find(L'\t');
    if (second_separator == std::wstring::npos)
    {
        payload.word = rest;
        return true;
    }
    payload.word = rest.substr(0, second_separator);
    const std::wstring tail = rest.substr(second_separator + 1);
    const size_t third_separator = tail.find(L'\t');
    if (third_separator == std::wstring::npos)
    {
        payload.display_preedit = tail;
        return true;
    }
    payload.display_preedit = tail.substr(0, third_separator);
    const std::wstring caret_text = tail.substr(third_separator + 1);
    if (caret_text.empty())
    {
        return true;
    }
    // A raw pinyin offset fits easily in nine digits; anything longer is a
    // malformed frame rather than a caret to obey.
    if (caret_text.size() > 9)
    {
        return false;
    }
    size_t caret = 0;
    for (const wchar_t ch : caret_text)
    {
        if (ch < L'0' || ch > L'9')
        {
            return false;
        }
        caret = caret * 10 + static_cast<size_t>(ch - L'0');
    }
    payload.has_caret = true;
    payload.caret = caret;
    return true;
}
constexpr ULONGLONG SMART_PUNCTUATION_REPEAT_INTERVAL_MS = 2000;
// The SendInput rewrite is posted from inside the edit session and normally
// runs on the very next pass through the message loop. Anything slower than
// this means other input has had time to move the caret, and the synthetic
// Backspace would land on text the user typed since.
constexpr ULONGLONG SMART_PUNCTUATION_SENDINPUT_TIMEOUT_MS = 500;
constexpr UINT_PTR TIMER_CONNECT_ALL_NAMEDPIPE = 1;
constexpr UINT_PTR TIMER_CONNECT_TO_TSF_NAMEDPIPE = 2;
constexpr UINT_PTR TIMER_REFRESH_LANG_BAR_THEME = 3;
constexpr UINT_PTR TIMER_DEFERRED_FOCUS_LOSS = 4;
constexpr UINT_PTR TIMER_FOCUS_STATUS_RESEND = 5;
constexpr UINT_PTR TIMER_PAIRED_PUNCTUATION_CARET = 6;
// （〈《“‘ and their closing halves are all Shift chords. An arrow key that
// arrives while Shift is still physically down reads as Shift+Arrow, so the
// host extends the selection over the closing punctuation instead of stepping
// past it. The move waits for the chord to be released, re-checking this often.
constexpr UINT PAIRED_PUNCTUATION_CARET_RETRY_MS = 15;
// A key held down forever must not leave a timer running or fire a caret move
// into text the user has since typed by other means.
constexpr ULONGLONG PAIRED_PUNCTUATION_CARET_TIMEOUT_MS = 2000;
// Bounds the burst emitted after a long deferral.
constexpr int PAIRED_PUNCTUATION_CARET_MAX_STEPS = 8;
constexpr size_t PAIRED_PUNCTUATION_MAX_DEPTH = 16;
constexpr UINT FOCUS_LOSS_DEFER_MS = 300;
// Chromium hosts fire a burst of OnSetFocus per window switch; coalesce them
// into one resend instead of one packet per callback.
constexpr UINT FOCUS_STATUS_RESEND_DELAY_MS = 50;
LRESULT CALLBACK CMetasequoiaIME_WindowProc(HWND wndHandle, UINT uMsg, WPARAM wParam, LPARAM lParam);

class CMetasequoiaIME : public ITfTextInputProcessorEx,
                        public ITfThreadMgrEventSink,
                        public ITfTextEditSink,
                        public ITfKeyEventSink,
                        public ITfCompositionSink,
                        public ITfDisplayAttributeProvider,
                        public ITfActiveLanguageProfileNotifySink,
                        public ITfThreadFocusSink,
                        public ITfFunctionProvider,
                        public ITfFnGetPreferredTouchKeyboardLayout
{
    friend class CCompositionProcessorEngine;
    friend class CKeyHandlerEditSession;
    // Needs _IsComposing() to tell a host's transient context-view teardown apart
    // from a real end of composition.
    friend class CCandidateListUIPresenter;

  public:
    CMetasequoiaIME();
    ~CMetasequoiaIME();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, _Outptr_ void **ppvObj);
    STDMETHODIMP_(ULONG) AddRef(void);
    STDMETHODIMP_(ULONG) Release(void);

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr *pThreadMgr, TfClientId tfClientId)
    {
        return ActivateEx(pThreadMgr, tfClientId, 0);
    }
    // ITfTextInputProcessorEx
    STDMETHODIMP ActivateEx(ITfThreadMgr *pThreadMgr, TfClientId tfClientId, DWORD dwFlags);
    STDMETHODIMP Deactivate();

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(_In_ ITfDocumentMgr *pDocMgr);
    STDMETHODIMP OnUninitDocumentMgr(_In_ ITfDocumentMgr *pDocMgr);
    STDMETHODIMP OnSetFocus(_In_ ITfDocumentMgr *pDocMgrFocus, _In_ ITfDocumentMgr *pDocMgrPrevFocus);
    STDMETHODIMP OnPushContext(_In_ ITfContext *pContext);
    STDMETHODIMP OnPopContext(_In_ ITfContext *pContext);

    // ITfTextEditSink
    STDMETHODIMP OnEndEdit(__RPC__in_opt ITfContext *pContext, TfEditCookie ecReadOnly,
                           __RPC__in_opt ITfEditRecord *pEditRecord);

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground);
    STDMETHODIMP OnTestKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pIsEaten);
    STDMETHODIMP OnKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pIsEaten);
    STDMETHODIMP OnTestKeyUp(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pIsEaten);
    STDMETHODIMP OnKeyUp(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pIsEaten);
    STDMETHODIMP OnPreservedKey(ITfContext *pContext, REFGUID rguid, BOOL *pIsEaten);

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, _In_ ITfComposition *pComposition);

    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(__RPC__deref_out_opt IEnumTfDisplayAttributeInfo **ppEnum);
    STDMETHODIMP GetDisplayAttributeInfo(__RPC__in REFGUID guidInfo,
                                         __RPC__deref_out_opt ITfDisplayAttributeInfo **ppInfo);

    // ITfActiveLanguageProfileNotifySink
    STDMETHODIMP OnActivated(_In_ REFCLSID clsid, _In_ REFGUID guidProfile, _In_ BOOL isActivated);

    // ITfThreadFocusSink
    STDMETHODIMP OnSetThreadFocus();
    STDMETHODIMP OnKillThreadFocus();

    // ITfFunctionProvider
    STDMETHODIMP GetType(__RPC__out GUID *pguid);
    STDMETHODIMP GetDescription(__RPC__deref_out_opt BSTR *pbstrDesc);
    STDMETHODIMP GetFunction(__RPC__in REFGUID rguid, __RPC__in REFIID riid, __RPC__deref_out_opt IUnknown **ppunk);

    // ITfFunction
    STDMETHODIMP GetDisplayName(_Out_ BSTR *pbstrDisplayName);

    // ITfFnGetPreferredTouchKeyboardLayout, it is the Optimized layout feature.
    STDMETHODIMP GetLayout(_Out_ TKBLayoutType *ptkblayoutType, _Out_ WORD *pwPreferredLayoutId);

    // CClassFactory factory callback
    static HRESULT CreateInstance(_In_ IUnknown *pUnkOuter, REFIID riid, _Outptr_ void **ppvObj);

    // utility function for thread manager.
    ITfThreadMgr *_GetThreadMgr()
    {
        return _pThreadMgr;
    }
    TfClientId _GetClientId()
    {
        return _tfClientId;
    }
    bool _IsServerUnavailableFallbackActive() const;

    // functions for the composition object.
    void _SetComposition(_In_ ITfComposition *pComposition);
    void _TerminateComposition(TfEditCookie ec, _In_ ITfContext *pContext, BOOL isCalledFromDeactivate = FALSE);
    void _SaveCompositionContext(_In_ ITfContext *pContext);
    // Reads the committed text of a terminating composition and queues one
    // statistics event for it. Safe to call from both composition exits; only
    // the first observer captures, and any failure is silent.
    void _CaptureCompositionStats(TfEditCookie ec, _In_ ITfComposition *pComposition);

    // key event handlers for composition/candidate/phrase common objects.
    HRESULT _HandleComplete(TfEditCookie ec, _In_ ITfContext *pContext);
    HRESULT _HandleCompleteCommitFirst(TfEditCookie ec, _In_ ITfContext *pContext);
    HRESULT _HandleCancel(TfEditCookie ec, _In_ ITfContext *pContext);
    HRESULT _HandleToogleIMEMode(TfEditCookie ec, _In_ ITfContext *pContext);
    HRESULT _HandleInsertText(TfEditCookie ec, _In_ ITfContext *pContext, const std::wstring &text);
    HRESULT _HandleCommitCandidateAndContinue(TfEditCookie ec, _In_ ITfContext *pContext, const std::wstring &payload);
    HRESULT _HandleUpdateVoiceComposition(TfEditCookie ec, _In_ ITfContext *pContext, const std::wstring &text);
    HRESULT _HandleCommitVoiceComposition(TfEditCookie ec, _In_ ITfContext *pContext, const std::wstring &text);
    HRESULT _HandleCancelVoiceComposition(TfEditCookie ec, _In_ ITfContext *pContext);

    // key event handlers for composition object.
    HRESULT _HandleCompositionInput(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR wch, uint64_t requestId);
    HRESULT _HandleCompositionFinalize(TfEditCookie ec, _In_ ITfContext *pContext, BOOL fCandidateList);
    HRESULT _HandleCompositionConvert(TfEditCookie ec, _In_ ITfContext *pContext, BOOL isWildcardSearch);
    HRESULT _HandleCompositionBackspace(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId);
    // Ctrl+Backspace: the Server deletes one input unit and the composition is
    // rebuilt from its authoritative CompositionRestored payload. Hosts that
    // cannot apply that payload fall back to the single-character deletion.
    HRESULT _HandleCompositionBackspaceSegment(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId);
    // Rebuild the composition from a creating-word payload: keystroke buffer,
    // accumulated word, preedit and caret. Used both when a selection continues a
    // word (NeedToCreateWord) and when the Server retracts it
    // (CompositionRestored).
    HRESULT _ApplyCreatingWordPayload(TfEditCookie ec, _In_ ITfContext *pContext, const CreatingWordPayload &payload);
    HRESULT _HandleCompositionDelete(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId);
    HRESULT _HandleCompositionArrowKey(TfEditCookie ec, _In_ ITfContext *pContext, KEYSTROKE_FUNCTION keyFunction,
                                       uint64_t requestId = FANY_IME_NO_REQUEST_ID);
    // Ctrl+Left / Ctrl+Right: the Server moves the caret by one input unit and
    // answers with the authoritative caret. Hosts that cannot apply that reply
    // fall back to the single-character move.
    HRESULT _HandleCompositionArrowKeySegment(TfEditCookie ec, _In_ ITfContext *pContext,
                                              KEYSTROKE_FUNCTION keyFunction, uint64_t requestId);
    // Place the TSF selection at the engine's rendered caret. Shared by the
    // plain arrow move and the Server-driven unit jump.
    HRESULT _SetCompositionCaretSelection(TfEditCookie ec, _In_ ITfContext *pContext);
    HRESULT _HandleCompositionPunctuation(TfEditCookie ec, _In_ ITfContext *pContext, UINT code, WCHAR wch,
                                          uint64_t requestId, const std::wstring &prefetchedText);
    // Character immediately before the caret / composition start (0 if unavailable).
    WCHAR _GetPrecedingDocumentChar(TfEditCookie ec, _In_ ITfContext *pContext);
    // The characters immediately before the caret / composition start, oldest
    // first, up to `count`. Returns how many were actually read; a shallow text
    // store accepts the shift but exposes nothing, which reads back as 0.
    int _GetPrecedingDocumentChars(TfEditCookie ec, _In_ ITfContext *pContext, _Out_writes_(count) WCHAR *buffer,
                                   int count);
    // Character immediately after the caret (0 if unavailable). Only meaningful
    // outside a composition.
    WCHAR _GetFollowingDocumentChar(TfEditCookie ec, _In_ ITfContext *pContext);
    // Shadow first, document read as the fallback. See _smartPunctuationShadowChar.
    WCHAR _GetPrecedingCharForSmartPunctuation(TfEditCookie ec, _In_ ITfContext *pContext);

    // Paired punctuation: the closing half is committed together with the
    // opening half and the caret then steps back between them, so every pair
    // still waiting to be closed is tracked. Pressing the closing half steps
    // over the existing one instead of typing a second.
    static WCHAR _GetPairedPunctuationClosingFor(WCHAR opening);
    void _PushPairedPunctuation(WCHAR opening, WCHAR closing);
    void _ClearPairedPunctuationStack();
    bool _TryStepOverPairedPunctuation(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR closing);
    void _NoteKeyForPairedPunctuation(UINT code);
    void _QueuePairedPunctuationCaretMove(int delta);
    void _RunPairedPunctuationCaretMove();
    void _CancelPairedPunctuationCaretMove();

    // Smart punctuation: backspacing the ASCII punctuation we just committed
    // means that form was unwanted, so the spot stays on Chinese punctuation.
    std::wstring _ResolveSmartPunctuation(WCHAR wch, WCHAR precedingChar);
    // Reversible conversion: a space after the just-committed Chinese
    // punctuation converts it to ASCII, and the same punctuation key right
    // after the conversion reverts it. Both run inside an edit session; the
    // key sinks only consult this state to decide whether to claim the key.
    // Key-claim predicates. These answer "may this key be eaten?", never
    // "may the pending state survive?" — the latter must use the key's
    // classification result, so an unclaimed passthrough key still clears it.
    bool _CanInterceptSmartPunctuationConvert();
    bool _CanInterceptSmartPunctuationRevert(WCHAR wch);
    void _ClearSmartPunctuationAction();
    void _NoteCommittedChinesePunctuation(const std::wstring &committedText, bool autoClosedPair, WCHAR beforeChar);
    void _WriteDirectSmartPunctuationState(WCHAR triggerKey, WCHAR chinese, WCHAR ascii, WCHAR beforeChar);
    HRESULT _RequestSmartPunctuationEditSession(_In_ ITfContext *pContext, WCHAR wch, KEYSTROKE_FUNCTION function,
                                                uint64_t expectedFocusGeneration);
    HRESULT _ExecuteSmartPunctuationAction(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR wch,
                                           KEYSTROKE_FUNCTION function);
    // Replaces the character immediately left of the caret inside the document.
    // The range must read back as `expected` first: a host whose text store
    // holds no committed text (terminals, proxy stores) accepts the shift but
    // exposes nothing, and there SetText would report success while the screen
    // keeps the old character. Returns false in that case, document untouched.
    bool _RewritePrecedingCharInPlace(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR expected, WCHAR replacement);
    // Host-agnostic fallback for those stores: one synthetic Backspace plus the
    // replacement character, posted so it runs after the current key finishes.
    // This is how the feature worked before the edit session existed; it is the
    // only thing that reaches what a terminal has already written to its pty.
    bool _QueueSmartPunctuationSendInputRewrite(WCHAR replacement);
    void _RunSmartPunctuationSendInputRewrite();
    void _CancelSmartPunctuationSendInputRewrite();
    // Terminal fallback when the smart-punctuation edit session could not be
    // requested at all: write the key's own character back so it is not lost.
    HRESULT _ExecuteSmartPunctuationFallback(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR wch,
                                             KEYSTROKE_FUNCTION function);
    // Commit-time fingerprint: the character that preceded the punctuation. A
    // mismatch means the caret was moved onto historical text that looks the
    // same as the spot just committed.
    bool _SmartPunctuationFingerprintMatches(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR beforeChar);
    void _NoteKeyForSmartPunctuation(UINT code, WCHAR wch, bool isEaten, KEYSTROKE_FUNCTION function);
    void _UpdateSmartPunctuationShadow(UINT code, WCHAR wch, bool isEaten);
    void _InvalidateSmartPunctuationShadow();
    HRESULT _HandleCompositionDoubleSingleByte(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR wch);

    // key event handlers for candidate object.
    HRESULT _HandleCandidateFinalize(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId,
                                     const std::wstring &prefetchedText);
    HRESULT _HandleCandidateFinalizeForVKReturn(TfEditCookie ec, _In_ ITfContext *pContext);
    HRESULT _HandleCandidateConvert(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId,
                                    const std::wstring &prefetchedText);
    HRESULT _HandleCandidateArrowKey(TfEditCookie ec, _In_ ITfContext *pContext, _In_ KEYSTROKE_FUNCTION keyFunction,
                                     uint64_t requestId = FANY_IME_NO_REQUEST_ID);
    HRESULT _HandleCandidateSelectByNumber(TfEditCookie ec, _In_ ITfContext *pContext, _In_ UINT uCode,
                                           uint64_t requestId, const std::wstring &prefetchedText);

    BOOL _IsSecureMode(void)
    {
        return (_dwActivateFlags & TF_TMAE_SECUREMODE) ? TRUE : FALSE;
    }
    BOOL _IsComLess(void)
    {
        return (_dwActivateFlags & TF_TMAE_COMLESS) ? TRUE : FALSE;
    }
    BOOL _IsStoreAppMode(void)
    {
        return (_dwActivateFlags & TF_TMF_IMMERSIVEMODE) ? TRUE : FALSE;
    }
    // Host requested UILess (games / fullscreen / console): only ITfUIElement
    // data is allowed — never an IME-owned HWND.
    BOOL _IsUiLessMode(void)
    {
        return (_dwActivateFlags & TF_TMF_UIELEMENTENABLEDONLY) ? TRUE : FALSE;
    }

    CCompositionProcessorEngine *GetCompositionProcessorEngine()
    {
        return (_pCompositionProcessorEngine);
    };

    // Async TSF edit sessions may be granted after a focus transition. Bind
    // every text-producing session to the exact focus token that scheduled it
    // so an old worker/key message cannot commit into the next document.
    uint64_t _CaptureFocusSessionToken() const;
    bool _IsFocusSessionCurrent(uint64_t focusToken, _In_opt_ ITfContext *expectedContext = nullptr) const;
    uint64_t _CaptureCompositionEpoch() const;
    bool _IsCompositionEpochCurrent(uint64_t compositionEpoch) const;
    bool _IsCompositionCurrent(_In_opt_ ITfComposition *expectedComposition) const;
    static bool _IsSameComObject(_In_opt_ IUnknown *left, _In_opt_ IUnknown *right);
    void _DebugCompositionRecovery(_In_z_ const WCHAR *reason, HRESULT hr) const;
    bool _IsLocalSessionResetCurrent(UINT resetToken) const;
    void _CompleteLocalSessionReset(UINT resetToken);
    bool _IsDeferredKeyReplayCurrent(uint64_t replayToken, uint64_t focusGeneration,
                                     _In_opt_ ITfContext *expectedContext) const;
    void _CompleteDeferredKeyReplay(uint64_t replayToken);
    void _RetryDeferredKeyReplay(uint64_t replayToken);

    // comless helpers
    static HRESULT CMetasequoiaIME::CreateInstance(REFCLSID rclsid, REFIID riid, _Outptr_result_maybenull_ LPVOID *ppv,
                                                   _Out_opt_ HINSTANCE *phInst, BOOL isComLessMode);
    static HRESULT CMetasequoiaIME::ComLessCreateInstance(REFGUID rclsid, REFIID riid,
                                                          _Outptr_result_maybenull_ void **ppv,
                                                          _Out_opt_ HINSTANCE *phInst);
    static HRESULT CMetasequoiaIME::GetComModuleName(REFGUID rclsid, _Out_writes_(cchPath) WCHAR *wchPath,
                                                     DWORD cchPath);

    static void IpcWorkerThread(CMetasequoiaIME *pIME);
    void _QueuePendingServerCandidate(UINT msgType, _In_z_ const WCHAR *pCandidateString);
    bool _TakePendingServerCandidate(_Out_ UINT *pMsgType, _Out_ std::wstring *pCandidateString);
    void _ScheduleCandidatePresenterCleanup(_In_ CCandidateListUIPresenter *pPresenter);
    void _DrainPendingCandidatePresenterCleanup();

  private:
    // functions for the composition object.
    HRESULT _HandleCompositionInputWorker(_In_ CCompositionProcessorEngine *pCompositionProcessorEngine,
                                          TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId);
    HRESULT _CreateAndStartCandidate(_In_ CCompositionProcessorEngine *pCompositionProcessorEngine, TfEditCookie ec,
                                     _In_ ITfContext *pContext);
    HRESULT _HandleCandidateWorker(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId,
                                   const std::wstring &prefetchedText);

    struct AsyncKeyRequest
    {
        UINT message = 0;
        UINT code = 0;
        WCHAR wch = L'\0';
        uint64_t requestId = UINT64_MAX;
        uint64_t focusToken = 0;
        uint64_t compositionEpoch = 0;
        uint64_t deferredReplayToken = 0;
        std::wstring prefetchedText;
    };
    bool _PostAsyncKeyRequest(UINT message, UINT code, WCHAR wch, uint64_t requestId, std::wstring prefetchedText = {},
                              uint64_t expectedFocusToken = 0, uint64_t expectedCompositionEpoch = 0,
                              uint64_t deferredReplayToken = 0);
    bool _TakeAsyncKeyRequest(UINT message, UINT token, _Out_ AsyncKeyRequest &request);
    struct WorkerCandidateCommit
    {
        std::wstring text;
        uint64_t focusToken = 0;
        uint64_t compositionEpoch = 0;
    };
    bool _PostServerCandidateCommit(_In_z_ const WCHAR *candidateText);
    bool _PostServerCandidateCommitAndContinue(_In_z_ const WCHAR *payload);
    bool _PostServerInsertText(_In_z_ const WCHAR *text);
    bool _PostServerTextDelivery(UINT windowMessage, _In_z_ const WCHAR *text);
    bool _TakeServerCandidateCommit(UINT token, _Out_ WorkerCandidateCommit &request);
    void _ResetVoiceCompositionAssemble();
    void _AssembleVoiceCompositionFrame(const FanyImeNamedpipeDataToTsfWorkerThread &buf);
    void _DispatchUnsolicitedVoiceText(WPARAM wParam, KEYSTROKE_FUNCTION function);
    struct WorkerCompartmentSwitch
    {
        UINT messageType = 0;
        uint64_t focusToken = 0;
        uint64_t compositionEpoch = 0;
    };
    bool _PostWorkerCompartmentSwitch(UINT messageType, uint64_t focusToken);
    bool _TakeWorkerCompartmentSwitch(UINT token, _Out_ WorkerCompartmentSwitch &request);
    void _ClearAsyncKeyRequests();
    void _ClearPendingIpcRequests();
    void _RequestLocalSessionReset(_In_opt_ ITfContext *preferredContext, UINT resetToken);
    bool _CaptureWindowsTextInputHostFocusLoss();

    struct DeferredKeyDown
    {
        enum class Kind
        {
            KeyDown,
            PreservedKey,
            ApplicationText
        };

        Kind kind = Kind::KeyDown;
        ITfContext *context = nullptr;
        WPARAM wParam = 0;
        LPARAM lParam = 0;
        WCHAR translatedWch = L'\0';
        UINT modifiersDown = 0;
        _KEYSTROKE_STATE keyState = {};
        GUID preservedKey = {};
        uint64_t focusGeneration = 0;
        ULONGLONG queuedAtMs = 0;
        UINT replayAttempts = 0;
        bool preservedApplied = false;
    };
    enum class KeyDownDispatchResult
    {
        Complete,
        Retry,
        AwaitingCompletion
    };
    bool _HasDeferredKeyBarrier() const;
    bool _DeferredKeyQueueHasCapacity() const;
    void _EnsureDeferredKeyProjection();
    void _ApplyDeferredKeyProjection(const _KEYSTROKE_STATE &keyState, WCHAR wch);
    void _ApplyDeferredPreservedKeyProjection(REFGUID preservedKey);
    bool _RefreshDeferredRecoveryPrefix(_In_ ITfContext *pContext);
    void _ArmDeferredRecoveryForTransport(_In_opt_ ITfContext *pContext);
    bool _ClassifyDeferredKeyDown(_In_ ITfContext *pContext, WPARAM wParam, LPARAM lParam,
                                  _In_opt_ const WCHAR *translatedWch, _In_opt_ const UINT *modifiersDown,
                                  _Out_ WCHAR *classifiedWch, _Out_ UINT *classifiedCode,
                                  _Out_ _KEYSTROKE_STATE *keyState);
    // Counts one printable character handed back to the application
    // (Statistics/stats_passthrough.h). keyboardKnownEnabled is true when the
    // caller already proved the keyboard is live through a non-zero
    // _IsKeyEaten out-char; otherwise the disabled compartment is queried here.
    void _NotePassthroughStatistics(UINT virtualKey, WCHAR wch, bool keyboardKnownEnabled);
    bool _QueueDeferredKeyDown(_In_ ITfContext *pContext, WPARAM wParam, LPARAM lParam, WCHAR translatedWch,
                               UINT modifiersDown, const _KEYSTROKE_STATE &keyState);
    bool _QueueDeferredPreservedKey(_In_ ITfContext *pContext, REFGUID preservedKey);
    void _ClearDeferredKeyDowns();
    void _ScheduleDeferredKeyDownDrain();
    void _DrainOneDeferredKeyDown();
    void _TryLeaveServerUnavailableFallback();
    void _WakeServerIfNeeded();
    void _NoteKeyEventIpcFailure();
    HRESULT _RequestDeferredApplicationTextEditSession(_In_ ITfContext *pContext, WCHAR wch,
                                                       uint64_t expectedFocusToken, uint64_t expectedFocusGeneration,
                                                       uint64_t deferredReplayToken);
    KeyDownDispatchResult _DispatchKeyDown(_In_ ITfContext *pContext, WPARAM wParam, LPARAM lParam,
                                           _Out_ BOOL *pIsEaten, _In_opt_ const WCHAR *translatedWch,
                                           _In_opt_ const UINT *modifiersDown,
                                           _In_opt_ const _KEYSTROKE_STATE *prevalidatedKeyState, bool canDefer,
                                           uint64_t expectedFocusGeneration, uint64_t deferredReplayToken = 0);
    void _DispatchPreservedKey(_In_ ITfContext *pContext, REFGUID preservedKey, _Out_ BOOL *pIsEaten,
                               uint64_t expectedFocusGeneration, bool isPrevalidated, uint64_t deferredReplayToken = 0);

    // Input-mode hotkeys (Shift/Ctrl toggle, Ctrl+Alt+Space, Ctrl+Shift+Space,
    // Ctrl+., Ctrl+Shift+E) are detected from ITfKeyEventSink like
    // Weasel/Rime ascii_composer, not via TSF PreserveKey. Global/admin
    // shortcuts stay on the Server LL hook.
    void _TrackModifierHotkeyArming(WPARAM wParam, LPARAM lParam, bool isKeyUp);
    bool _MatchChordInputHotkey(WPARAM wParam, _Out_ GUID *hotkeyGuid) const;
    bool _MatchModifierReleaseHotkey(WPARAM wParam, _Out_ GUID *hotkeyGuid);
    bool _QueueInputHotkey(_In_ ITfContext *pContext, REFGUID hotkeyGuid, _Out_ BOOL *pIsEaten);

    // mintty routes composition over the legacy IMM bridge and never offers a
    // bare modifier key-up to ITfKeyEventSink, so there is no sink event to
    // hang the toggle on. Observe only this host thread and feed the missed
    // bare-Shift release back into the normal deferred hotkey path.
    // _MarkBareShiftHandled() latches the presses the key-event sink already
    // toggled, so a host that does deliver the release never toggles twice.
    // Keep this mintty-only: hosts whose problem is a stale GetKeyState()
    // rather than a missing callback are handled in the sink, and windows/
    // AGENTS.md asks that hooks in the injected DLL stay the exception.
    void _InitBareShiftKeyboardHook();
    void _UninitBareShiftKeyboardHook();
    void _HandleHookedBareShiftRelease(UINT sequence);
    void _MarkBareShiftHandled();
    static LRESULT CALLBACK _BareShiftKeyboardHookProc(int code, WPARAM wParam, LPARAM lParam);
    static thread_local CMetasequoiaIME *_bareShiftHookOwner;

    void _StartComposition(_In_ ITfContext *pContext);
    HRESULT _EndComposition(_In_opt_ ITfContext *pContext, _In_opt_ ITfComposition *expectedComposition = nullptr,
                            bool bypassFocusValidation = false);
    BOOL _IsComposing();
    BOOL _IsKeyboardDisabled();

    HRESULT _AddComposingAndChar(TfEditCookie ec, _In_ ITfContext *pContext, _In_ CStringRange *pstrAddString);
    HRESULT _AddCharAndFinalize(TfEditCookie ec, _In_ ITfContext *pContext, _In_ CStringRange *pstrAddString);
    HRESULT _InsertTextToComposition(TfEditCookie ec, _In_ ITfContext *pContext, _In_ CStringRange *pstrAddString);
    HRESULT _SetCompositionTextAndSelection(TfEditCookie ec, _In_ ITfContext *pContext,
                                            _In_ CStringRange *pstrAddString);

    BOOL _FindComposingRange(TfEditCookie ec, _In_ ITfContext *pContext, _In_ ITfRange *pSelection,
                             _Outptr_result_maybenull_ ITfRange **ppRange);
    HRESULT _SetInputString(TfEditCookie ec, _In_ ITfContext *pContext, _Out_opt_ ITfRange *pRange,
                            _In_ CStringRange *pstrAddString, BOOL exist_composing);
    HRESULT _InsertAtSelection(TfEditCookie ec, _In_ ITfContext *pContext, _In_ CStringRange *pstrAddString,
                               _Outptr_ ITfRange **ppCompRange);

    HRESULT _RemoveDummyCompositionForComposing(TfEditCookie ec, _In_ ITfComposition *pComposition);

    // Invoke key handler edit session
    HRESULT _InvokeKeyHandler(_In_ ITfContext *pContext, UINT code, WCHAR wch, DWORD flags, _KEYSTROKE_STATE keyState,
                              uint64_t requestId, std::wstring prefetchedText = {}, UINT localResetToken = 0,
                              uint64_t expectedCompositionEpoch = 0, uint64_t expectedFocusToken = 0,
                              uint64_t deferredReplayToken = 0);
    HRESULT _RequestDirectPunctuationEditSession(_In_ ITfContext *pContext, UINT code, WCHAR wch, uint64_t requestId,
                                                 std::wstring prefetchedText, uint64_t expectedFocusToken = 0,
                                                 uint64_t expectedCompositionEpoch = 0,
                                                 uint64_t deferredReplayToken = 0);

    // function for the language property
    BOOL _SetCompositionLanguage(TfEditCookie ec, _In_ ITfContext *pContext);

    // function for the display attribute
    void _ClearCompositionDisplayAttributes(TfEditCookie ec, _In_ ITfContext *pContext,
                                            _In_opt_ ITfComposition *expectedComposition = nullptr);
    BOOL _SetCompositionDisplayAttributes(TfEditCookie ec, _In_ ITfContext *pContext, TfGuidAtom gaDisplayAttribute);
    BOOL _SetCompositionDisplayAttributesForRange(TfEditCookie ec, _In_ ITfContext *pContext,
                                                  _In_ ITfRange *pRangeComposition, TfGuidAtom gaDisplayAttribute);
    BOOL _InitDisplayAttributeGuidAtom();

    BOOL _InitThreadMgrEventSink();
    void _UninitThreadMgrEventSink();
    void _HandleFocusedContextStackChange(_In_opt_ ITfContext *changedContext);

    BOOL _InitTextEditSink(_In_opt_ ITfDocumentMgr *pDocMgr);

    void _UpdateLanguageBarOnSetFocus(_In_ ITfDocumentMgr *pDocMgrFocus);

    BOOL _InitKeyEventSink();
    void _UninitKeyEventSink();

    BOOL _InitActiveLanguageProfileNotifySink();
    void _UninitActiveLanguageProfileNotifySink();

    BOOL _IsKeyEaten(_In_ ITfContext *pContext, UINT codeIn, _Out_ UINT *pCodeOut, _Out_writes_(1) WCHAR *pwch,
                     _Out_opt_ _KEYSTROKE_STATE *pKeyState, _In_opt_ const WCHAR *translatedWch = nullptr,
                     bool freshCompositionState = false);

    bool _IsCompositionActiveForKeyGuard();
    bool _ApplyBackspaceHoldGuard(WPARAM wParam, LPARAM lParam);
    void _ApplyCapsLockKeyDownSideEffects(bool capsLockEnabled);

    BOOL _IsRangeCovered(TfEditCookie ec, _In_ ITfRange *pRangeTest, _In_ ITfRange *pRangeCover);
    VOID _DeleteCandidateList(BOOL fForce, _In_opt_ ITfContext *pContext);

    WCHAR ConvertVKey(UINT code);

    BOOL _InitThreadFocusSink();
    void _UninitThreadFocusSink();

    BOOL _InitFunctionProviderSink();
    void _UninitFunctionProviderSink();

    BOOL _AddTextProcessorEngine();

    void _StartThemeRegistryWatcher();
    void _StopThemeRegistryWatcher();
    void _RefreshLanguageBarThemeIcons();
    void _RequestLanguageBarCapsIconRefresh();

    BOOL VerifyMetasequoiaIMECLSID(_In_ REFCLSID clsid);

    friend LRESULT CALLBACK CMetasequoiaIME_WindowProc(HWND wndHandle, UINT uMsg, WPARAM wParam, LPARAM lParam);

  private:
    ITfThreadMgr *_pThreadMgr;
    TfClientId _tfClientId;
    DWORD _dwActivateFlags;

    // The cookie of ThreadMgrEventSink
    DWORD _threadMgrEventSinkCookie;

    ITfContext *_pTextEditSinkContext;
    DWORD _textEditSinkCookie;

    // The cookie of ActiveLanguageProfileNotifySink
    DWORD _activeLanguageProfileNotifySinkCookie;

    // The cookie of ThreadFocusSink
    DWORD _dwThreadFocusSinkCookie;

    // Composition Processor Engine object.
    CCompositionProcessorEngine *_pCompositionProcessorEngine;

    // Language bar item object.
    CLangBarItemButton *_pLangBarItem;

    // the current composition object.
    ITfComposition *_pComposition;

    // guidatom for the display attibute.
    TfGuidAtom _gaDisplayAttributeInput;
    TfGuidAtom _gaDisplayAttributeConverted;

    CANDIDATE_MODE _candidateMode;
    CCandidateListUIPresenter *_pCandidateListUIPresenter;
    BOOL _isCandidateWithWildcard : 1;

    // Reversible smart punctuation: the last Chinese punctuation commit
    // (waiting for a following space) or the last ASCII conversion (waiting
    // for the same punctuation key to revert it). Focus token and foreground
    // window keep a stale state from firing in another document or app.
    struct SmartPunctuationAction
    {
        enum class Kind
        {
            None,
            ChineseCommitted,
            AsciiConverted
        };

        Kind kind = Kind::None;
        WCHAR triggerKey = 0;
        WCHAR chineseLeft = 0;
        WCHAR asciiLeft = 0;
        // Character that preceded the punctuation at commit time. A mouse
        // click can park the caret right after a punctuation identical to the
        // one just committed; this fingerprint separates "the same spot" from
        // "a historical twin". 0 means the store gave nothing to record.
        WCHAR beforeChar = 0;
        // Written by both states, but only read for AsciiConverted (the revert
        // window). ChineseCommitted relies on the next key or the focus/window
        // checks to expire instead of a clock.
        ULONGLONG tick = 0;
        uint64_t focusToken = 0;
        HWND foregroundWindow = nullptr;
    };
    SmartPunctuationAction _smartPunctuationAction;

    // Rewrite owed to the SendInput fallback, with the focus session and
    // foreground window it was computed against. Cleared together with the
    // action itself: once the action is gone the rewrite describes a spot that
    // is no longer provably under the caret.
    WCHAR _pendingSmartPunctuationRewrite = 0;
    uint64_t _pendingSmartPunctuationRewriteFocusToken = 0;
    HWND _pendingSmartPunctuationRewriteForegroundWindow = nullptr;
    ULONGLONG _pendingSmartPunctuationRewriteDeadline = 0;

    // Last character known to have reached the application. Hosts such as the
    // VS Code terminal back the context with a proxy text store that only ever
    // receives what this tip commits: keys they route straight to the pty leave
    // no trace, so reading the document there yields nothing or stale text.
    // Keys passed through to the application are tracked here instead, and the
    // document read is used only while this is invalid.
    WCHAR _smartPunctuationShadowChar = 0;
    bool _smartPunctuationShadowValid = false;

    // Pairs whose closing half was auto-inserted and still sits immediately to
    // the right of the caret, innermost last.
    struct PairedPunctuationEntry
    {
        WCHAR opening = 0;
        WCHAR closing = 0;
        uint64_t focusToken = 0;
    };
    std::vector<PairedPunctuationEntry> _pairedPunctuationStack;
    // Caret steps owed to the paired-punctuation feature: negative is VK_LEFT
    // (a pair was just opened), positive is VK_RIGHT (a pair was stepped over).
    int _pendingPairedCaretDelta = 0;
    uint64_t _pendingPairedCaretFocusToken = 0;
    ULONGLONG _pendingPairedCaretDeadline = 0;
    bool _pairedCaretRetryTimerActive = false;

    ITfDocumentMgr *_pDocMgrLastFocused;

    ITfContext *_pContext;

    ITfCompartment *_pSIPIMEOnOffCompartment;
    DWORD _dwSIPIMEOnOffCompartmentSinkCookie;

    HWND _msgWndHandle;
    HKEY _themeRegKey;
    HANDLE _themeRegEvent;
    std::thread *_pThemeWatcherThread;
    std::atomic<bool> _stopThemeWatcher;
    std::thread *_pIpcThread;
    std::atomic<HANDLE> _hToTsfWorkerThreadPipe;
    std::atomic<UINT> _workerPipeGeneration;
    HANDLE _ipcStopEvent;
    std::atomic<bool> _shouldStopIpcThread;
    UINT _ipcReconnectDelayMs;
    UINT _ipcConsecutiveFailures;
    std::mutex _pendingCommitCandidateMutex;
    std::map<UINT, WorkerCandidateCommit> _pendingServerCommitMessages;
    std::map<UINT, WorkerCompartmentSwitch> _pendingWorkerSwitchMessages;
    std::map<UINT, AsyncKeyRequest> _pendingAsyncKeyMessages;
    std::atomic<bool> _workerCommitReady;
    std::atomic<uint64_t> _expectedWorkerFocusToken;
    std::atomic<uint64_t> _acknowledgedWorkerFocusToken;
    std::atomic<uint64_t> _compositionEpoch;
    // De-duplicates the two composition exits that can observe the same commit
    // (_TerminateComposition can re-enter OnCompositionTerminated). Reset when
    // the next composition is created; see _CaptureCompositionStats.
    std::atomic<bool> _compositionStatsCaptured{false};
    std::wstring _voiceCompositionAssemble;
    UINT _voiceCompositionAssembleMsg = 0;
    wchar_t _voiceCompositionAssembleGeneration = 0;
    bool _voiceCompositionAssembleActive = false;
    bool _voiceCompositionActive = false;
    std::atomic<bool> _localSessionResetPending;
    std::atomic<UINT> _localSessionResetToken;
    bool _localResetEditSessionQueued;
    UINT _queuedLocalResetToken;
    bool _focusResetPending;
    bool _activationRequired;
    bool _focusLostToWindowsTextInputHost;
    bool _focusLossDeferPending;
    bool _hasPendingServerCandidate;
    UINT _pendingServerCandidateMsgType;
    std::wstring _pendingServerCandidateString;
    std::deque<CCandidateListUIPresenter *> _pendingCandidatePresenterCleanup;
    std::deque<DeferredKeyDown> _deferredKeyDowns;
    std::deque<DeferredKeyDown> _deferredAppliedPrefix;
    DeferredKeyDown _deferredKeyInFlight;
    bool _hasDeferredKeyInFlight;
    uint64_t _deferredKeyReplayToken;
    uint64_t _nextDeferredKeyReplayToken;
    bool _deferredKeyProjectionValid;
    bool _deferredProjectedImeOpen;
    bool _deferredProjectedPunctuationOpen;
    bool _deferredProjectedDoubleSingleByteOpen;
    size_t _deferredProjectedInputLength;
    std::wstring _deferredProjectedRawInput;
    size_t _deferredProjectedCaret;
    bool _deferredProjectedCandidateActive;
    bool _deferredProjectedUnicodeMode;
    uint64_t _deferredKeyFocusGeneration;
    bool _deferredKeyDrainPosted;
    bool _serverUnavailableFallbackActive;

    // True while the current Backspace hold began inside a composition. The
    // auto-repeats that arrive after that composition is gone must be swallowed
    // instead of falling through to the host and deleting document text (#347).
    // Re-evaluated on every non-repeat Backspace press and cleared on focus
    // changes, thread-focus loss and top-context changes; see KeyRepeatGuard.h
    // for the repeat-bit rule. Deliberately not cleared in
    // ITfThreadMgrEventSink::OnSetFocus: Chromium swaps its document manager on
    // almost every edit, which would disarm the guard mid-hold.
    bool _backspaceHoldArmed;
    DWORD _capsLockTestKeyDownMessageTime;
    bool _capsLockTestKeyDownPending;

    // De-duplicates the passthrough statistics probe: OnTestKeyDown can be
    // called more than once for one key event, and a repeated (virtual key,
    // message time) pair is that same event. GetMessageTime only has tick
    // granularity, so two genuine presses of one key can share a time; the
    // marker is therefore consumed by the first suppression (virtual key 0 is
    // not a key) and the probe pair always arrives adjacently.
    UINT _passthroughStatsVirtualKey;
    LONG _passthroughStatsMessageTime;

    // Bare Shift/Ctrl toggle arming (Weasel-style: release within timeout).
    bool _shiftHotkeyArmed;
    bool _ctrlHotkeyArmed;
    std::chrono::steady_clock::time_point _modifierHotkeyExpire;

    HHOOK _bareShiftHook;
    BYTE _bareShiftDownMask;
    bool _bareShiftArmed;
    UINT _bareShiftSequence;
    UINT _bareShiftHandledSequence;
    uint64_t _bareShiftFocusGeneration;
    ULONGLONG _bareShiftExpireTick;

    LONG _refCount;

    // Support the search integration
    ITfFnSearchCandidateProvider *_pITfFnSearchCandidateProvider;
};
