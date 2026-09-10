#pragma once

#include "Private.h"

class CMetasequoiaIME;

POINT GetPhysicalTextAnchor(_In_ ITfContextView *pContextView, _In_ const RECT &textExtent);

class CTfTextLayoutSink : public ITfTextLayoutSink
{
  public:
    CTfTextLayoutSink(_In_ CMetasequoiaIME *pTextService);
    virtual ~CTfTextLayoutSink();

    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID riid, _Outptr_ void **ppvObj);
    STDMETHODIMP_(ULONG) AddRef(void);
    STDMETHODIMP_(ULONG) Release(void);

    // ITfTextLayoutSink
    STDMETHODIMP OnLayoutChange(_In_ ITfContext *pContext, TfLayoutCode lcode, _In_ ITfContextView *pContextView);

    HRESULT _StartLayout(_In_ ITfContext *pContextDocument, TfEditCookie ec, _In_ ITfRange *pRangeComposition);
    VOID _EndLayout();

    HRESULT _GetTextExt(_Out_ RECT *lpRect, _Out_ POINT *lpAnchor);
    ITfContext *_GetContextDocument()
    {
        return _pContextDocument;
    };

    // Callers that read the caret straight from ITfContextView (the edit-session
    // path) report their success here, so _GetTextExt knows whether Global::Point
    // already holds a usable anchor for this layout session.
    void _MarkAnchorValid()
    {
        _hasValidAnchor = true;
    }

    virtual VOID _LayoutChangeNotification(_In_ RECT *lpRect) = 0;
    virtual VOID _LayoutDestroyNotification() = 0;

  private:
    HRESULT _AdviseTextLayoutSink();
    HRESULT _UnadviseTextLayoutSink();

  private:
    ITfRange *_pRangeComposition;
    ITfContext *_pContextDocument;
    TfEditCookie _tfEditCookie;
    CMetasequoiaIME *_pTextService;
    DWORD _dwCookieTextLayoutSink;
    LONG _refCount;
    bool _hasValidAnchor;
};
