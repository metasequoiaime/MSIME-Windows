#include "Private.h"
#include "Globals.h"
#include "EditSession.h"
#include "MetasequoiaIME.h"
#include "CandidateListUIPresenter.h"
#include "CompositionProcessorEngine.h"
#include "MetasequoiaIMEBaseStructure.h"
#include <debugapi.h>
#include <minwindef.h>
#include <string>
#include <fmt/xchar.h>
#include "FanyUtils.h"
#include "Ipc.h"
#include "FanyDefines.h"

namespace
{
thread_local std::wstring g_toggleImeFallbackBuffer;

// The closing half the pressed key would step over, or 0 when the key cannot
// close a tracked pair. Quotes are keyed symmetrically: '"' resolves to either
// half depending on the legacy left/right toggle, so the resolved character
// says nothing about intent and the key itself has to answer.
WCHAR GetPairedPunctuationStepOverCandidate(WCHAR wch, const std::wstring &resolved)
{
    if (resolved.size() != 1)
    {
        return 0;
    }
    if (wch == L'"')
    {
        return L'”';
    }
    if (wch == L'\'')
    {
        return L'’';
    }
    return resolved[0];
}

DWORD_PTR MapRawCaretToPreedit(const CStringRange &raw, DWORD_PTR rawCaret, const std::wstring &preedit,
                               size_t prefixLength)
{
    rawCaret = min(rawCaret, raw.GetLength());
    size_t lettersBeforeCaret = 0;
    for (DWORD_PTR i = 0; i < rawCaret; ++i)
    {
        if (raw.Get()[i] != L'\'')
        {
            ++lettersBeforeCaret;
        }
    }
    size_t displayPosition = min(prefixLength, preedit.size());
    size_t seenLetters = 0;
    while (displayPosition < preedit.size() && seenLetters < lettersBeforeCaret)
    {
        if (preedit[displayPosition] != L'\'')
        {
            ++seenLetters;
        }
        ++displayPosition;
    }
    if (rawCaret > 0 && raw.Get()[rawCaret - 1] == L'\'')
    {
        while (displayPosition < preedit.size() && preedit[displayPosition] == L'\'')
        {
            ++displayPosition;
        }
    }
    return displayPosition;
}
} // namespace

//////////////////////////////////////////////////////////////////////
//
// CMetasequoiaIME class
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// _IsRangeCovered
//
// Returns TRUE if pRangeTest is entirely contained within pRangeCover.
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_IsRangeCovered(TfEditCookie ec, _In_ ITfRange *pRangeTest, _In_ ITfRange *pRangeCover)
{
    LONG lResult = 0;
    ;

    if (FAILED(pRangeCover->CompareStart(ec, pRangeTest, TF_ANCHOR_START, &lResult)) || (lResult > 0))
    {
        return FALSE;
    }

    if (FAILED(pRangeCover->CompareEnd(ec, pRangeTest, TF_ANCHOR_END, &lResult)) || (lResult < 0))
    {
        return FALSE;
    }

    return TRUE;
}

//+---------------------------------------------------------------------------
//
// _DeleteCandidateList
//
//----------------------------------------------------------------------------

VOID CMetasequoiaIME::_DeleteCandidateList(BOOL isForce, _In_opt_ ITfContext *pContext)
{
    pContext;

    CCompositionProcessorEngine *pCompositionProcessorEngine = nullptr;
    pCompositionProcessorEngine = _pCompositionProcessorEngine;
    if (pCompositionProcessorEngine)
    {
        pCompositionProcessorEngine->PurgeVirtualKey();
    }

    if (_pCandidateListUIPresenter)
    {
        CCandidateListUIPresenter *pPresenter = _pCandidateListUIPresenter;
        _pCandidateListUIPresenter = nullptr;
        if (isForce || _msgWndHandle == nullptr)
        {
            delete pPresenter; // destructor calls _EndCandidateList() once
        }
        else
        {
            _ScheduleCandidatePresenterCleanup(pPresenter);
        }

        _candidateMode = CANDIDATE_NONE;
        _isCandidateWithWildcard = FALSE;
    }
}

//+---------------------------------------------------------------------------
//
// _HandleComplete
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleComplete(TfEditCookie ec, _In_ ITfContext *pContext)
{
    g_toggleImeFallbackBuffer.clear();
    _DeleteCandidateList(FALSE, pContext);

    // just terminate the composition
    _TerminateComposition(ec, pContext);

    return S_OK;
}

