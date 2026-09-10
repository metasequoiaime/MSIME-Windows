#include "Private.h"
#include "TipCandidateString.h"

HRESULT CTipCandidateString::CreateInstance(_Outptr_ CTipCandidateString **ppobj)
{
    if (ppobj == nullptr)
    {
        return E_INVALIDARG;
    }
    *ppobj = nullptr;

    *ppobj = new (std::nothrow) CTipCandidateString();
    if (*ppobj == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    (*ppobj)->AddRef();
    return S_OK;
}

CTipCandidateString::CTipCandidateString(void)
{
    _refCount = 0;
    _index = 0;
}

CTipCandidateString::~CTipCandidateString()
{
}

// IUnknown methods
STDMETHODIMP CTipCandidateString::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (ppvObj == nullptr)
    {
        return E_POINTER;
    }
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown))
    {
        *ppvObj = (CTipCandidateString *)this;
    }
    else if (IsEqualIID(riid, IID_ITfCandidateString))
    {
        *ppvObj = (CTipCandidateString *)this;
    }

    if (*ppvObj == nullptr)
    {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CTipCandidateString::AddRef(void)
{
    return (ULONG)InterlockedIncrement((LONG *)&_refCount);
}

STDMETHODIMP_(ULONG) CTipCandidateString::Release(void)
{
    ULONG refT = (ULONG)InterlockedDecrement((LONG *)&_refCount);
    if (0 < refT)
    {
        return refT;
    }

    delete this;

    return 0;
}

// ITfCandidateString methods
STDMETHODIMP CTipCandidateString::GetString(BSTR *pbstr)
{
    *pbstr = SysAllocString(_candidateStr.c_str());
    return S_OK;
}

STDMETHODIMP CTipCandidateString::GetIndex(_Out_ ULONG *pnIndex)
{
    if (pnIndex == nullptr)
    {
        return E_POINTER;
    }

    *pnIndex = _index;
    return S_OK;
}

STDMETHODIMP CTipCandidateString::SetIndex(ULONG uIndex)
{
    _index = uIndex;
    return S_OK;
}

STDMETHODIMP CTipCandidateString::SetString(_In_ const WCHAR *pch, DWORD_PTR length)
{
    _candidateStr.assign(pch, 0, length);
    return S_OK;
}
