#include "FanyDefines.h"
#include "Globals.h"
#include "Private.h"
#include "MetasequoiaIME.h"
#include "CandidateListUIPresenter.h"
#include "CompositionProcessorEngine.h"
#include "MetasequoiaIMEBaseStructure.h"
#include "define.h"
#include <algorithm>
#include <cwchar>
#include <string>
#include <debugapi.h>
#include <intsafe.h>
#include <minwindef.h>
#include <winuser.h>
#include "Ipc.h"
#include "fmt/xchar.h"

//////////////////////////////////////////////////////////////////////
//
// CMetasequoiaIME candidate key handler methods
//
//////////////////////////////////////////////////////////////////////

const int MOVEUP_ONE = -1;
const int MOVEDOWN_ONE = 1;
const int MOVETO_TOP = 0;
const int MOVETO_BOTTOM = -1;

//+---------------------------------------------------------------------------
//
// _HandleCandidateFinalize
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCandidateFinalize(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId,
                                                  const std::wstring &prefetchedText)
{
    HRESULT hr = S_OK;

    CStringRange keyStrokebuffer = _pCompositionProcessorEngine->GetKeystrokeBuffer();
    DWORD_PTR keystrokeBufLen = keyStrokebuffer.GetLength();
    DWORD_PTR candidateLen = keystrokeBufLen;
    CStringRange candidateString(keyStrokebuffer);
    const std::wstring &pendingCommitCandidate = prefetchedText;

    // _pCandidateListUIPresenter is null in uwp/metro apps; the commit path below does not
    // need it, so there is nothing to branch on here.

    if (candidateLen)
    {
        if (!pendingCommitCandidate.empty())
        {
            candidateString.Set(pendingCommitCandidate.c_str(), pendingCommitCandidate.length());
            hr = _InsertTextToComposition(ec, pContext, &candidateString);
            if (FAILED(hr))
            {
                hr = _AddComposingAndChar(ec, pContext, &candidateString);
            }
            if (FAILED(hr))
            {
                return hr;
            }

            _HandleCompleteCommitFirst(ec, pContext);
            return hr;
        }

        UINT serverMsgType = Global::DataFromServerMsgType::OutofRange;
        std::wstring serverCandidateString;
        const bool hasPrefetchedServerCandidate = _TakePendingServerCandidate(&serverMsgType, &serverCandidateString);
        if (!hasPrefetchedServerCandidate)
        {
            struct FanyImeNamedpipeDataToTsf *receivedData = TryReadCommitReplyFromServerPipe(requestId);
            serverMsgType = receivedData->msg_type;
            serverCandidateString = receivedData->candidate_string;
        }

        if (serverMsgType == Global::DataFromServerMsgType::TransportUnavailable)
        {
            // Transport state is never candidate text. A request that never
            // left TSF is replayed after the replacement focus/session fence
            // is ready. One that was delivered may already have committed on
            // the Server, and replaying it would pick by index from a rebuilt
            // page that ranking may have reordered: rebuild the composition
            // but let the user choose again.
            return IsDeliveredServerRequestId(requestId) ? FANY_E_COMMIT_REPLY_AMBIGUOUS
                                                         : HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        }
        if (serverMsgType == Global::DataFromServerMsgType::OutofRange) // Candidate index out of range
        {
            return hr;
        }
        else if (serverMsgType == Global::DataFromServerMsgType::Normal) // 只有正常情况下才会上屏
        {
            GlobalIme::word_for_creating_word = L"";
            GlobalIme::pending_create_word_preedit.clear();
            candidateString.Set(serverCandidateString.c_str(), serverCandidateString.length());
            hr = _InsertTextToComposition(ec, pContext, &candidateString);
            if (FAILED(hr))
            {
                hr = _AddComposingAndChar(ec, pContext, &candidateString);
            }
            if (FAILED(hr))
            {
                return hr;
            }
        }
        /* 处理造词的逻辑 */
        else if (serverMsgType == Global::DataFromServerMsgType::NeedToCreateWord)
        {
            CreatingWordPayload payload;
            if (ParseCreatingWordPayload(serverCandidateString, payload))
            {
                (void)_ApplyCreatingWordPayload(ec, pContext, payload);
            }
            return hr;
        }
    }

    _HandleCompleteCommitFirst(ec, pContext);

    return hr;
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateFinalizeForVKReturn
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCandidateFinalizeForVKReturn(TfEditCookie ec, _In_ ITfContext *pContext)
{
    HRESULT hr = S_OK;
    WCHAR committedLastChar = 0;

    CStringRange keyStrokebuffer = _pCompositionProcessorEngine->GetKeystrokeBuffer();

    // While a word is being created the preedit is 已选汉字 + 剩余原始输入 (see
    // _HandleCompositionInputWorker); the keystroke buffer only holds the
    // remaining raw part. Enter commits what the preedit shows, so the
    // already selected prefix has to be committed with it. Clearing here
    // matches the Normal branch of _HandleCandidateFinalize and mirrors the
    // Server, which drops its own creating_word state when the candidate
    // presenter teardown below sends HideCandidateWnd.
    const std::wstring commitText = GlobalIme::word_for_creating_word + keyStrokebuffer.ToWString();
    GlobalIme::word_for_creating_word.clear();
    GlobalIme::pending_create_word_preedit.clear();

    DWORD_PTR candidateLen = commitText.length();
    CStringRange candidateString;
    candidateString.Set(commitText.c_str(), candidateLen);

    if (candidateLen)
    {
        hr = _AddComposingAndChar(ec, pContext, &candidateString);

        if (FAILED(hr))
        {
            return hr;
        }

        committedLastChar = candidateString.Get()[candidateLen - 1];
    }

    _HandleComplete(ec, pContext);

    // OnTestKeyDown invalidates the shadow for VK_RETURN because Enter usually
    // moves the caret or changes the host document. This Enter was consumed by
    // the IME, however, and committed the raw composition without moving the
    // caret. Preserve the actual last committed character for shallow text
    // stores, which cannot reliably expose it via GetText when the next smart
    // punctuation edit session runs.
    if (committedLastChar != 0)
    {
        _smartPunctuationShadowChar = committedLastChar;
        _smartPunctuationShadowValid = true;
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateConvert
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCandidateConvert(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId,
                                                 const std::wstring &prefetchedText)
{
    return _HandleCandidateWorker(ec, pContext, requestId, prefetchedText);
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateWorker
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCandidateWorker(TfEditCookie ec, _In_ ITfContext *pContext, uint64_t requestId,
                                                const std::wstring &prefetchedText)
{
    HRESULT hrReturn = E_FAIL;
    DWORD_PTR candidateLen = 0;
    const WCHAR *pCandidateString = nullptr;
    CStringRange candidateString;

    if (nullptr == _pCandidateListUIPresenter)
    {
        hrReturn = S_OK;
        goto Exit;
    }

    candidateLen = _pCandidateListUIPresenter->_GetSelectedCandidateString(&pCandidateString);
    if (0 == candidateLen)
    {
        hrReturn = S_FALSE;
        goto Exit;
    }

    candidateString.Set(pCandidateString, candidateLen);
    hrReturn = _HandleCandidateFinalize(ec, pContext, requestId, prefetchedText);

Exit:
    return hrReturn;
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateArrowKey
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCandidateArrowKey( //
    TfEditCookie ec,                               //
    _In_ ITfContext *pContext,                     //
    _In_ KEYSTROKE_FUNCTION keyFunction,           //
    uint64_t requestId                             //
)
{
    ec;
    pContext;

    if (_pCandidateListUIPresenter == nullptr)
    {
        return S_OK;
    }

    if (Global::IsUiLessMode() && _pCandidateListUIPresenter->_ConsumeUiLessCompositionReply(requestId))
    {
        return S_OK;
    }

    if ((keyFunction == FUNCTION_MOVE_PAGE_UP) || (keyFunction == FUNCTION_MOVE_PAGE_DOWN) ||
        (keyFunction == FUNCTION_MOVE_PAGE_TOP) || (keyFunction == FUNCTION_MOVE_PAGE_BOTTOM))
    {
        if (_pCandidateListUIPresenter->_GetCount() <= 1)
        {
            return S_OK;
        }
    }

    _pCandidateListUIPresenter->AdviseUIChangedByArrowKey(keyFunction);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateSelectByNumber
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_HandleCandidateSelectByNumber(TfEditCookie ec, _In_ ITfContext *pContext, _In_ UINT uCode,
                                                        uint64_t requestId, const std::wstring &prefetchedText)
{
    int iSelectAsNumber = _pCompositionProcessorEngine->GetCandidateListIndexRange()->GetIndex(uCode);
    if (iSelectAsNumber == -1)
    {
        return S_FALSE;
    }

    if (_pCandidateListUIPresenter)
    {
        if (_pCandidateListUIPresenter->_SetSelectionInPage(iSelectAsNumber))
        {
            return _HandleCandidateConvert(ec, pContext, requestId, prefetchedText);
        }
    }

    return S_FALSE;
}

//////////////////////////////////////////////////////////////////////
//
// CCandidateListUIPresenter class
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CCandidateListUIPresenter::CCandidateListUIPresenter(_In_ CMetasequoiaIME *pTextService, KEYSTROKE_CATEGORY Category,
                                                     _In_ CCandidateRange *pIndexRange, BOOL hideWindow)
    : CTfTextLayoutSink(pTextService), _candidateState(pIndexRange)
{
    _pIndexRange = pIndexRange;

    _Category = Category;

    _updatedFlags = 0;

    _uiElementId = (DWORD)-1;
    _isShowMode = TRUE;       // store return value from BeginUIElement
    _hideWindow = hideWindow; // Hide window flag from [Configuration] CandidateList.Phrase.HideWindow

    _pTextService = pTextService;
    _pTextService->AddRef();

    _refCount = 1;
    _candidateUiSessionActive = FALSE;
    _candidateWindowVisible = FALSE;
    _asyncCleanupPending = FALSE;
    _lastUiLessCandidatePage.clear();
}

//+---------------------------------------------------------------------------
//
// dtor
//
//----------------------------------------------------------------------------

CCandidateListUIPresenter::~CCandidateListUIPresenter()
{
    _EndCandidateList();
    _pTextService->Release();
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::IUnknown::QueryInterface
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (CTfTextLayoutSink::QueryInterface(riid, ppvObj) == S_OK)
    {
        return S_OK;
    }

    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_ITfUIElement) || IsEqualIID(riid, IID_ITfCandidateListUIElement))
    {
        *ppvObj = (ITfCandidateListUIElement *)this;
    }
    else if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfCandidateListUIElementBehavior))
    {
        *ppvObj = (ITfCandidateListUIElementBehavior *)this;
    }
    else if (IsEqualIID(riid, __uuidof(ITfIntegratableCandidateListUIElement)))
    {
        *ppvObj = (ITfIntegratableCandidateListUIElement *)this;
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
// ITfCandidateListUIElement::IUnknown::AddRef
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CCandidateListUIPresenter::AddRef()
{
    CTfTextLayoutSink::AddRef();
    return ++_refCount;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::IUnknown::Release
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CCandidateListUIPresenter::Release()
{
    CTfTextLayoutSink::Release();

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
// ITfCandidateListUIElement::ITfUIElement::GetDescription
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetDescription(BSTR *pbstr)
{
    if (pbstr)
    {
        *pbstr = SysAllocString(L"Cand");
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::ITfUIElement::GetGUID
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetGUID(GUID *pguid)
{
    *pguid = Global::MetasequoiaIMEGuidCandUIElement;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::ITfUIElement::Show
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::Show(BOOL showCandidateWindow)
{
    if (showCandidateWindow)
    {
        if (_hideWindow || !_isShowMode)
        {
            _candidateWindowVisible = FALSE;
        }
        else
        {
            _MoveWindowToTextExt();
            if (_candidateUiSessionActive)
            {
                UpdateCandidateUiSession();
            }
            else
            {
                BeginCandidateUiSession();
            }
            _candidateWindowVisible = TRUE;
        }
    }
    else
    {
        _candidateWindowVisible = FALSE;
        _updatedFlags = TF_CLUIE_SELECTION | TF_CLUIE_CURRENTPAGE;
        _UpdateUIElement();
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::ITfUIElement::IsShown
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::IsShown(BOOL *pIsShow)
{
    if (_isShowMode)
    {
        *pIsShow = _candidateWindowVisible;
    }
    else
    {
        // Host-drawn: report visible while the UI element is alive and has data.
        *pIsShow = (_uiElementId != static_cast<DWORD>(-1)) && (_candidateState.GetCount() > 0);
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetUpdatedFlags
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetUpdatedFlags(DWORD *pdwFlags)
{
    *pdwFlags = _updatedFlags;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetDocumentMgr
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetDocumentMgr(ITfDocumentMgr **ppdim)
{
    *ppdim = nullptr;

    if (_pTextService == nullptr)
    {
        return E_FAIL;
    }

    ITfThreadMgr *pThreadMgr = _pTextService->_GetThreadMgr();
    if (pThreadMgr == nullptr)
    {
        return E_FAIL;
    }

    if (FAILED(pThreadMgr->GetFocus(ppdim)) || (*ppdim == nullptr))
    {
        return E_FAIL;
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetCount
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetCount(UINT *pCandidateCount)
{
    if (!_isShowMode)
    {
        _LoadUiLessCandidatesFromSharedMemory();
    }
    *pCandidateCount = _candidateState.GetCount();
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetSelection
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetSelection(UINT *pSelectedCandidateIndex)
{
    if (!_isShowMode)
    {
        _LoadUiLessCandidatesFromSharedMemory();
    }
    *pSelectedCandidateIndex = _candidateState.GetSelection();
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetString
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetString(UINT uIndex, BSTR *pbstr)
{
    if (!_isShowMode)
    {
        _LoadUiLessCandidatesFromSharedMemory();
    }
    if (uIndex >= _candidateState.GetCount())
    {
        return E_FAIL;
    }

    DWORD candidateLen = 0;
    const WCHAR *pCandidateString = nullptr;

    candidateLen = _candidateState.GetCandidateString(static_cast<int>(uIndex), &pCandidateString);

    *pbstr = (candidateLen == 0) ? nullptr : SysAllocStringLen(pCandidateString, candidateLen);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetPageIndex
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetPageIndex(UINT *pIndex, UINT uSize, UINT *puPageCnt)
{
    return _candidateState.GetPageIndex(pIndex, uSize, puPageCnt);
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::SetPageIndex
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::SetPageIndex(UINT *pIndex, UINT uPageCnt)
{
    return _candidateState.SetPageIndex(pIndex, uPageCnt);
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetCurrentPage
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetCurrentPage(UINT *puPage)
{
    return _candidateState.GetCurrentPage(puPage);
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElementBehavior::SetSelection
// It is related of the mouse clicking behavior upon the suggestion window
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::SetSelection(UINT nIndex)
{
    _candidateState.SetSelectionSilently(nIndex);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElementBehavior::Finalize
// It is related of the mouse clicking behavior upon the suggestion window
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::Finalize(void)
{
    _CandidateChangeNotification(CAND_ITEM_SELECT);
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElementBehavior::Abort
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::Abort(void)
{
    _RequestCancelComposition();
    _EndCandidateList();
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfIntegratableCandidateListUIElement::SetIntegrationStyle
// To show candidateNumbers on the suggestion window
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::SetIntegrationStyle(GUID guidIntegrationStyle)
{
    return (guidIntegrationStyle == GUID_INTEGRATIONSTYLE_SEARCHBOX) ? S_OK : E_NOTIMPL;
}

//+---------------------------------------------------------------------------
//
// ITfIntegratableCandidateListUIElement::GetSelectionStyle
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetSelectionStyle(_Out_ TfIntegratableCandidateListSelectionStyle *ptfSelectionStyle)
{
    *ptfSelectionStyle = STYLE_ACTIVE_SELECTION;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfIntegratableCandidateListUIElement::OnKeyDown
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::OnKeyDown(_In_ WPARAM wParam, _In_ LPARAM lParam, _Out_ BOOL *pIsEaten)
{
    wParam;
    lParam;

    *pIsEaten = TRUE;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfIntegratableCandidateListUIElement::ShowCandidateNumbers
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::ShowCandidateNumbers(_Out_ BOOL *pIsShow)
{
    *pIsShow = TRUE;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfIntegratableCandidateListUIElement::FinalizeExactCompositionString
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::FinalizeExactCompositionString()
{
    return E_NOTIMPL;
}

//+---------------------------------------------------------------------------
//
// _StartCandidateList
//
//----------------------------------------------------------------------------

HRESULT CCandidateListUIPresenter::_StartCandidateList(TfClientId tfClientId, _In_ ITfDocumentMgr *pDocumentMgr,
                                                       _In_ ITfContext *pContextDocument, TfEditCookie ec,
                                                       _In_ ITfRange *pRangeComposition, UINT wndWidth)
{
    pDocumentMgr;
    tfClientId;
    pContextDocument;
    wndWidth;

    HRESULT hr = E_FAIL;

    if (FAILED(_StartLayout(pContextDocument, ec, pRangeComposition)))
    {
        goto Exit;
    }

    BeginUIElement();
    _candidateWindowVisible = FALSE;

    RECT rcTextExt;
    POINT anchor;
    if (SUCCEEDED(_GetTextExt(&rcTextExt, &anchor)))
    {
        Global::Point[0] = anchor.x;
        Global::Point[1] = anchor.y;
        _LayoutChangeNotification(&rcTextExt);
    }

    hr = S_OK;

Exit:
    if (FAILED(hr))
    {
        _EndCandidateList();
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// _EndCandidateList
//
//----------------------------------------------------------------------------

void CCandidateListUIPresenter::_EndCandidateList()
{
    EndUIElement();

    EndCandidateUiSession();

    _candidateState.Clear();
    _candidateWindowVisible = FALSE;
    _lastUiLessCandidatePage.clear();

    _EndLayout();
}

void CCandidateListUIPresenter::_PrepareForAsyncCleanup()
{
    _asyncCleanupPending = TRUE;
    // Hide and clear the exact Server-side candidate session now. Deferring
    // this until the presenter destructor lets an old cleanup message race a
    // newly started composition and clear its candidates.
    EndCandidateUiSession();
}

void CCandidateListUIPresenter::_NotifyUI()
{
    if (!_isShowMode)
    {
        // UILess: host draws via ITfUIElementSink — never raise an IME HWND.
        return;
    }
    if (_candidateUiSessionActive)
    {
        UpdateCandidateUiSession();
    }
    else
    {
        BeginCandidateUiSession();
    }
}

//+---------------------------------------------------------------------------
//
// _SetText
//
//----------------------------------------------------------------------------

void CCandidateListUIPresenter::_SetText(_In_ CMetasequoiaImeArray<CCandidateListItem> *pCandidateList,
                                         BOOL isAddFindKeyCode)
{
    if (!_isShowMode)
    {
        // Prefer the synchronous UiLessComposition pipe payload (already applied
        // via _ApplyUiLessCandidatePage). Fall back to shared memory if needed.
        if (_candidateState.GetCount() == 0)
        {
            _LoadUiLessCandidatesFromSharedMemory();
        }
        if (_candidateState.GetCount() == 0 && pCandidateList != nullptr && pCandidateList->Count() != 0)
        {
            AddCandidateToCandidateListUI(pCandidateList, isAddFindKeyCode);
            SetPageIndexWithScrollInfo(pCandidateList);
        }
        _NotifyUiLessHost();
        return;
    }

    AddCandidateToCandidateListUI(pCandidateList, isAddFindKeyCode);

    SetPageIndexWithScrollInfo(pCandidateList);

    _NotifyUI();
}

void CCandidateListUIPresenter::AddCandidateToCandidateListUI(     //
    _In_ CMetasequoiaImeArray<CCandidateListItem> *pCandidateList, //
    BOOL isAddFindKeyCode                                          //
)
{
    for (UINT index = 0; index < pCandidateList->Count(); index++)
    {
        _candidateState.AddCandidate(pCandidateList->GetAt(index), isAddFindKeyCode);
    }
}

void CCandidateListUIPresenter::SetPageIndexWithScrollInfo(       //
    _In_ CMetasequoiaImeArray<CCandidateListItem> *pCandidateList //
)
{
    if ((pCandidateList == nullptr) || (_pIndexRange == nullptr))
    {
        return;
    }

    const UINT candCntInPage = _pIndexRange->Count();
    if (candCntInPage == 0)
    {
        return;
    }

    const UINT candidateCount = pCandidateList->Count();
    const UINT bufferSize = (candidateCount == 0) ? 0 : ((candidateCount - 1) / candCntInPage + 1);
    UINT *puPageIndex = new (std::nothrow) UINT[bufferSize];
    if (puPageIndex != nullptr)
    {
        for (UINT i = 0; i < bufferSize; i++)
        {
            puPageIndex[i] = i * candCntInPage;
        }

        _candidateState.SetPageIndex(puPageIndex, bufferSize);
        delete[] puPageIndex;
    }
    else if (bufferSize == 0)
    {
        _candidateState.SetPageIndex(nullptr, 0);
    }

    _candidateState.SetScrollInfo(candidateCount,
                                  candCntInPage); // nMax:range of max, nPage:number of items in page
}
//+---------------------------------------------------------------------------
//
// _ClearList
//
//----------------------------------------------------------------------------

void CCandidateListUIPresenter::_ClearList()
{
    _candidateState.Clear();
}

//+---------------------------------------------------------------------------
//
// _GetSelectedCandidateString
//
//----------------------------------------------------------------------------

DWORD_PTR CCandidateListUIPresenter::_GetSelectedCandidateString(
    _Outptr_result_maybenull_ const WCHAR **ppwchCandidateString)
{
    return _candidateState.GetSelectedCandidateString(ppwchCandidateString);
}

//+---------------------------------------------------------------------------
//
// _MoveSelection
//
//----------------------------------------------------------------------------

BOOL CCandidateListUIPresenter::_MoveSelection(_In_ int offSet)
{
    BOOL ret = _candidateState.MoveSelection(offSet);
    if (ret)
    {
        if (_isShowMode)
        {
            _NotifyUI();
        }
        else
        {
            _updatedFlags = TF_CLUIE_SELECTION;
            _UpdateUIElement();
        }
    }
    return ret;
}

//+---------------------------------------------------------------------------
//
// _SetSelection
//
//----------------------------------------------------------------------------

BOOL CCandidateListUIPresenter::_SetSelection(_In_ int selectedIndex)
{
    BOOL ret = _candidateState.SetSelection(selectedIndex);
    if (ret)
    {
        if (_isShowMode)
        {
            _NotifyUI();
        }
        else
        {
            _updatedFlags = TF_CLUIE_SELECTION | TF_CLUIE_CURRENTPAGE;
            _UpdateUIElement();
        }
    }
    return ret;
}

//+---------------------------------------------------------------------------
//
// _MovePage
//
//----------------------------------------------------------------------------

BOOL CCandidateListUIPresenter::_MovePage(_In_ int offSet)
{
    BOOL ret = _candidateState.MovePage(offSet);
    if (ret)
    {
        if (_isShowMode)
        {
            _NotifyUI();
        }
        else
        {
            _updatedFlags = TF_CLUIE_SELECTION | TF_CLUIE_CURRENTPAGE;
            _UpdateUIElement();
        }
    }
    return ret;
}

//+---------------------------------------------------------------------------
//
// _MoveWindowToTextExt
//
//----------------------------------------------------------------------------

void CCandidateListUIPresenter::_MoveWindowToTextExt()
{
    RECT rc;
    POINT anchor;

    if (FAILED(_GetTextExt(&rc, &anchor)))
    {
        return;
    }

    Global::Point[0] = anchor.x;
    Global::Point[1] = anchor.y;
}
//+---------------------------------------------------------------------------
//
// _LayoutChangeNotification
//
//----------------------------------------------------------------------------

VOID CCandidateListUIPresenter::_LayoutChangeNotification(_In_ RECT *lpRect)
{
    lpRect;
    if (_asyncCleanupPending || !_candidateUiSessionActive)
    {
        // In UWP, layout updates can still arrive after candidate UI teardown; ignore them so the window stays hidden.
        return;
    }
    MoveCandidateUiSession();
}

//+---------------------------------------------------------------------------
//
// _LayoutDestroyNotification
//
//----------------------------------------------------------------------------

VOID CCandidateListUIPresenter::_LayoutDestroyNotification()
{
    if (_asyncCleanupPending)
    {
        return;
    }

    // Some hosts transiently destroy and recreate their TSF context view while
    // the same composition is still active — Telegram always did, and every
    // Chromium-based host (Electron apps such as VS Code / Cursor, browsers)
    // does it too once the machine is busy enough for the renderer to fall
    // behind. Treating that as candidate-session teardown sends HideCandidate
    // between ordinary keystrokes, so Server clears the live composition and the
    // HWND visibly disappears/reappears, and the pending candidate can no longer
    // be committed. A live composition is the reliable signal that this is not a
    // real teardown, so gate on that instead of on a process-name allowlist:
    // real commit, cancel, focus loss, and presenter cleanup all end the
    // composition first and still terminate the session through those paths.
    if (_pTextService != nullptr && _pTextService->_IsComposing())
    {
        if (Global::TsfDiagnosticLogEnabled.load(std::memory_order_relaxed))
        {
            QueueTsfDiagnosticLog(L"[candidate-layout] ignored transient TF_LC_DESTROY while composing process=" +
                                  Global::current_process_name);
        }
        return;
    }

    EndUIElement();
    EndCandidateUiSession();
    _candidateState.Clear();
    _candidateWindowVisible = FALSE;
    _EndLayout();
}

//+---------------------------------------------------------------------------
//
// _CandidateChangeNotifiction
//
//----------------------------------------------------------------------------

HRESULT CCandidateListUIPresenter::_CandidateChangeNotification(_In_ enum CANDWND_ACTION action)
{
    if (_pTextService == nullptr)
    {
        return E_FAIL;
    }

    HRESULT hr = E_FAIL;

    TfClientId tfClientId = _pTextService->_GetClientId();
    ITfThreadMgr *pThreadMgr = nullptr;
    ITfDocumentMgr *pDocumentMgr = nullptr;
    ITfContext *pContext = nullptr;

    _KEYSTROKE_STATE KeyState;
    KeyState.Category = _Category;
    KeyState.Function = FUNCTION_FINALIZE_CANDIDATELIST;

    if (CAND_ITEM_SELECT != action)
    {
        goto Exit;
    }

    pThreadMgr = _pTextService->_GetThreadMgr();
    if (nullptr == pThreadMgr)
    {
        goto Exit;
    }

    hr = pThreadMgr->GetFocus(&pDocumentMgr);
    if (FAILED(hr))
    {
        goto Exit;
    }
    if (nullptr == pDocumentMgr)
    {
        // GetFocus reports S_OK with a null document manager when no document has focus.
        hr = E_FAIL;
        goto Exit;
    }

    hr = pDocumentMgr->GetTop(&pContext);
    if (FAILED(hr))
    {
        pDocumentMgr->Release();
        goto Exit;
    }
    if (nullptr == pContext)
    {
        // GetTop reports S_OK with a null context when the document stack is empty.
        pDocumentMgr->Release();
        hr = E_FAIL;
        goto Exit;
    }

    CKeyHandlerEditSession *pEditSession = new (std::nothrow) CKeyHandlerEditSession(
        _pTextService, pContext, 0, 0, KeyState, FANY_IME_NO_REQUEST_ID, {}, {},
        _pTextService->_CaptureFocusSessionToken(), 0, _pTextService->_CaptureCompositionEpoch());
    if (nullptr != pEditSession)
    {
        HRESULT hrSession = S_OK;
        hr = pContext->RequestEditSession(tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
        if (hrSession == TF_E_SYNCHRONOUS || hrSession == TS_E_READONLY)
        {
            hr = pContext->RequestEditSession(tfClientId, pEditSession, TF_ES_ASYNC | TF_ES_READWRITE, &hrSession);
        }
        pEditSession->Release();
    }

    pContext->Release();
    pDocumentMgr->Release();

Exit:
    return hr;
}

//+---------------------------------------------------------------------------
//
// _CandWndCallback
//
//----------------------------------------------------------------------------

HRESULT CCandidateListUIPresenter::_UpdateUIElement()
{
    HRESULT hr = S_OK;

    ITfThreadMgr *pThreadMgr = _pTextService->_GetThreadMgr();
    if (nullptr == pThreadMgr)
    {
        return S_OK;
    }

    ITfUIElementMgr *pUIElementMgr = nullptr;

    hr = pThreadMgr->QueryInterface(IID_ITfUIElementMgr, (void **)&pUIElementMgr);
    if (hr == S_OK)
    {
        pUIElementMgr->UpdateUIElement(_uiElementId);
        pUIElementMgr->Release();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// OnSetThreadFocus
//
//----------------------------------------------------------------------------

HRESULT CCandidateListUIPresenter::OnSetThreadFocus()
{
    if (_isShowMode)
    {
        Show(TRUE);
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// OnKillThreadFocus
//
//----------------------------------------------------------------------------

HRESULT CCandidateListUIPresenter::OnKillThreadFocus()
{
    if (_isShowMode)
    {
        Show(FALSE);
    }
    return S_OK;
}

void CCandidateListUIPresenter::AdviseUIChangedByArrowKey(_In_ KEYSTROKE_FUNCTION arrowKey)
{
    switch (arrowKey)
    {
    case FUNCTION_MOVE_UP: {
        _MoveSelection(MOVEUP_ONE);
        break;
    }
    case FUNCTION_MOVE_DOWN: {
        _MoveSelection(MOVEDOWN_ONE);
        break;
    }
    case FUNCTION_MOVE_PAGE_UP: {
        // Page prev
        _MovePage(MOVEUP_ONE);
        break;
    }
    case FUNCTION_MOVE_PAGE_DOWN: {
        // Page next
        _MovePage(MOVEDOWN_ONE);
        break;
    }
    case FUNCTION_MOVE_PAGE_TOP: {
        _SetSelection(MOVETO_TOP);
        break;
    }
    case FUNCTION_MOVE_PAGE_BOTTOM: {
        _SetSelection(MOVETO_BOTTOM);
        break;
    }
    default:
        break;
    }
}

HRESULT CCandidateListUIPresenter::BeginUIElement()
{
    HRESULT hr = S_OK;

    ITfThreadMgr *pThreadMgr = _pTextService->_GetThreadMgr();
    if (nullptr == pThreadMgr)
    {
        hr = E_FAIL;
        goto Exit;
    }

    ITfUIElementMgr *pUIElementMgr = nullptr;
    hr = pThreadMgr->QueryInterface(IID_ITfUIElementMgr, (void **)&pUIElementMgr);
    if (hr == S_OK)
    {
        pUIElementMgr->BeginUIElement(this, &_isShowMode, &_uiElementId);
        pUIElementMgr->Release();
    }

    // Hosts that activate with TF_TMF_UIELEMENTENABLEDONLY (typical for games)
    // must never get an IME-owned candidate HWND, even if BeginUIElement left
    // pbShow TRUE.
    if (_pTextService->_IsUiLessMode())
    {
        _isShowMode = FALSE;
    }
    Global::CandidateUiLessMode = (_isShowMode == FALSE);

Exit:
    return hr;
}

HRESULT CCandidateListUIPresenter::EndUIElement()
{
    HRESULT hr = S_OK;

    ITfThreadMgr *pThreadMgr = _pTextService->_GetThreadMgr();
    if ((nullptr == pThreadMgr) || (-1 == _uiElementId))
    {
        hr = E_FAIL;
        goto Exit;
    }

    ITfUIElementMgr *pUIElementMgr = nullptr;
    hr = pThreadMgr->QueryInterface(IID_ITfUIElementMgr, (void **)&pUIElementMgr);
    if (hr == S_OK)
    {
        pUIElementMgr->EndUIElement(_uiElementId);
        pUIElementMgr->Release();
        _uiElementId = static_cast<DWORD>(-1);
    }

    Global::CandidateUiLessMode = false;

Exit:
    return hr;
}

void CCandidateListUIPresenter::WriteCandidateUiPayload(_In_ UINT writeFlag)
{
    CStringRange keyStringBuffer = _pTextService->GetCompositionProcessorEngine()->GetKeystrokeBuffer();
    std::wstring pinyinString(keyStringBuffer.Get(), keyStringBuffer.GetLength());
    Global::PinyinLength = static_cast<int>(pinyinString.length());

    WriteDataToSharedMemory(   //
        Global::Keycode,       //
        Global::wch,           //
        Global::ModifiersDown, //
        Global::Point,         //
        Global::PinyinLength,  //
        pinyinString,          //
        writeFlag              //
    );
}

void CCandidateListUIPresenter::BeginCandidateUiSession()
{
    if (!_isShowMode)
    {
        return;
    }
    WriteCandidateUiPayload(0b111111);
    SendShowCandidateWndEventToUIProcess();
    _candidateUiSessionActive = TRUE;
}

void CCandidateListUIPresenter::UpdateCandidateUiSession()
{
    if (!_isShowMode)
    {
        return;
    }
    WriteCandidateUiPayload(0b111111);
    SendShowCandidateWndEventToUIProcess();
}

void CCandidateListUIPresenter::MoveCandidateUiSession()
{
    if (_asyncCleanupPending || !_candidateUiSessionActive || !_isShowMode)
    {
        // UILess hosts draw candidates themselves; never chase an IME HWND.
        return;
    }
    WriteCandidateUiPayload(0b001000);
    SendMoveCandidateWndEventToUIProcess();
}

void CCandidateListUIPresenter::_ReplaceCandidateListFromPage(_In_ const std::wstring &page)
{
    const UINT previousSelection = _candidateState.GetSelection();
    _candidateState.Clear();
    _lastUiLessCandidatePage = page;

    size_t start = 0;
    while (start <= page.size())
    {
        const size_t comma = page.find(L',', start);
        const size_t end = (comma == std::wstring::npos) ? page.size() : comma;
        const std::wstring item = page.substr(start, end - start);
        if (!item.empty())
        {
            CCandidateListItem candidate;
            candidate._ItemString.Set(item.c_str(), item.size());
            _candidateState.AddCandidate(&candidate, FALSE);
        }
        if (comma == std::wstring::npos)
        {
            break;
        }
        start = comma + 1;
    }

    const UINT count = _candidateState.GetCount();
    if (count == 0)
    {
        return;
    }

    CMetasequoiaImeArray<CCandidateListItem> pageIndexSource;
    for (UINT i = 0; i < count; ++i)
    {
        pageIndexSource.Append();
    }
    SetPageIndexWithScrollInfo(&pageIndexSource);
    _candidateState.SetSelectionSilently(previousSelection < count ? static_cast<int>(previousSelection) : 0);
}

void CCandidateListUIPresenter::_ApplyUiLessCandidatePage(_In_ const std::wstring &page, int selectedIndex)
{
    _ReplaceCandidateListFromPage(page);
    if (_candidateState.GetCount() != 0)
    {
        const int clamped = (std::max)(0, (std::min)(selectedIndex, static_cast<int>(_candidateState.GetCount()) - 1));
        _candidateState.SetSelectionSilently(clamped);
    }
}

bool CCandidateListUIPresenter::_ConsumeUiLessCompositionReply(uint64_t requestId)
{
    if (requestId == FANY_IME_NO_REQUEST_ID)
    {
        return false;
    }
    struct FanyImeNamedpipeDataToTsf *receivedData =
        TryReadDataFromServerPipeWithTimeout(requestId, /*abortTransportOnTimeout=*/false);
    if (receivedData == nullptr || receivedData->msg_type != Global::DataFromServerMsgType::UiLessComposition)
    {
        return false;
    }

    const std::wstring payload(receivedData->candidate_string);
    std::wstring page;
    int selection = 0;
    const size_t firstTab = payload.find(L'\t');
    if (firstTab == std::wstring::npos)
    {
        page = payload;
    }
    else
    {
        const size_t secondTab = payload.find(L'\t', firstTab + 1);
        if (secondTab == std::wstring::npos)
        {
            page = payload.substr(firstTab + 1);
        }
        else
        {
            page = payload.substr(firstTab + 1, secondTab - firstTab - 1);
            selection = _wtoi(payload.c_str() + secondTab + 1);
        }
    }
    _ApplyUiLessCandidatePage(page, selection);
    _NotifyUiLessHost();
    return true;
}

void CCandidateListUIPresenter::_NotifyUiLessHost()
{
    _updatedFlags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX |
                    TF_CLUIE_CURRENTPAGE;
    _UpdateUIElement();
}

void CCandidateListUIPresenter::_LoadUiLessCandidatesFromSharedMemory()
{
    std::wstring page;
    if (!TryReadCandidatePageFromSharedMemory(&page) || page == _lastUiLessCandidatePage)
    {
        return;
    }
    _ReplaceCandidateListFromPage(page);
}

void CCandidateListUIPresenter::_RequestCancelComposition()
{
    if (_pTextService == nullptr)
    {
        return;
    }

    TfClientId tfClientId = _pTextService->_GetClientId();
    ITfThreadMgr *pThreadMgr = _pTextService->_GetThreadMgr();
    ITfDocumentMgr *pDocumentMgr = nullptr;
    ITfContext *pContext = nullptr;

    _KEYSTROKE_STATE KeyState;
    KeyState.Category = CATEGORY_COMPOSING;
    KeyState.Function = FUNCTION_CANCEL;

    if (nullptr == pThreadMgr)
    {
        return;
    }

    if (FAILED(pThreadMgr->GetFocus(&pDocumentMgr)) || pDocumentMgr == nullptr)
    {
        return;
    }

    if (FAILED(pDocumentMgr->GetTop(&pContext)) || pContext == nullptr)
    {
        pDocumentMgr->Release();
        return;
    }

    CKeyHandlerEditSession *pEditSession = new (std::nothrow) CKeyHandlerEditSession(
        _pTextService, pContext, 0, 0, KeyState, FANY_IME_NO_REQUEST_ID, {}, {},
        _pTextService->_CaptureFocusSessionToken(), 0, _pTextService->_CaptureCompositionEpoch());
    if (nullptr != pEditSession)
    {
        HRESULT hrSession = S_OK;
        HRESULT hr = pContext->RequestEditSession(tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
        if (hrSession == TF_E_SYNCHRONOUS || hrSession == TS_E_READONLY)
        {
            hr = pContext->RequestEditSession(tfClientId, pEditSession, TF_ES_ASYNC | TF_ES_READWRITE, &hrSession);
        }
        UNREFERENCED_PARAMETER(hr);
        pEditSession->Release();
    }

    pContext->Release();
    pDocumentMgr->Release();
}

void CCandidateListUIPresenter::EndCandidateUiSession()
{
    if (!_candidateUiSessionActive)
    {
        return;
    }

    SendHideCandidateWndEventToUIProcess();
    _candidateUiSessionActive = FALSE;
}
