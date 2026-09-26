#pragma once

#include "WebView2.h"
#include "fmt/core.h"
#include "fmt/xchar.h"
#include <boost/locale.hpp>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include <wrl.h>
#include <wrl/client.h>
#include <wil/com.h>
#include <dcomp.h>
#include <cmath>
#include "utils/common_utils.h"
#include "utils/window_utils.h"
#include "global/globals.h"

using namespace Microsoft::WRL;

const std::wstring LocalAssetsPath = fmt::format( //
    L"{}\\assets",                                //
    CommonUtils::get_ime_data_path_w()            //
);

inline std::wstring GetLocalAssetsPath()
{
    // Prefer a live lookup: static LocalAssetsPath can be empty if CRT init ran
    // before the user profile environment was fully available.
    const std::wstring live = fmt::format(L"{}\\assets", CommonUtils::get_ime_data_path_w());
    if (live.empty() || live[0] == L'\\')
    {
        return LocalAssetsPath;
    }
    return live;
}

void UpdateHtmlContentWithJavaScript( //
    ComPtr<ICoreWebView2> webview,    //
    const std::wstring &newContent    //
);                                    //
void UpdateHtmlContentWithJavaScript( //
    ComPtr<ICoreWebView2> webview,    //
    const std::wstring &newContent,   //
    std::function<void()> onComplete  //
);                                    //

//
// 候选窗口 webview
//
inline ComPtr<ICoreWebView2Controller> webviewControllerCandWnd;
inline ComPtr<ICoreWebView2> webviewCandWnd;
inline ComPtr<ICoreWebView2_3> webview3CandWnd;
inline ComPtr<ICoreWebView2Controller2> webviewController2CandWnd;
inline ComPtr<ICoreWebView2Controller3> webviewController3CandWnd;
inline EventRegistrationToken candidateRasterizationScaleChangedToken{};
inline bool candidateRasterizationScaleChangedRegistered = false;

inline std::wstring HTMLStringCandWnd = LR"()";
inline std::wstring BodyStringCandWnd = LR"()";
inline std::wstring MeasureStringCandWnd = LR"()";
inline std::wstring StrCandWnd = L"";

