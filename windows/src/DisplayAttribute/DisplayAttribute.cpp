#include "Private.h"
#include "Globals.h"
#include "MetasequoiaIME.h"

//+---------------------------------------------------------------------------
//
// _ClearCompositionDisplayAttributes
//
//----------------------------------------------------------------------------

void CMetasequoiaIME::_ClearCompositionDisplayAttributes(TfEditCookie ec, _In_ ITfContext *pContext,
                                                         _In_opt_ ITfComposition *expectedComposition)
{
    ITfRange *pRangeComposition = nullptr;
    ITfProperty *pDisplayAttributeProperty = nullptr;
    ITfComposition *composition = expectedComposition ? expectedComposition : _pComposition;
    if (composition == nullptr)
    {
        return;
    }

    // get the compositon range.
    if (FAILED(composition->GetRange(&pRangeComposition)))
    {
        return;
    }

    // get our the display attribute property
    if (SUCCEEDED(pContext->GetProperty(GUID_PROP_ATTRIBUTE, &pDisplayAttributeProperty)))
    {
        // clear the value over the range
        pDisplayAttributeProperty->Clear(ec, pRangeComposition);

        pDisplayAttributeProperty->Release();
    }

    pRangeComposition->Release();
}

//+---------------------------------------------------------------------------
//
// _SetCompositionDisplayAttributes
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_SetCompositionDisplayAttributes(TfEditCookie ec, _In_ ITfContext *pContext,
                                                       TfGuidAtom gaDisplayAttribute)
{
    ITfRange *pRangeComposition = nullptr;
    HRESULT hr = S_OK;

    // we need a range and the context it lives in
    hr = _pComposition->GetRange(&pRangeComposition);
    if (FAILED(hr))
    {
        return FALSE;
    }

    hr = E_FAIL;
    BOOL ret = _SetCompositionDisplayAttributesForRange(ec, pContext, pRangeComposition, gaDisplayAttribute);
    pRangeComposition->Release();
    return ret;
}

//+---------------------------------------------------------------------------
//
// _SetCompositionDisplayAttributesForRange
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_SetCompositionDisplayAttributesForRange(TfEditCookie ec, _In_ ITfContext *pContext,
                                                               _In_ ITfRange *pRangeComposition,
                                                               TfGuidAtom gaDisplayAttribute)
{
    if (pRangeComposition == nullptr)
    {
        return FALSE;
    }

    ITfProperty *pDisplayAttributeProperty = nullptr;
    HRESULT hr = E_FAIL;

    // get our the display attribute property
    if (SUCCEEDED(pContext->GetProperty(GUID_PROP_ATTRIBUTE, &pDisplayAttributeProperty)))
    {
        VARIANT var;
        // set the value over the range
        // the application will use this guid atom to lookup the acutal rendering information
        var.vt = VT_I4; // we're going to set a TfGuidAtom
        var.lVal = gaDisplayAttribute;

        hr = pDisplayAttributeProperty->SetValue(ec, pRangeComposition, &var);

        pDisplayAttributeProperty->Release();
    }

    return (hr == S_OK);
}

//+---------------------------------------------------------------------------
//
// _InitDisplayAttributeGuidAtom
//
// Because it's expensive to map our display attribute GUID to a TSF
// TfGuidAtom, we do it once when Activate is called.
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_InitDisplayAttributeGuidAtom()
{
    ITfCategoryMgr *pCategoryMgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                  (void **)&pCategoryMgr);

    if (FAILED(hr))
    {
        return FALSE;
    }

    // register the display attribute for input text.
    hr = pCategoryMgr->RegisterGUID(Global::MetasequoiaIMEGuidDisplayAttributeInput, &_gaDisplayAttributeInput);
    if (FAILED(hr))
    {
        goto Exit;
    }
    // register the display attribute for the converted text.
    hr = pCategoryMgr->RegisterGUID(Global::MetasequoiaIMEGuidDisplayAttributeConverted, &_gaDisplayAttributeConverted);
    if (FAILED(hr))
    {
        goto Exit;
    }

Exit:
    pCategoryMgr->Release();

    return (hr == S_OK);
}
