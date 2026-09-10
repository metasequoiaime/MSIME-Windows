#include "Private.h"
#include "Globals.h"
#include "MetasequoiaIME.h"
#include "CompositionProcessorEngine.h"
#include <cwctype>
#include <debugapi.h>
#include <fmt/xchar.h>
#include <string>
#include "FanyDefines.h"
#include "Ipc.h"
#include "../Utils/PerfTimer.h"

namespace
{
// Keep SEH helpers free of C++ objects with destructors (C2712).
HRESULT SafeRangeSetText(_In_ ITfRange *range, TfEditCookie ec, DWORD flags, _In_reads_opt_(len) const WCHAR *text,
                         LONG len)
{
    if (range == nullptr)
    {
        return E_INVALIDARG;
    }

    __try
    {
        return range->SetText(ec, flags, text, len);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return E_FAIL;
    }
}

HRESULT SafeRangeGetText(_In_ ITfRange *range, TfEditCookie ec, DWORD flags, _Out_writes_(len) WCHAR *text, ULONG len,
                         _Out_ ULONG *fetched)
{
    if (range == nullptr || text == nullptr || fetched == nullptr || len == 0)
    {
        return E_INVALIDARG;
    }

    __try
    {
        return range->GetText(ec, flags, text, len, fetched);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *fetched = 0;
        return E_FAIL;
    }
}

HRESULT SafeRangeShiftStart(_In_ ITfRange *range, TfEditCookie ec, LONG count, _Out_ LONG *shifted)
{
    if (range == nullptr || shifted == nullptr)
    {
        return E_INVALIDARG;
    }

    __try
    {
        return range->ShiftStart(ec, count, shifted, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *shifted = 0;
        return E_FAIL;
    }
}
} // namespace

WCHAR CMetasequoiaIME::_GetPrecedingDocumentChar(TfEditCookie ec, _In_ ITfContext *pContext)
{
    if (pContext == nullptr)
    {
        return 0;
    }

    ITfRange *pAnchor = nullptr;
    bool releaseAnchor = false;

    if (_IsComposing() && _pComposition != nullptr)
    {
        if (FAILED(_pComposition->GetRange(&pAnchor)) || pAnchor == nullptr)
        {
            return 0;
        }
        releaseAnchor = true;
    }
    else
    {
        TF_SELECTION tfSelection = {};
        ULONG fetched = 0;
        const HRESULT hr = pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched);
        if (FAILED(hr) || fetched != 1 || tfSelection.range == nullptr)
        {
            return 0;
        }
        pAnchor = tfSelection.range;
        releaseAnchor = true;
    }

    ITfRange *pClone = nullptr;
    WCHAR preceding = 0;
    HRESULT hr = pAnchor->Clone(&pClone);
    if (SUCCEEDED(hr) && pClone != nullptr)
    {
        hr = pClone->Collapse(ec, TF_ANCHOR_START);
        if (SUCCEEDED(hr))
        {
            LONG shifted = 0;
            hr = SafeRangeShiftStart(pClone, ec, -1, &shifted);
            if (SUCCEEDED(hr) && shifted == -1)
            {
                // Terminals and other shallow text stores accept the shift but
                // expose no text, leaving preceding at 0.
                WCHAR buffer[2] = {};
                ULONG fetched = 0;
                hr = SafeRangeGetText(pClone, ec, 0, buffer, 1, &fetched);
                if (SUCCEEDED(hr) && fetched == 1)
                {
                    preceding = buffer[0];
                }
            }
        }
        pClone->Release();
    }

    if (releaseAnchor && pAnchor != nullptr)
    {
        pAnchor->Release();
    }
    return preceding;
}

WCHAR CMetasequoiaIME::_GetPrecedingCharForSmartPunctuation(TfEditCookie ec, _In_ ITfContext *pContext)
{
    if (_smartPunctuationShadowValid)
    {
        return _smartPunctuationShadowChar;
    }
    return _GetPrecedingDocumentChar(ec, pContext);
}

