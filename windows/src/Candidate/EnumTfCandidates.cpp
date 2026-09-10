#include "Private.h"
#include "EnumTfCandidates.h"

HRESULT CEnumTfCandidates::CreateInstance(_Outptr_ IEnumTfCandidates **ppEnum,
                                          _In_ const CMetasequoiaImeArray<ITfCandidateString *> &rgelm, UINT currentNum)
{
    if (ppEnum == nullptr)
    {
        return E_INVALIDARG;
    }
    *ppEnum = new (std::nothrow) CEnumTfCandidates(rgelm, currentNum);
    if (*ppEnum == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    (*ppEnum)->AddRef();
    return S_OK;
}

CEnumTfCandidates::CEnumTfCandidates(_In_ const CMetasequoiaImeArray<ITfCandidateString *> &rgelm, UINT currentNum)
{
    _refCount = 0;
    _rgelm = rgelm;
    _currentCandidateStrIndex = currentNum;
}

CEnumTfCandidates::~CEnumTfCandidates()
{
}

//
// IUnknown methods
//
STDMETHODIMP CEnumTfCandidates::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (ppvObj == nullptr)
    {
        return E_POINTER;
    }
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IEnumTfCandidates)))
    {
        *ppvObj = (IEnumTfCandidates *)this;
    }

    if (*ppvObj == nullptr)
    {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CEnumTfCandidates::AddRef()
{
    return (ULONG)InterlockedIncrement(&_refCount);
}

STDMETHODIMP_(ULONG) CEnumTfCandidates::Release()
{
    ULONG cRef = (ULONG)InterlockedDecrement(&_refCount);
    if (0 < cRef)
    {
        return cRef;
    }

    delete this;
    return 0;
}

//
// IEnumTfCandidates methods
//
STDMETHODIMP CEnumTfCandidates::Next(ULONG ulCount, _Out_ ITfCandidateString **ppObj, _Out_ ULONG *pcFetched)
{
    ULONG fetched = 0;
    if (ppObj == nullptr)
    {
        return E_INVALIDARG;
    }
    *ppObj = nullptr;

    while ((fetched < ulCount) && (_currentCandidateStrIndex < _rgelm.Count()))
    {
        *ppObj = *_rgelm.GetAt(_currentCandidateStrIndex);
        (*ppObj)->AddRef();
        _currentCandidateStrIndex++;
        fetched++;
    }

    if (pcFetched)
    {
        *pcFetched = fetched;
    }

    return (fetched == ulCount) ? S_OK : S_FALSE;
}

STDMETHODIMP CEnumTfCandidates::Skip(ULONG ulCount)
{
    while ((0 < ulCount) && (_currentCandidateStrIndex < _rgelm.Count()))
    {
        _currentCandidateStrIndex++;
        ulCount--;
    }

    return (0 < ulCount) ? S_FALSE : S_OK;
}

STDMETHODIMP CEnumTfCandidates::Reset()
{
    _currentCandidateStrIndex = 0;
    return S_OK;
}

STDMETHODIMP CEnumTfCandidates::Clone(_Out_ IEnumTfCandidates **ppEnum)
{
    return CreateInstance(ppEnum, _rgelm, _currentCandidateStrIndex);
}
