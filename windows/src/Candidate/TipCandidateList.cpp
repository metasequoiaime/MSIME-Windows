#include "Private.h"
#include "TipCandidateList.h"
#include "EnumTfCandidates.h"
#include "TipCandidateString.h"

HRESULT CTipCandidateList::CreateInstance(_Outptr_ ITfCandidateList **ppobj, size_t candStrReserveSize)
{
    if (ppobj == nullptr)
    {
        return E_INVALIDARG;
    }
    *ppobj = nullptr;

    *ppobj = new (std::nothrow) CTipCandidateList(candStrReserveSize);
    if (*ppobj == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    (*ppobj)->AddRef();
    return S_OK;
}

CTipCandidateList::CTipCandidateList(size_t candStrReserveSize)
{
    _refCount = 0;

    if (0 < candStrReserveSize)
    {
        _tfCandStrList.reserve(candStrReserveSize);
    }
}

CTipCandidateList::~CTipCandidateList()
{
    for (UINT i = 0; i < _tfCandStrList.Count(); i++)
    {
        ITfCandidateString **ppCandStr = _tfCandStrList.GetAt(i);
        if ((ppCandStr != nullptr) && (*ppCandStr != nullptr))
        {
            (*ppCandStr)->Release();
        }
    }
}

STDMETHODIMP CTipCandidateList::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (ppvObj == nullptr)
    {
        return E_POINTER;
    }
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown))
    {
        *ppvObj = (ITfCandidateList *)this;
    }
    else if (IsEqualIID(riid, IID_ITfCandidateList))
    {
        *ppvObj = (ITfCandidateList *)this;
    }

    if (*ppvObj == nullptr)
    {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CTipCandidateList::AddRef()
{
    return (ULONG)InterlockedIncrement((LONG *)&_refCount);
}

STDMETHODIMP_(ULONG) CTipCandidateList::Release()
{
    ULONG cRefT = (ULONG)InterlockedDecrement((LONG *)&_refCount);
    if (0 < cRefT)
    {
        return cRefT;
    }

    delete this;

    return 0;
}

STDMETHODIMP CTipCandidateList::EnumCandidates(_Outptr_ IEnumTfCandidates **ppEnum)
{
    return CEnumTfCandidates::CreateInstance(ppEnum, _tfCandStrList);
}

STDMETHODIMP CTipCandidateList::GetCandidate(ULONG nIndex, _Outptr_result_maybenull_ ITfCandidateString **ppCandStr)
{
    if (ppCandStr == nullptr)
    {
        return E_POINTER;
    }
    *ppCandStr = nullptr;

    ULONG sizeCandStr = (ULONG)_tfCandStrList.Count();
    if (sizeCandStr <= nIndex)
    {
        return E_FAIL;
    }

    ITfCandidateString **ppCandStrCur = _tfCandStrList.GetAt(static_cast<UINT>(nIndex));
    if ((ppCandStrCur == nullptr) || (*ppCandStrCur == nullptr))
    {
        return E_FAIL;
    }

    BSTR bstr = nullptr;
    CTipCandidateString *pTipCandidateStrCur = static_cast<CTipCandidateString *>(*ppCandStrCur);
    if (FAILED(pTipCandidateStrCur->GetString(&bstr)))
    {
        return E_FAIL;
    }

    CTipCandidateString *pCandidateString = nullptr;
    const HRESULT hr = CTipCandidateString::CreateInstance(&pCandidateString);
    if (SUCCEEDED(hr) && (pCandidateString != nullptr))
    {
        pCandidateString->SetIndex(nIndex);
        pCandidateString->SetString((LPCWSTR)bstr, SysStringLen(bstr));
        *ppCandStr = pCandidateString;
    }

    SysFreeString(bstr);
    return hr;
}

STDMETHODIMP CTipCandidateList::GetCandidateNum(_Out_ ULONG *pnCnt)
{
    if (pnCnt == nullptr)
    {
        return E_POINTER;
    }

    *pnCnt = (ULONG)(_tfCandStrList.Count());
    return S_OK;
}

STDMETHODIMP CTipCandidateList::SetResult(ULONG nIndex, TfCandidateResult imcr)
{
    nIndex;
    imcr;

    return E_NOTIMPL;
}

STDMETHODIMP CTipCandidateList::SetCandidate(_In_ ITfCandidateString *pCandStr)
{
    if (pCandStr == nullptr)
    {
        return E_POINTER;
    }

    ITfCandidateString **ppCandLast = _tfCandStrList.Append();
    if (ppCandLast)
    {
        *ppCandLast = pCandStr;
        return S_OK;
    }

    pCandStr->Release();
    return E_FAIL;
}