void CMetasequoiaIME::_ResetSmartPunctuationHistory()
{
    _smartPunctuationKey = 0;
    _smartPunctuationPrecedingChar = 0;
    _smartPunctuationCommittedAscii = false;
    _smartPunctuationAsciiRejected = false;
    _smartPunctuationCommitTick = 0;
    _smartPunctuationFocusToken = 0;
    _smartPunctuationForegroundWindow = nullptr;
}

bool CMetasequoiaIME::_QueueRepeatedSmartPunctuationReplacement(WCHAR wch)
{
    // Backspace rejection means the ASCII form is already gone. Treating the
    // next press as "replace the still-visible ASCII punct" would SendInput a
    // Backspace into the preceding character instead.
    if (!_smartPunctuationCommittedAscii || _smartPunctuationAsciiRejected || _smartPunctuationKey != wch ||
        _smartPunctuationCommitTick == 0 || _msgWndHandle == nullptr || _pCompositionProcessorEngine == nullptr ||
        _IsComposing() || _candidateMode != CANDIDATE_NONE ||
        !Global::SmartPunctuationEnabled.load(std::memory_order_relaxed) ||
        !Global::SmartPunctuationRepeatToChineseEnabled.load(std::memory_order_relaxed))
    {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (now - _smartPunctuationCommitTick > SMART_PUNCTUATION_REPEAT_INTERVAL_MS ||
        !_IsFocusSessionCurrent(_smartPunctuationFocusToken) ||
        GetForegroundWindow() != _smartPunctuationForegroundWindow)
    {
        return false;
    }

    const WCHAR *chinese = _pCompositionProcessorEngine->GetPunctuation(wch);
    if (chinese == nullptr || chinese[0] == L'\0' || chinese[1] != L'\0')
    {
        return false;
    }

    _pendingSmartPunctuationReplacement = chinese[0];
    _pendingSmartPunctuationFocusToken = _smartPunctuationFocusToken;
    _pendingSmartPunctuationForegroundWindow = _smartPunctuationForegroundWindow;
    _pendingSmartPunctuationDeadline = _smartPunctuationCommitTick + SMART_PUNCTUATION_REPEAT_INTERVAL_MS;

    const uint64_t focusToken = _pendingSmartPunctuationFocusToken;
    if (!PostMessage(_msgWndHandle, WM_ReplaceRepeatedSmartPunctuation, static_cast<WPARAM>(focusToken & 0xFFFFFFFFULL),
                     static_cast<LPARAM>((focusToken >> 32) & 0xFFFFFFFFULL)))
    {
        _pendingSmartPunctuationReplacement = 0;
        _pendingSmartPunctuationFocusToken = 0;
        _pendingSmartPunctuationForegroundWindow = nullptr;
        _pendingSmartPunctuationDeadline = 0;
        return false;
    }

    _ResetSmartPunctuationHistory();
    return true;
}

void CMetasequoiaIME::_InvalidateSmartPunctuationShadow()
{
    _smartPunctuationShadowChar = 0;
    _smartPunctuationShadowValid = false;
}

void CMetasequoiaIME::_UpdateSmartPunctuationShadow(UINT code, WCHAR wch, bool isEaten)
{
    switch (code)
    {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_CAPITAL:
        // Modifier presses edit nothing, so the shadow still describes the caret.
        return;
    case VK_BACK:
    case VK_DELETE:
    case VK_INSERT:
    case VK_RETURN:
    case VK_TAB:
    case VK_ESCAPE:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
        _InvalidateSmartPunctuationShadow();
        return;
    default:
        break;
    }

    if (CCompositionProcessorEngine::IsSmartAsciiPunctuationKey(wch))
    {
        // _ResolveSmartPunctuation needs the current shadow to decide the form,
        // and records whatever it commits once that decision is made.
        return;
    }

    if (isEaten || wch == 0 || std::iswprint(static_cast<wint_t>(wch)) == 0)
    {
        // Eaten keys feed the composition and reach the document as committed
        // text, which even a proxy store exposes, so let the document answer.
        _InvalidateSmartPunctuationShadow();
        return;
    }

    _smartPunctuationShadowChar = wch;
    _smartPunctuationShadowValid = true;
}

void CMetasequoiaIME::_NoteKeyForSmartPunctuation(UINT code, WCHAR wch, bool isEaten)
{
    // The replacement message normally runs before another input event. If it
    // does not, never let a later key leave the queued Backspace targeting an
    // unrelated character.
    if (_pendingSmartPunctuationReplacement != 0)
    {
        _pendingSmartPunctuationReplacement = 0;
        _pendingSmartPunctuationFocusToken = 0;
        _pendingSmartPunctuationForegroundWindow = nullptr;
        _pendingSmartPunctuationDeadline = 0;
    }

    _UpdateSmartPunctuationShadow(code, wch, isEaten);

    if (_smartPunctuationKey == 0)
    {
        return;
    }

    switch (code)
    {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_CAPITAL:
        // ':' needs Shift; modifier presses are not edits.
        return;
    case VK_BACK:
        // Only deleting the ASCII form says that form was unwanted. Deleting
        // the Chinese punctuation that replaced it must not undo the rejection.
        if (_smartPunctuationCommittedAscii)
        {
            _smartPunctuationAsciiRejected = true;
            // ASCII punct is gone; disarm repeat-to-Chinese replacement so a
            // quick retype takes the reject path instead of SendInput(VK_BACK).
            _smartPunctuationCommitTick = 0;
            _smartPunctuationFocusToken = 0;
            _smartPunctuationForegroundWindow = nullptr;
            // UpdateShadow already cleared the punctuation shadow. Restore the
            // preceding character recorded at commit so a retype can still match
            // the reject spot when the host text store cannot re-read it.
            if (_smartPunctuationPrecedingChar != 0)
            {
                _smartPunctuationShadowChar = _smartPunctuationPrecedingChar;
                _smartPunctuationShadowValid = true;
            }
        }
        return;
    case VK_DECIMAL:
        // Numpad '.' bypasses smart punctuation entirely.
        _ResetSmartPunctuationHistory();
        return;
    default:
        break;
    }

    if (wch != _smartPunctuationKey)
    {
        _ResetSmartPunctuationHistory();
    }
}

std::wstring CMetasequoiaIME::_ResolveSmartPunctuation(WCHAR wch, WCHAR precedingChar)
{
    if (_pCompositionProcessorEngine == nullptr)
    {
        return {};
    }

    const bool smartEnabled = Global::SmartPunctuationEnabled.load(std::memory_order_relaxed);
    std::wstring resolved = _pCompositionProcessorEngine->ResolvePunctuation(wch, precedingChar);
    if (!CCompositionProcessorEngine::IsSmartAsciiPunctuationKey(wch) || !smartEnabled)
    {
        _ResetSmartPunctuationHistory();
        return resolved;
    }

    bool committedAscii = resolved.size() == 1 && resolved[0] == wch;
    // The rejection is sticky for as long as this spot survives, so repeated
    // delete/retype cycles keep producing Chinese punctuation.
    const bool asciiRejected = _smartPunctuationAsciiRejected && _smartPunctuationKey == wch &&
                               _smartPunctuationPrecedingChar == precedingChar;
    if (asciiRejected && committedAscii)
    {
        const WCHAR *chinese = _pCompositionProcessorEngine->GetPunctuation(wch);
        if (chinese != nullptr && *chinese != L'\0')
        {
            resolved.assign(chinese);
            committedAscii = false;
        }
    }

    _smartPunctuationKey = wch;
    _smartPunctuationPrecedingChar = precedingChar;
    _smartPunctuationCommittedAscii = committedAscii;
    _smartPunctuationAsciiRejected = asciiRejected;
    if (committedAscii)
    {
        _smartPunctuationCommitTick = GetTickCount64();
        _smartPunctuationFocusToken = _CaptureFocusSessionToken();
        _smartPunctuationForegroundWindow = GetForegroundWindow();
    }
    else
    {
        _smartPunctuationCommitTick = 0;
        _smartPunctuationFocusToken = 0;
        _smartPunctuationForegroundWindow = nullptr;
    }
    if (!resolved.empty())
    {
        _smartPunctuationShadowChar = resolved.back();
        _smartPunctuationShadowValid = true;
    }
    return resolved;
}

//+---------------------------------------------------------------------------
//
// ITfCompositionSink::OnCompositionTerminated
//
// Callback for ITfCompositionSink.  The system calls this method whenever
// someone other than this service ends a composition.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnCompositionTerminated(TfEditCookie ecWrite, _In_ ITfComposition *pComposition)
{
    PerfTimer timer;
    if (pComposition == nullptr || !_IsCompositionCurrent(pComposition))
    {
        DebugTsfIssue47(L"host-terminated-stale-composition", FANY_IME_NO_REQUEST_ID, 0, L'\0', 0, 0, -1,
                        _IsComposing(),
                        _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_FALSE,
                        _CaptureCompositionEpoch());
        // A delayed termination callback for an older composition must never
        // delete the current candidate presenter or release newer ownership.
        return S_OK;
    }

    DebugTsfIssue47(L"host-terminated-current-composition", FANY_IME_NO_REQUEST_ID, 0, L'\0', 0, 0, -1, TRUE,
                    _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_OK,
                    _CaptureCompositionEpoch());

    // The callback already carries a write cookie and the host has already
    // ended this exact composition. Detach ownership before making COM calls,
    // so a re-entrant/stale callback cannot observe it as current or tear down
    // a composition created later.
    ITfComposition *terminatedComposition = pComposition;
    terminatedComposition->AddRef();
    _pComposition->Release();
    _pComposition = nullptr;
    _voiceCompositionActive = false;

    ITfContext *ownerContext = _pContext;
    if (ownerContext)
    {
        ownerContext->AddRef();
        _pContext->Release();
        _pContext = nullptr;
    }

    uint64_t nextEpoch = _compositionEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (nextEpoch == 0)
    {
        _compositionEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    // Detach and end the old candidate/session before the COM cleanup calls
    // below can re-enter and create a presenter for a newer composition.
    _DeleteCandidateList(FALSE, ownerContext);
    if (Global::g_connected)
    {
        // EndCandidateUiSession is intentionally idempotent, but a presenter
        // can exist before its UI session becomes active. Always send one
        // exact routed clear so the Server cannot retain that composition.
        SendHideCandidateWndEventToUIProcess();
    }

    // Do NOT SetText(empty) here. Cancel paths already wipe via
    // _HandleCancel → _RemoveDummyCompositionForComposing. Wiping again after
    // a normal commit/EndComposition can delete the just-committed text and
    // destabilize fragile hosts (notably QQ).
    if (ownerContext)
    {
        _ClearCompositionDisplayAttributes(ecWrite, ownerContext, terminatedComposition);
    }
    terminatedComposition->Release();

    if (ownerContext)
    {
        ownerContext->Release();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _IsComposing
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_IsComposing()
{
    return _pComposition != nullptr;
}

//+---------------------------------------------------------------------------
//
// _SetComposition
//
//----------------------------------------------------------------------------

void CMetasequoiaIME::_SetComposition(_In_ ITfComposition *pComposition)
{
    _pComposition = pComposition;
    uint64_t nextEpoch = _compositionEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (nextEpoch == 0)
    {
        _compositionEpoch.fetch_add(1, std::memory_order_acq_rel);
    }
}

//+---------------------------------------------------------------------------
//
// _AddComposingAndChar
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_AddComposingAndChar(TfEditCookie ec, _In_ ITfContext *pContext,
                                              _In_ CStringRange *pstrAddString)
{
    HRESULT hr = S_OK;

    if (_pComposition != nullptr)
    {
        PerfTimer fastUpdateTimer;
        ITfRange *pRangeComposition = nullptr;
        hr = _pComposition->GetRange(&pRangeComposition);
        if (SUCCEEDED(hr) && pRangeComposition != nullptr)
        {
            PerfTimer setTextTimer;
            hr = SafeRangeSetText(pRangeComposition, ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
            double setTextElapsedMs = setTextTimer.ElapsedMs();
            if (SUCCEEDED(hr))
            {
                PerfTimer displayAttrTimer;
                _SetCompositionDisplayAttributesForRange(ec, pContext, pRangeComposition, _gaDisplayAttributeInput);
                double displayAttrElapsedMs = displayAttrTimer.ElapsedMs();

                PerfTimer selectionTimer;
                TF_SELECTION sel;
                pRangeComposition->Collapse(ec, TF_ANCHOR_END);
                sel.range = pRangeComposition;
                sel.style.ase = TF_AE_NONE;
                sel.style.fInterimChar = FALSE;
                pContext->SetSelection(ec, 1, &sel);
                double selectionElapsedMs = selectionTimer.ElapsedMs();

                pRangeComposition->Release();
                return hr;
            }
            pRangeComposition->Release();
        }
    }

    ULONG fetched = 0;
    TF_SELECTION tfSelection;

    if (pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched) != S_OK || fetched == 0)
        return S_FALSE;

    //
    // make range start to selection
    //
    ITfRange *pAheadSelection = nullptr;
    hr = pContext->GetStart(ec, &pAheadSelection);
    if (SUCCEEDED(hr))
    {
        hr = pAheadSelection->ShiftEndToRange(ec, tfSelection.range, TF_ANCHOR_START);
        if (SUCCEEDED(hr))
        {
            ITfRange *pRange = nullptr;
            BOOL exist_composing = _FindComposingRange(ec, pContext, pAheadSelection, &pRange);

            std::wstring strAddString(pstrAddString->Get(), pstrAddString->GetLength());

            _SetInputString(ec, pContext, pRange, pstrAddString, exist_composing);

            if (pRange)
            {
                pRange->Release();
            }
        }
    }

    tfSelection.range->Release();

    if (pAheadSelection)
    {
        pAheadSelection->Release();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _AddCharAndFinalize
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_AddCharAndFinalize(TfEditCookie ec, _In_ ITfContext *pContext,
                                             _In_ CStringRange *pstrAddString)
{
    HRESULT hr = E_FAIL;
    PerfTimer timer;

    if (_pComposition != nullptr)
    {
        PerfTimer directSetTimer;
        hr = _SetCompositionTextAndSelection(ec, pContext, pstrAddString);
        double directSetElapsedMs = directSetTimer.ElapsedMs();
        if (SUCCEEDED(hr))
        {
            return hr;
        }
    }

    ULONG fetched = 0;
    TF_SELECTION tfSelection;

    if ((hr = pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched)) != S_OK || fetched != 1)
        return hr;

    // We use SetText here instead of InsertTextAtSelection because we've already started a composition
    // We don't want to the app to adjust the insertion point inside our composition
    PerfTimer setTextTimer;
    hr = SafeRangeSetText(tfSelection.range, ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
    double setTextElapsedMs = setTextTimer.ElapsedMs();
    double setSelectionElapsedMs = 0;
    if (hr == S_OK)
    {
        // Update the selection, we'll make it an insertion point just past
        // the inserted text.
        tfSelection.range->Collapse(ec, TF_ANCHOR_END);
        PerfTimer setSelectionTimer;
        pContext->SetSelection(ec, 1, &tfSelection);
        setSelectionElapsedMs = setSelectionTimer.ElapsedMs();
    }

    tfSelection.range->Release();

    return hr;
}

HRESULT CMetasequoiaIME::_InsertTextToComposition(TfEditCookie ec, _In_ ITfContext *pContext,
                                                  _In_ CStringRange *pstrAddString)
{
    PerfTimer timer;
    if (_pComposition == nullptr)
    {
        return E_FAIL;
    }

    ITfRange *pRangeComposition = nullptr;
    PerfTimer getRangeTimer;
    HRESULT hr = _pComposition->GetRange(&pRangeComposition);
    double getRangeElapsedMs = getRangeTimer.ElapsedMs();
    if (FAILED(hr) || pRangeComposition == nullptr)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    PerfTimer setTextTimer;
    hr = SafeRangeSetText(pRangeComposition, ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
    double setTextElapsedMs = setTextTimer.ElapsedMs();
    double setSelectionElapsedMs = 0;
    if (SUCCEEDED(hr))
    {
        TF_SELECTION tfSelection;
        pRangeComposition->Collapse(ec, TF_ANCHOR_END);
        tfSelection.range = pRangeComposition;
        tfSelection.style.ase = TF_AE_NONE;
        tfSelection.style.fInterimChar = FALSE;
        PerfTimer setSelectionTimer;
        pContext->SetSelection(ec, 1, &tfSelection);
        setSelectionElapsedMs = setSelectionTimer.ElapsedMs();
    }

    pRangeComposition->Release();
    return hr;
}

//+---------------------------------------------------------------------------
//
// _SetCompositionTextAndSelection
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_SetCompositionTextAndSelection(TfEditCookie ec, _In_ ITfContext *pContext,
                                                         _In_ CStringRange *pstrAddString)
{
    PerfTimer timer;
    if (_pComposition == nullptr)
    {
        return E_FAIL;
    }

    ITfRange *pRangeComposition = nullptr;
    PerfTimer getRangeTimer;
    HRESULT hr = _pComposition->GetRange(&pRangeComposition);
    double getRangeElapsedMs = getRangeTimer.ElapsedMs();
    if (FAILED(hr) || pRangeComposition == nullptr)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    PerfTimer setTextTimer;
    hr = SafeRangeSetText(pRangeComposition, ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
    double setTextElapsedMs = setTextTimer.ElapsedMs();
    double setSelectionElapsedMs = 0;
    if (SUCCEEDED(hr))
    {
        TF_SELECTION tfSelection;
        pRangeComposition->Collapse(ec, TF_ANCHOR_END);
        tfSelection.range = pRangeComposition;
        tfSelection.style.ase = TF_AE_NONE;
        tfSelection.style.fInterimChar = FALSE;
        PerfTimer setSelectionTimer;
        pContext->SetSelection(ec, 1, &tfSelection);
        setSelectionElapsedMs = setSelectionTimer.ElapsedMs();
    }

    pRangeComposition->Release();
    return hr;
}

//+---------------------------------------------------------------------------
//
// _FindComposingRange
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_FindComposingRange(TfEditCookie ec, _In_ ITfContext *pContext, _In_ ITfRange *pSelection,
                                          _Outptr_result_maybenull_ ITfRange **ppRange)
{
    if (ppRange == nullptr)
    {
        return FALSE;
    }

    *ppRange = nullptr;

    // find GUID_PROP_COMPOSING
    ITfProperty *pPropComp = nullptr;
    IEnumTfRanges *enumComp = nullptr;

    HRESULT hr = pContext->GetProperty(GUID_PROP_COMPOSING, &pPropComp);
    if (FAILED(hr) || pPropComp == nullptr)
    {
        return FALSE;
    }

    hr = pPropComp->EnumRanges(ec, &enumComp, pSelection);
    if (FAILED(hr) || enumComp == nullptr)
    {
        pPropComp->Release();
        return FALSE;
    }

    BOOL isCompExist = FALSE;
    VARIANT var;
    ULONG fetched = 0;

    while (enumComp->Next(1, ppRange, &fetched) == S_OK && fetched == 1)
    {
        hr = pPropComp->GetValue(ec, *ppRange, &var);
        if (hr == S_OK)
        {
            if (var.vt == VT_I4 && var.lVal != 0)
            {
                isCompExist = TRUE;
                break;
            }
        }
        (*ppRange)->Release();
        *ppRange = nullptr;
    }

    pPropComp->Release();
    enumComp->Release();

    return isCompExist;
}

//+---------------------------------------------------------------------------
//
// _SetInputString
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_SetInputString(TfEditCookie ec, _In_ ITfContext *pContext, _Out_opt_ ITfRange *pRange,
                                         _In_ CStringRange *pstrAddString, BOOL exist_composing)
{
    ITfRange *pRangeInsert = nullptr;
    if (!exist_composing)
    {
        _InsertAtSelection(ec, pContext, pstrAddString, &pRangeInsert);
        if (pRangeInsert == nullptr)
        {
            return S_OK;
        }
        else
        {
            // pRange = pRangeInsert;

            /* To make TsfPad work, we need to get range manually */
            _pComposition->GetRange(&pRange);
        }
    }
    if (pRange != nullptr)
    {
        SafeRangeSetText(pRange, ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
    }

    _SetCompositionLanguage(ec, pContext);

    _SetCompositionDisplayAttributes(ec, pContext, _gaDisplayAttributeInput);

    // update the selection, we'll make it an insertion point just past
    // the inserted text.
    ITfRange *pSelection = nullptr;
    TF_SELECTION sel;

    if ((pRange != nullptr) && (pRange->Clone(&pSelection) == S_OK))
    {
        pSelection->Collapse(ec, TF_ANCHOR_END);

        sel.range = pSelection;
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &sel);
        pSelection->Release();
    }

    if (pRangeInsert)
    {
        pRangeInsert->Release();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _InsertAtSelection
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_InsertAtSelection(TfEditCookie ec, _In_ ITfContext *pContext,
                                            _In_ CStringRange *pstrAddString, _Outptr_ ITfRange **ppCompRange)
{
    ITfRange *rangeInsert = nullptr;
    ITfInsertAtSelection *pias = nullptr;
    HRESULT hr = S_OK;

    if (ppCompRange == nullptr)
    {
        hr = E_INVALIDARG;
        goto Exit;
    }

    *ppCompRange = nullptr;

    hr = pContext->QueryInterface(IID_ITfInsertAtSelection, (void **)&pias);
    if (FAILED(hr))
    {
        goto Exit;
    }

    hr = pias->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, pstrAddString->Get(), (LONG)pstrAddString->GetLength(),
                                     &rangeInsert);

    if (FAILED(hr) || rangeInsert == nullptr)
    {
        rangeInsert = nullptr;
        pias->Release();
        goto Exit;
    }

    *ppCompRange = rangeInsert;
    pias->Release();
    hr = S_OK;

Exit:
    return hr;
}

//+---------------------------------------------------------------------------
//
// _RemoveDummyCompositionForComposing
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_RemoveDummyCompositionForComposing(TfEditCookie ec, _In_ ITfComposition *pComposition)
{
    HRESULT hr = S_OK;

    ITfRange *pRange = nullptr;

    if (pComposition)
    {
        hr = pComposition->GetRange(&pRange);
        if (SUCCEEDED(hr))
        {
            hr = SafeRangeSetText(pRange, ec, 0, nullptr, 0);
            pRange->Release();
        }
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// _SetCompositionLanguage
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_SetCompositionLanguage(TfEditCookie ec, _In_ ITfContext *pContext)
{
    HRESULT hr = S_OK;
    BOOL ret = TRUE;

    CCompositionProcessorEngine *pCompositionProcessorEngine = nullptr;
    pCompositionProcessorEngine = _pCompositionProcessorEngine;

    LANGID langidProfile = 0;
    pCompositionProcessorEngine->GetLanguageProfile(&langidProfile);

    ITfRange *pRangeComposition = nullptr;
    ITfProperty *pLanguageProperty = nullptr;

    // we need a range and the context it lives in
    hr = _pComposition->GetRange(&pRangeComposition);
    if (FAILED(hr))
    {
        ret = FALSE;
        goto Exit;
    }

    // get our the language property
    hr = pContext->GetProperty(GUID_PROP_LANGID, &pLanguageProperty);
    if (FAILED(hr))
    {
        ret = FALSE;
        goto Exit;
    }

    VARIANT var;
    var.vt = VT_I4; // we're going to set DWORD
    var.lVal = langidProfile;

    hr = pLanguageProperty->SetValue(ec, pRangeComposition, &var);
    if (FAILED(hr))
    {
        ret = FALSE;
        goto Exit;
    }

    pLanguageProperty->Release();
    pRangeComposition->Release();

Exit:
    return ret;
}