HRESULT CMetasequoiaIME::_HandleCompleteCommitFirst(TfEditCookie ec, _In_ ITfContext *pContext)
{
    g_toggleImeFallbackBuffer.clear();

    _DeleteCandidateList(FALSE, pContext);

    _TerminateComposition(ec, pContext);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _HandleCancel
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCancel(TfEditCookie ec, _In_ ITfContext *pContext)
{
    g_toggleImeFallbackBuffer.clear();
    GlobalIme::word_for_creating_word = L"";
    GlobalIme::pending_create_word_preedit.clear();
    _RemoveDummyCompositionForComposing(ec, _pComposition);

    _DeleteCandidateList(FALSE, pContext);

    _TerminateComposition(ec, pContext);

    return S_OK;
}

HRESULT CMetasequoiaIME::_HandleToogleIMEMode(TfEditCookie ec, _In_ ITfContext *pContext)
{
    CStringRange keyStrokebuffer = _pCompositionProcessorEngine->GetKeystrokeBuffer();
    std::wstring commitString;

    if (keyStrokebuffer.GetLength())
    {
        commitString.assign(keyStrokebuffer.Get(), keyStrokebuffer.GetLength());
    }
    else if (!g_toggleImeFallbackBuffer.empty())
    {
        commitString = g_toggleImeFallbackBuffer;
    }

    // Claim the reading string before ending composition so a second toggle
    // edit session cannot commit the same text again.
    _pCompositionProcessorEngine->PurgeVirtualKey();
    g_toggleImeFallbackBuffer.clear();

    if (!commitString.empty())
    {
        // Keyboard OPENCLOSE is still open here when Shift deferred the close.
        // Finalize in place; closing first makes CUAS/Win32 EDIT double-insert.
        CStringRange commitStringRange;
        commitStringRange.Set(commitString.c_str(), commitString.length());
        const HRESULT hr = _AddCharAndFinalize(ec, pContext, &commitStringRange);
        if (SUCCEEDED(hr))
        {
            _HandleComplete(ec, pContext);
        }
        else
        {
            _HandleCancel(ec, pContext);
            FanyUtils::SendKeys(commitString);
        }
    }
    else
    {
        _HandleComplete(ec, pContext);
    }

    _pCompositionProcessorEngine->ApplyPendingImeModeAfterCompositionCommit(_GetThreadMgr(), _GetClientId());
    return S_OK;
}

HRESULT CMetasequoiaIME::_HandleInsertText(TfEditCookie ec, _In_ ITfContext *pContext, const std::wstring &text)
{
    if (text.empty())
    {
        return S_OK;
    }

    CStringRange insertString;
    insertString.Set(text.c_str(), text.length());
    HRESULT hr = _AddCharAndFinalize(ec, pContext, &insertString);
    if (FAILED(hr))
    {
        return hr;
    }
    return _HandleCompleteCommitFirst(ec, pContext);
}

HRESULT CMetasequoiaIME::_HandleUpdateVoiceComposition(TfEditCookie ec, _In_ ITfContext *pContext,
                                                       const std::wstring &text)
{
    if (text.empty())
    {
        return S_OK;
    }

    if (!_voiceCompositionActive)
    {
        if (_pCompositionProcessorEngine)
        {
            _pCompositionProcessorEngine->PurgeVirtualKey();
        }
        GlobalIme::word_for_creating_word.clear();
        GlobalIme::pending_create_word_preedit.clear();
        _DeleteCandidateList(FALSE, pContext);
        _voiceCompositionActive = true;
    }

    if (_pComposition == nullptr)
    {
        _StartComposition(pContext);
    }

    CStringRange voiceString;
    voiceString.Set(text.c_str(), text.length());
    return _AddComposingAndChar(ec, pContext, &voiceString);
}

HRESULT CMetasequoiaIME::_HandleCommitVoiceComposition(TfEditCookie ec, _In_ ITfContext *pContext,
                                                       const std::wstring &text)
{
    _voiceCompositionActive = false;
    if (_pCompositionProcessorEngine)
    {
        _pCompositionProcessorEngine->PurgeVirtualKey();
    }
    GlobalIme::word_for_creating_word.clear();
    GlobalIme::pending_create_word_preedit.clear();

    if (text.empty())
    {
        if (_pComposition == nullptr)
        {
            return S_OK;
        }
        return _HandleCompleteCommitFirst(ec, pContext);
    }

    CStringRange commitString;
    commitString.Set(text.c_str(), text.length());
    HRESULT hr = _AddCharAndFinalize(ec, pContext, &commitString);
    if (FAILED(hr))
    {
        return hr;
    }
    return _HandleCompleteCommitFirst(ec, pContext);
}

HRESULT CMetasequoiaIME::_HandleCancelVoiceComposition(TfEditCookie ec, _In_ ITfContext *pContext)
{
    if (!_voiceCompositionActive && _pComposition == nullptr)
    {
        return S_OK;
    }
    _voiceCompositionActive = false;
    if (_pCompositionProcessorEngine)
    {
        _pCompositionProcessorEngine->PurgeVirtualKey();
    }
    return _HandleCancel(ec, pContext);
}

//+---------------------------------------------------------------------------
//
// _HandleCompositionInput
//
// If the keystroke happens within a composition, eat the key and return S_OK.
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCompositionInput(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR wch,
                                                 uint64_t requestId)
{
    HRESULT workerResult = S_OK;
    ITfRange *pRangeComposition = nullptr;
    TF_SELECTION tfSelection;
    ULONG fetched = 0;
    BOOL isCovered = TRUE;

    CCompositionProcessorEngine *pCompositionProcessorEngine = nullptr;
    pCompositionProcessorEngine = _pCompositionProcessorEngine;

    if ((_pCandidateListUIPresenter != nullptr) && (_candidateMode != CANDIDATE_INCREMENTAL))
    {
        _HandleCompositionFinalize(ec, pContext, FALSE);
    }

    // Start the new (std::nothrow) compositon if there is no composition.
    if (!_IsComposing())
    {
        _StartComposition(pContext);
        if (!_IsComposing())
        {
            DebugTsfIssue47(L"composition-start-missing", requestId, 0, wch, CATEGORY_COMPOSING, FUNCTION_INPUT, 1,
                            FALSE, pCompositionProcessorEngine ? pCompositionProcessorEngine->GetVirtualKeyLength() : 0,
                            E_FAIL);
            return E_FAIL;
        }
    }

    // first, test where a keystroke would go in the document if we did an insert
    if (pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched) != S_OK || fetched != 1)
    {
        DebugTsfIssue47(L"composition-selection-failed", requestId, 0, wch, CATEGORY_COMPOSING, FUNCTION_INPUT, 1,
                        _IsComposing(),
                        pCompositionProcessorEngine ? pCompositionProcessorEngine->GetVirtualKeyLength() : 0, E_FAIL);
        return S_FALSE;
    }

    // is the insertion point covered by a composition?
    if (SUCCEEDED(_pComposition->GetRange(&pRangeComposition)))
    {
        isCovered = _IsRangeCovered(ec, tfSelection.range, pRangeComposition);

        pRangeComposition->Release();

        if (!isCovered)
        {
            DebugTsfIssue47(L"composition-selection-outside", requestId, 0, wch, CATEGORY_COMPOSING, FUNCTION_INPUT, 1,
                            _IsComposing(), pCompositionProcessorEngine->GetVirtualKeyLength(), S_FALSE);
            goto Exit;
        }
    }

    // Add virtual key to composition processor engine
    const DWORD_PTR previousLength = pCompositionProcessorEngine->GetVirtualKeyLength();
    if (pCompositionProcessorEngine->AddVirtualKey(wch) &&
        pCompositionProcessorEngine->GetVirtualKeyLength() > previousLength)
    {
        g_toggleImeFallbackBuffer.push_back(wch);
    }

    workerResult = _HandleCompositionInputWorker(pCompositionProcessorEngine, ec, pContext, requestId);

    DebugTsfIssue47(L"composition-input-complete", requestId, 0, wch, CATEGORY_COMPOSING, FUNCTION_INPUT, 1,
                    _IsComposing(), pCompositionProcessorEngine->GetVirtualKeyLength(), workerResult,
                    static_cast<uint64_t>(previousLength));

Exit:
    tfSelection.range->Release();
    return workerResult;
}

