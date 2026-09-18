#include "Private.h"
#include "Globals.h"
#include "EditSession.h"
#include "MetasequoiaIME.h"
#include <debugapi.h>
#include <fmt/xchar.h>

namespace
{
HRESULT SafeEndComposition(_In_ ITfComposition *composition, TfEditCookie ec)
{
    if (composition == nullptr)
    {
        return E_INVALIDARG;
    }

    __try
    {
        return composition->EndComposition(ec);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return E_FAIL;
    }
}
} // namespace

//////////////////////////////////////////////////////////////////////
//
//    ITfEditSession
//        CEditSessionBase
// CEndCompositionEditSession class
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// CEndCompositionEditSession
//
//----------------------------------------------------------------------------

class CEndCompositionEditSession : public CEditSessionBase
{
  public:
    CEndCompositionEditSession(_In_ CMetasequoiaIME *pTextService, _In_ ITfContext *pContext,
                               _In_ ITfComposition *expectedComposition, uint64_t focusToken,
                               bool bypassFocusValidation)
        : CEditSessionBase(pTextService, pContext), _expectedComposition(expectedComposition), _focusToken(focusToken),
          _bypassFocusValidation(bypassFocusValidation)
    {
        _expectedComposition->AddRef();
    }

    ~CEndCompositionEditSession() override
    {
        _expectedComposition->Release();
    }

    // ITfEditSession
    STDMETHODIMP DoEditSession(TfEditCookie ec)
    {
        if (!_pTextService->_IsCompositionCurrent(_expectedComposition) ||
            (!_bypassFocusValidation && !_pTextService->_IsFocusSessionCurrent(_focusToken, _pContext)))
        {
            return S_FALSE;
        }
        _pTextService->_TerminateComposition(ec, _pContext, TRUE);
        return S_OK;
    }

  private:
    ITfComposition *_expectedComposition;
    uint64_t _focusToken;
    bool _bypassFocusValidation;
};

//////////////////////////////////////////////////////////////////////
//
// CMetasequoiaIME class
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// _TerminateComposition
//
//----------------------------------------------------------------------------

void CMetasequoiaIME::_TerminateComposition(TfEditCookie ec, _In_ ITfContext *pContext, BOOL isCalledFromDeactivate)
{
    isCalledFromDeactivate;

    if (_pComposition != nullptr)
    {
        // Every operation below crosses COM and can re-enter
        // OnCompositionTerminated. Hold exact local references and only clear
        // service ownership if the same composition is still current after
        // the call returns.
        ITfComposition *terminatingComposition = _pComposition;
        terminatingComposition->AddRef();
        ITfContext *ownerContext = _pContext;
        if (ownerContext)
        {
            ownerContext->AddRef();
        }

        // remove the display attribute from the composition range.
        _ClearCompositionDisplayAttributes(ec, pContext, terminatingComposition);

        // Statistics: classify the committed range text before EndComposition
        // can re-enter OnCompositionTerminated and detach it. Read-only; any
        // failure is silent and the HRESULT below is untouched.
        _CaptureCompositionStats(ec, terminatingComposition);

        const HRESULT endResult = SafeEndComposition(terminatingComposition, ec);
        if (FAILED(endResult) && _pComposition == terminatingComposition)
        {
            // if we fail to EndComposition, then we need to close the reverse reading window.
            _DeleteCandidateList(TRUE, pContext);
        }

        if (_pComposition == terminatingComposition)
        {
            _pComposition->Release();
            _pComposition = nullptr;
            _voiceCompositionActive = false;
            uint64_t nextEpoch = _compositionEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (nextEpoch == 0)
            {
                _compositionEpoch.fetch_add(1, std::memory_order_acq_rel);
            }

            // Clear the context only as part of releasing this exact
            // composition. A re-entrant callback may already have installed a
            // newer composition on the same ITfContext pointer.
            if (_pContext == ownerContext && _pContext)
            {
                _pContext->Release();
                _pContext = nullptr;
            }
        }

        if (ownerContext)
        {
            ownerContext->Release();
        }
        terminatingComposition->Release();
    }
}

//+---------------------------------------------------------------------------
//
// _EndComposition
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_EndComposition(_In_opt_ ITfContext *pContext, _In_opt_ ITfComposition *expectedComposition,
                                         bool bypassFocusValidation)
{
    ITfComposition *target = expectedComposition ? expectedComposition : _pComposition;
    if (pContext == nullptr || target == nullptr)
    {
        return S_FALSE;
    }
    CEndCompositionEditSession *pEditSession = new (std::nothrow)
        CEndCompositionEditSession(this, pContext, target, _CaptureFocusSessionToken(), bypassFocusValidation);
    HRESULT hr = S_OK;

    if (nullptr != pEditSession)
    {
        HRESULT editSessionHr = E_FAIL;
        const HRESULT requestHr = pContext->RequestEditSession(_tfClientId, pEditSession,
                                                               TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &editSessionHr);
        hr = FAILED(requestHr) ? requestHr : editSessionHr;
        pEditSession->Release();
    }
    else
    {
        hr = E_OUTOFMEMORY;
    }
    return hr;
}
