#pragma once

#include "Private.h"
#include <string>

class CTipCandidateString : public ITfCandidateString
{
  protected:
    CTipCandidateString();
    virtual ~CTipCandidateString();

  public:
    static HRESULT CreateInstance(_Outptr_ CTipCandidateString **ppobj);

    // IUnknown methods
    virtual STDMETHODIMP QueryInterface(REFIID riid, _Outptr_ void **ppvObj);
    virtual STDMETHODIMP_(ULONG) AddRef();
    virtual STDMETHODIMP_(ULONG) Release();

    // ITfCandidateString methods
    virtual STDMETHODIMP GetString(BSTR *pbstr);
    virtual STDMETHODIMP GetIndex(_Out_ ULONG *pnIndex);

    virtual STDMETHODIMP SetIndex(ULONG uIndex);
    virtual STDMETHODIMP SetString(_In_ const WCHAR *pch, DWORD_PTR length);

  protected:
    long _refCount;
    int _index;
    std::wstring _candidateStr;
};
