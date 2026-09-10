#pragma once

#include "Private.h"
#include "MetasequoiaIMEBaseStructure.h"

//
// CTipCandidateList
//

class CTipCandidateList : public ITfCandidateList
{
  protected:
    CTipCandidateList(size_t candStrReserveSize);
    virtual ~CTipCandidateList();

  public:
    static HRESULT CreateInstance(_Outptr_ ITfCandidateList **ppobj, size_t candStrReserveSize = 0);

    // IUnknown methods
    virtual STDMETHODIMP QueryInterface(REFIID riid, _Outptr_ void **ppvObj);
    virtual STDMETHODIMP_(ULONG) AddRef();
    virtual STDMETHODIMP_(ULONG) Release();

    // ITfCandidateList methods
    virtual STDMETHODIMP EnumCandidates(_Outptr_ IEnumTfCandidates **ppEnum);
    virtual STDMETHODIMP GetCandidate(ULONG nIndex, _Outptr_result_maybenull_ ITfCandidateString **ppCandStr);
    virtual STDMETHODIMP GetCandidateNum(_Out_ ULONG *pnCnt);
    virtual STDMETHODIMP SetResult(ULONG nIndex, TfCandidateResult imcr);

    virtual STDMETHODIMP SetCandidate(_In_ ITfCandidateString *pCandStr);

  protected:
    long _refCount;
    CMetasequoiaImeArray<ITfCandidateString *> _tfCandStrList;
};