int PrepareHtmlForWnds();
bool ApplyConfiguredCandidateWindowLayout();
bool ApplyConfiguredUiThemes();
// Reconcile the actual WebView skin even when another thread consumed the
// config file timestamp before the UI timer observed it.
bool ApplyConfiguredCandidateSkinIfChanged();
bool ForceReloadConfiguredCandidateSkin();
uint64_t GetCandidateSkinReloadRevision();
bool ApplyConfiguredCandidateAppearance();
bool ApplyConfiguredFloatingToolbarAppearance();
bool ApplyConfiguredFloatingToolbarAppearance(std::function<void()> onComplete);
// Geometry declared by the active external skin package. Built-in skins return 0.
double GetActiveCandidateSkinDecorationTopDip();
double GetActiveCandidateSkinDecorationWidthDip();
// Push half-monitor CSS max width/height (DIP) into a small-window page.
void InjectSurfaceViewportLimits(ICoreWebView2 *webview, HWND hwnd);
// WebView2 rasterization scale includes both monitor DPI and the user's text
// scaling. It can therefore differ from GetDpiForWindow()/96.
FLOAT GetWebViewRasterizationScale(HWND hwnd);
HalfScreenDipLimits QueryWebViewHalfScreenDipLimitsForHwnd(HWND hwnd);
HalfScreenDipLimits QueryCandidateHalfScreenDipLimitsForPoint(HWND hwnd, POINT pt);
void ResetContainerHoverCandWnd(ComPtr<ICoreWebView2> webview);
void DisableMouseForAWhileWhenShownCandWnd(ComPtr<ICoreWebView2> webview);
// CSS hover is gated on .hover-active. WebView2 synthesizes mousemove when
// the HWND is placed under a stationary cursor; arm only after the OS cursor
// has actually moved (GetCursorPos), not after a Chromium mouse event.
void DisarmCandidatePointerHover();
void MaybeArmCandidatePointerHover();
void InflateCandWnd(std::wstring &str);
void InflateCandWnd(std::wstring &str, std::function<void()> onComplete);
void InflateCandWnd(std::wstring &str, std::function<void()> onComplete, bool contentOnly);
// DIP size of #realContainer from the last slot-update script, or {0,0}.
std::pair<double, double> LastCandidateSlotMeasuredSize();
void InflateMeasureDivCandWnd(std::wstring &str);
void InflateMeasureDivCandWnd(std::wstring &str, std::function<void()> onComplete);
void InitSmallWindowWebviews(HWND candHwnd, HWND menuHwnd, HWND ftbHwnd);
void ShutdownWebviews();
void UpdateSmallWindowWebviewVisibility(HWND hwnd, bool visible);
// Kick (or retry) shared small-window WebView2 init. Returns true when the menu
// controller already exists and the tray menu may be shown.
bool PrepareTrayMenuWebviewForShow();
// Remember a candidate show that arrived before this host's WebView2 was
// ready, and replay WM_SHOW_MAIN_WINDOW once navigation completes.
void DeferCandidateShowUntilWebviewReady();
void MaybeFlushPendingCandidateShow();
// After the candidate controller exists, pin TOPMOST and renotify WebView2
// (same uiAccess rule as the tray menu: never pin before CreateCoreWebView2Controller).
void RaiseCandidateHostForShow(const wchar_t *reason);
// Expand WebView bounds beyond the host client so horizontal measure/layout
// is not constrained by a still-narrow HWND.
void PrepareCandidateWebViewBoundsForMeasure(HWND hwnd);
void SyncCandidateWebViewBoundsToHost(HWND hwnd);
// Capture geometry-only browser/CSS state for clipping investigations. Never
// includes DOM text or candidate content.
void LogCandidateLayoutSnapshot(const wchar_t *stage);

// uiAccess + HWND WebView2: request TOPMOST on first real show. If WebViews are
// not ready yet, the request is queued and applied after all small-window
// navigations complete (avoid TopMost during CreateCoreWebView2Controller).
// Returns true when TOPMOST is already in effect after the call.
bool EnsureSmallWindowsTopmost(const wchar_t *reason);
// True only once all three hosts have reached the topmost band. The band is
// entered one host at a time on a timer, so this stays false for a few seconds
// after the WebViews report ready.
bool AreSmallWindowsTopmostApplied();
bool AreSmallWindowWebviewsReady();
bool IsCandidateWebviewReady();
// True once the floating toolbar's WebView2 has completed its first navigation
// and can actually paint. Its host must stay on-monitor and "visible" until
// then, otherwise WebView2 never finishes raster setup for it.
bool IsFloatingToolbarWebviewReady();
void NotifyFloatingToolbarPageReady();
// True while the post-navigation paint grace is active: the host is kept shown
// briefly after NavigationCompleted, then visibility is reconciled for real.
bool IsFloatingToolbarPaintGraceActive();
void BeginFloatingToolbarPaintGrace();
void EndFloatingToolbarPaintGrace();
void ClearFloatingToolbarNavigationState();
// True only when the tray menu is actually open in front of the user: its
// WebView2 has painted at least once and the host is neither hidden nor still
// DWM-cloaked for warmup. IsWindowVisible() alone cannot answer this, because
// the warmup leaves the host "visible" for the whole of startup.
bool IsTrayMenuOpenToUser();
// Controller-side view of the toolbar for the diagnostic trace: what WebView2
// itself believes about visibility and the area it was told to paint. Returns
// false when no controller exists yet. Must be called on the UI thread.
bool GetFloatingToolbarWebviewState(bool &isVisible, RECT &bounds);
// Same for the tray menu, whose blank-but-clickable failure mode needs exactly
// this to be separated from a host that was never uncloaked.
bool GetTrayMenuWebviewState(bool &isVisible, RECT &bounds);
bool GetCandidateWebviewState(bool &isVisible, RECT &bounds);
void LogSmallWindowReadyGate(const wchar_t *context);
// Lift the tray menu to the front of the small-window topmost band (e.g. after
// FTB was pinned last), renotifying its controller about the band change. Only
// call this once the menu has content: during WebView2 warmup a topmost host is
// what breaks controller creation and compositing. The show path may call it
// while still cloaked, since by then the first navigation has completed.
void RaiseTrayMenuAboveSmallWindows(const wchar_t *reason);

