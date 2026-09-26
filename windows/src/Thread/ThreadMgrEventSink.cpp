#include "Private.h"
#include "fmt/xchar.h"
#include "Globals.h"
#include "MetasequoiaIME.h"
#include "CandidateListUIPresenter.h"
#include "Ipc.h"

//+---------------------------------------------------------------------------
//
// ITfThreadMgrEventSink::OnInitDocumentMgr
//
// Sink called by the framework just before the first context is pushed onto
// a document.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnInitDocumentMgr(_In_ ITfDocumentMgr *pDocMgr)
{
    pDocMgr;
    return E_NOTIMPL;
}

//+---------------------------------------------------------------------------
//
// ITfThreadMgrEventSink::OnUninitDocumentMgr
//
// Sink called by the framework just after the last context is popped off a
// document.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnUninitDocumentMgr(_In_ ITfDocumentMgr *pDocMgr)
{
    pDocMgr;
    return E_NOTIMPL;
}

//+---------------------------------------------------------------------------
//
// ITfThreadMgrEventSink::OnSetFocus
//
// Sink called by the framework when focus changes from one document to
// another.  Either document may be NULL, meaning previously there was no
// focus document, or now no document holds the input focus.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnSetFocus(_In_ ITfDocumentMgr *pDocMgrFocus, _In_ ITfDocumentMgr * /*pDocMgrPrevFocus*/)
{
    if (!IsNamedpipeFocusStateOwner(this))
    {
        return S_OK;
    }

    const bool documentChanged = (pDocMgrFocus == nullptr) != (_pDocMgrLastFocused == nullptr) ||
                                 (pDocMgrFocus && !_IsSameComObject(pDocMgrFocus, _pDocMgrLastFocused));
    if (documentChanged)
    {
        // The badge belongs to the document it was shown for, even when
        // editors share an HWND (or expose none). This hides only the badge;
        // candidate and input state follow their own focus rules below.
        SendHideCaretStateEventToUIProcessViaNamedPipe();
    }

    if (pDocMgrFocus && _pContext && _IsComposing() && !_localSessionResetPending.load(std::memory_order_acquire))
    {
        ITfDocumentMgr *compositionDocumentMgr = nullptr;
        const HRESULT ownerHr = _pContext->GetDocumentMgr(&compositionDocumentMgr);
        if (FAILED(ownerHr) || compositionDocumentMgr == nullptr ||
            !_IsSameComObject(compositionDocumentMgr, pDocMgrFocus))
        {
            _DebugCompositionRecovery(L"focus-document-mismatch", ownerHr);
            MarkNamedpipeSessionDirtyForOwner(this);
        }
        if (compositionDocumentMgr)
        {
            compositionDocumentMgr->Release();
        }
    }

    // Chrome / Twitter contenteditable swaps ITfDocumentMgr on almost every
    // edit (digit passthrough, Backspace, etc.): both pointers are non-null and
    // unequal. Resetting smart-punctuation there drops a just-armed ASCII
    // rejection before the user can retype. The reject spot is already keyed by
    // (punct key + preceding char); other keys / caret moves clear it instead.
    const bool windowsTextInputHostTransition =
        _focusLostToWindowsTextInputHost || _CaptureWindowsTextInputHostFocusLoss();

    if (pDocMgrFocus && _focusLossDeferPending)
    {
        // Focus returned within the deferral window: Excel (and other apps)
        // briefly route document focus through NULL when entering an in-app
        // editor (e.g. cell edit mode) without actually leaving the app.
        // Cancel the deferral so the client session and its candidate window
        // survive the first keystroke.
        KillTimer(_msgWndHandle, TIMER_DEFERRED_FOCUS_LOSS);
        _focusLossDeferPending = false;
    }

    // bcaf34e used document focus as the authoritative reconnect boundary.
    // The modern protocol preserves that behavior with a new activation epoch
    // on the existing healthy transport; EnsureNamedpipeFocusSessionActivated
    // physically reopens a channel as well when its health check fails.
    if (pDocMgrFocus && (!Global::g_connected || windowsTextInputHostTransition))
    {
        // A genuine focus-session handover starts here: the activation token
        // that the pending smart-punctuation action was armed under is about
        // to be replaced, so the action must not survive into the new session.
        // The frequent Chromium document swap below keeps the token and must
        // leave the action alone.
        _ClearSmartPunctuationAction();
        Global::g_connected = true;
        _workerCommitReady.store(false, std::memory_order_release);
        RequireNamedpipeFocusActivation();
        PostMessage(_msgWndHandle, WM_ConnectNamedpipe, 0, 0);
        _focusLostToWindowsTextInputHost = false;
    }
    else if (!pDocMgrFocus && Global::g_connected)
    {
        if (!_focusLossDeferPending)
        {
            // Defer the disconnect: an immediate suspension here hides the
            // candidate window that the first keystroke just displayed when
            // Excel enters cell-edit mode (transient NULL document focus).
            // If focus does not return within FOCUS_LOSS_DEFER_MS, the timer
            // performs the normal MarkNamedpipeFocusLost + disconnect.
            _focusLossDeferPending = true;
            SetTimer(_msgWndHandle, TIMER_DEFERRED_FOCUS_LOSS, FOCUS_LOSS_DEFER_MS, nullptr);
        }
    }

    if (_msgWndHandle && IsWindow(_msgWndHandle))
    {
        if (pDocMgrFocus)
        {
            // Regaining document focus does not go through OnSetThreadFocus
            // when the switch stays on this thread (two windows of one
            // Chromium host), yet another process' client may have published
            // its own mode to the single toolbar during the gap. Restate ours.
            // Re-arming on every callback collapses the burst into one packet.
            SetTimer(_msgWndHandle, TIMER_FOCUS_STATUS_RESEND, FOCUS_STATUS_RESEND_DELAY_MS, nullptr);
        }
        else
        {
            // Thread focus outlives document focus by roughly one deferral, so
            // a pending resend would otherwise claim ownership on the way out.
            KillTimer(_msgWndHandle, TIMER_FOCUS_STATUS_RESEND);
        }
    }

    _InitTextEditSink(pDocMgrFocus);

    _UpdateLanguageBarOnSetFocus(pDocMgrFocus);

    //
    // We have to hide/unhide candidate list depending on whether they are
    // associated with pDocMgrFocus.
    //
    if (_pCandidateListUIPresenter)
    {
        ITfDocumentMgr *pCandidateListDocumentMgr = nullptr;
        ITfContext *pTfContext = _pCandidateListUIPresenter->_GetContextDocument();
        if ((nullptr != pTfContext) && SUCCEEDED(pTfContext->GetDocumentMgr(&pCandidateListDocumentMgr)))
        {
            if (_IsSameComObject(pCandidateListDocumentMgr, pDocMgrFocus))
            {
                _pCandidateListUIPresenter->OnSetThreadFocus();
            }
            else
            {
                _pCandidateListUIPresenter->OnKillThreadFocus();
            }

            pCandidateListDocumentMgr->Release();
        }
    }

    if (_pDocMgrLastFocused)
    {
        _pDocMgrLastFocused->Release();
        _pDocMgrLastFocused = nullptr;
    }

    _pDocMgrLastFocused = pDocMgrFocus;

    if (_pDocMgrLastFocused)
    {
        _pDocMgrLastFocused->AddRef();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfThreadMgrEventSink::OnPushContext
//
// Sink called by the framework when a context is pushed.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnPushContext(_In_ ITfContext *pContext)
{
    _HandleFocusedContextStackChange(pContext);
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfThreadMgrEventSink::OnPopContext
//
// Sink called by the framework when a context is popped.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnPopContext(_In_ ITfContext *pContext)
{
    _HandleFocusedContextStackChange(pContext);
    return S_OK;
}

void CMetasequoiaIME::_HandleFocusedContextStackChange(_In_opt_ ITfContext *changedContext)
{
    if (!IsNamedpipeFocusStateOwner(this) || changedContext == nullptr || _pThreadMgr == nullptr)
    {
        return;
    }

    ITfDocumentMgr *changedDocument = nullptr;
    ITfDocumentMgr *focusedDocument = nullptr;
    if (FAILED(changedContext->GetDocumentMgr(&changedDocument)) || changedDocument == nullptr ||
        FAILED(_pThreadMgr->GetFocus(&focusedDocument)) || focusedDocument == nullptr ||
        !_IsSameComObject(changedDocument, focusedDocument))
    {
        if (changedDocument)
        {
            changedDocument->Release();
        }
        if (focusedDocument)
        {
            focusedDocument->Release();
        }
        return;
    }

    ITfContext *newTopContext = nullptr;
    if (FAILED(focusedDocument->GetTop(&newTopContext)) || newTopContext == nullptr)
    {
        focusedDocument->Release();
        changedDocument->Release();
        return;
    }
    const bool topContextChanged = !_IsSameComObject(newTopContext, _pTextEditSinkContext);
    newTopContext->Release();
    if (!topContextChanged)
    {
        focusedDocument->Release();
        changedDocument->Release();
        return;
    }

    // Capture the old composition owner before rebinding the text-edit sink.
    // A pushed modal/context (or a popped editor context) must never inherit
    // replies or queued edit sessions that belonged to the previous top.
    ITfContext *resetContext = _pContext ? _pContext : _pTextEditSinkContext;
    if (resetContext)
    {
        resetContext->AddRef();
    }
    else
    {
        resetContext = changedContext;
        resetContext->AddRef();
    }

    _ClearDeferredKeyDowns();
    SendHideCaretStateEventToUIProcessViaNamedPipe();
    // The pending smart-punctuation action was armed for the old top context;
    // the edit session that would consume it is about to be invalidated too.
    _ClearSmartPunctuationAction();
    MarkNamedpipeSessionDirtyForOwner(this);
    const UINT resetToken = _localSessionResetToken.load(std::memory_order_acquire);
    _RequestLocalSessionReset(resetContext, resetToken);
    _InitTextEditSink(focusedDocument);

    resetContext->Release();
    focusedDocument->Release();
    changedDocument->Release();
}

//+---------------------------------------------------------------------------
//
// _InitThreadMgrEventSink
//
// Advise our sink.
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_InitThreadMgrEventSink()
{
    ITfSource *pSource = nullptr;
    BOOL ret = FALSE;

    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfSource, (void **)&pSource)))
    {
        return ret;
    }

    if (FAILED(
            pSource->AdviseSink(IID_ITfThreadMgrEventSink, (ITfThreadMgrEventSink *)this, &_threadMgrEventSinkCookie)))
    {
        _threadMgrEventSinkCookie = TF_INVALID_COOKIE;
        goto Exit;
    }

    ret = TRUE;

Exit:
    pSource->Release();
    return ret;
}

//+---------------------------------------------------------------------------
//
// _UninitThreadMgrEventSink
//
// Unadvise our sink.
//----------------------------------------------------------------------------

void CMetasequoiaIME::_UninitThreadMgrEventSink()
{
    ITfSource *pSource = nullptr;

    if (_threadMgrEventSinkCookie == TF_INVALID_COOKIE)
    {
        return;
    }

    if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfSource, (void **)&pSource)))
    {
        pSource->UnadviseSink(_threadMgrEventSinkCookie);
        pSource->Release();
    }

    _threadMgrEventSinkCookie = TF_INVALID_COOKIE;
}
