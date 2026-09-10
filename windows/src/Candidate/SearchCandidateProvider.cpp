#include "Private.h"
#include "SearchCandidateProvider.h"
#include <new>
#include "MetasequoiaIME.h"
#include "CompositionProcessorEngine.h"
#include "TipCandidateList.h"
#include "TipCandidateString.h"

/*------------------------------------------------------------------------------

create instance of CSearchCandidateProvider

------------------------------------------------------------------------------*/
HRESULT CSearchCandidateProvider::CreateInstance(_Outptr_ ITfFnSearchCandidateProvider **ppobj,
                                                 _In_ ITfTextInputProcessorEx *ptip)
{
    if (ppobj == nullptr)
    {
        return E_INVALIDARG;
    }
    *ppobj = nullptr;

    *ppobj = new (std::nothrow) CSearchCandidateProvider(ptip);
    if (nullptr == *ppobj)
    {
        return E_OUTOFMEMORY;
    }

    (*ppobj)->AddRef();
    return S_OK;
}

/*------------------------------------------------------------------------------

constructor of CSearchCandidateProvider

------------------------------------------------------------------------------*/
CSearchCandidateProvider::CSearchCandidateProvider(_In_ ITfTextInputProcessorEx *ptip)
{
    assert(ptip != nullptr);

    _pTip = ptip;
    _refCount = 0;
}

/*------------------------------------------------------------------------------

destructor of CSearchCandidateProvider

------------------------------------------------------------------------------*/
CSearchCandidateProvider::~CSearchCandidateProvider(void)
{
}

/*------------------------------------------------------------------------------

query interface
(IUnknown method)

------------------------------------------------------------------------------*/
STDMETHODIMP CSearchCandidateProvider::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (ppvObj == nullptr)
    {
        return E_POINTER;
    }
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ITfFnSearchCandidateProvider)))
    {
        *ppvObj = (ITfFnSearchCandidateProvider *)this;
    }
    else if (IsEqualIID(riid, IID_ITfFunction))
    {
        *ppvObj = (ITfFunction *)this;
    }

    if (*ppvObj == nullptr)
    {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

/*------------------------------------------------------------------------------

increment reference count
(IUnknown method)

------------------------------------------------------------------------------*/
STDMETHODIMP_(ULONG) CSearchCandidateProvider::AddRef()
{
    return (ULONG)InterlockedIncrement(&_refCount);
}

/*------------------------------------------------------------------------------

decrement reference count and release object
(IUnknown method)

------------------------------------------------------------------------------*/
STDMETHODIMP_(ULONG) CSearchCandidateProvider::Release()
{
    ULONG ref = (ULONG)InterlockedDecrement(&_refCount);
    if (0 < ref)
    {
        return ref;
    }

    delete this;
    return 0;
}

STDMETHODIMP CSearchCandidateProvider::GetDisplayName(_Out_ BSTR *pbstrName)
{
    if (pbstrName == nullptr)
    {
        return E_INVALIDARG;
    }

    *pbstrName = SysAllocString(L"SearchCandidateProvider");
    return S_OK;
}

STDMETHODIMP CSearchCandidateProvider::GetSearchCandidates(BSTR bstrQuery, BSTR bstrApplicationID,
                                                           _Outptr_result_maybenull_ ITfCandidateList **pplist)
{
    bstrApplicationID;
    bstrQuery;
    HRESULT hr = E_FAIL;
    *pplist = nullptr;

    if (nullptr == _pTip)
    {
        return hr;
    }

    CCompositionProcessorEngine *pCompositionProcessorEngine =
        ((CMetasequoiaIME *)_pTip)->GetCompositionProcessorEngine();
    if (nullptr == pCompositionProcessorEngine)
    {
        return hr;
    }

    CMetasequoiaImeArray<CCandidateListItem> candidateList;
    pCompositionProcessorEngine->GetCandidateList(&candidateList, TRUE, FALSE);

    int cCand = min(candidateList.Count(), FAKECANDIDATENUMBER);
    if (0 < cCand)
    {
        hr = CTipCandidateList::CreateInstance(pplist, cCand);
        if (FAILED(hr))
        {
            return hr;
        }
        for (int iCand = 0; iCand < cCand; iCand++)
        {
            CTipCandidateString *pCandStr = nullptr;
            hr = CTipCandidateString::CreateInstance(&pCandStr);
            if (FAILED(hr) || (pCandStr == nullptr))
            {
                return hr;
            }

            pCandStr->SetIndex(iCand);
            pCandStr->SetString(candidateList.GetAt(iCand)->_ItemString.Get(),
                                candidateList.GetAt(iCand)->_ItemString.GetLength());

            ((CTipCandidateList *)(*pplist))->SetCandidate(pCandStr);
        }
    }
    hr = S_OK;

    return hr;
}

/*------------------------------------------------------------------------------

set result
(ITfFnSearchCandidateProvider method)

------------------------------------------------------------------------------*/
STDMETHODIMP CSearchCandidateProvider::SetResult(BSTR bstrQuery, BSTR bstrApplicationID, BSTR bstrResult)
{
    bstrQuery;
    bstrApplicationID;
    bstrResult;

    return E_NOTIMPL;
}