//
// 菜单窗口 webview
//
inline ComPtr<ICoreWebView2Controller> webviewControllerMenuWnd;
inline ComPtr<ICoreWebView2> webviewMenuWnd;
inline ComPtr<ICoreWebView2_3> webview3MenuWnd;
inline ComPtr<ICoreWebView2Controller2> webviewController2MenuWnd;

inline std::wstring HTMLStringMenuWnd = LR"()";

// Sync tray-menu floating-toolbar toggle with general.floating_toolbar.
void SyncMenuFloatingToolbarToggle();

//
// settings 窗口 webview
//
inline ComPtr<ICoreWebView2Controller> webviewControllerSettingsWnd;
inline ComPtr<ICoreWebView2CompositionController> webviewCompositionControllerSettingsWnd;
inline ComPtr<ICoreWebView2> webviewSettingsWnd;
inline ComPtr<ICoreWebView2_3> webview3SettingsWnd;
inline ComPtr<ICoreWebView2Controller2> webviewController2SettingsWnd;
inline ComPtr<IDCompositionDevice> dcompDeviceSettingsWnd;
inline ComPtr<IDCompositionTarget> dcompTargetSettingsWnd;
inline ComPtr<IDCompositionVisual> dcompRootVisualSettingsWnd;
inline RECT maximizeButtonRectSettingsWnd{};
inline bool hasMaximizeButtonRectSettingsWnd = false;
inline bool isMaximizeButtonHoverSettingsWnd = false;

inline std::wstring HTMLStringSettingsWnd = LR"()";

void InitWebviewSettingsWnd(HWND hwnd);
void PostSettingsWindowState(HWND hwnd);
void PostSettingsConfig();

//
// floating toolbar 窗口 webview
//
inline ComPtr<ICoreWebView2Controller> webviewControllerFtbWnd;
inline ComPtr<ICoreWebView2> webviewFtbWnd;
inline ComPtr<ICoreWebView2_3> webview3FtbWnd;
inline ComPtr<ICoreWebView2Controller2> webviewController2FtbWnd;

inline std::wstring HTMLStringFtbWnd = LR"()";

// These entry points always update the canonical toolbar state, even when the
// WebView is not ready. A successful FTB navigation replays the complete state.
void UpdateFtbCnEnState(ComPtr<ICoreWebView2> webview, int cnEnState);
void UpdateFtbCnEnAndPuncState(ComPtr<ICoreWebView2> webview, int cnEnState, int puncState);
void UpdateFtbCnEnAndDoubleSingleAndPuncState( //
    ComPtr<ICoreWebView2> webview,             //
    int cnEnState,                             //
    int doubleSingleByteState,                 //
    int puncState,                             //
    int capsLockState = 0                      //
);
void UpdateFtbPuncState(ComPtr<ICoreWebView2> webview, int puncState);
void UpdateFtbDoubleSingleByteState(ComPtr<ICoreWebView2> webview, int doubleSingleByteState);
void UpdateFtbEnglishInputModeState(ComPtr<ICoreWebView2> webview, int enabled);
void UpdateFtbCapsLockState(ComPtr<ICoreWebView2> webview, int enabled);
void UpdateFtbCharacterSetState(ComPtr<ICoreWebView2> webview);
void UpdateFtbInputModeState(ComPtr<ICoreWebView2> webview, int japaneseMode);
// Reapply configured optional buttons to the live toolbar and resize its host.
void ApplyConfiguredFloatingToolbarItems();
