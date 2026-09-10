#ifndef UNICODE
#define UNICODE
#endif // !UNICODE

#ifndef _UNICODE
#define _UNICODE
#endif // !UNICODE

#include "Private.h"
#include "MetasequoiaIME.h"
#include "CompositionProcessorEngine.h"
#include "LanguageBar.h"
#include "Globals.h"
#include "Compartment.h"
#include "Ipc.h"
#include "fmt/xchar.h"

#include <atomic>

namespace
{
// Taskbar/system tray follows SystemUsesLightTheme (0 = dark).
bool IsSystemDarkMode()
{
    DWORD value = 1; // default to light
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
    {
        return false;
    }
    return value == 0;
}

DWORD ResolveThemeIconIndex(DWORD lightIconIndex)
{
    if (!IsSystemDarkMode())
    {
        return lightIconIndex;
    }

    if (lightIconIndex == static_cast<DWORD>(IME_MODE_ON_ICON_INDEX))
    {
        return static_cast<DWORD>(IME_MODE_ON_DARK_ICON_INDEX);
    }
    if (lightIconIndex == static_cast<DWORD>(IME_MODE_ON_JP_ICON_INDEX))
    {
        return static_cast<DWORD>(IME_MODE_ON_JP_DARK_ICON_INDEX);
    }
    if (lightIconIndex == static_cast<DWORD>(IME_MODE_CAP_ICON_INDEX))
    {
        return static_cast<DWORD>(IME_MODE_CAP_DARK_ICON_INDEX);
    }
    if (lightIconIndex == static_cast<DWORD>(IME_MODE_OFF_ICON_INDEX))
    {
        return static_cast<DWORD>(IME_MODE_OFF_DARK_ICON_INDEX);
    }
    return lightIconIndex;
}

// Design size of the mode icons for language bar / taskbar indicators (at 96 DPI).
constexpr int kSmallIconDesignSize = 16;

// Avoids relying on USER_DEFAULT_SCREEN_DPI, which older Windows SDK headers
// targeted by this project may not provide.
constexpr int kUserDefaultScreenDpi = 96;

using GetDpiForMonitorFn = HRESULT(WINAPI *)(HMONITOR, int /*MONITOR_DPI_TYPE*/, UINT *, UINT *);
using SetThreadDpiAwarenessCtxFn = HANDLE(WINAPI *)(HANDLE);

// Entry points are resolved lazily and cached in these atomics instead of
// function-local statics: the project compiles with /Zc:threadSafeInit-, under
// which magic statics are NOT thread-safe, while GetIcon can be reached from
// several TSF threads of the host process. Failed lookups are not cached - they
// are cheap enough to retry on the next call.
std::atomic<GetDpiForMonitorFn> s_getDpiForMonitor{nullptr};
std::atomic<SetThreadDpiAwarenessCtxFn> s_setThreadDpiAwarenessCtx{nullptr};

template <typename T> T ResolveOnce(std::atomic<T> &slot, T (*resolver)())
{
    T resolved = slot.load(std::memory_order_acquire);
    if (resolved == nullptr)
    {
        resolved = resolver();
        if (resolved != nullptr)
        {
            slot.store(resolved, std::memory_order_release);
        }
    }
    return resolved;
}

GetDpiForMonitorFn ResolveGetDpiForMonitor()
{
    // shcore.dll ships with Windows 8.1+; most hosts already have it mapped.
    HMODULE shcore = ::GetModuleHandleW(L"shcore.dll");
    if (!shcore)
    {
        shcore = ::LoadLibraryW(L"shcore.dll"); // system DLL; intentionally kept until process exit
    }
    return shcore ? reinterpret_cast<GetDpiForMonitorFn>(::GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
}

SetThreadDpiAwarenessCtxFn ResolveSetThreadDpiAwarenessContext()
{
    // user32.dll is always mapped inside any host of this DLL.
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    return user32 ? reinterpret_cast<SetThreadDpiAwarenessCtxFn>(
                        ::GetProcAddress(user32, "SetThreadDpiAwarenessContext")) // Win10 1607+
                  : nullptr;
}
} // namespace

//+---------------------------------------------------------------------------
//
// CMetasequoiaIME::_UpdateLanguageBarOnSetFocus
//
//----------------------------------------------------------------------------

void CMetasequoiaIME::_UpdateLanguageBarOnSetFocus(_In_ ITfDocumentMgr *pDocMgrFocus)
{
    BOOL needDisableButtons = FALSE;

    if (!pDocMgrFocus)
    {
        needDisableButtons = TRUE;
    }
    else
    {
        IEnumTfContexts *pEnumContext = nullptr;

        if (FAILED(pDocMgrFocus->EnumContexts(&pEnumContext)) || !pEnumContext)
        {
            needDisableButtons = TRUE;
        }
        else
        {
            ULONG fetched = 0;
            ITfContext *pContext = nullptr;

            if (FAILED(pEnumContext->Next(1, &pContext, &fetched)) || fetched != 1)
            {
                needDisableButtons = TRUE;
            }

            if (!pContext)
            {
                // context is not associated
                needDisableButtons = TRUE;
            }
            else
            {
                pContext->Release();
            }
        }

        if (pEnumContext)
        {
            pEnumContext->Release();
        }
    }

    CCompositionProcessorEngine *pCompositionProcessorEngine = nullptr;
    pCompositionProcessorEngine = _pCompositionProcessorEngine;

    pCompositionProcessorEngine->SetLanguageBarStatus(TF_LBI_STATUS_DISABLED, needDisableButtons);

    // SetStatus intentionally keeps the item enabled when focus is absent so
    // Firefox does not replace our mode glyph with its disabled/close glyph.
    // Consequently, returning to a document may not change the status bits and
    // therefore may not generate an ITfLangBarItemSink update.  Always refresh
    // the newly focused thread's item: Windows caches each host/thread's icon,
    // and another host may have changed the shared Chinese/Japanese mode while
    // this one was in the background.
    if (!needDisableButtons)
    {
        pCompositionProcessorEngine->RefreshLanguageBarIcons();
    }
}

//+---------------------------------------------------------------------------
//
// CCompositionProcessorEngine::SetLanguageBarStatus
//
//----------------------------------------------------------------------------

VOID CCompositionProcessorEngine::SetLanguageBarStatus(DWORD status, BOOL isSet)
{
    if (_pLanguageBar_IMEMode)
    {
        _pLanguageBar_IMEMode->SetStatus(status, isSet);
    }
    if (_pLanguageBar_DoubleSingleByte)
    {
        _pLanguageBar_DoubleSingleByte->SetStatus(status, isSet);
    }
    if (_pLanguageBar_Punctuation)
    {
        _pLanguageBar_Punctuation->SetStatus(status, isSet);
    }
}

void CCompositionProcessorEngine::RefreshLanguageBarIcons()
{
    if (_pLanguageBar_IMEMode)
    {
        _pLanguageBar_IMEMode->RefreshIcon();
    }
    if (_pLanguageBar_DoubleSingleByte)
    {
        _pLanguageBar_DoubleSingleByte->RefreshIcon();
    }
    if (_pLanguageBar_Punctuation)
    {
        _pLanguageBar_Punctuation->RefreshIcon();
    }
}

//+---------------------------------------------------------------------------
//
// CLangBarItemButton::ctor
//
//----------------------------------------------------------------------------

CLangBarItemButton::CLangBarItemButton(REFGUID guidLangBar, LPCWSTR description, LPCWSTR tooltip, DWORD onIconIndex,
                                       DWORD offIconIndex, BOOL isSecureMode)
{
    DWORD bufLen = 0;

    DllAddRef();

    // initialize TF_LANGBARITEMINFO structure.
    _tfLangBarItemInfo.clsidService = Global::MetasequoiaIMECLSID; // This LangBarItem belongs to this TextService.
    _tfLangBarItemInfo.guidItem = guidLangBar;                     // GUID of this LangBarItem.
    _tfLangBarItemInfo.dwStyle = (TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY); // This LangBar is a button type.
    _tfLangBarItemInfo.ulSort = 0; // The position of this LangBar Item is not specified.
    StringCchCopy(_tfLangBarItemInfo.szDescription, ARRAYSIZE(_tfLangBarItemInfo.szDescription),
                  description); // Set the description of this LangBar Item.

    // Initialize the sink pointer to NULL.
    _pLangBarItemSink = nullptr;

    // Initialize ICON index and file name.
    _onIconIndex = onIconIndex;
    _offIconIndex = offIconIndex;

    // Initialize compartment.
    _pCompartment = nullptr;
    _pCompartmentEventSink = nullptr;

    _isAddedToLanguageBar = FALSE;
    _isSecureMode = isSecureMode;
    _status = 0;

    _refCount = 1;

    // Initialize Tooltip
    _pTooltipText = nullptr;
    if (tooltip)
    {
        size_t len = 0;
        if (StringCchLength(tooltip, STRSAFE_MAX_CCH, &len) != S_OK)
        {
            len = 0;
        }
        bufLen = static_cast<DWORD>(len) + 1;
        _pTooltipText = (LPCWSTR) new (std::nothrow) WCHAR[bufLen];
        if (_pTooltipText)
        {
            StringCchCopy((LPWSTR)_pTooltipText, bufLen, tooltip);
        }
    }
}

//+---------------------------------------------------------------------------
//
// CLangBarItemButton::dtor
//
//----------------------------------------------------------------------------

CLangBarItemButton::~CLangBarItemButton()
{
    DllRelease();
    CleanUp();
}

//+---------------------------------------------------------------------------
//
// CLangBarItemButton::CleanUp
//
//----------------------------------------------------------------------------

void CLangBarItemButton::CleanUp()
{
    if (_pTooltipText)
    {
        delete[] _pTooltipText;
        _pTooltipText = nullptr;
    }

    // TSF still holds the advised _pCompartmentEventSink until _Unadvise runs, so unregister before the
    // deletes below and independently of the thread manager: under COM-less activation (TF_TMAE_COMLESS)
    // CoCreateInstance fails, and freeing a still-advised sink leaves msctf calling into released memory.
    _UnregisterCompartment();

    ITfThreadMgr *pThreadMgr = nullptr;
    HRESULT hr =
        CoCreateInstance(CLSID_TF_ThreadMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr, (void **)&pThreadMgr);
    if (SUCCEEDED(hr))
    {
        _RemoveItem(pThreadMgr);
        pThreadMgr->Release();
        pThreadMgr = nullptr;
    }

    if (_pCompartment)
    {
        delete _pCompartment;
        _pCompartment = nullptr;
    }

    if (_pCompartmentEventSink)
    {
        delete _pCompartmentEventSink;
        _pCompartmentEventSink = nullptr;
    }
}

//+---------------------------------------------------------------------------
//
// CLangBarItemButton::QueryInterface
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfLangBarItem) ||
        IsEqualIID(riid, IID_ITfLangBarItemButton))
    {
        *ppvObj = (ITfLangBarItemButton *)this;
    }
    else if (IsEqualIID(riid, IID_ITfSource))
    {
        *ppvObj = (ITfSource *)this;
    }

    if (*ppvObj)
    {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

//+---------------------------------------------------------------------------
//
// CLangBarItemButton::AddRef
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CLangBarItemButton::AddRef()
{
    return ++_refCount;
}

//+---------------------------------------------------------------------------
//
// CLangBarItemButton::Release
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CLangBarItemButton::Release()
{
    LONG cr = --_refCount;

    assert(_refCount >= 0);

    if (_refCount == 0)
    {
        delete this;
    }

    return cr;
}

//+---------------------------------------------------------------------------
//
// GetInfo
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::GetInfo(_Out_ TF_LANGBARITEMINFO *pInfo)
{
    _tfLangBarItemInfo.dwStyle |= TF_LBI_STYLE_SHOWNINTRAY;
    *pInfo = _tfLangBarItemInfo;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// GetStatus
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::GetStatus(_Out_ DWORD *pdwStatus)
{
    if (pdwStatus == nullptr)
    {
        return E_INVALIDARG;
    }

    *pdwStatus = _status;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// SetStatus
//
//----------------------------------------------------------------------------

void CLangBarItemButton::SetStatus(DWORD dwStatus, BOOL fSet)
{
    BOOL isChange = FALSE;

    if (fSet)
    {
        if (!(_status & dwStatus))
        {
            // _status |= dwStatus;
            _status &= ~dwStatus;
            isChange = TRUE;
        }

        if (isChange && _pLangBarItemSink)
        {
            _pLangBarItemSink->OnUpdate(TF_LBI_STATUS | TF_LBI_ICON);
        }
    }
    else
    {
        if (_status & dwStatus)
        {
            _status &= ~dwStatus;
            isChange = TRUE;
        }

        if (isChange && _pLangBarItemSink)
        {
            _pLangBarItemSink->OnUpdate(TF_LBI_STATUS | TF_LBI_ICON);
        }
    }

    return;
}

//+---------------------------------------------------------------------------
//
// RefreshIcon
//
//----------------------------------------------------------------------------

void CLangBarItemButton::RefreshIcon()
{
    if (_pLangBarItemSink)
    {
        _pLangBarItemSink->OnUpdate(TF_LBI_ICON | TF_LBI_STATUS | TF_LBI_TOOLTIP);
    }
}

//+---------------------------------------------------------------------------
//
// Show
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::Show(BOOL fShow)
{
    fShow;
    if (_pLangBarItemSink)
    {
        _pLangBarItemSink->OnUpdate(TF_LBI_STATUS);
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// GetTooltipString
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::GetTooltipString(_Out_ BSTR *pbstrToolTip)
{
    *pbstrToolTip = SysAllocString(_pTooltipText);

    return (*pbstrToolTip == nullptr) ? E_OUTOFMEMORY : S_OK;
}

//+---------------------------------------------------------------------------
//
// OnClick
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::OnClick(TfLBIClick click, POINT pt, _In_ const RECT *prcArea)
{
    if (click == TF_LBI_CLK_RIGHT)
    {
        SendLangbarRightClickEventToUIProcess(prcArea);
        return S_OK;
    }

    if (click == TF_LBI_CLK_LEFT && _pCompartment)
    {
        BOOL isOn = FALSE;

        _pCompartment->_GetCompartmentBOOL(isOn);
        _pCompartment->_SetCompartmentBOOL(isOn ? FALSE : TRUE);
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// InitMenu
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::InitMenu(_In_ ITfMenu *pMenu)
{
    pMenu;

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// OnMenuSelect
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::OnMenuSelect(UINT wID)
{
    wID;

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// GetIcon
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::GetIcon(_Out_ HICON *phIcon)
{
    BOOL isOn = FALSE;

    if (!_pCompartment)
    {
        return E_FAIL;
    }
    if (!phIcon)
    {
        return E_FAIL;
    }
    *phIcon = nullptr;

    _pCompartment->_GetCompartmentBOOL(isOn);

    DWORD status = 0;
    GetStatus(&status);

    // GetIcon runs on the *focused application's* TSF thread, so sizing must
    // not depend on that process's DPI awareness. The taskbar lays the input
    // indicator out in physical pixels of its monitor (e.g. a 28px slot at
    // 175%), while GetSystemMetrics(SM_CXSMICON) reports the system-DPI
    // snapshot cached when the host started (e.g. 20px). The shell then has to
    // upscale our returned icon into the larger slot, which is the blur of
    // issue #50 (focus-dependent only because explorer happens to start at the
    // correct scale).
    //
    // Query the primary monitor's effective DPI instead. Per MS docs,
    // GetDpiForMonitor scales its answer to the caller's DPI awareness
    // (PROCESS_DPI_UNAWARE -> 96, PROCESS_SYSTEM_DPI_AWARE -> system DPI,
    // PROCESS_PER_MONITOR_DPI_AWARE -> the real value), so temporarily flip
    // this thread to PER_MONITOR_AWARE_V2 around the query; otherwise unaware
    // or system-aware hosts would still end up with the wrong size. Hosts
    // lacking these APIs fall back to the legacy SM_CXSMICON path unchanged.
    int desiredSize = kSmallIconDesignSize;
    bool sizedByMonitorDpi = false;
    if (HMONITOR mon = ::MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY))
    {
        SetThreadDpiAwarenessCtxFn setCtx =
            ResolveOnce(s_setThreadDpiAwarenessCtx, &ResolveSetThreadDpiAwarenessContext);
        HANDLE prevCtx = nullptr;
        if (setCtx)
        {
            // (HANDLE)(INT_PTR)-4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
            // spelled out via casts to avoid requiring WINVER >= Win10 targets.
            prevCtx = setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
        }

        UINT dpiX = 0;
        UINT dpiY = 0;
        GetDpiForMonitorFn getDpi = ResolveOnce(s_getDpiForMonitor, &ResolveGetDpiForMonitor);
        if (getDpi && SUCCEEDED(getDpi(mon, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY)) && dpiX > 0)
        {
            desiredSize = MulDiv(kSmallIconDesignSize, static_cast<int>(dpiX), kUserDefaultScreenDpi);
            sizedByMonitorDpi = true;
        }

        if (prevCtx)
        {
            setCtx(prevCtx); // restore the caller's thread awareness
        }
    }
    if (!sizedByMonitorDpi)
    {
        // No shcore/awareness-switch available: exactly the legacy behavior.
        desiredSize = GetSystemMetrics(SM_CXSMICON);
    }
    if (desiredSize <= 0)
    {
        desiredSize = kSmallIconDesignSize;
    }
    // UAC/secure desktop historically expects at least 24x24.
    if (_isSecureMode && desiredSize < 24)
    {
        desiredSize = 24;
    }

    DWORD lightIconIndex = (isOn && !(status & TF_LBI_STATUS_DISABLED)) ? _onIconIndex : _offIconIndex;
    if (!(status & TF_LBI_STATUS_DISABLED) && _onIconIndex == static_cast<DWORD>(IME_MODE_ON_ICON_INDEX) &&
        Global::CapsLockEnabled.load(std::memory_order_relaxed))
    {
        lightIconIndex = static_cast<DWORD>(IME_MODE_CAP_ICON_INDEX);
    }
    else if (isOn && !(status & TF_LBI_STATUS_DISABLED) && _onIconIndex == static_cast<DWORD>(IME_MODE_ON_ICON_INDEX) &&
             Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed))
    {
        lightIconIndex = static_cast<DWORD>(IME_MODE_ON_JP_ICON_INDEX);
    }
    const DWORD iconIndex = ResolveThemeIconIndex(lightIconIndex);

    if (Global::dllInstanceHandle)
    {
        *phIcon = reinterpret_cast<HICON>(
            LoadImage(Global::dllInstanceHandle, MAKEINTRESOURCE(iconIndex), IMAGE_ICON, desiredSize, desiredSize, 0));
    }

    return (*phIcon != NULL) ? S_OK : E_FAIL;
}

//+---------------------------------------------------------------------------
//
// GetText
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::GetText(_Out_ BSTR *pbstrText)
{
    *pbstrText = SysAllocString(_tfLangBarItemInfo.szDescription);

    return (*pbstrText == nullptr) ? E_OUTOFMEMORY : S_OK;
}

//+---------------------------------------------------------------------------
//
// AdviseSink
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::AdviseSink(__RPC__in REFIID riid, __RPC__in_opt IUnknown *punk, __RPC__out DWORD *pdwCookie)
{
    // We allow only ITfLangBarItemSink interface.
    if (!IsEqualIID(IID_ITfLangBarItemSink, riid))
    {
        return CONNECT_E_CANNOTCONNECT;
    }

    // We support only one sink once.
    if (_pLangBarItemSink != nullptr)
    {
        return CONNECT_E_ADVISELIMIT;
    }

    // Query the ITfLangBarItemSink interface and store it into _pLangBarItemSink.
    if (punk == nullptr)
    {
        return E_INVALIDARG;
    }
    if (punk->QueryInterface(IID_ITfLangBarItemSink, (void **)&_pLangBarItemSink) != S_OK)
    {
        _pLangBarItemSink = nullptr;
        return E_NOINTERFACE;
    }

    // return our cookie.
    *pdwCookie = _cookie;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// UnadviseSink
//
//----------------------------------------------------------------------------

STDAPI CLangBarItemButton::UnadviseSink(DWORD dwCookie)
{
    // Check the given cookie.
    if (dwCookie != _cookie)
    {
        return CONNECT_E_NOCONNECTION;
    }

    // If there is nno connected sink, we just fail.
    if (_pLangBarItemSink == nullptr)
    {
        return CONNECT_E_NOCONNECTION;
    }

    _pLangBarItemSink->Release();
    _pLangBarItemSink = nullptr;

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _AddItem
//
//----------------------------------------------------------------------------

HRESULT CLangBarItemButton::_AddItem(_In_ ITfThreadMgr *pThreadMgr)
{
    HRESULT hr = S_OK;
    ITfLangBarItemMgr *pLangBarItemMgr = nullptr;

    if (_isAddedToLanguageBar)
    {
        return S_OK;
    }

    hr = pThreadMgr->QueryInterface(IID_ITfLangBarItemMgr, (void **)&pLangBarItemMgr);
    if (SUCCEEDED(hr))
    {
        hr = pLangBarItemMgr->AddItem(this);
        if (SUCCEEDED(hr))
        {
            _isAddedToLanguageBar = TRUE;
        }
        pLangBarItemMgr->Release();
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// _RemoveItem
//
//----------------------------------------------------------------------------

HRESULT CLangBarItemButton::_RemoveItem(_In_ ITfThreadMgr *pThreadMgr)
{
    HRESULT hr = S_OK;
    ITfLangBarItemMgr *pLangBarItemMgr = nullptr;

    if (!_isAddedToLanguageBar)
    {
        return S_OK;
    }

    hr = pThreadMgr->QueryInterface(IID_ITfLangBarItemMgr, (void **)&pLangBarItemMgr);
    if (SUCCEEDED(hr))
    {
        hr = pLangBarItemMgr->RemoveItem(this);
        if (SUCCEEDED(hr))
        {
            _isAddedToLanguageBar = FALSE;
        }
        pLangBarItemMgr->Release();
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// _RegisterCompartment
//
//----------------------------------------------------------------------------

BOOL CLangBarItemButton::_RegisterCompartment(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                                              REFGUID guidCompartment)
{
    _pCompartment = new (std::nothrow) CCompartment(pThreadMgr, tfClientId, guidCompartment);
    if (_pCompartment)
    {
        // Advice ITfCompartmentEventSink
        _pCompartmentEventSink = new (std::nothrow) CCompartmentEventSink(_CompartmentCallback, this);
        if (_pCompartmentEventSink)
        {
            _pCompartmentEventSink->_Advise(pThreadMgr, guidCompartment);
        }
        else
        {
            delete _pCompartment;
            _pCompartment = nullptr;
        }
    }

    return _pCompartment ? TRUE : FALSE;
}

//+---------------------------------------------------------------------------
//
// _UnregisterCompartment
//
//----------------------------------------------------------------------------

BOOL CLangBarItemButton::_UnregisterCompartment()
{
    if (_pCompartment)
    {
        // Unadvice ITfCompartmentEventSink
        if (_pCompartmentEventSink)
        {
            _pCompartmentEventSink->_Unadvise();
        }

        // clear ITfCompartment
        _pCompartment->_ClearCompartment();
    }

    return TRUE;
}

//+---------------------------------------------------------------------------
//
// _CompartmentCallback
//
//----------------------------------------------------------------------------

// static
HRESULT CLangBarItemButton::_CompartmentCallback(_In_ void *pv, REFGUID guidCompartment)
{
    CLangBarItemButton *fakeThis = (CLangBarItemButton *)pv;

    GUID guid = GUID_NULL;
    fakeThis->_pCompartment->_GetGUID(&guid);

    if (IsEqualGUID(guid, guidCompartment))
    {
        if (fakeThis->_pLangBarItemSink)
        {
            fakeThis->_pLangBarItemSink->OnUpdate(TF_LBI_STATUS | TF_LBI_ICON);
        }
    }

    return S_OK;
}
