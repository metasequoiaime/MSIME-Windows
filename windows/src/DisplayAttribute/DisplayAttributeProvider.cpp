#include "Private.h"
#include "Globals.h"
#include "MetasequoiaIME.h"
#include "DisplayAttributeInfo.h"
#include "EnumDisplayAttributeInfo.h"

//+---------------------------------------------------------------------------
//
// ITfDisplayAttributeProvider::EnumDisplayAttributeInfo
//
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::EnumDisplayAttributeInfo(__RPC__deref_out_opt IEnumTfDisplayAttributeInfo **ppEnum)
{
    CEnumDisplayAttributeInfo *pAttributeEnum = nullptr;

    if (ppEnum == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppEnum = nullptr;

    pAttributeEnum = new (std::nothrow) CEnumDisplayAttributeInfo();
    if (pAttributeEnum == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    *ppEnum = pAttributeEnum;

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfDisplayAttributeProvider::GetDisplayAttributeInfo
//
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::GetDisplayAttributeInfo(__RPC__in REFGUID guidInfo,
                                                __RPC__deref_out_opt ITfDisplayAttributeInfo **ppInfo)
{
    if (ppInfo == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppInfo = nullptr;

    // Which display attribute GUID?
    if (IsEqualGUID(guidInfo, Global::MetasequoiaIMEGuidDisplayAttributeInput))
    {
        *ppInfo = new (std::nothrow) CDisplayAttributeInfoInput();
        if ((*ppInfo) == nullptr)
        {
            return E_OUTOFMEMORY;
        }
    }
    else if (IsEqualGUID(guidInfo, Global::MetasequoiaIMEGuidDisplayAttributeConverted))
    {
        *ppInfo = new (std::nothrow) CDisplayAttributeInfoConverted();
        if ((*ppInfo) == nullptr)
        {
            return E_OUTOFMEMORY;
        }
    }
    else
    {
        return E_INVALIDARG;
    }

    return S_OK;
}