//+---------------------------------------------------------------------------
//
// _HandleCompositionInputWorker
//
// If the keystroke happens within a composition, eat the key and return S_OK.
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCompositionInputWorker(_In_ CCompositionProcessorEngine *pCompositionProcessorEngine,
                                                       TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId)
{
    HRESULT hr = S_OK;
    CMetasequoiaImeArray<CStringRange> readingStrings;
    BOOL isWildcardIncluded = FALSE;

    //
    // Get reading string from composition processor engine
    //
    pCompositionProcessorEngine->GetReadingStrings(&readingStrings, &isWildcardIncluded);

    if (readingStrings.Count())
    {
    }

    std::wstring uiLessPreedit;
    std::wstring uiLessCandidatePage;
    int uiLessSelection = 0;
    bool gotUiLessComposition = false;

    /* 一般来说，readingStrings 数组中只有一个元素，这个元素就是当前输入的拼音 */

    // UILess hosts need a synchronous candidate page before UpdateUIElement.
    if (Global::IsUiLessMode() && requestId != FANY_IME_NO_REQUEST_ID)
    {
        struct FanyImeNamedpipeDataToTsf *receivedData =
            TryReadDataFromServerPipeWithTimeout(requestId, /*abortTransportOnTimeout=*/false);
        if (receivedData->msg_type == Global::DataFromServerMsgType::TransportUnavailable)
        {
            return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        }
        if (receivedData->msg_type == Global::DataFromServerMsgType::UiLessComposition)
        {
            const std::wstring payload(receivedData->candidate_string);
            const size_t firstTab = payload.find(L'\t');
            if (firstTab == std::wstring::npos)
            {
                uiLessPreedit = payload;
            }
            else
            {
                uiLessPreedit = payload.substr(0, firstTab);
                const size_t secondTab = payload.find(L'\t', firstTab + 1);
                if (secondTab == std::wstring::npos)
                {
                    uiLessCandidatePage = payload.substr(firstTab + 1);
                }
                else
                {
                    uiLessCandidatePage = payload.substr(firstTab + 1, secondTab - firstTab - 1);
                    uiLessSelection = _wtoi(payload.c_str() + secondTab + 1);
                }
            }
            gotUiLessComposition = true;
        }
        else if (receivedData->msg_type == Global::DataFromServerMsgType::Preedit)
        {
            uiLessPreedit.assign(receivedData->candidate_string, wcslen(receivedData->candidate_string));
            gotUiLessComposition = true;
        }
    }

    for (UINT index = 0; index < readingStrings.Count(); index++)
    {
        CStringRange curReadingStr;
        std::wstring readingStr = readingStrings.GetAt(0)->ToWString();
        const auto &preeditStyle = GlobalSettings::getTsfPreeditStyle();

        if (preeditStyle == GlobalSettings::TsfPreeditStyle::Empty)
        {
            // Inline preedit hidden; composition/candidates still run as usual.
            GlobalIme::pending_create_word_preedit.clear();
            readingStr.clear();
            curReadingStr.Set(readingStr.c_str(), readingStr.length());
        }
        else if (preeditStyle == GlobalSettings::TsfPreeditStyle::Pinyin)
        {
            bool gotServerPreedit = false;
            if (!GlobalIme::pending_create_word_preedit.empty())
            {
                readingStr = std::move(GlobalIme::pending_create_word_preedit);
                GlobalIme::pending_create_word_preedit.clear();
                gotServerPreedit = true;
            }
            else if (gotUiLessComposition)
            {
                readingStr = uiLessPreedit;
                gotServerPreedit = true;
            }
            else if (requestId != FANY_IME_NO_REQUEST_ID)
            {
                struct FanyImeNamedpipeDataToTsf *receivedData =
                    TryReadDataFromServerPipeWithTimeout(requestId, /*abortTransportOnTimeout=*/false);
                if (receivedData->msg_type == Global::DataFromServerMsgType::TransportUnavailable)
                {
                    return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
                }
                if (receivedData->msg_type == Global::DataFromServerMsgType::Preedit)
                {
                    readingStr.assign(receivedData->candidate_string, wcslen(receivedData->candidate_string));
                    gotServerPreedit = true;
                }
            }

            if (!gotServerPreedit && !GlobalIme::word_for_creating_word.empty())
            {
                // Fallback when Preedit is missing: keep 汉字 + remaining raw,
                // matching raw create-word structure until the next Preedit.
                readingStr = GlobalIme::word_for_creating_word + readingStr;
            }
            curReadingStr.Set(readingStr.c_str(), readingStr.length());
        }
        else
        {
            // raw (default)
            GlobalIme::pending_create_word_preedit.clear();
            if (!GlobalIme::word_for_creating_word.empty())
            { /* 造词过程中 */
                readingStr = GlobalIme::word_for_creating_word + readingStr;
            }
            curReadingStr.Set(readingStr.c_str(), readingStr.length());
        }

        const size_t preeditPrefixLength =
            preeditStyle == GlobalSettings::TsfPreeditStyle::Empty ? 0 : GlobalIme::word_for_creating_word.size();
        const DWORD_PTR displayCaret = MapRawCaretToPreedit(pCompositionProcessorEngine->GetKeystrokeBuffer(),
                                                            pCompositionProcessorEngine->GetCaretPosition(),
                                                            curReadingStr.ToWString(), preeditPrefixLength);
        pCompositionProcessorEngine->SetRenderedPreedit(curReadingStr.ToWString(), preeditPrefixLength);

        hr = _AddComposingAndChar(ec, pContext, &curReadingStr);

        if (FAILED(hr))
        {
            return hr;
        }

        if (_pComposition)
        {
            ITfRange *caretRange = nullptr;
            if (SUCCEEDED(_pComposition->GetRange(&caretRange)) && caretRange)
            {
                caretRange->Collapse(ec, TF_ANCHOR_START);
                LONG shifted = 0;
                caretRange->ShiftEnd(ec, static_cast<LONG>(displayCaret), &shifted, nullptr);
                caretRange->Collapse(ec, TF_ANCHOR_END);
                TF_SELECTION caretSelection = {};
                caretSelection.range = caretRange;
                caretSelection.style.ase = TF_AE_NONE;
                caretSelection.style.fInterimChar = FALSE;
                pContext->SetSelection(ec, 1, &caretSelection);
                caretRange->Release();
            }
        }
    }

    //
    // Get candidate string from composition processor engine
    //
    CMetasequoiaImeArray<CCandidateListItem> candidateList;

    //
    // Important: Generate candidate list here
    //
    // There is no need to use neither IncrementalWordSearch nor WildcardSearch, so we set them both FALSE
    pCompositionProcessorEngine->GetCandidateList(&candidateList, FALSE, FALSE);

    if ((candidateList.Count()))
    {
        hr = _CreateAndStartCandidate(pCompositionProcessorEngine, ec, pContext);
        if (SUCCEEDED(hr))
        {
            if (gotUiLessComposition && _pCandidateListUIPresenter)
            {
                _pCandidateListUIPresenter->_ApplyUiLessCandidatePage(uiLessCandidatePage, uiLessSelection);
            }
            if (!gotUiLessComposition)
            {
                _pCandidateListUIPresenter->_ClearList();
            }
            _pCandidateListUIPresenter->_SetText(&candidateList, TRUE);
        }
    }
    else if (_pCandidateListUIPresenter)
    {
        if (gotUiLessComposition)
        {
            _pCandidateListUIPresenter->_ApplyUiLessCandidatePage(uiLessCandidatePage, uiLessSelection);
            _pCandidateListUIPresenter->_NotifyUiLessHost();
        }
        else
        {
            _pCandidateListUIPresenter->_ClearList();
        }
    }
    else if (readingStrings.Count() && isWildcardIncluded)
    {
        hr = _CreateAndStartCandidate(pCompositionProcessorEngine, ec, pContext);
        if (SUCCEEDED(hr))
        {
            _pCandidateListUIPresenter->_ClearList();
        }
    }
    return hr;
}
//+---------------------------------------------------------------------------
//
// _CreateAndStartCandidate
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_CreateAndStartCandidate(_In_ CCompositionProcessorEngine *pCompositionProcessorEngine,
                                                  TfEditCookie ec, _In_ ITfContext *pContext)
{
    HRESULT hr = S_OK;

    if ((_candidateMode == CANDIDATE_NONE) && (_pCandidateListUIPresenter))
    {
        // Recreate candidate list — dtor handles _EndCandidateList()
        delete _pCandidateListUIPresenter;
        _pCandidateListUIPresenter = nullptr;

        _candidateMode = CANDIDATE_NONE;
        _isCandidateWithWildcard = FALSE;
    }

    if (_pCandidateListUIPresenter == nullptr)
    {
        _pCandidateListUIPresenter = new (std::nothrow) CCandidateListUIPresenter(
            this, CATEGORY_CANDIDATE, pCompositionProcessorEngine->GetCandidateListIndexRange(), FALSE);
        if (!_pCandidateListUIPresenter)
        {
            return E_OUTOFMEMORY;
        }

        _candidateMode = CANDIDATE_INCREMENTAL;
        _isCandidateWithWildcard = FALSE;

        // we don't cache the document manager object. So get it from pContext.
        ITfDocumentMgr *pDocumentMgr = nullptr;
        if (SUCCEEDED(pContext->GetDocumentMgr(&pDocumentMgr)))
        {
            // get the composition range.
            ITfRange *pRange = nullptr;
            if (SUCCEEDED(_pComposition->GetRange(&pRange)))
            {
                hr = _pCandidateListUIPresenter->_StartCandidateList(
                    _tfClientId, pDocumentMgr, pContext, ec, pRange,
                    pCompositionProcessorEngine->GetCandidateWindowWidth());
                pRange->Release();
            }
            pDocumentMgr->Release();
        }
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// _HandleCompositionFinalize
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCompositionFinalize(TfEditCookie ec, _In_ ITfContext *pContext, BOOL isCandidateList)
{
    HRESULT hr = S_OK;

    if (isCandidateList && _pCandidateListUIPresenter)
    {
        // Finalize selected candidate string from CCandidateListUIPresenter
        DWORD_PTR candidateLen = 0;
        const WCHAR *pCandidateString = nullptr;

        candidateLen = _pCandidateListUIPresenter->_GetSelectedCandidateString(&pCandidateString);

        CStringRange candidateString;
        candidateString.Set(pCandidateString, candidateLen);

        if (candidateLen)
        {
            // Finalize character
            hr = _AddCharAndFinalize(ec, pContext, &candidateString);
            if (FAILED(hr))
            {
                return hr;
            }
        }
    }
    // For the non-candidate path, the current composition text is already in
    // the text store. _HandleCancel below owns the exact write cookie and
    // terminates it synchronously; requesting a nested edit session here can
    // legitimately fail with TF_E_SYNCHRONOUS and is redundant.

    _HandleCancel(ec, pContext);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _HandleCompositionConvert
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCompositionConvert(TfEditCookie ec, _In_ ITfContext *pContext, BOOL isWildcardSearch)
{
    HRESULT hr = S_OK;

    CMetasequoiaImeArray<CCandidateListItem> candidateList;

    //
    // Get candidate string from composition processor engine
    //
    CCompositionProcessorEngine *pCompositionProcessorEngine = nullptr;
    pCompositionProcessorEngine = _pCompositionProcessorEngine;
    pCompositionProcessorEngine->GetCandidateList(&candidateList, FALSE, isWildcardSearch);

    // If there is no candlidate listin the current reading string, we don't do anything. Just wait for
    // next char to be ready for the conversion with it.
    int nCount = candidateList.Count();
    if (nCount)
    {
        if (_pCandidateListUIPresenter)
        {
            delete _pCandidateListUIPresenter; // dtor handles _EndCandidateList()
            _pCandidateListUIPresenter = nullptr;

            _candidateMode = CANDIDATE_NONE;
            _isCandidateWithWildcard = FALSE;
        }

        //
        // create an instance of the candidate list class.
        //
        if (_pCandidateListUIPresenter == nullptr)
        {
            _pCandidateListUIPresenter = new (std::nothrow) CCandidateListUIPresenter(
                this, CATEGORY_CANDIDATE, pCompositionProcessorEngine->GetCandidateListIndexRange(), FALSE);
            if (!_pCandidateListUIPresenter)
            {
                return E_OUTOFMEMORY;
            }

            _candidateMode = CANDIDATE_ORIGINAL;
        }

        _isCandidateWithWildcard = isWildcardSearch;

        // we don't cache the document manager object. So get it from pContext.
        ITfDocumentMgr *pDocumentMgr = nullptr;
        if (SUCCEEDED(pContext->GetDocumentMgr(&pDocumentMgr)))
        {
            // get the composition range.
            ITfRange *pRange = nullptr;
            if (SUCCEEDED(_pComposition->GetRange(&pRange)))
            {
                hr = _pCandidateListUIPresenter->_StartCandidateList(
                    _tfClientId, pDocumentMgr, pContext, ec, pRange,
                    pCompositionProcessorEngine->GetCandidateWindowWidth());
                pRange->Release();
            }
            pDocumentMgr->Release();
        }
        if (SUCCEEDED(hr))
        {
            _pCandidateListUIPresenter->_SetText(&candidateList, FALSE);
        }
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// _HandleCompositionBackspace
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCompositionBackspace(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId)
{
    HRESULT workerResult = S_OK;
    ITfRange *pRangeComposition = nullptr;
    TF_SELECTION tfSelection;
    ULONG fetched = 0;
    BOOL isCovered = TRUE;

    // Start the new (std::nothrow) compositon if there is no composition.
    if (!_IsComposing())
    {
        return S_OK;
    }

    // first, test where a keystroke would go in the document if we did an insert
    if (FAILED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched)) || fetched != 1)
    {
        return S_FALSE;
    }

    // is the insertion point covered by a composition?
    if (SUCCEEDED(_pComposition->GetRange(&pRangeComposition)))
    {
        isCovered = _IsRangeCovered(ec, tfSelection.range, pRangeComposition);

        pRangeComposition->Release();

        if (!isCovered)
        {
            goto Exit;
        }
    }

    //
    // Add virtual key to composition processor engine
    //
    CCompositionProcessorEngine *pCompositionProcessorEngine = nullptr;
    pCompositionProcessorEngine = _pCompositionProcessorEngine;

    DWORD_PTR vKeyLen = pCompositionProcessorEngine->GetVirtualKeyLength();

    if (!g_toggleImeFallbackBuffer.empty())
    {
        g_toggleImeFallbackBuffer.pop_back();
    }

    if (vKeyLen)
    {
        pCompositionProcessorEngine->RemoveVirtualKeyBeforeCaret();

        if (pCompositionProcessorEngine->GetVirtualKeyLength())
        {
            workerResult = _HandleCompositionInputWorker(pCompositionProcessorEngine, ec, pContext, requestId);
        }
        else
        {
            _HandleCancel(ec, pContext);
        }
    }

Exit:
    tfSelection.range->Release();
    return workerResult;
}

//+---------------------------------------------------------------------------
//
// _HandleCompositionDelete
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCompositionDelete(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId)
{
    HRESULT workerResult = S_OK;
    ITfRange *pRangeComposition = nullptr;
    TF_SELECTION tfSelection = {};
    ULONG fetched = 0;

    if (!_IsComposing())
    {
        return S_OK;
    }

    if (FAILED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched)) || fetched != 1)
    {
        return S_FALSE;
    }

    BOOL isCovered = TRUE;
    if (SUCCEEDED(_pComposition->GetRange(&pRangeComposition)))
    {
        isCovered = _IsRangeCovered(ec, tfSelection.range, pRangeComposition);
        pRangeComposition->Release();
    }

    if (isCovered)
    {
        CCompositionProcessorEngine *pCompositionProcessorEngine = _pCompositionProcessorEngine;
        const DWORD_PTR caret = pCompositionProcessorEngine->GetCaretPosition();
        const BOOL removed = pCompositionProcessorEngine->RemoveVirtualKeyAtCaret();

        if (removed && caret < g_toggleImeFallbackBuffer.size())
        {
            g_toggleImeFallbackBuffer.erase(static_cast<size_t>(caret), 1);
        }

        if (removed)
        {
            if (pCompositionProcessorEngine->GetVirtualKeyLength())
            {
                workerResult = _HandleCompositionInputWorker(pCompositionProcessorEngine, ec, pContext, requestId);
            }
            else
            {
                _HandleCancel(ec, pContext);
            }
        }
    }

    tfSelection.range->Release();
    return workerResult;
}

//+---------------------------------------------------------------------------
//
// _HandleCompositionArrowKey
//
// Update the selection within a composition.
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCompositionArrowKey(TfEditCookie ec, _In_ ITfContext *pContext,
                                                    KEYSTROKE_FUNCTION keyFunction, uint64_t requestId)
{
    if (keyFunction == FUNCTION_MOVE_LEFT || keyFunction == FUNCTION_MOVE_RIGHT)
    {
        _pCompositionProcessorEngine->MoveCaret(keyFunction == FUNCTION_MOVE_LEFT ? -1 : 1);
        if (_pComposition == nullptr)
        {
            return S_OK;
        }

        ITfRange *caretRange = nullptr;
        if (FAILED(_pComposition->GetRange(&caretRange)) || caretRange == nullptr)
        {
            return S_OK;
        }
        caretRange->Collapse(ec, TF_ANCHOR_START);
        LONG shifted = 0;
        caretRange->ShiftEnd(ec, static_cast<LONG>(_pCompositionProcessorEngine->GetRenderedCaretPosition()), &shifted,
                             nullptr);
        caretRange->Collapse(ec, TF_ANCHOR_END);
        TF_SELECTION caretSelection = {};
        caretSelection.range = caretRange;
        caretSelection.style.ase = TF_AE_NONE;
        caretSelection.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &caretSelection);
        caretRange->Release();
        if (Global::IsUiLessMode() && _pCandidateListUIPresenter)
        {
            _pCandidateListUIPresenter->_ConsumeUiLessCompositionReply(requestId);
        }
        return S_OK;
    }

    if ((keyFunction == FUNCTION_MOVE_PAGE_UP) || (keyFunction == FUNCTION_MOVE_PAGE_DOWN) ||
        (keyFunction == FUNCTION_MOVE_PAGE_TOP) || (keyFunction == FUNCTION_MOVE_PAGE_BOTTOM))
    {
        if ((_pCandidateListUIPresenter == nullptr) || (_pCandidateListUIPresenter->_GetCount() <= 1))
        {
            if (Global::IsUiLessMode() && _pCandidateListUIPresenter)
            {
                _pCandidateListUIPresenter->_ConsumeUiLessCompositionReply(requestId);
            }
            return S_OK;
        }
    }

    ITfRange *pRangeComposition = nullptr;
    TF_SELECTION tfSelection;
    ULONG fetched = 0;

    // get the selection
    if (FAILED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched)) || fetched != 1)
    {
        // no selection, eat the keystroke
        return S_OK;
    }

    // get the composition range
    if ((_pComposition == nullptr) || FAILED(_pComposition->GetRange(&pRangeComposition)))
    {
        goto Exit;
    }

    // For incremental candidate list
    if (_pCandidateListUIPresenter)
    {
        if (Global::IsUiLessMode())
        {
            _pCandidateListUIPresenter->_ConsumeUiLessCompositionReply(requestId);
        }
        else
        {
            _pCandidateListUIPresenter->AdviseUIChangedByArrowKey(keyFunction);
        }
    }

    pContext->SetSelection(ec, 1, &tfSelection);

    pRangeComposition->Release();

