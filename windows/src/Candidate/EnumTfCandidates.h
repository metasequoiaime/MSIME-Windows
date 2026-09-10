#pragma once

#include "Globals.h"

class CEnumTfCandidates : public IEnumTfCandidates
{
  protected:
    // constructor/destructor
    CEnumTfCandidates(_In_ const CMetasequoiaImeArray<ITfCandidateString *> &rgelm, UINT currentNum);

    virtual ~CEnumTfCandidates(void);

  public:
    // create instance
    static HRESULT CreateInstance(_Outptr_ IEnumTfCandidates **ppEnum,
                                  _In_ const CMetasequoiaImeArray<ITfCandidateString *> &rgelm, UINT currentNum = 0);

    // IUnknown methods
    virtual STDMETHODIMP QueryInterface(REFIID riid, _Outptr_ void **ppvObj);
    virtual STDMETHODIMP_(ULONG) AddRef(void);
    virtual STDMETHODIMP_(ULONG) Release(void);

    // IEnumTfCandidates methods
    virtual STDMETHODIMP Next(ULONG ulCount, _Out_ ITfCandidateString **ppObj, _Out_ ULONG *pcFetched);
    virtual STDMETHODIMP Skip(ULONG ulCount);
    virtual STDMETHODIMP Reset();
    virtual STDMETHODIMP Clone(_Out_ IEnumTfCandidates **ppEnum);

  protected:
    LONG _refCount;
    CMetasequoiaImeArray<ITfCandidateString *> _rgelm;
    UINT _currentCandidateStrIndex;
};
