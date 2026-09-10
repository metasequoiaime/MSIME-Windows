#include "Private.h"
#include "EditSession.h"
#include "GetTextExtentEditSession.h"
#include "TfTextLayoutSink.h"
#include "Ipc.h"
#include "FanyDefines.h"
#include <debugapi.h>
#include <fmt/xchar.h>
#include "../Utils/PerfTimer.h"

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CGetTextExtentEditSession::CGetTextExtentEditSession(_In_ CMetasequoiaIME *pTextService, _In_ ITfContext *pContext,
                                                     _In_ ITfContextView *pContextView,
                                                     _In_ ITfRange *pRangeComposition,
                                                     _In_ CTfTextLayoutSink *pTfTextLayoutSink)
    : CEditSessionBase(pTextService, pContext)
{
    _pContextView = pContextView;
    _pRangeComposition = pRangeComposition;
    _pTfTextLayoutSink = pTfTextLayoutSink;
}

//+---------------------------------------------------------------------------
//
// ITfEditSession::DoEditSession
//
//----------------------------------------------------------------------------

STDAPI CGetTextExtentEditSession::DoEditSession(TfEditCookie ec)
{
    PerfTimer timer;
    RECT rc = {0, 0, 0, 0};
    BOOL isClipped = TRUE;
    HRESULT hr = S_OK;
    double getTextExtElapsedMs = 0;
    double layoutChangeElapsedMs = 0;

    PerfTimer getTextExtTimer;
    hr = _pContextView->GetTextExt(ec, _pRangeComposition, &rc, &isClipped);
    getTextExtElapsedMs = getTextExtTimer.ElapsedMs();

    if (SUCCEEDED(hr))
    {
        // Office may set isClipped with a usable rect; ignore only empty extents.
        if (isClipped && (rc.right <= rc.left || rc.bottom <= rc.top))
        {
            Global::Point[0] = 0;
            Global::Point[1] = Global::INVALID_Y;
            return S_OK;
        }

        const POINT anchor = GetPhysicalTextAnchor(_pContextView, rc);
        Global::Point[0] = anchor.x;
        Global::Point[1] = anchor.y;
        // This is the per-keystroke caret source. Tell the sink the session now
        // has a usable anchor, so a later transient TS_E_NOLAYOUT can be answered
        // by keeping this position instead of parking the window off-screen.
        _pTfTextLayoutSink->_MarkAnchorValid();
        if (Global::current_process_name == Global::ZEN_BROWSER)
        {
            Global::firefox_like_cnt++;
            if (Global::firefox_like_cnt == 3)
            {
                PerfTimer layoutChangeTimer;
                _pTfTextLayoutSink->_LayoutChangeNotification(&rc);
                layoutChangeElapsedMs = layoutChangeTimer.ElapsedMs();
            }
        }
        else
        {
            PerfTimer layoutChangeTimer;
            _pTfTextLayoutSink->_LayoutChangeNotification(&rc);
            layoutChangeElapsedMs = layoutChangeTimer.ElapsedMs();
        }
    }

    return S_OK;
}