Exit:
    tfSelection.range->Release();
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _HandleCompositionPunctuation
// 处理标点的上屏：
//   1. 没有候选词的情况下，纯标点的上屏
//   2. 有候选词的情况下，候选词和标点的一并上屏
//
// 标点这里不会触发造词行为。
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCompositionPunctuation(TfEditCookie ec, _In_ ITfContext *pContext, UINT code, WCHAR wch,
                                                       uint64_t requestId, const std::wstring &prefetchedText)
{
    HRESULT hr = S_OK;

    //
    // Get punctuation char from composition processor engine
    //
    CCompositionProcessorEngine *pCompositionProcessorEngine = nullptr;
    pCompositionProcessorEngine = _pCompositionProcessorEngine;

    std::wstring pendingPunctuationCommitText = prefetchedText;
    const bool hasPendingPunctuationCommitText = !pendingPunctuationCommitText.empty();
    std::wstring punctuationStr;
    if (hasPendingPunctuationCommitText)
    {
        // Prefetch already contains the fully resolved commit text (candidate
        // plus punctuation, or a pure punctuation string resolved upstream).
        punctuationStr = std::move(pendingPunctuationCommitText);
    }
    else if (code == VK_DECIMAL)
    {
        // Numpad decimal should always commit ASCII '.' even in Chinese
        // punctuation mode (main-keyboard '.' still maps to '。').
        punctuationStr = L".";
    }

    if (!hasPendingPunctuationCommitText && _candidateMode != CANDIDATE_NONE && _pCandidateListUIPresenter)
    {
        //
        // 请求当前高亮候选词；服务端也可能返回以词定字的精确提交文本。
        //
        if (Global::CommitWithHighlightedCandPunc.count(wch) > 0)
        {
            struct FanyImeNamedpipeDataToTsf *receivedData = TryReadDataFromServerPipeWithTimeout(requestId);

            if (receivedData->msg_type == Global::DataFromServerMsgType::TransportUnavailable)
            {
                return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
            }

            // The Server is authoritative for configurable candidate-navigation
            // keys. The local paging snapshot can briefly lag behind while a TSF
            // client connects or a setting changes, so a comma/period may have
            // entered this punctuation path while the Server already treated it
            // as navigation. Never turn such a response into committed text.
            if (receivedData->msg_type == Global::DataFromServerMsgType::CommitExactText)
            {
                punctuationStr.assign(receivedData->candidate_string);
            }
            else if (receivedData->msg_type != Global::DataFromServerMsgType::Normal)
            {
                // The Server already updated the authoritative candidate state.
                // Consuming the response is sufficient; advancing the TSF-side
                // presenter here would apply the same navigation a second time.
                return S_OK;
            }
            else
            {
                const std::wstring candidate(receivedData->candidate_string);
                const WCHAR preceding =
                    candidate.empty() ? _GetPrecedingCharForSmartPunctuation(ec, pContext) : candidate.back();
                if (code == VK_DECIMAL || wch == L'/' || wch == L'-' || wch == L'+')
                {
                    punctuationStr = candidate + (code == VK_DECIMAL ? L'.' : wch);
                }
                else
                {
                    punctuationStr = candidate + _ResolveSmartPunctuation(wch, preceding);
                }
            }
        }
    }

    if (!hasPendingPunctuationCommitText && punctuationStr.empty() && code != VK_DECIMAL)
    {
        // Pure punctuation (no candidate prefix): choose ASCII vs Chinese from
        // the character immediately before the caret / composition.
        const WCHAR preceding = _GetPrecedingCharForSmartPunctuation(ec, pContext);
        punctuationStr = _ResolveSmartPunctuation(wch, preceding);
    }

    // 宿主级排除优先于开关：被排除的宿主里既不补全右半边，也不做跨越式闭合，
    // 标点回落到原有的左右轮换行为。
    const bool pairedPunctuationEnabled = Global::PairedPunctuationEnabled.load(std::memory_order_relaxed) &&
                                          !Global::IsPairedPunctuationExcludedProcess(Global::current_process_name);
    // The '-' + '>' arrow commits a half-width '>' on purpose. Feeding it to
    // the step-over probe would look like a mismatched closing and clear the
    // whole paired stack, breaking a still-pending 《》 step-over.
    const bool halfWidthArrowAngle = wch == L'>' && punctuationStr.size() == 1 && punctuationStr[0] == L'>';
    if (pairedPunctuationEnabled && !halfWidthArrowAngle && !_IsComposing() && _candidateMode == CANDIDATE_NONE)
    {
        // A pair whose closing half is still waiting on the right of the caret
        // is closed by stepping over it. Without this the closing key inserts a
        // second one （内容）） and, because of the pinning below, the right
        // quote could never be typed at all.
        const WCHAR stepOver = GetPairedPunctuationStepOverCandidate(wch, punctuationStr);
        if (_TryStepOverPairedPunctuation(ec, pContext, stepOver))
        {
            return S_OK;
        }
    }

    if (pairedPunctuationEnabled && !punctuationStr.empty())
    {
        // Quotes share one physical key for both sides. In paired mode every
        // press starts a fresh pair instead of following the legacy left/right
        // toggle maintained by GetPunctuation().
        if (wch == L'"' && punctuationStr.back() == L'”')
        {
            punctuationStr.back() = L'“';
        }
        else if (wch == L'\'' && punctuationStr.back() == L'’')
        {
            punctuationStr.back() = L'‘';
        }
    }

    const WCHAR pairedOpening = punctuationStr.empty() ? 0 : punctuationStr.back();
    const WCHAR pairedClosing = pairedPunctuationEnabled ? _GetPairedPunctuationClosingFor(pairedOpening) : 0;
    if (pairedClosing != 0)
    {
        punctuationStr.push_back(pairedClosing);
    }

    CStringRange punctuationString;
    punctuationString.Set(punctuationStr.c_str(), punctuationStr.length());

    const bool hasActiveComposition = _IsComposing() ? true : false;
    if (hasActiveComposition)
    {
        hr = _InsertTextToComposition(ec, pContext, &punctuationString);
        if (FAILED(hr))
        {
            hr = _AddComposingAndChar(ec, pContext, &punctuationString);
        }
        if (FAILED(hr))
        {
            return hr;
        }
    }
    else
    {
        hr = _AddCharAndFinalize(ec, pContext, &punctuationString);
        if (FAILED(hr))
        {
            return hr;
        }
    }

    if (hasActiveComposition)
    {
        _HandleCompleteCommitFirst(ec, pContext);
    }
    else
    {
        _HandleComplete(ec, pContext);
    }
    if (pairedClosing != 0)
    {
        // The closing half was emitted here, not by a closing keystroke, so the
        // nest-pair depth that resolving the opening advanced would never be
        // paid back (the '>' is consumed by step-over). Balance it now, or the
        // next 《》 degrades into 〈〉.
        pCompositionProcessorEngine->BalanceNestPairAfterAutoClose(wch);
        _InvalidateSmartPunctuationShadow();
        _PushPairedPunctuation(pairedOpening, pairedClosing);
        _QueuePairedPunctuationCaretMove(-1);
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _HandleCompositionDoubleSingleByte
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCompositionDoubleSingleByte(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR wch)
{
    HRESULT hr = S_OK;

    WCHAR fullWidth = Global::FullWidthCharTable[wch - 0x20];

    CStringRange fullWidthString;
    fullWidthString.Set(&fullWidth, 1);

    // Finalize character
    hr = _AddCharAndFinalize(ec, pContext, &fullWidthString);
    if (FAILED(hr))
    {
        return hr;
    }

    _HandleCancel(ec, pContext);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _InvokeKeyHandler
//
// This text service is interested in handling keystrokes to demonstrate the
// use the compositions. Some apps will cancel compositions if they receive
// keystrokes while a compositions is ongoing.
//
// param
//    [in] uCode - virtual key code of WM_KEYDOWN wParam
//    [in] dwFlags - WM_KEYDOWN lParam
//    [in] dwKeyFunction - Function regarding virtual key
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_InvokeKeyHandler(_In_ ITfContext *pContext, UINT code, WCHAR wch, DWORD flags,
                                           _KEYSTROKE_STATE keyState, uint64_t requestId, std::wstring prefetchedText,
                                           UINT localResetToken, uint64_t expectedCompositionEpoch,
                                           uint64_t expectedFocusToken, uint64_t deferredReplayToken)
{
    flags;

    CKeyHandlerEditSession *pEditSession = nullptr;
    HRESULT hr = E_FAIL;

    // we'll insert a char ourselves in place of this keystroke
    LARGE_INTEGER requestStartQpc;
    QueryPerformanceCounter(&requestStartQpc);
    pEditSession = new (std::nothrow) CKeyHandlerEditSession(
        this, pContext, code, wch, keyState, requestId, requestStartQpc, std::move(prefetchedText),
        localResetToken == 0 ? (expectedFocusToken != 0 ? expectedFocusToken : _CaptureFocusSessionToken()) : 0,
        localResetToken, expectedCompositionEpoch, deferredReplayToken);
    if (pEditSession == nullptr)
    {
        if (deferredReplayToken != 0)
        {
            _RetryDeferredKeyReplay(deferredReplayToken);
        }
        goto Exit;
    }

    //
    // Call CKeyHandlerEditSession::DoEditSession().
    //
    // Do not specify TF_ES_SYNC so edit session is not invoked on WinWord
    //
    HRESULT editSessionHr = E_FAIL;
    HRESULT requestHr =
        pContext->RequestEditSession(_tfClientId, pEditSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &editSessionHr);
    hr = FAILED(requestHr) ? requestHr : editSessionHr;
    DebugTsfIssue47(L"edit-session-request", requestId, code, wch, keyState.Category, keyState.Function, 1,
                    _IsComposing(),
                    _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, hr,
                    deferredReplayToken);
    if ((FAILED(requestHr) || FAILED(editSessionHr)) && deferredReplayToken != 0)
    {
        _RetryDeferredKeyReplay(deferredReplayToken);
    }

    pEditSession->Release();

Exit:
    return hr;
}
