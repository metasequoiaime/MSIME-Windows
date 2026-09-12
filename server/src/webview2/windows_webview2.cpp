#include "engine/contracts/webview/validator.h"
#include "windows_webview2.h"
#include "webview2/inline_protocol.h"
#include "webview2/candidate_window_template.h"
#include "webview2/skin_css_policy.h"
#include "config/ime_config.h"
#include "defines/globals.h"
#include "utils/common_utils.h"
#include "utils/ime_utils.h"
#include "utils/webview_utils.h"
#include "window/candidate_presenter.h"
#include "window/floating_toolbar_presenter.h"
#include "window/tray_menu_presenter.h"
#include "window/floating_toolbar_visibility_policy.h"
#include <debugapi.h>
#include <boost/json.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <windows.h>
#include <dwmapi.h>
#include <winuser.h>
#include "defines/defines.h"
#include "global/globals.h"
#include "fmt/xchar.h"
#include "ipc/ipc.h"
#include "ipc/event_listener.h"
#include "ipc/candidate_ui_action_policy.h"
#include "log/candidate_diag_log.h"
#include "log/ftb_diag_log.h"
#include "settings/settings_launcher.h"
#include "skin/candidate_skin_catalog.h"
#include "utils/window_utils.h"
#include "voice-input/voice_input_service.h"
#include <WebView2EnvironmentOptions.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <vector>

// WebView diagnostics were useful while fixing the rendering issues, but they
// overwhelm the input-latency trace. Keep these call sites compiled out.
// The disabled macro still has to *mention* its arguments, otherwise every HRESULT and parameter
// that exists only to be logged looks unreferenced (C4189/C4100). sizeof keeps them in an
// unevaluated context, so nothing is computed and no side effect runs — only the name is used.
template <typename... Args> int DiscardDiagLogArgs(const Args &...);
#undef DIAG_LOGF
#define DIAG_LOGF(...) ((void)sizeof(DiscardDiagLogArgs(__VA_ARGS__)))
#define CAND_WEBVIEW_TRACE_LOGF(...)                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::DiagnosticLog::IsEnabled())                                                                              \
        {                                                                                                              \
            ::DiagnosticLog::Write(fmt::format(__VA_ARGS__));                                                          \
        }                                                                                                              \
    } while (0)

#pragma comment(lib, "dcomp.lib")

namespace json = boost::json;

int FineTuneWindow(HWND hwnd);
void ApplyConfiguredFloatingToolbarVisibility(const wchar_t *reason);
void ApplyConfiguredFloatingToolbarSize();
void ReconcileFloatingToolbarVisibilityAfterReady(const wchar_t *reason);
void ApplyConfiguredInputScheme();
void ApplyConfiguredShuangpinSchema();
void BroadcastLowercaseNumberModeToTsf();
bool EnsureSmallWindowsTopmost(const wchar_t *reason);
void UpdateSmallWindowWebviewVisibility(HWND hwnd, bool visible);
void SetCandidateHostCloaked(bool cloaked);
void ClearFloatingToolbarNavigationState();
std::wstring DescribeTrayMenuHostState();
std::wstring DescribeCandidateHostState();
std::wstring GetAppdataPath();
HRESULT OnEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env);
HRESULT OnMenuWindowEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env);
HRESULT OnFtbWindowEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env);

// Legacy floor kept for 100% DPI; high-DPI reserves are computed from DIPs below.
constexpr int candidateBoundExtraFloorPx = 1000;

std::wstring bodyRes = L"";
std::string loadedCandidateSkin;
std::string loadedFloatingToolbarSkin;
std::string preparedCandidateSkin;
uint64_t candidateSkinReloadRevision = 0;

namespace
{
ComPtr<ICoreWebView2Environment> smallWindowWebviewEnvironment;

HWND smallWindowCandHwnd = nullptr;
HWND smallWindowMenuHwnd = nullptr;
HWND smallWindowFtbHwnd = nullptr;

enum class SmallWindowInitState
{
    Idle,
    InProgress,
    Ready,
    Failed
};

SmallWindowInitState smallWindowInitState = SmallWindowInitState::Idle;
int smallWindowInitAttempts = 0;
bool pendingTrayMenuShow = false;
bool pendingCandidateShow = false;
bool floatingToolbarNavigationReady = false;
// After NavigationCompleted, keep the host shown briefly so a cold WebView2
// user-data folder can finish first paint; then reconcile hide/show for real.
bool floatingToolbarPaintGraceActive = false;
bool floatingToolbarNavigationRetryUsed = false;
// WebView2 rejects a CreateCoreWebView2Controller request with E_INVALIDARG when
// another controller creation on the same environment is still in flight. The
// three small windows must therefore be brought up strictly one at a time.
bool smallWindowControllerRequestInFlight = false;
ULONGLONG smallWindowControllerRequestStartTick = 0;
constexpr ULONGLONG kSmallWindowControllerRequestTimeoutMs = 15000;
constexpr int kMaxSmallWindowInitAttempts = 12;
constexpr UINT_PTR kRetrySmallWindowWebviewTimerId = 9001;
// Avoid remasure storms when HTML keeps reporting contentTruncated after DPI.
constexpr ULONGLONG kContentTruncationCooldownMs = 400;
ULONGLONG g_last_content_truncation_ftb_ms = 0;
ULONGLONG g_last_content_truncation_menu_ms = 0;
ULONGLONG g_last_content_truncation_cand_ms = 0;

std::optional<CandidateSkinCatalog::Package> activeExternalCandidateSkin;

bool AllowContentTruncationRemeasure(ULONGLONG &last_ms)
{
    const ULONGLONG now = GetTickCount64();
    if (last_ms != 0 && now - last_ms < kContentTruncationCooldownMs)
    {
        return false;
    }
    last_ms = now;
    return true;
}

constexpr double kTruncationSizeFactor = 1.2;

double JsonNumberAsDouble(const json::value &value)
{
    if (value.is_double())
    {
        return value.as_double();
    }
    if (value.is_int64())
    {
        return static_cast<double>(value.as_int64());
    }
    if (value.is_uint64())
    {
        return static_cast<double>(value.as_uint64());
    }
    return 0.0;
}

ICoreWebView2Controller *ControllerForHost(HWND hwnd)
{
    if (hwnd == ::global_hwnd)
        return webviewControllerCandWnd.Get();
    if (hwnd == ::global_hwnd_menu)
        return webviewControllerMenuWnd.Get();
    if (hwnd == ::global_hwnd_ftb)
        return webviewControllerFtbWnd.Get();
    if (hwnd == ::global_hwnd_settings)
        return webviewControllerSettingsWnd.Get();
    return nullptr;
}

HalfScreenDipLimits ApplyRasterizationScale(HalfScreenDipLimits limits, FLOAT scale)
{
    if (scale <= 0.0f)
        return limits;
    const double monitorWidthPx = static_cast<double>((std::max)(1, limits.monitor.right - limits.monitor.left));
    const double monitorHeightPx = static_cast<double>((std::max)(1, limits.monitor.bottom - limits.monitor.top));
    limits.scale = scale;
    limits.maxWidthDip = (monitorWidthPx * 0.5) / static_cast<double>(scale);
    limits.maxHeightDip = (monitorHeightPx * 0.5) / static_cast<double>(scale);
    return limits;
}

} // namespace

double GetActiveCandidateSkinDecorationTopDip()
{
    return activeExternalCandidateSkin ? activeExternalCandidateSkin->decorationTopDip : 0.0;
}

double GetActiveCandidateSkinDecorationWidthDip()
{
    return activeExternalCandidateSkin ? activeExternalCandidateSkin->decorationWidthDip : 0.0;
}

FLOAT GetWebViewRasterizationScale(HWND hwnd)
{
    ICoreWebView2Controller *controller = ControllerForHost(hwnd);
    if (controller)
    {
        ComPtr<ICoreWebView2Controller3> controller3;
        double scale = 0.0;
        if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller3))) &&
            SUCCEEDED(controller3->get_RasterizationScale(&scale)) && std::isfinite(scale) && scale > 0.0)
        {
            return static_cast<FLOAT>(scale);
        }
    }
    return GetWindowScale(hwnd);
}

HalfScreenDipLimits QueryWebViewHalfScreenDipLimitsForHwnd(HWND hwnd)
{
    return ApplyRasterizationScale(QueryHalfScreenDipLimitsForHwnd(hwnd), GetWebViewRasterizationScale(hwnd));
}

HalfScreenDipLimits QueryCandidateHalfScreenDipLimitsForPoint(HWND hwnd, POINT pt)
{
    HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForPoint(pt);
    const FLOAT hostNativeScale = GetWindowScale(hwnd);
    const FLOAT webViewScale = GetWebViewRasterizationScale(hwnd);
    // RasterizationScale = monitor DPI scale * user text scale. Preserve the
    // text-scale component when the caret is on another monitor.
    const FLOAT textScale = hostNativeScale > 0.0f ? webViewScale / hostNativeScale : 1.0f;
    const FLOAT targetScale = limits.scale * (textScale > 0.0f ? textScale : 1.0f);
    return ApplyRasterizationScale(limits, targetScale);
}

namespace
{

void InjectSurfaceViewportLimitsImpl(ICoreWebView2 *webview, HWND hwnd)
{
    if (!webview || !hwnd)
    {
        return;
    }
    const HalfScreenDipLimits limits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
    nlohmann::json cfg = {
        {"maxWidthDip", limits.maxWidthDip}, {"maxHeightDip", limits.maxHeightDip}, {"scale", limits.scale}};
    const std::wstring script = L"(function(c){"
                                L"const root=document.documentElement;"
                                L"if(!root)return;"
                                L"root.style.setProperty('--msime-max-width-dip',String(c.maxWidthDip||0)+'px');"
                                L"root.style.setProperty('--msime-max-height-dip',String(c.maxHeightDip||0)+'px');"
                                L"root.style.setProperty('--msime-dpi-scale',String(c.scale||1));"
                                L"if(window.CheckContentTruncation)window.CheckContentTruncation();"
                                L"})(" +
                                string_to_wstring(cfg.dump()) + L");";
    const HRESULT hr = webview->ExecuteScript(script.c_str(), nullptr);
    DIAG_LOGF(L"ui-viewport-limits hwnd={:#x} monitor=({},{})-({},{}) max_dip=({:.2f},{:.2f}) "
              L"scale={:.3f} submit_hr={:#x}",
              reinterpret_cast<UINT_PTR>(hwnd), limits.monitor.left, limits.monitor.top, limits.monitor.right,
              limits.monitor.bottom, limits.maxWidthDip, limits.maxHeightDip, static_cast<double>(limits.scale),
              static_cast<unsigned>(hr));
}

// Fallback only: grow the host to at least 1.2x HTML content (DIP), capped at
// half the monitor. Never shrink — truncation means the viewport is too small.
// Candidate hosts are already quarter-screen; shrinking them to the card and
// clearing CSS margins was parking the window away from the caret.
bool ApplyContentTruncationResize(       //
    HWND hwnd,                           //
    ICoreWebView2 *webview,              //
    ICoreWebView2Controller *controller, //
    double contentWidthDip,              //
    double contentHeightDip,             //
    int extraShadowDip                   //
)
{
    if (!hwnd || contentWidthDip < 1.0 || contentHeightDip < 1.0)
    {
        return false;
    }
    const HalfScreenDipLimits limits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
    const double cappedContentW = ClampWidthDipToHalfScreen(contentWidthDip, limits);
    const double cappedContentH = ClampHeightDipToHalfScreen(contentHeightDip, limits);
    double hostWidthDip = ClampWidthDipToHalfScreen(cappedContentW * kTruncationSizeFactor, limits) +
                          static_cast<double>((std::max)(0, extraShadowDip));
    double hostHeightDip = ClampHeightDipToHalfScreen(cappedContentH * kTruncationSizeFactor, limits) +
                           static_cast<double>((std::max)(0, extraShadowDip));

    int physWidth = static_cast<int>(std::ceil(hostWidthDip * static_cast<double>(limits.scale)));
    int physHeight = static_cast<int>(std::ceil(hostHeightDip * static_cast<double>(limits.scale)));
    const int monitorWidth = (std::max)(1, limits.monitor.right - limits.monitor.left);
    const int monitorHeight = (std::max)(1, limits.monitor.bottom - limits.monitor.top);
    physWidth = (std::min)(physWidth, monitorWidth);
    physHeight = (std::min)(physHeight, monitorHeight);

    RECT current{};
    GetWindowRect(hwnd, &current);
    const int curW = current.right - current.left;
    const int curH = current.bottom - current.top;
    // Grow-only: a larger current host (e.g. quarter-screen candidate) must stay.
    physWidth = (std::max)(physWidth, curW);
    physHeight = (std::max)(physHeight, curH);
    int posX = current.left;
    int posY = current.top;
    if (posX + physWidth > limits.monitor.right)
    {
        posX = limits.monitor.right - physWidth;
    }
    if (posY + physHeight > limits.monitor.bottom)
    {
        posY = limits.monitor.bottom - physHeight;
    }
    if (posX < limits.monitor.left)
    {
        posX = limits.monitor.left;
    }
    if (posY < limits.monitor.top)
    {
        posY = limits.monitor.top;
    }

    const bool sizeUnchanged = std::abs(curW - physWidth) <= 1 && std::abs(curH - physHeight) <= 1;
    if (!sizeUnchanged)
    {
        SetWindowPos(hwnd, nullptr, posX, posY, physWidth, physHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (controller)
    {
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        controller->put_Bounds(bounds);
        controller->NotifyParentWindowPositionChanged();
    }
    InjectSurfaceViewportLimitsImpl(webview, hwnd);
    FTB_DIAG_LOGF(
        L"trunc-fallback dip=({:.1f},{:.1f})→host=({:.1f},{:.1f}) px=({},{}) half=({:.1f},{:.1f}) scale={:.3f}",
        contentWidthDip, contentHeightDip, hostWidthDip, hostHeightDip, physWidth, physHeight, limits.maxWidthDip,
        limits.maxHeightDip, static_cast<double>(limits.scale));
    return !sizeUnchanged;
}

bool HandleContentTruncatedMessage(      //
    HWND hwnd,                           //
    ICoreWebView2 *webview,              //
    ICoreWebView2Controller *controller, //
    const json::value &val,              //
    ULONGLONG &cooldownSlot,             //
    int extraShadowDip                   //
)
{
    double widthDip = 0.0;
    double heightDip = 0.0;
    double viewportWidthDip = 0.0;
    double viewportHeightDip = 0.0;
    std::string surface = "unknown";
    if (const auto *data = val.as_object().if_contains("data"))
    {
        if (data->is_object())
        {
            const auto &obj = data->as_object();
            if (const auto *w = obj.if_contains("width"))
            {
                widthDip = JsonNumberAsDouble(*w);
            }
            if (const auto *h = obj.if_contains("height"))
            {
                heightDip = JsonNumberAsDouble(*h);
            }
            if (const auto *w = obj.if_contains("viewportWidth"))
            {
                viewportWidthDip = JsonNumberAsDouble(*w);
            }
            if (const auto *h = obj.if_contains("viewportHeight"))
            {
                viewportHeightDip = JsonNumberAsDouble(*h);
            }
            if (const auto *s = obj.if_contains("surface"); s && s->is_string())
            {
                surface = s->as_string().c_str();
            }
        }
    }
    const bool cooldownAllowed = AllowContentTruncationRemeasure(cooldownSlot);
    DIAG_LOGF(L"ui-truncated surface={} content_dip=({:.2f},{:.2f}) viewport_dip=({:.2f},{:.2f}) "
              L"cooldown_allowed={} {}",
              string_to_wstring(surface), widthDip, heightDip, viewportWidthDip, viewportHeightDip, cooldownAllowed,
              hwnd == ::global_hwnd ? DescribeCandidateHostState() : L"");
    if (!cooldownAllowed)
    {
        return false;
    }
    if (widthDip < 1.0 || heightDip < 1.0)
    {
        return false;
    }
    return ApplyContentTruncationResize(hwnd, webview, controller, widthDip, heightDip, extraShadowDip);
}

bool candidateNavigationReady = false;
bool menuNavigationReady = false;
bool smallWindowTopmostRequested = false;
bool smallWindowTopmostApplied = false;
// Guards against scheduling the staggered pin more than once.
bool smallWindowTopmostScheduled = false;

// The three small-window hosts enter the topmost band one at a time, on their
// own timers, rather than together. Under uiAccess a TOPMOST transition is
// exactly what breaks a WebView2 that is still bringing up its first frames, so
// the first step waits well past navigation-ready; the gaps after it only need
// to keep two hosts from changing bands in the same frame. The tray menu goes
// last because later HWND_TOPMOST wins within the band, which is the order an
// open menu needs. Steps must stay listed in firing order: the timer id is the
// enum value, and the delays are indexed by it.
enum class SmallWindowTopmostStep
{
    Candidate,
    FloatingToolbar,
    TrayMenu,
};
constexpr UINT_PTR kTopmostStepTimerIdBase = 9100;
constexpr UINT kTopmostStepCount = 3;
constexpr UINT kCandidateTopmostDelayMs = 1000;
constexpr UINT kFloatingToolbarTopmostDelayMs = 1200;
constexpr UINT kTrayMenuTopmostDelayMs = 1400;
// Remembered so the pending steps can be cancelled. Leaving them armed across a
// controller rebuild would let a stale step pin a host TOPMOST while WebView2 is
// creating a controller for it, which fails with E_INVALIDARG under uiAccess.
HWND smallWindowTopmostTimerHost = nullptr;
// Counted down rather than finalizing in whichever case happens to be last, so
// reordering the steps cannot silently leave the gate open or strand a timer.
UINT smallWindowTopmostStepsPending = 0;

// Retained as a no-op sink so the call sites stay in place; the parameter is named only in the
// comment because nothing consumes it while webview logging is compiled out.
void WebviewDebugLog(const std::wstring & /*message*/)
{
}

void ScheduleSmallWindowWebviewRetry(DWORD delay_ms);
void BeginSmallWindowWebviewEnvironmentCreate();
void RequestNextSmallWindowController();
void OnSmallWindowWebviewInitFailed(HRESULT hr);
void MaybeFlushPendingTrayMenuShow();
void ResetSmallWindowTopmostGate();

void CALLBACK SmallWindowWebviewRetryTimerProc(HWND hwnd, UINT /*msg*/, UINT_PTR id, DWORD /*time*/)
{
    KillTimer(hwnd, id);
    // An existing environment must never be rebuilt: that would replace the
    // candidate / floating-toolbar controllers that are already working and make
    // the toolbar disappear. Only fill in the controllers that are missing.
    if (smallWindowWebviewEnvironment)
    {
        RequestNextSmallWindowController();
        return;
    }
    BeginSmallWindowWebviewEnvironmentCreate();
}

void ScheduleSmallWindowWebviewRetry(DWORD delay_ms)
{
    HWND timer_hwnd = smallWindowCandHwnd ? smallWindowCandHwnd : ::global_hwnd;
    if (!timer_hwnd)
    {
        return;
    }
    KillTimer(timer_hwnd, kRetrySmallWindowWebviewTimerId);
    SetTimer(timer_hwnd, kRetrySmallWindowWebviewTimerId, delay_ms, SmallWindowWebviewRetryTimerProc);
}

void ScheduleSmallWindowRetryWithBackoff()
{
    if (smallWindowInitAttempts >= kMaxSmallWindowInitAttempts)
    {
        return;
    }
    // 1s, 2s, 4s, 8s capped at 10s: covers user-data-folder locks left by
    // orphaned WebView2 processes and slow bring-up right after logon.
    const DWORD delay_ms =
        (std::min)(DWORD{1000} << (std::min)((std::max)(smallWindowInitAttempts - 1, 0), 3), DWORD{10000});
    ScheduleSmallWindowWebviewRetry(delay_ms);
}

// The HRESULT is part of the callback contract but the retry path is the same for every failure
// code, so it is only reported by the caller's own logging.
void OnSmallWindowWebviewInitFailed(HRESULT /*hr*/)
{
    smallWindowInitState = SmallWindowInitState::Failed;
    smallWindowWebviewEnvironment.Reset();
    ScheduleSmallWindowRetryWithBackoff();
}

void MaybeFlushPendingTrayMenuShow()
{
    if (!pendingTrayMenuShow || !::global_hwnd_menu)
    {
        return;
    }
    if (TrayMenuPresenter::Instance().IsBound())
    {
        pendingTrayMenuShow = false;
        PostMessage(::global_hwnd_menu, WM_LANGBAR_RIGHTCLICK, 0, 0);
        return;
    }
    if (!pendingTrayMenuShow || !webviewControllerMenuWnd || !menuNavigationReady || !::global_hwnd_menu)
    {
        return;
    }
    pendingTrayMenuShow = false;
    FTB_DIAG_LOGF(L"menu replaying show that was deferred until the webview was ready");
    // Global::Point / Keycode / ModifiersDown still hold the langbar rect from
    // the right-click that arrived while the menu WebView was not ready.
    PostMessage(::global_hwnd_menu, WM_LANGBAR_RIGHTCLICK, 0, 0);
}

int currentSmallWindowHostIndex = -1;
int lastFailedSmallWindowHostIndex = -1;

// Request a controller for exactly one host at a time, and only for hosts that
// do not have one yet, so neither concurrency nor a retry can disturb siblings
// that already came up. The scan starts after the host that failed last so a
// persistently failing host cannot starve its siblings.
void RequestNextSmallWindowController()
{
    ICoreWebView2Environment *env = smallWindowWebviewEnvironment.Get();
    if (!env)
    {
        return;
    }
    if (smallWindowControllerRequestInFlight)
    {
        // A completion handler that never runs would otherwise wedge the queue.
        if (GetTickCount64() - smallWindowControllerRequestStartTick < kSmallWindowControllerRequestTimeoutMs)
        {
            return;
        }
        smallWindowControllerRequestInFlight = false;
    }

    struct Host
    {
        const wchar_t *name;
        HWND hwnd;
        bool hasController;
        HRESULT (*request)(HWND, HRESULT, ICoreWebView2Environment *);
    };
    const Host hosts[] = {
        {L"cand", smallWindowCandHwnd, webviewControllerCandWnd != nullptr, &OnEnvironmentCreated},
        {L"menu", smallWindowMenuHwnd, webviewControllerMenuWnd != nullptr, &OnMenuWindowEnvironmentCreated},
        {L"ftb", smallWindowFtbHwnd, webviewControllerFtbWnd != nullptr, &OnFtbWindowEnvironmentCreated},
    };
    constexpr int kHostCount = 3;

    int chosen = -1;
    for (int step = 1; step <= kHostCount; ++step)
    {
        const int i = (lastFailedSmallWindowHostIndex + step) % kHostCount;
        if (!hosts[i].hasController && hosts[i].hwnd)
        {
            chosen = i;
            break;
        }
    }
    if (chosen < 0)
    {
        MaybeFlushPendingTrayMenuShow();
        MaybeFlushPendingCandidateShow();
        return;
    }
    const Host &host = hosts[chosen];

    // In a uiAccess=true process a TOPMOST parent makes WebView2's internal
    // cross-process SetParent fail with E_INVALIDARG (WebView2Feedback #486),
    // and the failure persists as long as WS_EX_TOPMOST stays on the window.
    // Demote before creating; the lazy-topmost gate re-pins once all three
    // WebViews are ready.
    if (GetWindowLongPtrW(host.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
    {
        SetWindowPos(host.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    smallWindowControllerRequestInFlight = true;
    smallWindowControllerRequestStartTick = GetTickCount64();
    currentSmallWindowHostIndex = chosen;
    const HRESULT hr = host.request(host.hwnd, S_OK, env);
    if (FAILED(hr))
    {
        smallWindowControllerRequestInFlight = false;
        lastFailedSmallWindowHostIndex = chosen;
        ++smallWindowInitAttempts;
        ScheduleSmallWindowRetryWithBackoff();
    }
}

// Called from every controller-created handler so the next host is only started
// once the previous creation has fully settled.
void OnSmallWindowControllerSettled(HRESULT hr)
{
    smallWindowControllerRequestInFlight = false;
    if (FAILED(hr))
    {
        lastFailedSmallWindowHostIndex = currentSmallWindowHostIndex;
        ++smallWindowInitAttempts;
        ScheduleSmallWindowRetryWithBackoff();
        return;
    }
    // Forward progress: give the remaining hosts a full attempt budget.
    smallWindowInitAttempts = 0;
    ScheduleSmallWindowWebviewRetry(1);
}

void BeginSmallWindowWebviewEnvironmentCreate()
{
    if (!smallWindowCandHwnd || !smallWindowMenuHwnd || !smallWindowFtbHwnd)
    {
        return;
    }
    if (smallWindowInitState == SmallWindowInitState::InProgress)
    {
        return;
    }
    if (smallWindowWebviewEnvironment)
    {
        RequestNextSmallWindowController();
        return;
    }

    smallWindowInitState = SmallWindowInitState::InProgress;
    ++smallWindowInitAttempts;

    ResetSmallWindowTopmostGate();
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments( //
        L"--disable-features=TranslateUI "
        L"--disable-background-networking "
        L"--disable-default-apps "
        L"--disable-sync "
        L"--disable-prompt-on-repost "
        L"--no-first-run");

    const std::wstring appDataPath = GetAppdataPath();
    if (appDataPath.empty() || appDataPath[0] == L'\\')
    {
        OnSmallWindowWebviewInitFailed(E_INVALIDARG);
        return;
    }

    const HRESULT createHr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, appDataPath.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT result,
                                                                                ICoreWebView2Environment *env)
                                                                                 -> HRESULT {
            if (FAILED(result) || !env)
            {
                OnSmallWindowWebviewInitFailed(FAILED(result) ? result : E_FAIL);
                return FAILED(result) ? result : E_FAIL;
            }

            smallWindowWebviewEnvironment = env;
            smallWindowInitState = SmallWindowInitState::Ready;
            smallWindowInitAttempts = 0;
            RequestNextSmallWindowController();
            return S_OK;
        }).Get());

    if (FAILED(createHr))
    {
        OnSmallWindowWebviewInitFailed(createHr);
    }
}

struct FloatingToolbarState
{
    // Keep these defaults aligned with the TSF compartment defaults:
    // Chinese input, half-width characters, and Chinese punctuation.
    int cn_en = 1;
    int double_single_byte = 0;
    int punctuation = 1;
    int english_input_mode = 0;
    int caps_lock = 0;
    int japanese_input_mode = 0;
};

FloatingToolbarState floatingToolbarState;

bool AreSmallWindowWebviewsReadyUnlocked()
{
    const bool candReady =
        CandidatePresenter::Instance().IsBound() ||
        (candidateNavigationReady && webviewCandWnd != nullptr && webviewControllerCandWnd != nullptr);
    const bool menuReady = TrayMenuPresenter::Instance().IsBound() ||
                           (menuNavigationReady && webviewMenuWnd != nullptr && webviewControllerMenuWnd != nullptr);
    const bool ftbReady =
        FloatingToolbarPresenter::Instance().IsBound() ||
        (floatingToolbarNavigationReady && webviewFtbWnd != nullptr && webviewControllerFtbWnd != nullptr);
    return candReady && menuReady && ftbReady;
}

void CancelStaggeredTopmost()
{
    smallWindowTopmostStepsPending = 0;
    if (!smallWindowTopmostTimerHost)
    {
        return;
    }
    for (UINT_PTR step = 0; step < kTopmostStepCount; ++step)
    {
        KillTimer(smallWindowTopmostTimerHost, kTopmostStepTimerIdBase + step);
    }
    smallWindowTopmostTimerHost = nullptr;
}

void ResetSmallWindowTopmostGate()
{
    CancelStaggeredTopmost();
    candidateNavigationReady = false;
    menuNavigationReady = false;
    ClearFloatingToolbarNavigationState();
    smallWindowTopmostRequested = false;
    smallWindowTopmostApplied = false;
    smallWindowTopmostScheduled = false;
    (void)0;
}

// Same as WebviewDebugLog: a no-op sink kept for its call sites while the gate logging is off.
void LogSmallWindowReadyGateUnlocked(const wchar_t * /*context*/)
{
}

void PinHostTopmost(HWND hwnd)
{
    if (!hwnd)
    {
        return;
    }
    constexpr UINT flag = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flag);
}

// HWND_TOPMOST moves the host between z-bands. In a uiAccess process a WebView2
// that is not told about it keeps compositing against the old parent state: the
// host stays visible with correct bounds while nothing is ever painted into it.
void RenotifyControllerAfterPin(ICoreWebView2Controller *controller, HWND hwnd)
{
    if (!controller || !hwnd || !IsWindowVisible(hwnd))
    {
        return;
    }
    UpdateSmallWindowWebviewVisibility(hwnd, true);
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    controller->put_Bounds(bounds);
    controller->NotifyParentWindowPositionChanged();
}

// The menu host is shown DWM-cloaked for WebView2 warmup, so IsWindowVisible()
// reports true from startup onwards even though the user sees nothing and the
// first frame may not exist yet. Callers that mean "the menu is open in front of
// the user" must exclude that state: treating warmup as open is what pins the
// host into the topmost band mid-initialisation and leaves it permanently blank.
bool TrayMenuIsOpenToUser()
{
    if (TrayMenuPresenter::Instance().IsBound())
    {
        return TrayMenuPresenter::Instance().IsOpenToUser();
    }
    if (!::global_hwnd_menu || !webviewControllerMenuWnd || !menuNavigationReady)
    {
        return false;
    }
    if (!IsWindowVisible(::global_hwnd_menu))
    {
        return false;
    }
    DWORD cloaked = 0;
    DwmGetWindowAttribute(::global_hwnd_menu, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return cloaked == 0;
}

void ApplySmallWindowTopmostStep(SmallWindowTopmostStep step)
{
    FTB_DIAG_LOGF(L"topmost step {} applying", step == SmallWindowTopmostStep::Candidate         ? L"candidate"
                                               : step == SmallWindowTopmostStep::FloatingToolbar ? L"floating-toolbar"
                                                                                                 : L"tray-menu");
    switch (step)
    {
    case SmallWindowTopmostStep::Candidate:
        PinHostTopmost(::global_hwnd);
        if (CandidatePresenter::Instance().IsBound())
        {
            if (::is_global_wnd_cand_shown && ::global_hwnd)
            {
                CandidatePresenter::Instance().Present();
            }
        }
        else
        {
            RenotifyControllerAfterPin(webviewControllerCandWnd.Get(), ::global_hwnd);
            if (::is_global_wnd_cand_shown && ::global_hwnd)
            {
                FineTuneWindow(::global_hwnd);
            }
        }
        break;

    case SmallWindowTopmostStep::FloatingToolbar:
        PinHostTopmost(::global_hwnd_ftb);
        if (FloatingToolbarPresenter::Instance().IsBound())
        {
            FloatingToolbarPresenter::Instance().Present();
        }
        else
        {
            RenotifyControllerAfterPin(webviewControllerFtbWnd.Get(), ::global_hwnd_ftb);
        }
        // The menu step lands a moment later and would fix the order anyway;
        // raising now keeps an already-open menu from being covered in between.
        if (TrayMenuIsOpenToUser())
        {
            RaiseTrayMenuAboveSmallWindows(L"after-staggered-topmost");
        }
        break;

    case SmallWindowTopmostStep::TrayMenu:
        PinHostTopmost(::global_hwnd_menu);
        if (TrayMenuPresenter::Instance().IsBound())
        {
            if (TrayMenuPresenter::Instance().IsOpenToUser())
            {
                TrayMenuPresenter::Instance().Present();
            }
        }
        else
        {
            RenotifyControllerAfterPin(webviewControllerMenuWnd.Get(), ::global_hwnd_menu);
        }
        break;
    }

    if (smallWindowTopmostStepsPending > 0 && --smallWindowTopmostStepsPending == 0)
    {
        // Only now is the whole band in effect, and no timer is left to cancel.
        smallWindowTopmostApplied = true;
        smallWindowTopmostTimerHost = nullptr;
        LogSmallWindowReadyGateUnlocked(L"after-topmost-applied");
    }
}

void CALLBACK SmallWindowTopmostTimerProc(HWND hwnd, UINT, UINT_PTR timerId, DWORD)
{
    KillTimer(hwnd, timerId);
    ApplySmallWindowTopmostStep(static_cast<SmallWindowTopmostStep>(timerId - kTopmostStepTimerIdBase));
}

// Returns false when there is no host window to hang the timers on, leaving the
// caller to pin inline rather than never.
bool ScheduleStaggeredTopmost()
{
    const HWND timer_host = ::global_hwnd_ftb ? ::global_hwnd_ftb : ::global_hwnd;
    if (!timer_host)
    {
        return false;
    }
    const UINT delays[kTopmostStepCount] = {kCandidateTopmostDelayMs, kFloatingToolbarTopmostDelayMs,
                                            kTrayMenuTopmostDelayMs};
    UINT scheduled = 0;
    for (UINT_PTR step = 0; step < kTopmostStepCount; ++step)
    {
        if (SetTimer(timer_host, kTopmostStepTimerIdBase + step, delays[step], SmallWindowTopmostTimerProc) != 0)
        {
            ++scheduled;
        }
    }
    // Counting what was armed rather than kTopmostStepCount keeps the countdown
    // able to reach zero if SetTimer fails for one of the steps.
    smallWindowTopmostStepsPending = scheduled;
    smallWindowTopmostTimerHost = scheduled > 0 ? timer_host : nullptr;
    return scheduled > 0;
}

// Pinning z-order is what breaks WebView2 rendering under uiAccess, and the
// gate normally opens inside the toolbar's own navigation-completed handler --
// before it has painted a single frame. Spread the three transitions out in
// time so each WebView2 is well settled before its host moves, and so that no
// two hosts change bands close enough together to interact.
void TryApplyPendingLazyTopmost(const wchar_t *reason)
{
    if (!smallWindowTopmostRequested || smallWindowTopmostApplied)
    {
        return;
    }
    if (!AreSmallWindowWebviewsReadyUnlocked())
    {
        LogSmallWindowReadyGateUnlocked(L"topmost-still-waiting-webviews");
        return;
    }
    // Already scheduled. Falling through here would pin z-order from whatever
    // is running right now, which is the navigation-completed handler this
    // deferral exists to stay out of: the toolbar reports ready last, so its
    // own apply asks for topmost again a few lines later.
    if (smallWindowTopmostScheduled)
    {
        return;
    }
    smallWindowTopmostScheduled = true;
    if (ScheduleStaggeredTopmost())
    {
        FTB_DIAG_LOGF(L"topmost staggered from reason={}: candidate +{}ms, floating-toolbar +{}ms, "
                      L"tray-menu +{}ms",
                      reason, kCandidateTopmostDelayMs, kFloatingToolbarTopmostDelayMs, kTrayMenuTopmostDelayMs);
        return;
    }
    FTB_DIAG_LOGF(L"topmost timers unavailable for reason={}, pinning inline", reason);
    smallWindowTopmostStepsPending = kTopmostStepCount;
    ApplySmallWindowTopmostStep(SmallWindowTopmostStep::Candidate);
    ApplySmallWindowTopmostStep(SmallWindowTopmostStep::FloatingToolbar);
    ApplySmallWindowTopmostStep(SmallWindowTopmostStep::TrayMenu);
}

// `which` names the window for diagnostics only; the gate logic is identical for every caller.
void NotifySmallWindowNavigationReady(bool &readyFlag, const wchar_t * /*which*/)
{
    if (readyFlag)
    {
        return;
    }
    readyFlag = true;
    LogSmallWindowReadyGateUnlocked(L"after-nav-ready");
    TryApplyPendingLazyTopmost(L"pending-after-nav-ready");
    MaybeFlushPendingTrayMenuShow();
    MaybeFlushPendingCandidateShow();
}

bool UpdateBinaryState(int value, int &state)
{
    // The existing UI contract only defines 0 and 1. Ignore malformed values
    // instead of replacing a known-good cached state.
    if ((value == 0 || value == 1) && value != state)
    {
        state = value;
        return true;
    }
    return false;
}

void RenderFloatingToolbarState(ICoreWebView2 *webview)
{
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().SyncUi(
            floatingToolbarState.cn_en, floatingToolbarState.double_single_byte, floatingToolbarState.punctuation,
            floatingToolbarState.english_input_mode, floatingToolbarState.caps_lock,
            floatingToolbarState.japanese_input_mode);
    }

    if (!floatingToolbarNavigationReady || webview == nullptr)
    {
        return;
    }

    std::wstring script;
    script.reserve(1600);
    const bool japanese_mode = floatingToolbarState.japanese_input_mode == 1;
    constexpr wchar_t kHideJa[] = L"{const ja=document.getElementById('ja');if(ja)ja.style.display='none';}";
    constexpr wchar_t kShowJa[] = L"{const ja=document.getElementById('ja');if(ja)ja.style.display='flex';}";
    constexpr wchar_t kHideCap[] = L"{const cap=document.getElementById('cap');if(cap)cap.style.display='none';}";
    constexpr wchar_t kShowCap[] = L"{const cap=document.getElementById('cap');if(cap)cap.style.display='flex';}";
    const wchar_t *lang_token = L"cn";
    if (floatingToolbarState.cn_en != 1)
    {
        lang_token = L"en";
    }
    else if (floatingToolbarState.english_input_mode == 1)
    {
        lang_token = L"en-candidate";
    }
    else if (japanese_mode)
    {
        lang_token = L"ja";
    }
    script.append(L"{const host=document.getElementById('cn-en');if(host)host.dataset.lang='");
    script.append(lang_token);
    script.append(L"';}");

    if (floatingToolbarState.caps_lock == 1)
    {
        script.append(L"document.getElementById('cn').style.display = 'none';"
                      L"document.getElementById('en-candidate').style.display = 'none';"
                      L"document.getElementById('en').style.display = 'none';");
        script.append(kHideJa);
        script.append(kShowCap);
    }
    else if (floatingToolbarState.cn_en == 1)
    {
        script.append(kHideCap);
        if (floatingToolbarState.english_input_mode == 1)
        {
            script.append(L"document.getElementById('cn').style.display = 'none';"
                          L"document.getElementById('en-candidate').style.display = 'flex';"
                          L"document.getElementById('en').style.display = 'none';");
            script.append(kHideJa);
        }
        else if (japanese_mode)
        {
            script.append(L"document.getElementById('cn').style.display = 'none';"
                          L"document.getElementById('en-candidate').style.display = 'none';"
                          L"document.getElementById('en').style.display = 'none';");
            script.append(kShowJa);
        }
        else
        {
            script.append(L"document.getElementById('cn').style.display = 'flex';"
                          L"document.getElementById('en-candidate').style.display = 'none';"
                          L"document.getElementById('en').style.display = 'none';");
            script.append(kHideJa);
        }
    }
    else
    {
        script.append(kHideCap);
        script.append(L"document.getElementById('cn').style.display = 'none';");
        script.append(L"document.getElementById('en-candidate').style.display = 'none';");
        script.append(L"document.getElementById('en').style.display = 'flex';");
        script.append(kHideJa);
    }

    if (floatingToolbarState.double_single_byte == 1)
    {
        script.append(L"document.getElementById('fullwidth').style.display = 'flex';");
        script.append(L"document.getElementById('halfwidth').style.display = 'none';");
    }
    else
    {
        script.append(L"document.getElementById('fullwidth').style.display = 'none';");
        script.append(L"document.getElementById('halfwidth').style.display = 'flex';");
    }

    if (floatingToolbarState.punctuation == 1)
    {
        script.append(L"document.getElementById('puncCn').style.display = 'flex';");
        script.append(L"document.getElementById('puncEn').style.display = 'none';");
    }
    else
    {
        script.append(L"document.getElementById('puncCn').style.display = 'none';");
        script.append(L"document.getElementById('puncEn').style.display = 'flex';");
    }

    if (GetConfiguredCharacterSet() == "traditional")
    {
        script.append(L"document.getElementById('character-set-simplified').style.display = 'none';");
        script.append(L"document.getElementById('character-set-traditional').style.display = 'flex';");
    }
    else
    {
        script.append(L"document.getElementById('character-set-simplified').style.display = 'flex';");
        script.append(L"document.getElementById('character-set-traditional').style.display = 'none';");
    }

    const FloatingToolbarItemsConfig &items = GetConfiguredFloatingToolbarItems();
    script.append(L"window.setToolbarItem=(id,shown)=>{const item=document.getElementById(id);"
                  L"if(item)item.style.display=shown?'flex':'none';};");
    script.append(items.fullwidth ? L"window.setToolbarItem('char-width-mode',true);"
                                  : L"window.setToolbarItem('char-width-mode',false);");
    script.append(items.punctuation ? L"window.setToolbarItem('punctuation-mode',true);"
                                    : L"window.setToolbarItem('punctuation-mode',false);");
    script.append(items.character_set ? L"window.setToolbarItem('character-set',true);"
                                      : L"window.setToolbarItem('character-set',false);");
    script.append(items.emoji ? L"window.setToolbarItem('emoji-panel',true);"
                              : L"window.setToolbarItem('emoji-panel',false);");
    script.append(items.screen_keyboard ? L"window.setToolbarItem('screen-keyboard',true);"
                                        : L"window.setToolbarItem('screen-keyboard',false);");
    script.append(items.settings ? L"window.setToolbarItem('settings',true);"
                                 : L"window.setToolbarItem('settings',false);");
    script.append(L"if(window.CheckContentTruncation)window.CheckContentTruncation();");

    webview->ExecuteScript(script.c_str(), nullptr);
}

void SetWebviewMemoryUsageTarget(ComPtr<ICoreWebView2> webview, COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL level)
{
    if (!webview)
        return;

    ComPtr<ICoreWebView2_23> webview23;
    if (SUCCEEDED(webview.As(&webview23)))
    {
        webview23->put_MemoryUsageTargetLevel(level);
    }
}
} // namespace

void InjectSurfaceViewportLimits(ICoreWebView2 *webview, HWND hwnd)
{
    InjectSurfaceViewportLimitsImpl(webview, hwnd);
}

bool EnsureSmallWindowsTopmost(const wchar_t *reason)
{
    smallWindowTopmostRequested = true;
    if (smallWindowTopmostApplied)
    {
        return true;
    }
    if (!AreSmallWindowWebviewsReadyUnlocked())
    {
        (void)0;
        LogSmallWindowReadyGateUnlocked(L"topmost-deferred");
        return false;
    }

    // Shares the staggered path so the pin never lands on the stack of a
    // navigation-completed handler and every host gets renotified afterwards.
    // This used to pin all three inline and skip the renotification entirely.
    TryApplyPendingLazyTopmost(reason);
    return smallWindowTopmostApplied;
}

void RaiseTrayMenuAboveSmallWindows(const wchar_t *reason)
{
    if (!::global_hwnd_menu)
    {
        return;
    }
    if (TrayMenuPresenter::Instance().IsBound())
    {
        constexpr UINT flag = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
        SetWindowPos(::global_hwnd_menu, HWND_TOPMOST, 0, 0, 0, 0, flag);
        if (TrayMenuPresenter::Instance().IsOpenToUser())
        {
            TrayMenuPresenter::Instance().Present();
        }
        return;
    }
    // Backstop for the callers above: making the host TOPMOST before a
    // controller exists is fatal in a uiAccess=true process, because UIPI then
    // blocks WebView2's cross-process SetParent and every
    // CreateCoreWebView2Controller for this window fails with E_INVALIDARG
    // (WebView2Feedback #486). Nothing needs raising before content exists.
    if (!webviewControllerMenuWnd)
    {
        FTB_DIAG_LOGF(L"menu raise reason={} skipped: no controller yet", reason);
        return;
    }
    // Re-assert TOPMOST after FTB (or a peer) was pinned last. A second
    // HWND_TOPMOST is what actually lifts the menu within the topmost band;
    // HWND_TOP alone is unreliable here with WS_EX_NOACTIVATE layered hosts.
    FTB_DIAG_LOGF(L"menu raise reason={} nav_ready={} {}", reason, menuNavigationReady, DescribeTrayMenuHostState());
    constexpr UINT flag = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
    SetLastError(0);
    if (!SetWindowPos(::global_hwnd_menu, HWND_TOPMOST, 0, 0, 0, 0, flag))
    {
        FTB_DIAG_LOGF(L"menu raise reason={} SetWindowPos failed err={}", reason, GetLastError());
        return;
    }
    // This is the same band transition PinHostTopmost performs and so needs the
    // same repair. Skipping it is what leaves the menu interactive but blank:
    // the controller keeps compositing against the parent state it saw before
    // the move, while the host reports correct bounds throughout.
    RenotifyControllerAfterPin(webviewControllerMenuWnd.Get(), ::global_hwnd_menu);
}

void DeferCandidateShowUntilWebviewReady()
{
    pendingCandidateShow = true;
    CAND_DIAG_LOGF(L"candidate show deferred until webview ready {}", DescribeCandidateHostState());
}

void MaybeFlushPendingCandidateShow()
{
    if (!pendingCandidateShow || !::global_hwnd || !IsWindow(::global_hwnd))
    {
        return;
    }
    if (!::is_global_wnd_cand_shown)
    {
        pendingCandidateShow = false;
        return;
    }
    if (!IsCandidateWebviewReady())
    {
        return;
    }
    pendingCandidateShow = false;
    g_candidate_show_msg_pending.store(false);
    CAND_DIAG_LOGF(L"candidate replaying show that was deferred until the webview was ready");
    PostMessage(::global_hwnd, WM_SHOW_MAIN_WINDOW, 0, 0);
}

void RaiseCandidateHostForShow(const wchar_t *reason)
{
    if (!::global_hwnd)
    {
        return;
    }
    if (CandidatePresenter::Instance().IsBound())
    {
        constexpr UINT flag = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
        SetWindowPos(::global_hwnd, HWND_TOPMOST, 0, 0, 0, 0, flag);
        return;
    }
    if (!webviewControllerCandWnd)
    {
        CAND_DIAG_LOGF(L"candidate raise reason={} skipped: no controller yet", reason);
        return;
    }
    CAND_DIAG_LOGF(L"candidate raise reason={} nav_ready={} {}", reason, candidateNavigationReady,
                   DescribeCandidateHostState());
    PinHostTopmost(::global_hwnd);
    RenotifyControllerAfterPin(webviewControllerCandWnd.Get(), ::global_hwnd);
}

bool AreSmallWindowsTopmostApplied()
{
    return smallWindowTopmostApplied;
}

bool AreSmallWindowWebviewsReady()
{
    return AreSmallWindowWebviewsReadyUnlocked();
}

bool IsCandidateWebviewReady()
{
    return CandidatePresenter::Instance().IsBound() ||
           (candidateNavigationReady && webviewCandWnd != nullptr && webviewControllerCandWnd != nullptr);
}

bool IsFloatingToolbarWebviewReady()
{
    return FloatingToolbarPresenter::Instance().IsBound() ||
           (floatingToolbarNavigationReady && webviewControllerFtbWnd != nullptr);
}

bool IsFloatingToolbarPaintGraceActive()
{
    return floatingToolbarPaintGraceActive;
}

void BeginFloatingToolbarPaintGrace()
{
    floatingToolbarPaintGraceActive = true;
}

void EndFloatingToolbarPaintGrace()
{
    floatingToolbarPaintGraceActive = false;
}

void NotifyFloatingToolbarPageReady()
{
    ReconcileFloatingToolbarVisibilityAfterReady(L"ftb-page-ready");
}

void ClearFloatingToolbarNavigationState()
{
    floatingToolbarNavigationReady = false;
    floatingToolbarPaintGraceActive = false;
    floatingToolbarNavigationRetryUsed = false;
}

bool IsTrayMenuOpenToUser()
{
    return TrayMenuIsOpenToUser();
}

bool GetFloatingToolbarWebviewState(bool &isVisible, RECT &bounds)
{
    isVisible = false;
    bounds = RECT{};
    if (!webviewControllerFtbWnd)
    {
        return false;
    }
    BOOL visible = FALSE;
    webviewControllerFtbWnd->get_IsVisible(&visible);
    isVisible = visible != FALSE;
    webviewControllerFtbWnd->get_Bounds(&bounds);
    return true;
}

bool GetTrayMenuWebviewState(bool &isVisible, RECT &bounds)
{
    isVisible = false;
    bounds = RECT{};
    if (!webviewControllerMenuWnd)
    {
        return false;
    }
    BOOL visible = FALSE;
    webviewControllerMenuWnd->get_IsVisible(&visible);
    isVisible = visible != FALSE;
    webviewControllerMenuWnd->get_Bounds(&bounds);
    return true;
}

void LogSmallWindowReadyGate(const wchar_t *context)
{
    LogSmallWindowReadyGateUnlocked(context);
}

void UpdateSmallWindowWebviewVisibility(HWND hwnd, bool visible)
{
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    bool lowerMemoryWhenHidden = false;

    if (hwnd == ::global_hwnd)
    {
        controller = webviewControllerCandWnd;
        webview = webviewCandWnd;
    }
    else if (hwnd == ::global_hwnd_menu)
    {
        controller = webviewControllerMenuWnd;
        webview = webviewMenuWnd;
        // The language-bar menu is reopened interactively and must paint on
        // the first frame. Returning its renderer from LOW is asynchronous
        // and can leave the host window visible with transparent content.
        lowerMemoryWhenHidden = false;
    }
    else if (hwnd == ::global_hwnd_ftb)
    {
        controller = webviewControllerFtbWnd;
        webview = webviewFtbWnd;
        // Keep the small, persistent toolbar warm when configuration or
        // fullscreen policy temporarily hides it. Switching its WebView back
        // from LOW can otherwise produce a visible white repaint.
        lowerMemoryWhenHidden = false;
    }
    else
    {
        (void)0;
        return;
    }

    if (controller)
    {
        controller->put_IsVisible(visible ? TRUE : FALSE);
    }
    else
    {
        (void)0;
    }

    if (lowerMemoryWhenHidden)
    {
        SetWebviewMemoryUsageTarget(webview, visible ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL
                                                     : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
    }
}

bool GetCandidateWebviewState(bool &isVisible, RECT &bounds)
{
    if (!webviewControllerCandWnd)
        return false;
    BOOL visible = FALSE;
    if (FAILED(webviewControllerCandWnd->get_IsVisible(&visible)) ||
        FAILED(webviewControllerCandWnd->get_Bounds(&bounds)))
        return false;
    isVisible = visible != FALSE;
    return true;
}

std::wstring ReadHtmlFile(const std::wstring &filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file)
        return L"";
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return string_to_wstring(buffer.str());
}

std::wstring GetAppdataPath()
{
    return CommonUtils::get_webview2_user_data_path(L"webview2");
}

// An unreadable themed asset must never turn into an empty NavigateToString:
// that renders a blank, title-less page which looks like a dead WebView.
std::wstring ReadHtmlFileWithFallback(const std::wstring &primaryPath, const std::wstring &fallbackPath)
{
    std::wstring content = ReadHtmlFile(primaryPath);
    if (content.empty() && primaryPath != fallbackPath)
    {
        content = ReadHtmlFile(fallbackPath);
    }
    return content;
}

namespace
{
std::wstring NormalizeSkinCssUrl(std::wstring url)
{
    while (!url.empty() && iswspace(url.front()))
    {
        url.erase(url.begin());
    }
    while (!url.empty() && iswspace(url.back()))
    {
        url.pop_back();
    }
    while (url.size() >= 2 && url[0] == L'.' && url[1] == L'/')
    {
        url.erase(0, 2);
    }
    std::replace(url.begin(), url.end(), L'\\', L'/');
    return url;
}

std::wstring EncodeBase64(const std::vector<unsigned char> &bytes)
{
    static constexpr wchar_t kTable[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < bytes.size())
    {
        const unsigned int triple = (static_cast<unsigned int>(bytes[i]) << 16) |
                                    (static_cast<unsigned int>(bytes[i + 1]) << 8) |
                                    static_cast<unsigned int>(bytes[i + 2]);
        out.push_back(kTable[(triple >> 18) & 63]);
        out.push_back(kTable[(triple >> 12) & 63]);
        out.push_back(kTable[(triple >> 6) & 63]);
        out.push_back(kTable[triple & 63]);
        i += 3;
    }
    if (i < bytes.size())
    {
        unsigned int triple = static_cast<unsigned int>(bytes[i]) << 16;
        if (i + 1 < bytes.size())
        {
            triple |= static_cast<unsigned int>(bytes[i + 1]) << 8;
        }
        out.push_back(kTable[(triple >> 18) & 63]);
        out.push_back(kTable[(triple >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kTable[(triple >> 6) & 63] : L'=');
        out.push_back(L'=');
    }
    return out;
}

std::wstring MimeForSkinAsset(const std::wstring &relativePath)
{
    std::wstring lower = relativePath;
    for (wchar_t &ch : lower)
    {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".png") == 0)
        return L"image/png";
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".jpg") == 0)
        return L"image/jpeg";
    if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, L".jpeg") == 0)
        return L"image/jpeg";
    if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, L".webp") == 0)
        return L"image/webp";
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".gif") == 0)
        return L"image/gif";
    if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, L".svg") == 0)
        return L"image/svg+xml";
    return L"application/octet-stream";
}

std::wstring EmbedSkinCssUrl(const std::wstring &skinsRoot, const std::string &skinId, const std::wstring &rawUrl)
{
    const std::wstring relative = NormalizeSkinCssUrl(rawUrl);
    const std::filesystem::path filePath = std::filesystem::path(skinsRoot) / std::filesystem::u8path(skinId) /
                                           std::filesystem::u8path(wstring_to_string(relative));
    std::error_code ec;
    constexpr std::uintmax_t kMaxEmbedBytes = 1500 * 1024;
    const auto fileSize = std::filesystem::file_size(filePath, ec);
    if (!ec && fileSize > 0 && fileSize <= kMaxEmbedBytes)
    {
        std::ifstream stream(filePath, std::ios::binary);
        std::vector<unsigned char> bytes(static_cast<size_t>(fileSize));
        if (stream && stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(fileSize)))
        {
            return L"url(\"data:" + MimeForSkinAsset(relative) + L";base64," + EncodeBase64(bytes) + L"\")";
        }
    }
    return L"url(\"https://candidate-skins/" + string_to_wstring(skinId) + L"/" + relative + L"\")";
}

std::wstring RewriteCandidateSkinCssUrls(const std::wstring &css, const std::wstring &skinsRoot,
                                         const std::string &skinId)
{
    std::wstring result;
    result.reserve(css.size() + 64);
    size_t pos = 0;
    while (pos < css.size())
    {
        size_t urlPos = std::wstring::npos;
        for (size_t i = pos; i + 4 <= css.size(); ++i)
        {
            if ((css[i] == L'u' || css[i] == L'U') && (css[i + 1] == L'r' || css[i + 1] == L'R') &&
                (css[i + 2] == L'l' || css[i + 2] == L'L') && css[i + 3] == L'(')
            {
                urlPos = i;
                break;
            }
        }
        if (urlPos == std::wstring::npos)
        {
            result.append(css, pos, std::wstring::npos);
            break;
        }
        result.append(css, pos, urlPos - pos);
        size_t cursor = urlPos + 4;
        while (cursor < css.size() && iswspace(css[cursor]))
        {
            ++cursor;
        }
        wchar_t quote = 0;
        if (cursor < css.size() && (css[cursor] == L'"' || css[cursor] == L'\''))
        {
            quote = css[cursor++];
        }
        const size_t valueStart = cursor;
        while (cursor < css.size())
        {
            if (quote != 0)
            {
                if (css[cursor] == quote)
                {
                    break;
                }
            }
            else if (css[cursor] == L')' || iswspace(css[cursor]))
            {
                break;
            }
            ++cursor;
        }
        const std::wstring rawUrl = css.substr(valueStart, cursor - valueStart);
        switch (msime::skin_css::ClassifyUrl(rawUrl))
        {
        case msime::skin_css::UrlAction::Embed:
            result.append(EmbedSkinCssUrl(skinsRoot, skinId, rawUrl));
            break;
        case msime::skin_css::UrlAction::Keep:
            result.append(L"url(");
            if (quote != 0)
            {
                result.push_back(quote);
                result.append(rawUrl);
                result.push_back(quote);
            }
            else
            {
                result.append(rawUrl);
            }
            result.push_back(L')');
            break;
        case msime::skin_css::UrlAction::Drop:
            // Nothing is written. The declaration is left incomplete, so CSS discards it -- which
            // is what should happen to `background-image: url(https://attacker/beacon.png)` in a
            // skin someone downloaded. Passing it through, as this branch used to, meant the
            // candidate window fetched that URL on every render.
            break;
        }
        if (quote != 0 && cursor < css.size() && css[cursor] == quote)
        {
            ++cursor;
        }
        while (cursor < css.size() && iswspace(css[cursor]))
        {
            ++cursor;
        }
        if (cursor < css.size() && css[cursor] == L')')
        {
            ++cursor;
        }
        pos = cursor;
    }
    return result;
}

void NeutralizeEmbeddedStyleClosers(std::wstring &css)
{
    for (size_t i = 0; i + 7 < css.size(); ++i)
    {
        if (css[i] != L'<' || css[i + 1] != L'/')
        {
            continue;
        }
        std::wstring tag = css.substr(i + 2, 5);
        for (wchar_t &ch : tag)
        {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        if (tag == L"style")
        {
            css.insert(i + 1, 1, L' ');
            i += 2;
        }
    }
}

bool InjectExternalSkinCssFile(std::wstring &html, const CandidateSkinCatalog::Package &skin,
                               const std::wstring &skinsRoot, const std::string &stylesheet, const wchar_t *styleId)
{
    if (html.empty() || skinsRoot.empty() || stylesheet.empty() || !styleId)
    {
        return false;
    }
    // NavigateToString documents cannot reliably load a cross-origin <link>
    // stylesheet (virtual-host CORS). Built-in skins are inlined for the same
    // reason; keep external skins on that path so padding/decoration CSS is
    // present before SetWindowRgn applies candidateWindow.decoration.
    const std::wstring cssPath = skinsRoot + L"\\" + string_to_wstring(skin.id) + L"\\" + string_to_wstring(stylesheet);
    std::wstring css =
        RewriteCandidateSkinCssUrls(msime::skin_css::StripRemoteImports(ReadHtmlFile(cssPath)), skinsRoot, skin.id);
    if (css.empty())
    {
        return false;
    }
    NeutralizeEmbeddedStyleClosers(css);
    const size_t headEnd = html.find(L"</head>");
    if (headEnd == std::wstring::npos)
    {
        return false;
    }
    html.insert(headEnd, std::wstring(L"<style id=\"") + styleId + L"\">" + css + L"</style>");
    return true;
}

void AppendExternalCandidateColorCss(std::wstring &css, const CandidateSkinCatalog::CandidateColors &colors)
{
    auto add = [&](const std::string &value, const wchar_t *selector, const wchar_t *property) {
        if (value.empty())
        {
            return;
        }
        css.append(selector);
        css.append(L" { ");
        css.append(property);
        css.append(L": ");
        css.append(string_to_wstring(value));
        css.append(L"; }\n");
    };
    add(colors.accent, L".cursor, .first::before", L"background");
    add(colors.selected, L".first, .cand.first", L"background-color");
    add(colors.hover, L".hover-active .cand:not(.first):hover", L"background-color");
    add(colors.surface, L".container", L"background");
    add(colors.border, L".container", L"border-color");
    add(colors.text, L".container", L"color");
    add(colors.number, L".num, .cand-no", L"color");
    if (colors.showSelectedBar.has_value() && !*colors.showSelectedBar)
    {
        css.append(L".first::before { display: none; }\n");
    }
}

std::wstring BuildExternalCandidateSkinCss(const CandidateSkinCatalog::Package &skin, const std::wstring &skinsRoot)
{
    std::wstring css;
    if (skin.decorationTopDip > 0.0)
    {
        // skin.preview comes from the package's own skin.toml, so it gets the same treatment as a
        // URL written inside the stylesheet rather than being trusted because it arrived through a
        // manifest field.
        std::wstring preview = L"none";
        if (!skin.preview.empty() &&
            msime::skin_css::ClassifyUrl(string_to_wstring(skin.preview)) == msime::skin_css::UrlAction::Embed)
        {
            preview = EmbedSkinCssUrl(skinsRoot, skin.id, string_to_wstring(skin.preview));
        }
        css.append(L".containerParent { padding-top: var(--msime-skin-decoration-top, 0px); "
                   L"position: relative; box-sizing: border-box; }\n"
                   L".containerParent:not(:empty)::before { content: \"\"; position: absolute; "
                   L"z-index: 0; top: 0; right: 0; width: var(--msime-skin-decoration-width, 0px); "
                   L"height: var(--msime-skin-decoration-top, 0px); background: ");
        css.append(preview);
        css.append(L" center / contain no-repeat; pointer-events: none; }\n"
                   L".container { position: relative; z-index: 1; "
                   L"min-width: max(7em, var(--msime-skin-min-width, 0px)); }\n");
    }
    const bool light = ResolveConfiguredTheme(GetConfiguredThemeCand()) == "light";
    AppendExternalCandidateColorCss(css, light ? skin.light : skin.dark);
    return css;
}

bool InjectExternalCandidateSkin(std::wstring &html, const CandidateSkinCatalog::Package &skin,
                                 const std::wstring &skinsRoot)
{
    if (html.empty() || skinsRoot.empty())
    {
        return false;
    }
    const std::wstring vars =
        fmt::format(L"<style id=\"external-candidate-skin-vars\">:root{{--msime-skin-min-width:{}px;"
                    L"--msime-skin-decoration-top:{}px;--msime-skin-decoration-width:{}px;}}</style>",
                    skin.minWidthDip, skin.decorationTopDip, skin.decorationWidthDip);
    std::wstring css = BuildExternalCandidateSkinCss(skin, skinsRoot);
    NeutralizeEmbeddedStyleClosers(css);
    std::wstring generated;
    if (!css.empty())
    {
        generated = L"<style id=\"external-candidate-skin\">" + css + L"</style>";
    }
    const size_t headEnd = html.find(L"</head>");
    if (headEnd == std::wstring::npos)
    {
        return false;
    }
    html.insert(headEnd, vars + generated);
    return true;
}

void InjectCandidateDocumentSkin(std::wstring &html, const std::wstring &builtInCss, const std::string &skin,
                                 const std::string &base, const std::string &layout, const std::string &theme)
{
    if (html.empty())
        return;
    const size_t htmlTagEnd = html.find(L'>', html.find(L"<html"));
    if (htmlTagEnd != std::wstring::npos)
    {
        html.insert(htmlTagEnd, fmt::format(L" data-candidate-skin=\"{}\" data-candidate-base=\"{}\" "
                                            L"data-candidate-layout=\"{}\" data-candidate-theme=\"{}\"",
                                            string_to_wstring(skin), string_to_wstring(base), string_to_wstring(layout),
                                            string_to_wstring(theme)));
    }
    const size_t headEnd = html.find(L"</head>");
    if (headEnd != std::wstring::npos && !builtInCss.empty())
        html.insert(headEnd, L"<style id=\"built-in-candidate-skin\">" + builtInCss + L"</style>");
}
} // namespace

static std::wstring EscapeForJsTemplateLiteral(const std::wstring &text)
{
    // Content is injected as an untagged JavaScript template literal. Kaomoji
    // commonly contain `\`, backticks (e.g. `( -'`-)`), and `${`; any of those
    // will terminate or invalidate the literal and the DOM update is dropped.
    std::wstring escaped;
    escaped.reserve(text.size() + 16);
    for (size_t index = 0; index < text.size(); ++index)
    {
        const wchar_t ch = text[index];
        if (ch == L'\\')
        {
            escaped += L"\\\\";
        }
        else if (ch == L'`')
        {
            escaped += L"\\`";
        }
        else if (ch == L'$' && index + 1 < text.size() && text[index + 1] == L'{')
        {
            escaped += L"\\${";
            ++index;
        }
        else
        {
            escaped += ch;
        }
    }
    return escaped;
}

namespace
{
constexpr UINT_PTR kCandidateHoverArmTimerId = 21;
constexpr int kCandidateHoverArmDistancePx = 2;

POINT g_candidate_hover_cursor{};
bool g_candidate_hover_armed = false;
ULONGLONG g_candidate_hover_disarm_tick = 0;

// CSS :hover is applied by Chromium when the HWND sits under a still
// cursor. That does not go through mousemove JS. Kill those paints until
// the physical screen cursor actually moves after the card is up.
constexpr wchar_t kInstallCandidateHoverLockScript[] = LR"(
(function () {
  if (!document.getElementById('msime-hover-lock-style')) {
    const style = document.createElement('style');
    style.id = 'msime-hover-lock-style';
    style.textContent = `
html:not(.msime-hover-armed) #realContainer .cand:not(.first):hover,
html:not(.msime-hover-armed) #realContainer.hover-active .cand:not(.first):hover {
  background-color: transparent !important;
  background-image: none !important;
  box-shadow: none !important;
  outline: none !important;
}
`;
    document.documentElement.appendChild(style);
  }
  document.documentElement.classList.remove('msime-hover-armed');
  const container = document.getElementById('realContainer');
  if (container) {
    container.classList.remove('hover-active');
  }
  if (!window.__msimeBlockSynthHover) {
    window.__msimeBlockSynthHover = true;
    document.addEventListener('mousemove', function (event) {
      if (!document.documentElement.classList.contains('msime-hover-armed')) {
        if (event.movementX !== 0 || event.movementY !== 0) {
          document.documentElement.classList.add('msime-hover-armed');
          const liveContainer = document.getElementById('realContainer');
          if (liveContainer) {
            liveContainer.classList.add('hover-active');
          }
          window.chrome.webview.postMessage(JSON.stringify({type:'candidatePointerArmed'}));
          return;
        }
        window.chrome.webview.postMessage(JSON.stringify({type:'candidatePointerMotion'}));
        event.stopImmediatePropagation();
      }
    }, true);
  }
})();
)";

VOID CALLBACK CandidateHoverArmTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    MaybeArmCandidatePointerHover();
}
} // namespace

void DisarmCandidatePointerHover()
{
    g_candidate_hover_armed = false;
    g_candidate_hover_disarm_tick = GetTickCount64();
    if (!GetPhysicalCursorPos(&g_candidate_hover_cursor))
    {
        GetCursorPos(&g_candidate_hover_cursor);
    }
    CAND_WEBVIEW_TRACE_LOGF(L"candidate-hover disarm tick={} native_cursor=({},{}) shown={}",
                            g_candidate_hover_disarm_tick, g_candidate_hover_cursor.x, g_candidate_hover_cursor.y,
                            ::is_global_wnd_cand_shown);
    if (!::global_hwnd)
    {
        return;
    }
    if (::is_global_wnd_cand_shown)
    {
        SetTimer(::global_hwnd, kCandidateHoverArmTimerId, 32, CandidateHoverArmTimerProc);
    }
    else
    {
        KillTimer(::global_hwnd, kCandidateHoverArmTimerId);
    }
}

void MaybeArmCandidatePointerHover()
{
    if (g_candidate_hover_armed || !::is_global_wnd_cand_shown || !webviewCandWnd)
    {
        if (!::is_global_wnd_cand_shown && ::global_hwnd)
        {
            KillTimer(::global_hwnd, kCandidateHoverArmTimerId);
        }
        return;
    }
    POINT now{};
    if (!GetPhysicalCursorPos(&now))
    {
        GetCursorPos(&now);
    }
    const int dx = now.x - g_candidate_hover_cursor.x;
    const int dy = now.y - g_candidate_hover_cursor.y;
    if (dx * dx + dy * dy < kCandidateHoverArmDistancePx * kCandidateHoverArmDistancePx)
    {
        return;
    }
    g_candidate_hover_armed = true;
    CAND_WEBVIEW_TRACE_LOGF(L"candidate-hover arm tick={} baseline=({},{}) native_cursor=({},{}) delta=({},{})",
                            GetTickCount64(), g_candidate_hover_cursor.x, g_candidate_hover_cursor.y, now.x, now.y, dx,
                            dy);
    if (::global_hwnd)
    {
        KillTimer(::global_hwnd, kCandidateHoverArmTimerId);
    }
    webviewCandWnd->ExecuteScript(LR"(document.documentElement.classList.add('msime-hover-armed');
const c = document.getElementById('realContainer');
if (c) { c.classList.add('hover-active'); })",
                                  nullptr);
}

void ResetContainerHoverCandWnd(ComPtr<ICoreWebView2> webview)
{
    DisarmCandidatePointerHover();
    if (webview != nullptr)
    {
        webview->ExecuteScript(kInstallCandidateHoverLockScript, nullptr);
    }
}

constexpr wchar_t kEnsureApplyCandidateFrameScript[] = LR"(
window.ApplyCandidateFrame = function (payload) {
    const container = document.getElementById('realContainer');
    const parent = document.getElementById('realContainerParent');
    if (!container) return {width: 0, height: 0};
    if (payload.resetHover && window.ClearState) window.ClearState();
    if (parent && payload.applyMargins) {
      if (payload.marginTop != null) parent.style.marginTop = payload.marginTop + 'px';
      if (payload.marginLeft != null) parent.style.marginLeft = payload.marginLeft + 'px';
    }
    if (window.SetCandidatePreeditVisible) {
      window.SetCandidatePreeditVisible(payload.preeditVisible !== false);
    }
    const preedit = container.querySelector('.pinyin .text');
    if (preedit) {
      preedit.textContent = payload.preedit || '';
      if (window.SetPreeditCaret) window.SetPreeditCaret();
    }
    const items = Array.isArray(payload.items) ? payload.items : [];
    const wrappers = container.querySelectorAll('.row-wrapper');
    wrappers.forEach(function (wrapper, index) {
      const html = index < items.length ? String(items[index] || '') : '';
      wrapper.style.display = html ? '' : 'none';
      const cand = wrapper.querySelector('.cand');
      if (!cand) return;
      let slot = cand.querySelector('.cand-content');
      if (!slot) {
        const text = cand.querySelector('.text') || cand;
        slot = document.createElement('span');
        slot.className = 'cand-content';
        const num = text.querySelector('.num, .cand-no');
        if (num) {
          while (num.nextSibling) text.removeChild(num.nextSibling);
          text.appendChild(slot);
        } else {
          text.appendChild(slot);
        }
      }
      slot.innerHTML = html;
    });
    if (window.SetCandidateSelection) {
      window.SetCandidateSelection(payload.selected || 0);
    }
    void container.offsetWidth;
    const rect = container.getBoundingClientRect();
    // scrollWidth must be part of this: rect/offsetWidth report the clipped box
    // once .container{overflow-x:hidden} bites, so without it the native region
    // is built from a width narrower than what is actually painted.
    return {
      width: Math.max(rect.width, container.offsetWidth || 0, container.scrollWidth || 0) + 1,
      height: Math.max(rect.height, container.offsetHeight || 0) + 1,
      rw: rect.width, ow: container.offsetWidth || 0,
      sw: container.scrollWidth || 0, cw: container.clientWidth || 0
    };
};
// Do NOT force white-space:nowrap on .container/.row-wrapper here. The skin CSS
// wraps the inline-block candidates at --msime-max-width-dip; pinning them to a
// single line makes the card outgrow that cap, and .container's overflow-x:hidden
// then slices the last candidate mid-glyph instead of flowing it onto a new row.
)";

std::pair<double, double> g_last_candidate_slot_measured_size{};
bool g_candidate_slot_api_installed = false;

namespace
{
bool g_candidate_slot_update_inflight = false;
std::wstring g_candidate_slot_update_pending;
std::function<void()> g_candidate_slot_update_pending_complete;
bool g_candidate_slot_update_pending_content_only = false;

void SubmitCandidateSlotScript(ComPtr<ICoreWebView2> webview, const std::wstring &payload,
                               std::function<void()> onComplete, bool contentOnly);

void FlushPendingCandidateSlotUpdate(ComPtr<ICoreWebView2> webview)
{
    if (g_candidate_slot_update_pending.empty())
    {
        return;
    }
    std::wstring pending = std::move(g_candidate_slot_update_pending);
    std::function<void()> complete = std::move(g_candidate_slot_update_pending_complete);
    const bool pendingContentOnly = g_candidate_slot_update_pending_content_only;
    g_candidate_slot_update_pending.clear();
    g_candidate_slot_update_pending_complete = nullptr;
    g_candidate_slot_update_pending_content_only = false;
    SubmitCandidateSlotScript(webview, pending, std::move(complete), pendingContentOnly);
}

void SubmitCandidateSlotScript(ComPtr<ICoreWebView2> webview, const std::wstring &payload,
                               std::function<void()> onComplete, bool contentOnly)
{
    g_candidate_slot_update_inflight = true;
    if (!contentOnly)
    {
        DisarmCandidatePointerHover();
    }
    std::wstring script;
    script.reserve(payload.size() + 2048);
    if (!g_candidate_slot_api_installed)
    {
        script.append(kEnsureApplyCandidateFrameScript);
        g_candidate_slot_api_installed = true;
    }
    script.append(L"window.msimeCandidateDiagnostics = ");
    script.append(GetConfiguredDiagnosticLogEnabled() ? L"true;\n" : L"false;\n");
    script.append(L"window.ApplyCandidateFrame(");
    script.append(payload);
    script.append(L");");

    const HRESULT submitHr = webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([webview, onComplete](HRESULT errorCode,
                                                                                   LPCWSTR result) -> HRESULT {
            if (FAILED(errorCode))
            {
                CAND_WEBVIEW_TRACE_LOGF(L"candidate-slot execute-failed hr={:#x}", static_cast<unsigned>(errorCode));
            }
            else if (result)
            {
                g_last_candidate_slot_measured_size = ParseDivSize(result);
                // Numbers only (no candidate text): shows painted vs reported width.
                CAND_WEBVIEW_TRACE_LOGF(L"candidate-slot measured_dip=({:.2f},{:.2f}) raw={}",
                                        g_last_candidate_slot_measured_size.first,
                                        g_last_candidate_slot_measured_size.second, result);
            }
            g_candidate_slot_update_inflight = false;
            if (!g_candidate_slot_update_pending.empty())
            {
                FlushPendingCandidateSlotUpdate(webview);
                return S_OK;
            }
            if (onComplete)
            {
                onComplete();
            }
            return S_OK;
        }).Get());
    if (FAILED(submitHr))
    {
        g_candidate_slot_update_inflight = false;
        CAND_WEBVIEW_TRACE_LOGF(L"candidate-slot submit-failed hr={:#x}", static_cast<unsigned>(submitHr));
        if (onComplete)
        {
            onComplete();
        }
    }
}

void UpdateCandidateSlotsWithJavaScript(ComPtr<ICoreWebView2> webview, const std::wstring &payload,
                                        std::function<void()> onComplete, bool contentOnly)
{
    if (!webview)
    {
        if (onComplete)
        {
            onComplete();
        }
        return;
    }
    if (g_candidate_slot_update_inflight)
    {
        g_candidate_slot_update_pending = payload;
        g_candidate_slot_update_pending_complete = std::move(onComplete);
        g_candidate_slot_update_pending_content_only = contentOnly;
        return;
    }
    SubmitCandidateSlotScript(webview, payload, std::move(onComplete), contentOnly);
}

std::wstring BuildCandidateSlotPayloadJson(const std::wstring &commaSeparated, bool contentOnly)
{
    std::vector<std::wstring> words = SplitCandidateTemplatePayload(commaSeparated);
    nlohmann::json payload;
    payload["preedit"] = words.empty() ? std::string{} : wstring_to_string(words[0]);
    payload["items"] = nlohmann::json::array();
    // Preserve slot indices: do not compress out empty tokens.
    // The WebView DOM renderer expects wrapper<->slot index alignment.
    uint16_t nonEmptyMask = 0;
    for (size_t i = 1; i < words.size(); ++i)
    {
        if (i <= 9 && !words[i].empty())
        {
            nonEmptyMask |= static_cast<uint16_t>(1u) << static_cast<uint16_t>(i - 1);
        }
        payload["items"].push_back(wstring_to_string(words[i]));
    }
    if (nonEmptyMask == 0 && !words.empty())
    {
        CAND_DIAG_LOGF(L"candidate-slot payload all-empty words_sz={} content_only={} preedit_len={}", words.size(),
                       contentOnly ? 1 : 0, words[0].size());
    }
    payload["selected"] = Global::candidate_ui.selected_index_in_page;
    payload["preeditVisible"] = GetConfiguredCandidateWindowPreeditStyle() != "empty";
    payload["applyMargins"] = !contentOnly;
    payload["resetHover"] = !contentOnly;
    if (!contentOnly)
    {
        payload["marginTop"] = Global::MarginTop;
        payload["marginLeft"] = Global::MarginLeft;
    }
    return string_to_wstring(payload.dump());
}
} // namespace

std::pair<double, double> LastCandidateSlotMeasuredSize()
{
    return g_last_candidate_slot_measured_size;
}

void UpdateHtmlContentWithJavaScript(ComPtr<ICoreWebView2> webview, const std::wstring &newContent)
{
    UpdateHtmlContentWithJavaScript(webview, newContent, nullptr);
}

void UpdateHtmlContentWithJavaScript(ComPtr<ICoreWebView2> webview, const std::wstring &newContent,
                                     std::function<void()> onComplete)
{
    if (!webview)
    {
        if (onComplete)
        {
            onComplete();
        }
        return;
    }

    const std::wstring escaped = EscapeForJsTemplateLiteral(newContent);

    std::wstring script;
    script.reserve(escaped.length() + 2200);

    const bool diagnosticsEnabled = GetConfiguredDiagnosticLogEnabled();
    script.append(L"window.msimeCandidateDiagnostics = ");
    script.append(diagnosticsEnabled ? L"true;\n" : L"false;\n");
    script.append(L"document.getElementById('realContainer').innerHTML = `");
    script.append(escaped);
    script.append(L"`;\n");
    script.append(L"window.ClearState();\n");
    script.append(kInstallCandidateHoverLockScript);
    DisarmCandidatePointerHover();
    script.append(L"var el = document.getElementById('realContainerParent');\n");
    script.append(L"if (el) {\n");
    script.append(L"  el.style.marginTop = \"");
    script.append(std::to_wstring(Global::MarginTop));
    script.append(L"px\";\n");
    script.append(L"  el.style.marginLeft = \"");
    script.append(std::to_wstring(Global::MarginLeft));
    script.append(L"px\";\n");
    script.append(L"}\n");
    script.append(L"if (window.SetCandidateSelection) { window.SetCandidateSelection(");
    script.append(std::to_wstring(Global::candidate_ui.selected_index_in_page));
    script.append(L"); }\n");
    script.append(L"if (window.SetCandidatePreeditVisible) { window.SetCandidatePreeditVisible(");
    script.append(GetConfiguredCandidateWindowPreeditStyle() == "empty" ? L"false" : L"true");
    script.append(L"); }\n");
    script.append(L"if (window.SetPreeditCaret) { window.SetPreeditCaret(); }\n");
    script.append(L"if (window.CheckContentTruncation) { window.CheckContentTruncation(); }\n");
    if (diagnosticsEnabled)
    {
        script.append(LR"(
(function () {
  window.__msimeCandidateDomRevision = (window.__msimeCandidateDomRevision || 0) + 1;
  const revision = window.__msimeCandidateDomRevision;
  requestAnimationFrame(() => {
    window.chrome.webview.postMessage(JSON.stringify({type:'candidateFrameProbe',data:{
      revision:revision,stage:1,performanceMs:performance.now()
    }}));
    requestAnimationFrame(() => {
      window.chrome.webview.postMessage(JSON.stringify({type:'candidateFrameProbe',data:{
        revision:revision,stage:2,performanceMs:performance.now()
      }}));
    });
  });
})();
)");
    }

    if (!onComplete)
    {
        const HRESULT submitHr = webview->ExecuteScript(script.c_str(), nullptr);
        if (FAILED(submitHr))
        {
            CAND_WEBVIEW_TRACE_LOGF(L"candidate-script submit-failed hr={:#x} callback=false",
                                    static_cast<unsigned>(submitHr));
        }
        return;
    }

    const HRESULT submitHr = webview->ExecuteScript(
        script.c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([onComplete](HRESULT errorCode, LPCWSTR) -> HRESULT {
            if (FAILED(errorCode))
            {
                CAND_WEBVIEW_TRACE_LOGF(L"candidate-script execute-failed hr={:#x} callback=true",
                                        static_cast<unsigned>(errorCode));
            }
            onComplete();
            return S_OK;
        }).Get());
    if (FAILED(submitHr))
    {
        CAND_WEBVIEW_TRACE_LOGF(L"candidate-script submit-failed hr={:#x} callback=true",
                                static_cast<unsigned>(submitHr));
    }
}

void PrepareCandidateWebViewBoundsForMeasure(HWND hwnd)
{
    if (!webviewControllerCandWnd || !hwnd)
    {
        return;
    }
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    // MarginLeft can push the card nearly a full max-width across the viewport
    // near a screen edge. Reserve that room in DIPs so 200% scaling still has
    // enough CSS pixels; a fixed 1000 physical-px pad is only ~500 DIP at 200%.
    FLOAT scale = GetWebViewRasterizationScale(hwnd);
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    const int extraRightDip = ::CANDIDATE_WINDOW_MAX_WIDTH_DIP + (2 * ::SHADOW_WIDTH) + ::POP_UP_WND_WIDTH;
    const int extraBottomDip = ::CANDIDATE_WINDOW_HEIGHT + (2 * ::SHADOW_HEIGHT) + ::POP_UP_WND_HEIGHT;
    const int extraRightPx = (std::max)(candidateBoundExtraFloorPx, static_cast<int>(std::ceil(extraRightDip * scale)));
    const int extraBottomPx =
        (std::max)(candidateBoundExtraFloorPx, static_cast<int>(std::ceil(extraBottomDip * scale)));
    bounds.right += extraRightPx;
    bounds.bottom += extraBottomPx;
    const HRESULT hr = webviewControllerCandWnd->put_Bounds(bounds);
    DIAG_LOGF(L"ui-webview-bounds phase=measure client_plus_reserve={}x{} extra_px=({},{}) scale={:.3f} hr={:#x}",
              bounds.right - bounds.left, bounds.bottom - bounds.top, extraRightPx, extraBottomPx,
              static_cast<double>(scale), static_cast<unsigned>(hr));
}

void SyncCandidateWebViewBoundsToHost(HWND hwnd)
{
    if (!webviewControllerCandWnd || !hwnd)
    {
        return;
    }
    // Keep the measure-time reserve instead of shrinking the viewport to the
    // host client. MarginLeft pushes the card towards the right of the viewport
    // near a screen edge, so an exactly-sized viewport leaves the card less
    // room than it was measured with and it wraps into a taller card that the
    // window region then clips.
    PrepareCandidateWebViewBoundsForMeasure(hwnd);
    const HRESULT hr = webviewControllerCandWnd->NotifyParentWindowPositionChanged();
    DIAG_LOGF(L"ui-webview-bounds phase=sync-parent notify_hr={:#x}", static_cast<unsigned>(hr));
}

void LogCandidateLayoutSnapshot(const wchar_t *stage)
{
    (void)stage;
}

int PrepareHtmlForWnds()
{
    // e.g. C:\\Users\\SonnyCalcr\\AppData\\Local\\metasequoiaime
    std::wstring assetPath = fmt::format( //
        L"{}\\{}",                        //
        CommonUtils::get_local_appdata_path_w(), GlobalIme::AppName);

    //
    // 候选窗口
    //
    const bool isHorizontal = GetConfiguredCandidateWindowLayout() == "horizontal";
    const bool candLight = ResolveConfiguredTheme(GetConfiguredThemeCand()) == "light";
    const std::string candidateSkin = GetConfiguredCandidateSkin();
    const std::string candidateLayout = isHorizontal ? "horizontal" : "vertical";
    const std::string candidateTheme = candLight ? "light" : "dark";
    activeExternalCandidateSkin.reset();
    std::string baseCandidateSkin = candidateSkin;
    if (!CandidateSkinCatalog::IsBuiltIn(candidateSkin))
    {
        const std::wstring skinsRoot = assetPath + L"\\skins";
        activeExternalCandidateSkin = CandidateSkinCatalog::Load(skinsRoot, candidateSkin);
        if (activeExternalCandidateSkin &&
            !CandidateSkinCatalog::Supports(*activeExternalCandidateSkin, candidateLayout, candidateTheme))
            activeExternalCandidateSkin.reset();
        baseCandidateSkin = activeExternalCandidateSkin ? activeExternalCandidateSkin->base : "fluent";
    }
    std::wstring htmlCandWnd;
    std::wstring bodyHtmlCandWnd;
    std::wstring measureHtmlCandWnd;
    if (isHorizontal)
    {
        htmlCandWnd = L"/html/webview2/candwnd/horizontal_candidate_window.html";
        // Body/measure fragments are theme-agnostic markup; keep the existing dark assets.
        bodyHtmlCandWnd = L"/html/webview2/candwnd/body/horizontal_candidate_window_dark.html";
        measureHtmlCandWnd = L"/html/webview2/candwnd/body/horizontal_candidate_window_dark_measure.html";
    }
    else
    {
        htmlCandWnd = L"/html/webview2/candwnd/vertical_candidate_window.html";
        bodyHtmlCandWnd = L"/html/webview2/candwnd/body/vertical_candidate_window_dark.html";
        measureHtmlCandWnd = L"/html/webview2/candwnd/body/vertical_candidate_window_dark_measure.html";
    }

    std::wstring entireHtmlPathCandWnd = assetPath + htmlCandWnd;
    ::HTMLStringCandWnd = ReadHtmlFile(entireHtmlPathCandWnd);
    const std::wstring candidateStyleName =
        fmt::format(L"{}_{}.css", string_to_wstring(candidateLayout), string_to_wstring(candidateTheme));
    const std::wstring candidateStylePath =
        fmt::format(L"/html/webview2/candwnd/skins/{}/{}", string_to_wstring(baseCandidateSkin), candidateStyleName);
    std::wstring builtInCandidateCss = ReadHtmlFile(assetPath + candidateStylePath);
    if (builtInCandidateCss.empty())
    {
        builtInCandidateCss = ReadHtmlFile(assetPath + L"/html/webview2/candwnd/skins/fluent/" + candidateStyleName);
    }
    if (builtInCandidateCss.empty() && candLight)
    {
        builtInCandidateCss = ReadHtmlFile(assetPath + L"/html/webview2/candwnd/skins/fluent/" +
                                           string_to_wstring(candidateLayout) + L"_dark.css");
    }
    InjectCandidateDocumentSkin(::HTMLStringCandWnd, builtInCandidateCss, candidateSkin, baseCandidateSkin,
                                candidateLayout, candidateTheme);
    if (activeExternalCandidateSkin)
    {
        const std::wstring skinsRoot = assetPath + L"\\skins";
        if (!InjectExternalCandidateSkin(::HTMLStringCandWnd, *activeExternalCandidateSkin, skinsRoot))
        {
            // Without the stylesheet, native decoration insets would clip the card.
            activeExternalCandidateSkin.reset();
        }
    }
    std::wstring bodyHtmlPathCandWnd = assetPath + bodyHtmlCandWnd;
    ::BodyStringCandWnd = ReadHtmlFile(bodyHtmlPathCandWnd);
    std::wstring measureHtmlPathCandWnd = assetPath + measureHtmlCandWnd;
    ::MeasureStringCandWnd = ReadHtmlFile(measureHtmlPathCandWnd);
    (void)0;

    //
    // 托盘语言区菜单窗口
    //
    const bool menuLight = ResolveConfiguredTheme(GetConfiguredThemeMenu()) == "light";
    std::wstring htmlMenuWnd =
        menuLight ? L"/html/webview2/menu/default_light.html" : L"/html/webview2/menu/default.html";
    std::wstring entireHtmlPathMenuWnd = assetPath + htmlMenuWnd;
    ::HTMLStringMenuWnd =
        ReadHtmlFileWithFallback(entireHtmlPathMenuWnd, assetPath + L"/html/webview2/menu/default.html");

    //
    // settings 窗口
    // 这里暂时没有用到，因为 settings 窗口使用的是映射 url 导航
    //
    /*
    std::wstring htmlSettingsWnd = L"/html/webview2/settings/default.html";
    std::wstring entireHtmlPathSettingsWnd = assetPath + htmlSettingsWnd;
    ::HTMLStringSettingsWnd = ReadHtmlFile(entireHtmlPathSettingsWnd);
    */

    //
    // floating toolbar 窗口
    //
    const bool ftbLight = ResolveConfiguredTheme(GetConfiguredThemeFtb()) == "light";
    std::wstring htmlFtbWnd;
    if (baseCandidateSkin == "wechat")
    {
        htmlFtbWnd = ftbLight ? L"/html/webview2/ftb/wechat_light.html" : L"/html/webview2/ftb/wechat.html";
    }
    else if (baseCandidateSkin == "graphite")
    {
        htmlFtbWnd = ftbLight ? L"/html/webview2/ftb/graphite_light.html" : L"/html/webview2/ftb/graphite.html";
    }
    else if (baseCandidateSkin == "willow_green")
    {
        htmlFtbWnd = ftbLight ? L"/html/webview2/ftb/willow_green_light.html" : L"/html/webview2/ftb/willow_green.html";
    }
    else
    {
        htmlFtbWnd = ftbLight ? L"/html/webview2/ftb/default_light.html" : L"/html/webview2/ftb/default.html";
    }
    std::wstring entireHtmlPathFtbWnd = assetPath + htmlFtbWnd;
    ::HTMLStringFtbWnd = ReadHtmlFileWithFallback(entireHtmlPathFtbWnd, assetPath + L"/html/webview2/ftb/default.html");
    if (activeExternalCandidateSkin && !::HTMLStringFtbWnd.empty())
    {
        const size_t htmlTag = ::HTMLStringFtbWnd.find(L"<html");
        const size_t htmlTagEnd =
            htmlTag == std::wstring::npos ? std::wstring::npos : ::HTMLStringFtbWnd.find(L'>', htmlTag);
        if (htmlTagEnd != std::wstring::npos)
        {
            ::HTMLStringFtbWnd.insert(htmlTagEnd,
                                      fmt::format(L" data-candidate-skin=\"{}\" data-candidate-base=\"{}\" "
                                                  L"data-candidate-theme=\"{}\"",
                                                  string_to_wstring(candidateSkin),
                                                  string_to_wstring(baseCandidateSkin), ftbLight ? L"light" : L"dark"));
        }
        if (!activeExternalCandidateSkin->toolbarStylesheet.empty())
        {
            const std::wstring skinsRoot = assetPath + L"\\skins";
            InjectExternalSkinCssFile(::HTMLStringFtbWnd, *activeExternalCandidateSkin, skinsRoot,
                                      activeExternalCandidateSkin->toolbarStylesheet, L"external-toolbar-skin");
        }
    }
    // The small windows navigate from strings. Inline the pinned local runtime
    // before navigation instead of blocking first paint on two virtual-host loads.
    // Immutable for this process, like the executable's protocol contract.
    static const std::wstring protocolSchema = ReadHtmlFile(assetPath + L"/html/webview2/shared/schema.js");
    static const std::wstring protocolRuntime = ReadHtmlFile(assetPath + L"/html/webview2/shared/runtime.js");
    InlineWebViewProtocolScripts(::HTMLStringCandWnd, protocolSchema, protocolRuntime);
    InlineWebViewProtocolScripts(::HTMLStringMenuWnd, protocolSchema, protocolRuntime);
    InlineWebViewProtocolScripts(::HTMLStringFtbWnd, protocolSchema, protocolRuntime);
    preparedCandidateSkin = candidateSkin;

    return 0;
}

bool ApplyConfiguredCandidateWindowLayout()
{
    PrepareHtmlForWnds();
    if (CandidatePresenter::Instance().IsBound() && !webviewCandWnd)
    {
        return true;
    }
    if (!webviewCandWnd || HTMLStringCandWnd.empty())
    {
        return false;
    }
    const bool ok = SUCCEEDED(webviewCandWnd->NavigateToString(HTMLStringCandWnd.c_str()));
    if (ok)
    {
        loadedCandidateSkin = preparedCandidateSkin;
    }
    return ok;
}

bool ApplyConfiguredUiThemes()
{
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().ApplyTheme();
    }
    const std::string candidateSkin = GetConfiguredCandidateSkin();
    PrepareHtmlForWnds();
    bool ok = true;
    if (webviewCandWnd && !HTMLStringCandWnd.empty())
    {
        const bool candidateOk = SUCCEEDED(webviewCandWnd->NavigateToString(HTMLStringCandWnd.c_str()));
        if (candidateOk)
        {
            loadedCandidateSkin = candidateSkin;
        }
        ok = candidateOk && ok;
    }
    if (webviewFtbWnd && !HTMLStringFtbWnd.empty())
    {
        ClearFloatingToolbarNavigationState();
        const bool floatingToolbarOk = SUCCEEDED(webviewFtbWnd->NavigateToString(HTMLStringFtbWnd.c_str()));
        if (floatingToolbarOk)
        {
            loadedFloatingToolbarSkin = candidateSkin;
        }
        ok = floatingToolbarOk && ok;
    }
    if (webviewMenuWnd && !HTMLStringMenuWnd.empty())
    {
        ok = SUCCEEDED(webviewMenuWnd->NavigateToString(HTMLStringMenuWnd.c_str())) && ok;
    }
    return ok;
}

bool ApplyConfiguredCandidateSkinIfChanged()
{
    const std::string &candidateSkin = GetConfiguredCandidateSkin();
    const bool candidateCurrent = !webviewCandWnd || loadedCandidateSkin == candidateSkin;
    const bool floatingToolbarCurrent = !webviewFtbWnd || loadedFloatingToolbarSkin == candidateSkin;
    if (candidateCurrent && floatingToolbarCurrent)
    {
        return true;
    }
    return ApplyConfiguredUiThemes();
}

bool ForceReloadConfiguredCandidateSkin()
{
    ++candidateSkinReloadRevision;
    loadedCandidateSkin.clear();
    loadedFloatingToolbarSkin.clear();
    const bool cloakCandidate = ::is_global_wnd_cand_shown && ::global_hwnd && IsWindow(::global_hwnd) &&
                                !CandidatePresenter::Instance().IsBound();
    if (cloakCandidate)
        SetCandidateHostCloaked(true);
    const bool ok = ApplyConfiguredUiThemes();
    if (!ok && cloakCandidate)
        SetCandidateHostCloaked(false);
    return ok;
}

bool ApplyConfiguredCandidateAppearance()
{
    if (CandidatePresenter::Instance().IsBound() && !webviewCandWnd)
    {
        return true;
    }
    if (!webviewCandWnd)
    {
        return false;
    }

    nlohmann::json cfg = {{"font", ResolveSystemFontFamilyForCss(GetConfiguredCandidateFont())},
                          {"english_font", ResolveSystemFontFamilyForCss(GetConfiguredCandidateEnglishFont())},
                          {"fallback_fonts", GetConfiguredCandidateFallbackFontFamilies()},
                          {"font_size", GetConfiguredCandidateFontSize()},
                          {"preedit_font_size", GetConfiguredCandidateWindowPreeditFontSize()},
                          {"cand_text_color", GetConfiguredCandidateTextColor()}};
    std::string family;
    auto appendFont = [&](const std::string &font) {
        // JSON quoting also escapes CSS quotes/backslashes; font names exclude control characters.
        if (!family.empty())
            family += ", ";
        family += nlohmann::json(font).dump(-1, ' ', false);
    };
    appendFont(ResolveSystemFontFamilyForCss(GetConfiguredCandidateEnglishFont()));
    for (const auto &font : GetConfiguredCandidateFallbackFontFamilies())
        appendFont(font);
    cfg["font_family"] = family + ", sans-serif";
    const std::wstring script =
        L"(function(c){"
        L"const root=document.documentElement;"
        L"const family=c.font_family;"
        L"root.style.setProperty('--cand-font-family', family);"
        L"root.style.setProperty('--cand-font-size', String(c.font_size||16)+'px');"
        L"root.style.setProperty('--preedit-font-size', String(c.preedit_font_size||c.font_size||16)+'px');"
        L"const color=(c.cand_text_color||'auto');"
        L"if(color&&color!=='auto'){"
        L"root.style.setProperty('--cand-text', color);"
        L"root.style.setProperty('--cand-num', color.length===7?color+'9d':color);"
        L"}else{"
        L"root.style.removeProperty('--cand-text');"
        L"root.style.removeProperty('--cand-num');"
        L"}"
        // Drop any stale nowrap fast-layout sheet so the skin's wrap-at-max-width
        // rules apply; forcing a single line makes the card overflow the cap and
        // get clipped by .container{overflow-x:hidden}.
        L"document.getElementById('msime-fast-layout')?.remove();"
        L"})(" +
        string_to_wstring(cfg.dump()) + L");";
    return SUCCEEDED(webviewCandWnd->ExecuteScript(script.c_str(), nullptr));
}

bool ApplyConfiguredFloatingToolbarAppearance()
{
    return ApplyConfiguredFloatingToolbarAppearance(nullptr);
}

bool ApplyConfiguredFloatingToolbarAppearance(std::function<void()> onComplete)
{
    if (FloatingToolbarPresenter::Instance().IsBound())
    {
        FloatingToolbarPresenter::Instance().ApplyAppearance();
        if (onComplete)
        {
            onComplete();
        }
        return true;
    }
    if (!webviewFtbWnd)
    {
        if (onComplete)
        {
            onComplete();
        }
        return false;
    }

    nlohmann::json cfg = {{"scale", GetConfiguredFloatingToolbarScale()},
                          {"font_size", GetConfiguredFloatingToolbarFontSize()}};
    const std::wstring script = L"(function(c){"
                                L"const root=document.documentElement;"
                                L"const scale=(typeof c.scale==='number'&&c.scale>0)?c.scale:1;"
                                L"const icon=(typeof c.font_size==='number'&&c.font_size>0)?c.font_size:24;"
                                L"root.style.setProperty('--ftb-scale', String(scale));"
                                L"root.style.setProperty('--ftb-icon-size', String(icon)+'px');"
                                L"return true;"
                                L"})(" +
                                string_to_wstring(cfg.dump()) + L");";
    if (!onComplete)
    {
        return SUCCEEDED(webviewFtbWnd->ExecuteScript(script.c_str(), nullptr));
    }
    return SUCCEEDED(webviewFtbWnd->ExecuteScript(
        script.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>([onComplete](HRESULT, LPCWSTR) -> HRESULT {
                            onComplete();
                            return S_OK;
                        }).Get()));
}

//
//
// 候选窗口 webview
//
//

void UpdateMeasureContentWithJavaScript(ComPtr<ICoreWebView2> webview, const std::wstring &newContent,
                                        std::function<void()> onComplete)
{
    if (webview == nullptr)
    {
        if (onComplete)
        {
            onComplete();
        }
        return;
    }

    const std::wstring escaped = EscapeForJsTemplateLiteral(newContent);

    std::wstring script;
    script.reserve(escaped.length() + 256);

    script.append(L"document.getElementById('measureContainer').innerHTML = `");
    script.append(escaped);
    script.append(L"`;\n");
    script.append(L"if (window.SetCandidatePreeditVisible) { window.SetCandidatePreeditVisible(");
    script.append(GetConfiguredCandidateWindowPreeditStyle() == "empty" ? L"false" : L"true");
    script.append(L"); }\n");

    if (!onComplete)
    {
        webview->ExecuteScript(script.c_str(), nullptr);
        return;
    }

    webview->ExecuteScript(
        script.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>([onComplete](HRESULT, LPCWSTR) -> HRESULT {
                            onComplete();
                            return S_OK;
                        }).Get());
}

void UpdateMeasureContentWithJavaScript(ComPtr<ICoreWebView2> webview, const std::wstring &newContent)
{
    UpdateMeasureContentWithJavaScript(webview, newContent, nullptr);
}

void DisableMouseForAWhileWhenShownCandWnd(ComPtr<ICoreWebView2> webview)
{
    if (webview != nullptr)
    {
        std::wstring script = LR"(
if (window.mouseBlockTimeout) {
    clearTimeout(window.mouseBlockTimeout);
}

document.documentElement.style.pointerEvents = "none";

window.mouseBlockTimeout = setTimeout(() => {
    document.documentElement.style.pointerEvents = "auto";
    window.mouseBlockTimeout = null;
}, 500);
        )";
        webview->ExecuteScript(script.c_str(), nullptr);
    }
}

void InflateCandWnd(std::wstring &str)
{
    InflateCandWnd(str, nullptr);
}

void InflateCandWnd(std::wstring &str, std::function<void()> onComplete)
{
    std::wstring result = InflateCandidateTemplate(BodyStringCandWnd, str);
    UpdateHtmlContentWithJavaScript(webviewCandWnd, result, std::move(onComplete));
}

void InflateCandWnd(std::wstring &str, std::function<void()> onComplete, bool contentOnly)
{
    (void)contentOnly;
    std::wstring result = InflateCandidateTemplate(BodyStringCandWnd, str);
    UpdateHtmlContentWithJavaScript(webviewCandWnd, result, std::move(onComplete));
}

void InflateMeasureDivCandWnd(std::wstring &str)
{
    InflateMeasureDivCandWnd(str, nullptr);
}

void InflateMeasureDivCandWnd(std::wstring &str, std::function<void()> onComplete)
{
    str.erase(std::remove(str.begin(), str.end(), L'\uE000'), str.end());
    std::wstring result = InflateCandidateTemplate(::MeasureStringCandWnd, str);

    UpdateMeasureContentWithJavaScript(webviewCandWnd, result, std::move(onComplete));
}

/**
 * @brief Handle candidate window webview2 controller creation
 *
 * @param hwnd
 * @param result
 * @param controller
 * @return HRESULT
 */
HRESULT OnControllerCreatedCandWnd(     //
    HWND hwnd,                          //
    HRESULT result,                     //
    ICoreWebView2Controller *controller //
)
{
    CAND_DIAG_LOGF(L"webview controller callback hr={:#x} controller_present={} hwnd_valid={}",
                   static_cast<unsigned>(result), controller != nullptr, IsWindow(hwnd) != FALSE);
    if (!controller || FAILED(result))
    {
        OnSmallWindowControllerSettled(FAILED(result) ? result : E_FAIL);
        return E_FAIL;
    }

    webviewControllerCandWnd = controller;
    const HRESULT getWebviewHr = webviewControllerCandWnd->get_CoreWebView2(webviewCandWnd.GetAddressOf());

    if (!webviewCandWnd)
    {
        CAND_DIAG_LOGF(L"webview get_CoreWebView2 failed hr={:#x}", static_cast<unsigned>(getWebviewHr));
        webviewControllerCandWnd.Reset();
        OnSmallWindowControllerSettled(FAILED(getWebviewHr) ? getWebviewHr : E_FAIL);
        return E_FAIL;
    }

    UpdateSmallWindowWebviewVisibility(hwnd, IsWindowVisible(hwnd) != FALSE);

    // Configure WebView settings
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webviewCandWnd->get_Settings(&settings)))
    {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(FALSE); // Since we only use WebMessages
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);

        // Try to disable browser accelerators (Ctrl+R, F5, etc.)
        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings3))))
        {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
            // settings3->put_IsPinchZoomEnabled(FALSE); // Unsupported in this header version
        }

        // Try to disable autofill and password saving
        ComPtr<ICoreWebView2Settings5> settings5;
        if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings5))))
        {
            settings5->put_IsGeneralAutofillEnabled(FALSE);
            settings5->put_IsPasswordAutosaveEnabled(FALSE);
        }

        // Try to disable built-in error pages for a cleaner UI
        ComPtr<ICoreWebView2Settings6> settings6;
        if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings6))))
        {
            settings6->put_IsBuiltInErrorPageEnabled(FALSE);
        }
    }

    webviewControllerCandWnd->put_ZoomFactor(1.0);

    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&webviewController3CandWnd))))
    {
        // Let WebView2 track both monitor DPI and the user's accessibility text
        // scale. Every native clip/layout conversion reads this same value.
        const HRESULT detectHr = webviewController3CandWnd->put_ShouldDetectMonitorScaleChanges(TRUE);
        double rasterizationScale = 0.0;
        const HRESULT scaleHr = webviewController3CandWnd->get_RasterizationScale(&rasterizationScale);
        const HRESULT eventHr = webviewController3CandWnd->add_RasterizationScaleChanged(
            Callback<ICoreWebView2RasterizationScaleChangedEventHandler>([hwnd](ICoreWebView2Controller *sender,
                                                                                IUnknown *) -> HRESULT {
                if (!IsWindow(hwnd))
                {
                    return S_OK;
                }
                ComPtr<ICoreWebView2Controller3> controller3;
                double scale = 0.0;
                const HRESULT hr = sender ? sender->QueryInterface(IID_PPV_ARGS(&controller3)) : E_POINTER;
                const HRESULT scaleHr = SUCCEEDED(hr) ? controller3->get_RasterizationScale(&scale) : hr;
                DIAG_LOGF(L"candidate rasterization scale changed scale={:.4f} native_scale={:.4f} hr={:#x}", scale,
                          static_cast<double>(GetWindowScale(hwnd)), static_cast<unsigned>(scaleHr));
                InjectSurfaceViewportLimits(webviewCandWnd.Get(), hwnd);
                if (::is_global_wnd_cand_shown && IsCandidateWebviewReady())
                {
                    FineTuneWindow(hwnd);
                }
                return S_OK;
            }).Get(),
            &candidateRasterizationScaleChangedToken);
        candidateRasterizationScaleChangedRegistered = SUCCEEDED(eventHr);
        DIAG_LOGF(L"candidate rasterization scale initialized scale={:.4f} native_scale={:.4f} "
                  L"scale_hr={:#x} detect_hr={:#x} event_hr={:#x}",
                  rasterizationScale, static_cast<double>(GetWindowScale(hwnd)), static_cast<unsigned>(scaleHr),
                  static_cast<unsigned>(detectHr), static_cast<unsigned>(eventHr));
    }

    // Configure virtual host path
    if (SUCCEEDED(webviewCandWnd->QueryInterface(IID_PPV_ARGS(&webview3CandWnd))))
    {
        const auto contractsPath = std::filesystem::path(CommonUtils::get_local_appdata_path_w()) / GlobalIme::AppName /
                                   L"html" / L"webview2" / L"shared";
        webview3CandWnd->SetVirtualHostNameToFolderMapping(L"msime-contracts", contractsPath.wstring().c_str(),
                                                           COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

        const std::wstring assetPath = fmt::format(  //
            L"{}\\{}\\html\\webview2\\candwnd",      //
            CommonUtils::get_local_appdata_path_w(), //
            GlobalIme::AppName                       //
        );

        // Assets mapping
        webview3CandWnd->SetVirtualHostNameToFolderMapping(  //
            L"candwnd",                                      //
            assetPath.c_str(),                               //
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS //
        );                                                   //
        const std::wstring skinsPath = fmt::format(          //
            L"{}\\{}\\skins",                                //
            CommonUtils::get_local_appdata_path_w(),         //
            GlobalIme::AppName                               //
        );
        const HRESULT skinsMappingHr = webview3CandWnd->SetVirtualHostNameToFolderMapping(
            L"candidate-skins", skinsPath.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        (void)skinsMappingHr;
        (void)0;
    }

    // Set transparent background
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&webviewController2CandWnd))))
    {
        COREWEBVIEW2_COLOR backgroundColor = {0, 0, 0, 0};
        webviewController2CandWnd->put_DefaultBackgroundColor(backgroundColor);
    }

    // Adjust to window size — keep the same DIP-based measure reserve used by
    // FineTune so the first layout is not constrained by a narrow HWND.
    PrepareCandidateWebViewBoundsForMeasure(hwnd);

    // Navigate to HTML
    if (HTMLStringCandWnd.empty())
    {
        PrepareHtmlForWnds();
    }
    HRESULT hr = webviewCandWnd->NavigateToString(HTMLStringCandWnd.c_str());
    CAND_DIAG_LOGF(L"webview NavigateToString hr={:#x} html_chars={} skin={}", static_cast<unsigned>(hr),
                   HTMLStringCandWnd.size(), string_to_wstring(preparedCandidateSkin));
    if (SUCCEEDED(hr))
    {
        loadedCandidateSkin = preparedCandidateSkin;
    }
    (void)0;
    if (FAILED(hr))
    {
        ShowErrorMessage(hwnd, L"Failed to navigate to string.");
    }

    /* Debug console */
    // webview->OpenDevToolsWindow();

    webviewCandWnd->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [hwnd](ICoreWebView2 * /*sender*/, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                HRESULT hr = args->TryGetWebMessageAsString(&message);

                if (SUCCEEDED(hr) && message.get())
                {
                    std::wstring msg(message.get());
                    // 解析 msg，执行相应操作
                    try
                    {
                        json::value val = json::parse(wstring_to_string(msg));
                        if (!metasequoia::webview::Validate(val, "client", "candidate"))
                            return S_OK;
                        std::string type = json::value_to<std::string>(val.at("type"));
                        if (type == "candidateFrameProbe")
                        {
                            const auto &data = val.at("data");
                            const int revision = json::value_to<int>(data.at("revision"));
                            const int stage = json::value_to<int>(data.at("stage"));
                            const double performanceMs = json::value_to<double>(data.at("performanceMs"));
                            RECT rect{};
                            GetWindowRect(hwnd, &rect);
                            CAND_WEBVIEW_TRACE_LOGF(
                                L"candidate-browser-frame revision={} stage={} performance_ms={:.3f} native_tick={} "
                                L"hwnd_rect=({},{},{}x{}) shown={} window_visible={}",
                                revision, stage, performanceMs, GetTickCount64(), rect.left, rect.top,
                                rect.right - rect.left, rect.bottom - rect.top, ::is_global_wnd_cand_shown,
                                IsWindowVisible(hwnd) != FALSE);
                        }
                        else if (type == "candidatePointerArmed")
                        {
                            g_candidate_hover_armed = true;
                            if (::global_hwnd)
                            {
                                KillTimer(::global_hwnd, kCandidateHoverArmTimerId);
                            }
                            CAND_WEBVIEW_TRACE_LOGF(L"candidate-hover arm source=dom-motion tick={}", GetTickCount64());
                        }
                        else if (type == "candidatePointerMotion")
                        {
                            // A WebView mousemove can be synthesized when the
                            // candidate HWND moves under a stationary cursor.
                            // The native screen-coordinate comparison arms only
                            // when the physical pointer itself really moved.
                            MaybeArmCandidatePointerHover();
                        }
                        else if (type == "candidatePointerProbe")
                        {
                            const auto &data = val.at("data");
                            POINT cursor{};
                            GetCursorPos(&cursor);
                            RECT rect{};
                            GetWindowRect(hwnd, &rect);
                            CAND_WEBVIEW_TRACE_LOGF(
                                L"candidate-pointer probe={} event_screen=({},{}) event_client=({},{}) "
                                L"movement=({},{}) js_armed={} native_cursor=({},{}) baseline=({},{}) "
                                L"native_armed={} hwnd=({},{},{}x{}) tick={}",
                                json::value_to<int>(data.at("probe")), json::value_to<int>(data.at("screenX")),
                                json::value_to<int>(data.at("screenY")), json::value_to<int>(data.at("clientX")),
                                json::value_to<int>(data.at("clientY")), json::value_to<int>(data.at("movementX")),
                                json::value_to<int>(data.at("movementY")), json::value_to<bool>(data.at("armed")),
                                cursor.x, cursor.y, g_candidate_hover_cursor.x, g_candidate_hover_cursor.y,
                                g_candidate_hover_armed, rect.left, rect.top, rect.right - rect.left,
                                rect.bottom - rect.top, GetTickCount64());
                        }
                        else if (type == "delete")
                        {
                            int idx = json::value_to<int>(val.at("data"));
                            if (FanyImeIpc::IsValidCandidateUiOneBasedIndex(idx))
                            {
                                PostMessage(::global_hwnd, WM_DELETE_CANDIDATE, idx, 0);
                            }
                        }
                        else if (type == "pin")
                        {
                            int idx = json::value_to<int>(val.at("data"));
                            if (FanyImeIpc::IsValidCandidateUiOneBasedIndex(idx))
                            {
                                PostMessage(::global_hwnd, WM_PIN_TO_TOP_CANDIDATE, idx, 0);
                            }
                        }
                        else if (type == "fixPosition")
                        {
                            int idx = json::value_to<int>(val.at("data").at("index"));
                            int position = json::value_to<int>(val.at("data").at("position"));
                            if (FanyImeIpc::IsValidCandidateUiOneBasedIndex(idx) && position >= 1 && position <= 5)
                                PostMessage(::global_hwnd, WM_FIX_CANDIDATE_POSITION, idx, position);
                        }
                        else if (type == "clearPosition")
                        {
                            int idx = json::value_to<int>(val.at("data"));
                            if (FanyImeIpc::IsValidCandidateUiOneBasedIndex(idx))
                                PostMessage(::global_hwnd, WM_CLEAR_CANDIDATE_POSITION, idx, 0);
                        }
                        else if (type == "contextMenuResize")
                        {
                            const auto &data = val.at("data");
                            const int width = json::value_to<int>(data.at("width"));
                            const int height = json::value_to<int>(data.at("height"));
                            int top_expansion = 0;
                            if (const auto *value = data.as_object().if_contains("topExpansion"))
                                top_expansion = (std::max)(0, json::value_to<int>(*value));
                            int left_expansion = 0;
                            if (const auto *value = data.as_object().if_contains("leftExpansion"))
                                left_expansion = (std::max)(0, json::value_to<int>(*value));
                            RECT window_rect{};
                            GetWindowRect(hwnd, &window_rect);
                            const FLOAT scale = GetWebViewRasterizationScale(hwnd);
                            const int current_width = static_cast<int>(window_rect.right - window_rect.left);
                            const int current_height = static_cast<int>(window_rect.bottom - window_rect.top);
                            const int top_expansion_px = static_cast<int>(std::ceil(top_expansion * scale));
                            const int left_expansion_px = static_cast<int>(std::ceil(left_expansion * scale));
                            SetWindowPos(hwnd, nullptr, window_rect.left - left_expansion_px,
                                         window_rect.top - top_expansion_px,
                                         (std::max)(current_width, static_cast<int>(std::ceil(width * scale))) +
                                             left_expansion_px,
                                         (std::max)(current_height, static_cast<int>(std::ceil(height * scale))) +
                                             top_expansion_px,
                                         SWP_NOZORDER | SWP_NOACTIVATE);
                            // FineTuneWindow clips the stable candidate host to
                            // the card. The context menu is allowed to use the
                            // temporarily expanded host and FineTuneWindow
                            // reapplies the tight region after it closes.
                            SetWindowRgn(hwnd, nullptr, TRUE);
                            RECT bounds{};
                            GetClientRect(hwnd, &bounds);
                            webviewControllerCandWnd->put_Bounds(bounds);
                            if (top_expansion > 0 || left_expansion > 0)
                            {
                                const std::wstring script = L"if(window.ApplyContextMenuTopExpansion)"
                                                            L"window.ApplyContextMenuTopExpansion(" +
                                                            std::to_wstring(top_expansion) +
                                                            L");if(window.ApplyContextMenuLeftExpansion)"
                                                            L"window.ApplyContextMenuLeftExpansion(" +
                                                            std::to_wstring(left_expansion) + L");";
                                webviewCandWnd->ExecuteScript(script.c_str(), nullptr);
                            }
                        }
                        else if (type == "contextMenuClosed")
                        {
                            const std::wstring preedit = GetConfiguredCandidateWindowPreeditStyle() == "empty"
                                                             ? std::wstring{}
                                                             : GetPreeditWithCaretMarker();
                            std::wstring measurement = preedit + L"," + Global::CandidateString;
                            InflateMeasureDivCandWnd(measurement, [hwnd]() { FineTuneWindow(hwnd); });
                        }
                        else if (type == "candidate")
                        {
                            int idx = json::value_to<int>(val.at("data"));
                            if (FanyImeIpc::IsValidCandidateUiOneBasedIndex(idx))
                            {
                                PostMessage(::global_hwnd, WM_COMMIT_CANDIDATE, idx, 0);
                            }
                        }
                        else if (type == "contentTruncated")
                        {
                            // Candidate host is intentionally quarter-screen; the card is
                            // placed with CSS margins + SetWindowRgn. HTML truncation checks
                            // use clientWidth/Height, so margin+content past the right/bottom
                            // of the host looks "truncated" even when FineTune is correct.
                            // Never shrink the HWND or zero margins here — that parked the
                            // card at the host origin away from the caret.
                            if (::is_global_wnd_cand_shown)
                            {
                                const bool grew = HandleContentTruncatedMessage(hwnd, webviewCandWnd.Get(),
                                                                                webviewControllerCandWnd.Get(), val,
                                                                                g_last_content_truncation_cand_ms, 0);
                                if (grew)
                                {
                                    // Host grew beyond the previous region; drop the clip so
                                    // the next FineTune/clip-measure can reapply margins.
                                    SetWindowRgn(hwnd, nullptr, TRUE);
                                    CAND_DIAG_LOGF(L"candidate contentTruncated grew host; region cleared "
                                                   L"(margins kept) {}",
                                                   DescribeCandidateHostState());
                                }
                                else
                                {
                                    CAND_DIAG_LOGF(L"candidate contentTruncated ignored (no shrink) {}",
                                                   DescribeCandidateHostState());
                                }
                            }
                        }
                    }
                    catch (const std::exception &)
                    {
                        // A malformed message from the page must not tear down the host.
                        return S_OK;
                    }
                }
                return S_OK;
            })
            .Get(),
        nullptr);

    webviewCandWnd->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [hwnd](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL success = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS errorStatus = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                const HRESULT successHr = args->get_IsSuccess(&success);
                const HRESULT statusHr = args->get_WebErrorStatus(&errorStatus);
                CAND_DIAG_LOGF(L"webview navigation completed success={} success_hr={:#x} status_hr={:#x} "
                               L"web_error={} logical_shown={}",
                               success != FALSE, static_cast<unsigned>(successHr), static_cast<unsigned>(statusHr),
                               static_cast<int>(errorStatus), ::is_global_wnd_cand_shown);
                if (success)
                {
                    g_candidate_slot_api_installed = false;
                    NotifySmallWindowNavigationReady(candidateNavigationReady, L"candidate");
                    ApplyConfiguredCandidateAppearance();
                    InjectSurfaceViewportLimits(webviewCandWnd.Get(), hwnd);
                    webviewCandWnd->ExecuteScript(kInstallCandidateHoverLockScript, nullptr);
                    DisarmCandidatePointerHover();
                }
                else
                {
                    (void)0;
                    if (::is_global_wnd_cand_shown)
                        SetCandidateHostCloaked(false);
                }
                if (success && ::is_global_wnd_cand_shown)
                {
                    const std::wstring preedit = GetConfiguredCandidateWindowPreeditStyle() == "empty"
                                                     ? std::wstring{}
                                                     : GetPreeditWithCaretMarker();
                    std::wstring str = preedit + L"," + Global::CandidateString;
                    InflateMeasureDivCandWnd(str, [hwnd]() {
                        if (!::is_global_wnd_cand_shown)
                        {
                            return;
                        }
                        FineTuneWindow(hwnd);
                    });
                }
                return S_OK;
            })
            .Get(),
        nullptr);

    webviewCandWnd->add_ProcessFailed(Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
                                          [](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *args) -> HRESULT {
                                              COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                                                  COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                                              const HRESULT hr = args->get_ProcessFailedKind(&kind);
                                              CAND_DIAG_LOGF(L"webview process failed kind={} hr={:#x}",
                                                             static_cast<int>(kind), static_cast<unsigned>(hr));
                                              return S_OK;
                                          })
                                          .Get(),
                                      nullptr);

    OnSmallWindowControllerSettled(S_OK);
    return S_OK;
}

/**
 * @brief Handle candidate window webview2 environment creation
 *
 * @param hwnd
 * @param result
 * @param env
 * @return HRESULT
 */
HRESULT OnEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env)
{
    (void)0;
    if (FAILED(result) || !env)
    {
        ShowErrorMessage(hwnd, L"Failed to create WebView2 environment.");
        return result;
    }

    // Create WebView2 controller
    const HRESULT hr = env->CreateCoreWebView2Controller(                                    //
        hwnd,                                                                                //
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>( //
            [hwnd](HRESULT result,                                                           //
                   ICoreWebView2Controller *controller) -> HRESULT {                         //
                return OnControllerCreatedCandWnd(hwnd, result, controller);                 //
            })                                                                               //
            .Get()                                                                           //
    );                                                                                       //
    (void)0;
    return hr;
}

//
//
// 菜单窗口 webview
//
//

/**
 * @brief Handle menu window webview2 controller creation
 *
 * @param hwnd
 * @param result
 * @param controller
 * @return HRESULT
 */
HRESULT OnControllerCreatedMenuWnd(     //
    HWND hwnd,                          //
    HRESULT result,                     //
    ICoreWebView2Controller *controller //
)
{
    if (!controller || FAILED(result))
    {
        FTB_DIAG_LOGF(L"menu controller creation failed hr={:#x}", static_cast<unsigned>(result));
        OnSmallWindowControllerSettled(FAILED(result) ? result : E_FAIL);
        return E_FAIL;
    }

    /* 给 controller 和 webview 赋值 */
    webviewControllerMenuWnd = controller;
    const HRESULT getMenuWebviewHr = webviewControllerMenuWnd->get_CoreWebView2(webviewMenuWnd.GetAddressOf());
    // A controller built against a host that is already topmost, or that is not
    // on a monitor, is the state that never recovers.
    FTB_DIAG_LOGF(L"menu controller created {}", DescribeTrayMenuHostState());

    if (!webviewMenuWnd)
    {
        webviewControllerMenuWnd.Reset();
        OnSmallWindowControllerSettled(FAILED(getMenuWebviewHr) ? getMenuWebviewHr : E_FAIL);
        return E_FAIL;
    }

    UpdateSmallWindowWebviewVisibility(hwnd, IsWindowVisible(hwnd) != FALSE);

    // Configure webviewMenuWindow settings
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webviewMenuWnd->get_Settings(&settings)))
    {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);

        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3)))
        {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }

        ComPtr<ICoreWebView2Settings5> settings5;
        if (SUCCEEDED(settings.As(&settings5)))
        {
            settings5->put_IsGeneralAutofillEnabled(FALSE);
            settings5->put_IsPasswordAutosaveEnabled(FALSE);
        }
    }

    webviewControllerMenuWnd->put_ZoomFactor(1.0);

    // Configure virtual host path
    if (SUCCEEDED(webviewMenuWnd->QueryInterface(IID_PPV_ARGS(&webview3MenuWnd))))
    {
        const auto contractsPath = std::filesystem::path(CommonUtils::get_local_appdata_path_w()) / GlobalIme::AppName /
                                   L"html" / L"webview2" / L"shared";
        webview3MenuWnd->SetVirtualHostNameToFolderMapping(L"msime-contracts", contractsPath.wstring().c_str(),
                                                           COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

        // Assets mapping
        webview3MenuWnd->SetVirtualHostNameToFolderMapping(  //
            L"appassets",                                    //
            GetLocalAssetsPath().c_str(),                    //
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS //
        );                                                   //
    }

    // Set transparent background
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&webviewController2MenuWnd))))
    {
        COREWEBVIEW2_COLOR backgroundColor = {0, 0, 0, 0};
        webviewController2MenuWnd->put_DefaultBackgroundColor(backgroundColor);
    }

    // Adjust to window size
    RECT bounds;
    GetClientRect(hwnd, &bounds);
    webviewControllerMenuWnd->put_Bounds(bounds);

    EventRegistrationToken menuNavigationCompletedToken{};
    webviewMenuWnd->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [hwnd](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL success = FALSE;
                if (!args || FAILED(args->get_IsSuccess(&success)) || !success)
                {
                    // PrepareTrayMenuWebviewForShow re-navigates over an in-flight
                    // navigation, which aborts the first one exactly like this. Only
                    // a run of these means the menu is never becoming ready.
                    FTB_DIAG_LOGF(L"menu navigation completed unsuccessfully");
                    return S_OK;
                }
                FTB_DIAG_LOGF(L"menu navigation completed {}", DescribeTrayMenuHostState());
                NotifySmallWindowNavigationReady(menuNavigationReady, L"menu");
                // Older menu assets already render the handwriting entry but do not
                // give it an id or native click bridge. Wire it at runtime so existing
                // installations gain the launcher without replacing their skin.
                sender->ExecuteScript(LR"((() => {
                        const item = [...document.querySelectorAll('.menu-item')]
                            .find(element => element.textContent.includes('\u624b\u5199\u8bc6\u522b\u677f'));
                        if (item && !item.dataset.nativeHandwritingLauncher) {
                            item.dataset.nativeHandwritingLauncher = 'true';
                            item.addEventListener('click', () => {
                                window.chrome.webview.postMessage(JSON.stringify({ type: 'handwritingPanel' }));
                            });
                        }
                    })())",
                                      nullptr);
                SyncMenuFloatingToolbarToggle();
                InjectSurfaceViewportLimits(sender, hwnd);
                PostMessage(hwnd, WM_REFRESH_MENU_SIZE, 0, 0);
                return S_OK;
            })
            .Get(),
        &menuNavigationCompletedToken);

    // Navigate to HTML
    webviewMenuWnd->NavigateToString(::HTMLStringMenuWnd.c_str());

    /* Debug console */
    // webviewMenuWindow->OpenDevToolsWindow();

    webviewMenuWnd->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [hwnd](ICoreWebView2 * /*sender*/, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                HRESULT hr = args->TryGetWebMessageAsString(&message);

                if (SUCCEEDED(hr) && message.get())
                {
                    std::wstring msg(message.get());
                    // 解析 msg，执行相应操作
                    json::value val = json::parse(wstring_to_string(msg));
                    if (!metasequoia::webview::Validate(val, "client", "menu"))
                        return S_OK;
                    std::string type = json::value_to<std::string>(val.at("type"));
                    if (type == "floatingToggle")
                    {
                        bool needShown = json::value_to<bool>(val.at("data"));
                        if (SetConfiguredFloatingToolbarEnabled(needShown))
                        {
                            ApplyConfiguredFloatingToolbarVisibility(L"tray-menu-toggle");
                            PostSettingsConfig();
                        }
                    }
                    else if (type == "settings")
                    {
                        OpenSettingsApplication();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "about")
                    {
                        OpenSettingsAboutApplication();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "emojiSymbols")
                    {
                        OpenEmojiPanelApplication();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "keyboardPanel")
                    {
                        OpenKeyboardPanelApplication();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "handwritingPanel")
                    {
                        OpenHandwritingPanelApplication();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "voiceInput")
                    {
                        VoiceInput::ToggleRecording();
                        ShowWindow(::global_hwnd_menu, SW_HIDE);
                    }
                    else if (type == "contentTruncated")
                    {
                        if (HandleContentTruncatedMessage(hwnd, webviewMenuWnd.Get(), webviewControllerMenuWnd.Get(),
                                                          val, g_last_content_truncation_menu_ms, 0))
                        {
                            ::MENU_CONTENT_WIDTH_DIP =
                                (std::max)(::MENU_CONTENT_WIDTH_DIP, JsonNumberAsDouble(val.at("data").at("width")));
                            ::MENU_CONTENT_HEIGHT_DIP =
                                (std::max)(::MENU_CONTENT_HEIGHT_DIP, JsonNumberAsDouble(val.at("data").at("height")));
                            const HalfScreenDipLimits limits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
                            ::MENU_CONTENT_WIDTH_DIP = ClampWidthDipToHalfScreen(::MENU_CONTENT_WIDTH_DIP, limits);
                            ::MENU_CONTENT_HEIGHT_DIP = ClampHeightDipToHalfScreen(::MENU_CONTENT_HEIGHT_DIP, limits);
                        }
                    }
                }
                return S_OK;
            })
            .Get(),
        nullptr);

    OnSmallWindowControllerSettled(S_OK);
    return S_OK;
}

/**
 * @brief Handle menu window webview2 environment creation
 *
 * @param hwnd
 * @param result
 * @param env
 * @return HRESULT
 */
HRESULT OnMenuWindowEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env)
{
    if (FAILED(result) || !env)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return result;
    }

    // Create WebView2 controller
    return env->CreateCoreWebView2Controller(                                                //
        hwnd,                                                                                //
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>( //
            [hwnd](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT {         //
                return OnControllerCreatedMenuWnd(hwnd, result, controller);                 //
            })                                                                               //
            .Get()                                                                           //
    );                                                                                       //
}

//
//
// settings 窗口 webview
//
//

HRESULT EnsureCompositionVisualTreeSettingsWnd(HWND hwnd)
{
    if (dcompDeviceSettingsWnd && dcompTargetSettingsWnd && dcompRootVisualSettingsWnd)
    {
        return S_OK;
    }

    HRESULT hr = DCompositionCreateDevice(nullptr, __uuidof(IDCompositionDevice),
                                          reinterpret_cast<void **>(dcompDeviceSettingsWnd.GetAddressOf()));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = dcompDeviceSettingsWnd->CreateTargetForHwnd(hwnd, TRUE, &dcompTargetSettingsWnd);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = dcompDeviceSettingsWnd->CreateVisual(&dcompRootVisualSettingsWnd);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = dcompTargetSettingsWnd->SetRoot(dcompRootVisualSettingsWnd.Get());
    if (FAILED(hr))
    {
        return hr;
    }

    return dcompDeviceSettingsWnd->Commit();
}

/**
 * @brief Handle settings window webview2 controller creation
 *
 * @param hwnd
 * @param result
 * @param controller
 * @return HRESULT
 */
HRESULT OnControllerCreatedSettingsWnd(            //
    HWND hwnd,                                     //
    HRESULT result,                                //
    ICoreWebView2CompositionController *controller //
)
{
    if (!controller || FAILED(result))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return E_FAIL;
    }

    /* 给 controller 和 webview 赋值 */
    webviewCompositionControllerSettingsWnd = controller;
    if (FAILED(webviewCompositionControllerSettingsWnd.As(&webviewControllerSettingsWnd)))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return E_NOINTERFACE;
    }

    webviewControllerSettingsWnd->get_CoreWebView2(webviewSettingsWnd.GetAddressOf());

    if (!webviewSettingsWnd)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return E_FAIL;
    }

    // Configure webviewSettingsWindow settings
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webviewSettingsWnd->get_Settings(&settings)))
    {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(TRUE);
        settings->put_IsZoomControlEnabled(FALSE);
    }

    webviewControllerSettingsWnd->put_ZoomFactor(1.0);

    // Configure virtual host path
    if (SUCCEEDED(webviewSettingsWnd->QueryInterface(IID_PPV_ARGS(&webview3SettingsWnd))))
    {
        const std::wstring assetPath = fmt::format(                  //
            L"{}\\{}\\html\\webview2\\settings\\ime-settings\\dist", //
            CommonUtils::get_local_appdata_path_w(),                 //
            GlobalIme::AppName                                       //
        );
        // Assets mapping
        webview3SettingsWnd->SetVirtualHostNameToFolderMapping( //
            L"imesettings",                                     //
            assetPath.c_str(),                                  //
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW        //
        );                                                      //
    }

    // The settings page is fully opaque. Keeping the composition surface
    // transparent makes DWM briefly expose the host backdrop on input-driven
    // WebView repaints, which looks like the window/taskbar is flashing.
    if (SUCCEEDED(webviewControllerSettingsWnd.As(&webviewController2SettingsWnd)))
    {
        const bool settingsLight = ResolveConfiguredTheme(GetConfiguredThemeSettings()) == "light";
        COREWEBVIEW2_COLOR backgroundColor =
            settingsLight ? COREWEBVIEW2_COLOR{255, 243, 243, 243} : COREWEBVIEW2_COLOR{255, 32, 32, 32};
        webviewController2SettingsWnd->put_DefaultBackgroundColor(backgroundColor);
    }

    const HRESULT compositionResult = EnsureCompositionVisualTreeSettingsWnd(hwnd);
    if (FAILED(compositionResult))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return compositionResult;
    }

    const HRESULT rootVisualResult =
        webviewCompositionControllerSettingsWnd->put_RootVisualTarget(dcompRootVisualSettingsWnd.Get());
    if (FAILED(rootVisualResult))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return rootVisualResult;
    }

    dcompDeviceSettingsWnd->Commit();

    // Adjust to window size
    RECT bounds;
    GetClientRect(hwnd, &bounds);
    webviewControllerSettingsWnd->put_Bounds(bounds);

    // Navigate to HTML
    // HRESULT hr = webviewSettingsWnd->NavigateToString(::HTMLStringSettingsWnd.c_str());
    std::wstring url = L"https://imesettings/index.html";
    HRESULT hr = webviewSettingsWnd->Navigate(url.c_str());
    if (FAILED(hr))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
    }

    EventRegistrationToken navCompletedToken;
    webviewSettingsWnd->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>( //
            [hwnd](ICoreWebView2 * /*sender*/, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL success;
                args->get_IsSuccess(&success);
                if (success)
                {
// 隐藏窗口
#ifdef FANY_DEBUG
                    (void)0;
#endif
                    BOOL cloak = FALSE;
                    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
                }

                PostSettingsWindowState(hwnd);
                PostSettingsConfig();
                return S_OK;
            })
            .Get(),
        &navCompletedToken);

    /* 处理 js 发过来的消息 */
    webviewSettingsWnd->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [hwnd](ICoreWebView2 * /*sender*/, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                HRESULT hr = args->TryGetWebMessageAsString(&message);
                if (SUCCEEDED(hr) && message.get())
                {
                    std::wstring msg(message.get());
                    // 解析 msg，执行相应操作
                    json::value val = json::parse(wstring_to_string(msg));
                    if (!metasequoia::webview::Validate(val, "client", "settings"))
                        return S_OK;
                    std::string type = json::value_to<std::string>(val.at("type"));
                    /* 使 settings 窗口可拖动 */
                    if (type == "dragStart")
                    {
                        ReleaseCapture();
                        PostMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    }
                    else if (type == "resizeHitTest" || type == "resizeStart")
                    {
                        std::string hit = json::value_to<std::string>(val.at("data"));
                        int hitTest = HTCLIENT;
                        if (hit == "left")
                        {
                            hitTest = HTLEFT;
                        }
                        else if (hit == "right")
                        {
                            hitTest = HTRIGHT;
                        }
                        else if (hit == "top")
                        {
                            hitTest = HTTOP;
                        }
                        else if (hit == "bottom")
                        {
                            hitTest = HTBOTTOM;
                        }
                        else if (hit == "left-top")
                        {
                            hitTest = HTTOPLEFT;
                        }
                        else if (hit == "right-top")
                        {
                            hitTest = HTTOPRIGHT;
                        }
                        else if (hit == "left-bottom")
                        {
                            hitTest = HTBOTTOMLEFT;
                        }
                        else if (hit == "right-bottom")
                        {
                            hitTest = HTBOTTOMRIGHT;
                        }
                        if (hitTest != HTCLIENT)
                        {
                            ReleaseCapture();
                            PostMessage(hwnd, WM_NCLBUTTONDOWN, hitTest, 0);
                        }
                    }
                    else if (type == "focus")
                    {
                        SetFocus(hwnd);
                    }
                    else if (type == "windowControl")
                    {
                        std::string value = json::value_to<std::string>(val.at("data"));
                        if (value == "minimize")
                        {
                            ShowWindow(hwnd, SW_MINIMIZE);
                        }
                        else if (value == "maximize")
                        {
                            ShowWindow(hwnd, SW_MAXIMIZE);
                        }
                        else if (value == "close")
                        {
                            ShowWindow(hwnd, SW_HIDE);
                        }
                        else if (value == "restore")
                        {
                            ShowWindow(hwnd, SW_RESTORE);
                        }
                    }
                    else if (type == "maximizeButtonRect")
                    {
                        try
                        {
                            auto &data = val.at("data").as_object();
                            double x = data.if_contains("x") ? json::value_to<double>(*data.if_contains("x")) : 0.0;
                            double y = data.if_contains("y") ? json::value_to<double>(*data.if_contains("y")) : 0.0;
                            double width =
                                data.if_contains("width") ? json::value_to<double>(*data.if_contains("width")) : 0.0;
                            double height =
                                data.if_contains("height") ? json::value_to<double>(*data.if_contains("height")) : 0.0;
                            double scale =
                                data.if_contains("dpr") ? json::value_to<double>(*data.if_contains("dpr")) : 0.0;

                            if (scale <= 0.0)
                            {
                                scale = static_cast<double>(GetDpiForWindow(hwnd)) / 96.0;
                            }

                            const int left = static_cast<int>(std::lround(x * scale));
                            const int top = static_cast<int>(std::lround(y * scale));
                            const int right = static_cast<int>(std::lround((x + width) * scale));
                            const int bottom = static_cast<int>(std::lround((y + height) * scale));

                            maximizeButtonRectSettingsWnd = {left, top, right, bottom};
                            hasMaximizeButtonRectSettingsWnd = true;
                        }
                        catch (const std::exception &)
                        {
                        }
                    }
                    else if (type == "configRequest")
                    {
                        PostSettingsConfig();
                    }
                    else if (type == "configUpdate")
                    {
                        try
                        {
                            const auto &data = val.at("data").as_object();
                            const std::string path = json::value_to<std::string>(data.at("path"));
                            if (path == "input.mode")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredInputMode(value))
                                {
                                    ApplyConfiguredInputScheme();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.schema")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredInputScheme(value))
                                {
                                    ApplyConfiguredInputScheme();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.character_set")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredCharacterSet(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.default_ime_mode")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredDefaultImeMode(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.ime_mode_scope")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredImeModeScope(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.shuangpin_schema")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredShuangpinSchema(value))
                                {
                                    ApplyConfiguredShuangpinSchema();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.wubi_schema")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredWubiSchema(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.tsf_preedit_style")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredTsfPreeditStyle(value))
                                {
                                    BroadcastToTsfWorkerThreadViaNamedpipe(
                                        Global::DataFromServerMsgTypeToTsfWorkerThread::PagingCommaPeriodChanged,
                                        FormatPagingCommaPeriodWorkerPayload());
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.ui_backend")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredUiBackend(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.candidate_window_layout")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredCandidateWindowLayout(value))
                                {
                                    ApplyConfiguredCandidateWindowLayout();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.candidate_window_follow_cursor")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredCandidateWindowFollowCursor(value))
                                {
                                    CAND_WEBVIEW_TRACE_LOGF(L"candidate-position config-update follow_cursor={}",
                                                            value);
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.candidate_skin")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredCandidateSkin(value))
                                {
                                    ApplyConfiguredUiThemes();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.candidate_window_preedit_style")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredCandidateWindowPreeditStyle(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.page_size")
                            {
                                const int value = static_cast<int>(data.at("value").as_int64());
                                if (SetConfiguredCandidatePageSize(value))
                                {
                                    FanyNamedPipe::EnqueueApplyCandidatePageSizeTask();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.font")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredCandidateFont(value))
                                {
                                    ApplyConfiguredCandidateAppearance();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.fallback_fonts")
                            {
                                // configUpdate accepts scalar values; structured settings use a JSON string.
                                const auto fonts = nlohmann::json::parse(json::value_to<std::string>(data.at("value")))
                                                       .get<std::vector<std::string>>();
                                if (SetConfiguredCandidateFallbackFonts(fonts))
                                {
                                    ApplyConfiguredCandidateAppearance();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.english_font")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredCandidateEnglishFont(value))
                                {
                                    ApplyConfiguredCandidateAppearance();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.font_size")
                            {
                                const int value = static_cast<int>(data.at("value").as_int64());
                                if (SetConfiguredCandidateFontSize(value))
                                {
                                    ApplyConfiguredCandidateAppearance();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.candidate_window_preedit_font_size")
                            {
                                const int value = static_cast<int>(data.at("value").as_int64());
                                if (SetConfiguredCandidateWindowPreeditFontSize(value))
                                {
                                    ApplyConfiguredCandidateAppearance();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.cand_text_color")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredCandidateTextColor(value))
                                {
                                    ApplyConfiguredCandidateAppearance();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.theme_mode")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredThemeMode(value))
                                {
                                    ApplyConfiguredUiThemes();
                                    if (webviewController2SettingsWnd)
                                    {
                                        const bool settingsLight =
                                            ResolveConfiguredTheme(GetConfiguredThemeSettings()) == "light";
                                        COREWEBVIEW2_COLOR backgroundColor =
                                            settingsLight ? COREWEBVIEW2_COLOR{255, 243, 243, 243}
                                                          : COREWEBVIEW2_COLOR{255, 32, 32, 32};
                                        webviewController2SettingsWnd->put_DefaultBackgroundColor(backgroundColor);
                                    }
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.theme_settings")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredThemeSettings(value))
                                {
                                    if (webviewController2SettingsWnd)
                                    {
                                        const bool settingsLight =
                                            ResolveConfiguredTheme(GetConfiguredThemeSettings()) == "light";
                                        COREWEBVIEW2_COLOR backgroundColor =
                                            settingsLight ? COREWEBVIEW2_COLOR{255, 243, 243, 243}
                                                          : COREWEBVIEW2_COLOR{255, 32, 32, 32};
                                        webviewController2SettingsWnd->put_DefaultBackgroundColor(backgroundColor);
                                    }
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.theme_cand")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredThemeCand(value))
                                {
                                    ApplyConfiguredUiThemes();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.theme_ftb")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredThemeFtb(value))
                                {
                                    ApplyConfiguredUiThemes();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.theme_menu")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredThemeMenu(value))
                                {
                                    ApplyConfiguredUiThemes();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.theme_emoji")
                            {
                                if (SetConfiguredThemeEmoji(json::value_to<std::string>(data.at("value"))))
                                    PostSettingsConfig();
                            }
                            else if (path == "appearance.theme_screen_keyboard")
                            {
                                if (SetConfiguredThemeScreenKeyboard(json::value_to<std::string>(data.at("value"))))
                                    PostSettingsConfig();
                            }
                            else if (path == "appearance.theme_handwriting")
                            {
                                if (SetConfiguredThemeHandwriting(json::value_to<std::string>(data.at("value"))))
                                    PostSettingsConfig();
                            }
                            else if (path == "appearance.theme_voice")
                            {
                                if (SetConfiguredThemeVoice(json::value_to<std::string>(data.at("value"))))
                                    PostSettingsConfig();
                            }
                            else if (path == "general.floating_toolbar")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredFloatingToolbarEnabled(value))
                                {
                                    ApplyConfiguredFloatingToolbarVisibility(L"settings-toggle");
                                    SyncMenuFloatingToolbarToggle();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.floating_toolbar_scale")
                            {
                                const double value = data.at("value").is_double()
                                                         ? data.at("value").as_double()
                                                         : static_cast<double>(data.at("value").as_int64());
                                if (SetConfiguredFloatingToolbarScale(value))
                                {
                                    ApplyConfiguredFloatingToolbarSize();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.floating_toolbar_font_size")
                            {
                                if (SetConfiguredFloatingToolbarFontSize(static_cast<int>(data.at("value").as_int64())))
                                {
                                    ApplyConfiguredFloatingToolbarSize();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.word_to_character")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                SetConfiguredWordToCharacterEnabled(value);
                                PostSettingsConfig();
                            }
                            else if (path == "input.word_to_character_keys")
                            {
                                SetConfiguredWordToCharacterKeys(json::value_to<std::string>(data.at("value")));
                                PostSettingsConfig();
                            }
                            else if (path == "input.smart_punctuation")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredSmartPunctuationEnabled(value))
                                {
                                    BroadcastToTsfWorkerThreadViaNamedpipe(
                                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationChanged,
                                        value ? L"1" : L"0");
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.smart_punctuation_repeat_to_chinese")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredSmartPunctuationRepeatToChineseEnabled(value))
                                {
                                    BroadcastToTsfWorkerThreadViaNamedpipe(
                                        Global::DataFromServerMsgTypeToTsfWorkerThread::
                                            SmartPunctuationRepeatToChineseChanged,
                                        value ? L"1" : L"0");
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.paired_punctuation")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredPairedPunctuationEnabled(value))
                                {
                                    BroadcastToTsfWorkerThreadViaNamedpipe(
                                        Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged,
                                        value ? L"1" : L"0");
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.punctuation_lock")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredPunctuationLock(value))
                                {
                                    BroadcastToTsfWorkerThreadViaNamedpipe(
                                        Global::DataFromServerMsgTypeToTsfWorkerThread::PunctuationLockChanged,
                                        FormatPunctuationLockWorkerPayload());
                                    if (value == "chinese")
                                    {
                                        UpdateFtbPuncState(::webviewFtbWnd, 1);
                                    }
                                    else if (value == "english")
                                    {
                                        UpdateFtbPuncState(::webviewFtbWnd, 0);
                                    }
                                    PostSettingsConfig();
                                }
                            }
                            else if (path.rfind("general.floating_toolbar_", 0) == 0)
                            {
                                const std::string item = path.substr(std::string("general.floating_toolbar_").size());
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredFloatingToolbarItemEnabled(item, value))
                                {
                                    ApplyConfiguredFloatingToolbarItems();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.cn_en_mixed_input")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredEnglishCandidatesEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.japanese_schema")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredJapaneseSchema(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.candidate_translations")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredCandidateTranslationsEnabled(value))
                                {
                                    FanyNamedPipe::EnqueueRefreshCandidatePageTask();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.diagnostic_log" ||
                                     path == "general.candidate_window_diagnostic_log")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredDiagnosticLogEnabled(value))
                                {
                                    CAND_DIAG_LOGF(L"diagnostic logging enabled from Settings");
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.tsf_diagnostic_log")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredTsfDiagnosticLogEnabled(value))
                                {
                                    BroadcastToTsfWorkerThreadViaNamedpipe(
                                        Global::DataFromServerMsgTypeToTsfWorkerThread::TsfDiagnosticLogChanged,
                                        value ? L"1" : L"0");
                                    PostSettingsConfig();
                                }
                            }
                            else if (path.rfind("tencent_tmt.", 0) == 0)
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredTencentTmtString(path.substr(std::string("tencent_tmt.").size()),
                                                                  value))
                                {
                                    if (path == "tencent_tmt.target_language")
                                        FanyNamedPipe::EnqueueRefreshCandidatePageTask();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "custom_translation.enabled")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredCustomTranslationBool("enabled", value))
                                {
                                    FanyNamedPipe::EnqueueRefreshCandidatePageTask();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path.rfind("custom_translation.", 0) == 0)
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredCustomTranslationString(
                                        path.substr(std::string("custom_translation.").size()), value))
                                {
                                    FanyNamedPipe::EnqueueRefreshCandidatePageTask();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.cn_en_mixed_input_min_chars")
                            {
                                const int value = static_cast<int>(data.at("value").as_int64());
                                if (SetConfiguredEnglishMixedInputMinChars(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.emoji_mixed_input")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredEmojiMixedInputEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.kaomoji_mixed_input")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredKaomojiMixedInputEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.cloud_candidates")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredCloudCandidatesEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "utility.unicode_mode")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredUnicodeModeEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "utility.quick_phrase")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredQuickPhraseEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "utility.date_time_mode")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredDateTimeModeEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "utility.emoji_mode")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredEmojiModeEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "utility.kaomoji_mode")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredKaomojiModeEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "utility.jianpin_mode")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredJianpinModeEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "utility.y_mode")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredYModeEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "utility.number_mode")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredNumberModeEnabled(value))
                                {
                                    BroadcastLowercaseNumberModeToTsf();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "utility.r_mode")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredRModeEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "utility.clipboard_history")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredClipboardHistoryEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.paging_minus_equal")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                SetConfiguredPagingMinusEqualEnabled(value);
                                PostSettingsConfig();
                            }
                            else if (path == "general.paging_tab")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredPagingTabEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.paging_comma_period")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredPagingCommaPeriodEnabled(value))
                                {
                                    BroadcastToTsfWorkerThreadViaNamedpipe(
                                        Global::DataFromServerMsgTypeToTsfWorkerThread::PagingCommaPeriodChanged,
                                        FormatPagingCommaPeriodWorkerPayload());
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.paging_brackets")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                SetConfiguredPagingBracketsEnabled(value);
                                PostSettingsConfig();
                            }
                            else if (path == "general.paging_page_up_down")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredPagingPageUpDownEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.candidate_arrow_navigation")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredCandidateArrowNavigationEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "keybindings.toggle_character_set_ctrl_shift_f")
                            {
                                SetConfiguredCharacterSetShortcutEnabled(json::value_to<bool>(data.at("value")));
                                PostSettingsConfig();
                            }
                            else if (path == "keybindings.switch_language_shift")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredSwitchLanguageShiftEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "keybindings.switch_language_ctrl")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredSwitchLanguageCtrlEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "keybindings.switch_language_ctrl_alt_space")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredSwitchLanguageCtrlAltSpaceEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "helpcode.show_sp_helpcode_in_candidate_window")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredShowShuangpinHelpcodeInCandidateWindow(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "helpcode.shuangpin_helpcode")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredShuangpinHelpcodeEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "helpcode.shuangpin_helpcode_schema")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredShuangpinHelpcodeSchema(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "helpcode.quanpin_helpcode")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredQuanpinHelpcodeEnabled(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "helpcode.quanpin_helpcode_schema")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredQuanpinHelpcodeSchema(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "helpcode.show_qp_helpcode_in_candidate_window")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredShowQuanpinHelpcodeInCandidateWindow(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                        }
                        catch (const std::exception &)
                        {
                        }
                    }
                }
                return S_OK;
            })
            .Get(),
        nullptr);

    /* Debug console */
    // webviewSettingsWindow->OpenDevToolsWindow();

    return S_OK;
}

/**
 * @brief Handle settings window webview2 environment creation
 *
 * @param hwnd
 * @param result
 * @param env
 * @return HRESULT
 */
HRESULT OnSettingsWindowEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env)
{
    if (FAILED(result) || !env)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return result;
    }

    ComPtr<ICoreWebView2Environment3> env3;
    if (FAILED(env->QueryInterface(IID_PPV_ARGS(&env3))) || !env3)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return E_NOINTERFACE;
    }

    // Create WebView2 controller
    return env3->CreateCoreWebView2CompositionController(                                               //
        hwnd,                                                                                           //
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>( //
            [hwnd](HRESULT result, ICoreWebView2CompositionController *controller) -> HRESULT {         //
                return OnControllerCreatedSettingsWnd(hwnd, result, controller);                        //
            })                                                                                          //
            .Get()                                                                                      //
    );                                                                                                  //
}

/**
 * @brief Keep tray-menu floating-toolbar toggle aligned with config.toml.
 */
void SyncMenuFloatingToolbarToggle()
{
    if (TrayMenuPresenter::Instance().IsBound())
    {
        if (TrayMenuPresenter::Instance().IsOpenToUser())
        {
            TrayMenuPresenter::Instance().ShowFromLangBar();
        }
        else
        {
            TrayMenuPresenter::Instance().ApplyTheme();
        }
        return;
    }
    if (!::webviewMenuWnd)
    {
        return;
    }

    // Keep the tray-menu toggle aligned with config.toml so Settings and tray
    // never drift apart when either side writes general.floating_toolbar.
    const wchar_t *script = GetConfiguredFloatingToolbarEnabled() ? LR"((() => {
                                      const toggle = document.getElementById('floatingToggle');
                                      if (toggle) toggle.classList.add('active');
                                  })())"
                                                                  : LR"((() => {
                                      const toggle = document.getElementById('floatingToggle');
                                      if (toggle) toggle.classList.remove('active');
                                  })())";
    ::webviewMenuWnd->ExecuteScript(script, nullptr);
}

/**
 * @brief Post the window state of the settings window, 即，是否最大化了，供 settings 窗口的 js 进行相应的调整
 *
 * @param hwnd
 */
void PostSettingsWindowState(HWND hwnd)
{
    if (!::webviewSettingsWnd)
    {
        return;
    }

    nlohmann::json payload = {{"type", "windowState"}, {"data", {{"isMaximized", IsZoomed(hwnd) != FALSE}}}};

    const std::wstring message = string_to_wstring(payload.dump());
    ::webviewSettingsWnd->PostWebMessageAsJson(message.c_str());
}

void PostSettingsConfig()
{
    if (!::webviewSettingsWnd)
    {
        return;
    }

    const FloatingToolbarItemsConfig &toolbar = GetConfiguredFloatingToolbarItems();
    const TencentTmtConfig &tencent_tmt = GetConfiguredTencentTmt();
    const CustomTranslationConfig &custom_translation = GetConfiguredCustomTranslation();
    nlohmann::json payload = {
        {"type", "configSnapshot"},
        {"data",
         {{"input",
           {{"mode", GetConfiguredInputMode()},
            {"schema", GetConfiguredInputSchemeName()},
            {"japanese_schema", GetConfiguredJapaneseSchema()},
            {"character_set", GetConfiguredCharacterSet()},
            {"default_ime_mode", GetConfiguredDefaultImeMode()},
            {"ime_mode_scope", GetConfiguredImeModeScope()},
            {"shuangpin_schema", GetConfiguredShuangpinSchema()},
            {"wubi_schema", GetConfiguredWubiSchema()},
            {"word_to_character", GetConfiguredWordToCharacterEnabled()},
            {"word_to_character_keys", GetConfiguredWordToCharacterKeys()},
            {"smart_punctuation", GetConfiguredSmartPunctuationEnabled()},
            {"smart_punctuation_repeat_to_chinese", GetConfiguredSmartPunctuationRepeatToChineseEnabled()},
            {"paired_punctuation", GetConfiguredPairedPunctuationEnabled()},
            {"punctuation_lock", GetConfiguredPunctuationLock()}}},
          {"general",
           {{"diagnostic_log", GetConfiguredDiagnosticLogEnabled()},
            {"candidate_window_diagnostic_log", GetConfiguredDiagnosticLogEnabled()},
            {"tsf_diagnostic_log", GetConfiguredTsfDiagnosticLogEnabled()},
            {"floating_toolbar", GetConfiguredFloatingToolbarEnabled()},
            {"floating_toolbar_fullwidth", toolbar.fullwidth},
            {"floating_toolbar_punctuation", toolbar.punctuation},
            {"floating_toolbar_character_set", toolbar.character_set},
            {"floating_toolbar_emoji", toolbar.emoji},
            {"floating_toolbar_screen_keyboard", toolbar.screen_keyboard},
            {"floating_toolbar_settings", toolbar.settings},
            {"floating_toolbar_scale", GetConfiguredFloatingToolbarScale()},
            {"floating_toolbar_font_size", GetConfiguredFloatingToolbarFontSize()},
            {"cn_en_mixed_input", GetConfiguredEnglishCandidatesEnabled()},
            {"candidate_translations", GetConfiguredCandidateTranslationsEnabled()},
            {"cn_en_mixed_input_min_chars", GetConfiguredEnglishMixedInputMinChars()},
            {"emoji_mixed_input", GetConfiguredEmojiMixedInputEnabled()},
            {"kaomoji_mixed_input", GetConfiguredKaomojiMixedInputEnabled()},
            {"cloud_candidates", GetConfiguredCloudCandidatesEnabled()},
            {"paging_minus_equal", GetConfiguredPagingMinusEqualEnabled()},
            {"paging_comma_period", GetConfiguredPagingCommaPeriodEnabled()},
            {"paging_brackets", GetConfiguredPagingBracketsEnabled()},
            {"paging_tab", GetConfiguredPagingTabEnabled()},
            {"paging_page_up_down", GetConfiguredPagingPageUpDownEnabled()},
            {"candidate_arrow_navigation", GetConfiguredCandidateArrowNavigationEnabled()}}},
          {"keybindings",
           {{"switch_language_shift", GetConfiguredSwitchLanguageShiftEnabled()},
            {"switch_language_ctrl", GetConfiguredSwitchLanguageCtrlEnabled()},
            {"switch_language_ctrl_alt_space", GetConfiguredSwitchLanguageCtrlAltSpaceEnabled()},
            {"toggle_character_set_ctrl_shift_f", GetConfiguredCharacterSetShortcutEnabled()}}},
          {"tencent_tmt",
           {{"secret_id", tencent_tmt.secret_id},
            {"secret_key", tencent_tmt.secret_key},
            {"region", tencent_tmt.region},
            {"target_language", tencent_tmt.target_language}}},
          {"custom_translation",
           {{"enabled", custom_translation.enabled},
            {"endpoint", custom_translation.endpoint},
            {"api_key", custom_translation.api_key}}},
          {"utility",
           {{"unicode_mode", GetConfiguredUnicodeModeEnabled()},
            {"quick_phrase", GetConfiguredQuickPhraseEnabled()},
            {"date_time_mode", GetConfiguredDateTimeModeEnabled()},
            {"emoji_mode", GetConfiguredEmojiModeEnabled()},
            {"kaomoji_mode", GetConfiguredKaomojiModeEnabled()},
            {"jianpin_mode", GetConfiguredJianpinModeEnabled()},
            {"y_mode", GetConfiguredYModeEnabled()},
            {"number_mode", GetConfiguredNumberModeEnabled()},
            {"r_mode", GetConfiguredRModeEnabled()}}},
          {"appearance",
           {{"ui_backend", GetConfiguredUiBackend()},
            {"candidate_window_layout", GetConfiguredCandidateWindowLayout()},
            {"candidate_window_follow_cursor", GetConfiguredCandidateWindowFollowCursor()},
            {"candidate_skin", GetConfiguredCandidateSkin()},
            {"candidate_window_preedit_style", GetConfiguredCandidateWindowPreeditStyle()},
            {"tsf_preedit_style", GetConfiguredTsfPreeditStyle()},
            {"theme_mode", GetConfiguredThemeMode()},
            {"theme_settings", GetConfiguredThemeSettings()},
            {"theme_cand", GetConfiguredThemeCand()},
            {"theme_ftb", GetConfiguredThemeFtb()},
            {"theme_menu", GetConfiguredThemeMenu()},
            {"theme_emoji", GetConfiguredThemeEmoji()},
            {"theme_screen_keyboard", GetConfiguredThemeScreenKeyboard()},
            {"theme_handwriting", GetConfiguredThemeHandwriting()},
            {"theme_voice", GetConfiguredThemeVoice()},
            {"page_size", GetConfiguredCandidatePageSize()},
            {"font", GetConfiguredCandidateFont()},
            {"font_css_family", ResolveSystemFontFamilyForCss(GetConfiguredCandidateFont())},
            {"fallback_fonts", GetConfiguredCandidateFallbackFonts()},
            {"fallback_font_css_families", GetConfiguredCandidateFallbackFontFamilies()},
            {"english_font", GetConfiguredCandidateEnglishFont()},
            {"english_font_css_family", ResolveSystemFontFamilyForCss(GetConfiguredCandidateEnglishFont())},
            {"default_font", GetConfiguredCandidateDefaultFont()},
            {"default_font_css_family", ResolveSystemFontFamilyForCss(GetConfiguredCandidateDefaultFont())},
            {"font_size", GetConfiguredCandidateFontSize()},
            {"candidate_window_preedit_font_size", GetConfiguredCandidateWindowPreeditFontSize()},
            {"cand_text_color", GetConfiguredCandidateTextColor()},
            {"system_fonts", GetSystemFontFamilies()}}},
          {"helpcode",
           {{"shuangpin_helpcode", GetConfiguredShuangpinHelpcodeEnabled()},
            {"shuangpin_helpcode_schema", GetConfiguredShuangpinHelpcodeSchema()},
            {"quanpin_helpcode", GetConfiguredQuanpinHelpcodeEnabled()},
            {"quanpin_helpcode_schema", GetConfiguredQuanpinHelpcodeSchema()},
            {"show_sp_helpcode_in_candidate_window", GetConfiguredShowShuangpinHelpcodeInCandidateWindow()},
            {"show_qp_helpcode_in_candidate_window", GetConfiguredShowQuanpinHelpcodeInCandidateWindow()}}}}}};
    const std::wstring message = string_to_wstring(payload.dump());
    ::webviewSettingsWnd->PostWebMessageAsJson(message.c_str());
}

/**
 * @brief 初始化 settings 窗口的 webview
 *
 * @param hwnd
 */
void InitWebviewSettingsWnd(HWND hwnd)
{
    std::wstring appDataPath = GetAppdataPath();
    CreateCoreWebView2EnvironmentWithOptions(                                                 //
        nullptr,                                                                              //
        appDataPath.c_str(),                                                                  //
        nullptr,                                                                              //
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>( //
            [hwnd](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {                //
                return OnSettingsWindowEnvironmentCreated(hwnd, result, env);                 //
            })                                                                                //
            .Get()                                                                            //
    );                                                                                        //
}

//
//
// floating toolbar(ftb) 窗口 webview
//
//

/**
 * @brief Handle floating toolbar window webview2 controller creation
 *
 * @param hwnd
 * @param result
 * @param controller
 * @return HRESULT
 */
HRESULT OnControllerCreatedFtbWnd(      //
    HWND hwnd,                          //
    HRESULT result,                     //
    ICoreWebView2Controller *controller //
)
{
    if (!controller || FAILED(result))
    {
        OnSmallWindowControllerSettled(FAILED(result) ? result : E_FAIL);
        return E_FAIL;
    }

    /* 给 controller 和 webview 赋值 */
    webviewControllerFtbWnd = controller;
    const HRESULT getFtbWebviewHr = webviewControllerFtbWnd->get_CoreWebView2(webviewFtbWnd.GetAddressOf());

    if (!webviewFtbWnd)
    {
        webviewControllerFtbWnd.Reset();
        OnSmallWindowControllerSettled(FAILED(getFtbWebviewHr) ? getFtbWebviewHr : E_FAIL);
        return E_FAIL;
    }

    // WebView2 only completes raster setup against an on-monitor visible host.
    // If this ever reports host_visible=false, the toolbar for this session will
    // stay blank no matter how often it is later shown. Cloaked is the healthy
    // warmup state: visible to WebView2, invisible to the user.
    DWORD hostCloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &hostCloaked, sizeof(hostCloaked));
    FTB_DIAG_LOGF(L"ftb controller created host_visible={} host_cloaked={}", IsWindowVisible(hwnd) != FALSE,
                  hostCloaked != 0);
    UpdateSmallWindowWebviewVisibility(hwnd, IsWindowVisible(hwnd) != FALSE);

    // Configure webviewFtbWindow settings
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webviewFtbWnd->get_Settings(&settings)))
    {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(FALSE);
        // 禁用右键菜单和开发者工具
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        // 禁止界面缩放
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);

        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3)))
        {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }

        ComPtr<ICoreWebView2Settings5> settings5;
        if (SUCCEEDED(settings.As(&settings5)))
        {
            settings5->put_IsGeneralAutofillEnabled(FALSE);
            settings5->put_IsPasswordAutosaveEnabled(FALSE);
        }
    }

    // 初始时缩放设置成 1.0
    webviewControllerFtbWnd->put_ZoomFactor(1.0);

    // Configure virtual host path
    if (SUCCEEDED(webviewFtbWnd->QueryInterface(IID_PPV_ARGS(&webview3FtbWnd))))
    {
        const auto contractsPath = std::filesystem::path(CommonUtils::get_local_appdata_path_w()) / GlobalIme::AppName /
                                   L"html" / L"webview2" / L"shared";
        webview3FtbWnd->SetVirtualHostNameToFolderMapping(L"msime-contracts", contractsPath.wstring().c_str(),
                                                          COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

        // Assets mapping
        webview3FtbWnd->SetVirtualHostNameToFolderMapping(   //
            L"appassets",                                    //
            GetLocalAssetsPath().c_str(),                    //
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS //
        );                                                   //
    }

    // Set transparent background
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&webviewController2FtbWnd))))
    {
        COREWEBVIEW2_COLOR backgroundColor = {0, 0, 0, 0};
        webviewController2FtbWnd->put_DefaultBackgroundColor(backgroundColor);
    }

    // Adjust to window size
    RECT bounds;
    GetClientRect(hwnd, &bounds);
    webviewControllerFtbWnd->put_Bounds(bounds);

    // State notifications can arrive before the controller exists or while
    // NavigateToString is still loading. Only render after a successful
    // navigation, then replay the complete cached state in one operation.
    ClearFloatingToolbarNavigationState();
    EventRegistrationToken navigationCompletedToken{};
    const HRESULT navigationCompletedResult = webviewFtbWnd->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                BOOL success = FALSE;
                if (args)
                {
                    args->get_IsSuccess(&success);
                }
                if (success)
                {
                    NotifySmallWindowNavigationReady(floatingToolbarNavigationReady, L"floating-toolbar");
                    ApplyConfiguredFloatingToolbarAppearance();
                    floatingToolbarState.japanese_input_mode = GetConfiguredInputMode() == "japanese" ? 1 : 0;
                    RenderFloatingToolbarState(sender);
                    ApplyConfiguredFloatingToolbarSize();
                    InjectSurfaceViewportLimits(sender, ::global_hwnd_ftb);
                    // Show first so a cold user-data folder can finish painting;
                    // a short timer then hides or keeps it based on real state.
                    ApplyConfiguredFloatingToolbarVisibility(L"ftb-navigation-completed");
                }
                else
                {
                    // Cold user-data bring-up can fail the first NavigateToString.
                    // Retry once against the same controller instead of leaving
                    // the host permanently cloaked with webview_ready=false.
                    if (!floatingToolbarNavigationRetryUsed && !::HTMLStringFtbWnd.empty())
                    {
                        floatingToolbarNavigationRetryUsed = true;
                        floatingToolbarNavigationReady = false;
                        floatingToolbarPaintGraceActive = false;
                        FTB_DIAG_LOGF(L"ftb navigation failed; retrying NavigateToString once");
                        sender->NavigateToString(::HTMLStringFtbWnd.c_str());
                    }
                    else
                    {
                        FTB_DIAG_LOGF(L"ftb navigation failed permanently after retry");
                    }
                }
                return S_OK;
            })
            .Get(),
        &navigationCompletedToken);
    if (FAILED(navigationCompletedResult))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return navigationCompletedResult;
    }

    // Navigate to HTML
    HRESULT hr = webviewFtbWnd->NavigateToString(::HTMLStringFtbWnd.c_str());
    if (SUCCEEDED(hr))
    {
        loadedFloatingToolbarSkin = preparedCandidateSkin;
    }
    if (FAILED(hr))
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return hr;
    }

    /* Debug console */
    // webviewFtbWindow->OpenDevToolsWindow();

    /* 处理 js 发过来的消息 */
    webviewFtbWnd->add_WebMessageReceived(
        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [hwnd](ICoreWebView2 * /*sender*/, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                HRESULT hr = args->TryGetWebMessageAsString(&message);
                if (SUCCEEDED(hr) && message.get())
                {
                    std::wstring msg(message.get());
                    // 解析 msg，执行相应操作
                    json::value val = json::parse(wstring_to_string(msg));
                    if (!metasequoia::webview::Validate(val, "client", "toolbar"))
                        return S_OK;
                    std::string type = json::value_to<std::string>(val.at("type"));
                    /* 使 floating toolbar 窗口可拖动 */
                    if (type == "dragStart")
                    {
                        ReleaseCapture();
                        PostMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                    }
                    else if (type == "ready")
                    {
                        NotifyFloatingToolbarPageReady();
                    }
                    else if (type == "changeIMEMode")
                    {
                        std::string mode = json::value_to<std::string>(val.at("data"));

                        if (mode == "cn") // Change to CN
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToCn, L"");
                        }
                        else if (mode == "en") // Change to EN
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToEn, L"");
                        }
                    }
                    else if (type == "exitEnglishInputMode")
                    {
                        FanyNamedPipe::EnqueueExitEnglishInputModeTask();
                    }
                    else if (type == "changeCharMode")
                    {
                        std::string mode = json::value_to<std::string>(val.at("data"));
                        if (mode == "fullwidth")
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToFullwidth, L"");
                        }
                        else if (mode == "halfwidth")
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToHalfwidth, L"");
                        }
                    }
                    else if (type == "changePuncMode")
                    {
                        std::string mode = json::value_to<std::string>(val.at("data"));
                        if (mode == "puncEn")
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToPuncEn, L"");
                        }
                        else if (mode == "puncCn")
                        {
#ifdef FANY_DEBUG
                            (void)0;
#endif
                            SendToTsfWorkerThreadViaNamedpipe(
                                Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToPuncCn, L"");
                        }
                    }
                    else if (type == "changeCharacterSet")
                    {
                        const std::string next =
                            GetConfiguredCharacterSet() == "traditional" ? "simplified" : "traditional";
                        if (SetConfiguredCharacterSet(next))
                        {
                            PostSettingsConfig();
                        }
                    }
                    else if (type == "openSettings")
                    {
#ifdef FANY_DEBUG
                        (void)0;
#endif
                        OpenSettingsApplication();
                    }
                    else if (type == "openEmojiPanel")
                    {
#ifdef FANY_DEBUG
                        (void)0;
#endif
                        OpenEmojiPanelApplication();
                    }
                    else if (type == "openKeyboardPanel")
                    {
                        OpenKeyboardPanelApplication();
                    }
                    else if (type == "contentTruncated")
                    {
                        if (HandleContentTruncatedMessage(hwnd, webviewFtbWnd.Get(), webviewControllerFtbWnd.Get(), val,
                                                          g_last_content_truncation_ftb_ms, ::FTB_WND_SHADOW_WIDTH))
                        {
                            const double widthDip = JsonNumberAsDouble(val.at("data").at("width"));
                            const double heightDip = JsonNumberAsDouble(val.at("data").at("height"));
                            const HalfScreenDipLimits limits = QueryWebViewHalfScreenDipLimitsForHwnd(hwnd);
                            ::FTB_CONTENT_WIDTH_DIP = ClampWidthDipToHalfScreen(widthDip, limits);
                            ::FTB_CONTENT_HEIGHT_DIP = ClampHeightDipToHalfScreen(heightDip, limits);
                            ::FTB_WND_WIDTH =
                                static_cast<int>(std::ceil(::FTB_CONTENT_WIDTH_DIP * kTruncationSizeFactor));
                            ::FTB_WND_HEIGHT =
                                static_cast<int>(std::ceil(::FTB_CONTENT_HEIGHT_DIP * kTruncationSizeFactor));
                        }
                    }
                }

                return S_OK;
            })
            .Get(),
        nullptr);

    OnSmallWindowControllerSettled(S_OK);
    return S_OK;
}

/**
 * @brief Handle floating toolbar window webview2 environment creation
 *
 * @param hwnd
 * @param result
 * @param env
 * @return HRESULT
 */
HRESULT OnFtbWindowEnvironmentCreated(HWND hwnd, HRESULT result, ICoreWebView2Environment *env)
{
    if (FAILED(result) || !env)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return result;
    }

    // Create WebView2 controller
    return env->CreateCoreWebView2Controller(                                        //
        hwnd,                                                                        //
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(         //
            [hwnd](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT { //
                return OnControllerCreatedFtbWnd(hwnd, result, controller);          //
            })                                                                       //
            .Get()                                                                   //
    );                                                                               //
}

/**
 * @brief Initialize candidate, tray menu, and floating toolbar WebViews in one
 *        environment when appearance.ui_backend is webview2.
 */
void InitSmallWindowWebviews(HWND candHwnd, HWND menuHwnd, HWND ftbHwnd)
{
    smallWindowCandHwnd = candHwnd;
    smallWindowMenuHwnd = menuHwnd;
    smallWindowFtbHwnd = ftbHwnd;
    smallWindowInitAttempts = 0;
    smallWindowInitState = SmallWindowInitState::Idle;
    smallWindowControllerRequestInFlight = false;
    currentSmallWindowHostIndex = -1;
    lastFailedSmallWindowHostIndex = -1;
    pendingTrayMenuShow = false;
    pendingCandidateShow = false;
    if (UseD2dSmallWindowUi())
    {
        FTB_DIAG_LOGF(L"skip small-window webview init: ui_backend=d2d");
        return;
    }
    BeginSmallWindowWebviewEnvironmentCreate();
}

bool PrepareTrayMenuWebviewForShow()
{
    if (TrayMenuPresenter::Instance().IsBound())
    {
        return true;
    }
    if (webviewControllerMenuWnd && menuNavigationReady)
    {
        return true;
    }

    pendingTrayMenuShow = true;

    if (webviewControllerMenuWnd)
    {
        // Controller exists but nothing ever painted. An unreadable menu asset
        // makes NavigateToString run on an empty string, which yields a blank
        // document with no title (and therefore no visible WebView2 child entry).
        if (::HTMLStringMenuWnd.empty())
        {
            PrepareHtmlForWnds();
        }
        FTB_DIAG_LOGF(L"menu prepare: controller exists, navigation not ready, html_empty={} -> renavigate",
                      ::HTMLStringMenuWnd.empty());
        if (webviewMenuWnd && !::HTMLStringMenuWnd.empty())
        {
            webviewMenuWnd->NavigateToString(::HTMLStringMenuWnd.c_str());
        }
        return false;
    }

    FTB_DIAG_LOGF(L"menu prepare: no controller yet, init_state={} attempts={}", static_cast<int>(smallWindowInitState),
                  smallWindowInitAttempts);
    if (smallWindowInitState != SmallWindowInitState::InProgress)
    {
        // An explicit right-click is a fresh user intent: reset the attempt
        // budget so a long-idle session can still recover the menu.
        smallWindowInitAttempts = 0;
        ScheduleSmallWindowWebviewRetry(200);
    }
    return false;
}

void ShutdownWebviews()
{
    // WebView2 objects are apartment-bound. Release every controller and
    // interface on the UI STA before WinMain balances CoInitializeEx.
    ResetSmallWindowTopmostGate();
    pendingTrayMenuShow = false;
    pendingCandidateShow = false;
    smallWindowInitState = SmallWindowInitState::Idle;
    smallWindowControllerRequestInFlight = false;
    if (smallWindowCandHwnd)
    {
        KillTimer(smallWindowCandHwnd, kRetrySmallWindowWebviewTimerId);
    }

    if (candidateRasterizationScaleChangedRegistered && webviewController3CandWnd)
    {
        webviewController3CandWnd->remove_RasterizationScaleChanged(candidateRasterizationScaleChangedToken);
        candidateRasterizationScaleChangedRegistered = false;
    }
    if (webviewControllerCandWnd)
    {
        webviewControllerCandWnd->Close();
    }
    if (webviewControllerMenuWnd)
    {
        webviewControllerMenuWnd->Close();
    }
    if (webviewControllerFtbWnd)
    {
        webviewControllerFtbWnd->Close();
    }
    if (webviewControllerSettingsWnd)
    {
        webviewControllerSettingsWnd->Close();
    }

    webviewController2CandWnd.Reset();
    webviewController3CandWnd.Reset();
    webviewController2MenuWnd.Reset();
    webviewController2FtbWnd.Reset();
    webviewController2SettingsWnd.Reset();

    webview3CandWnd.Reset();
    webview3MenuWnd.Reset();
    webview3FtbWnd.Reset();
    webview3SettingsWnd.Reset();

    webviewCandWnd.Reset();
    webviewMenuWnd.Reset();
    webviewFtbWnd.Reset();
    webviewSettingsWnd.Reset();

    webviewCompositionControllerSettingsWnd.Reset();
    webviewControllerCandWnd.Reset();
    webviewControllerMenuWnd.Reset();
    webviewControllerFtbWnd.Reset();
    webviewControllerSettingsWnd.Reset();

    dcompRootVisualSettingsWnd.Reset();
    dcompTargetSettingsWnd.Reset();
    dcompDeviceSettingsWnd.Reset();
    smallWindowWebviewEnvironment.Reset();
}

/**
 * @brief 更新 floating toolbar 窗口的中英文切换状态
 *
 * @param webview
 * @param cnEnState 1: 中文, 0: 英文
 */
void UpdateFtbCnEnState(ComPtr<ICoreWebView2> webview, int cnEnState)
{
    if (UpdateBinaryState(cnEnState, floatingToolbarState.cn_en))
    {
        RenderFloatingToolbarState(webview.Get());
    }
}

/**
 * @brief 更新 floating toolbar 窗口的中英文切换状态和标点切换状态
 *
 * @param webview
 * @param cnEnState 1: 中文, 0: 英文
 * @param puncState 1: 中文标点, 0: 英文标点
 */
void UpdateFtbCnEnAndPuncState(ComPtr<ICoreWebView2> webview, int cnEnState, int puncState)
{
    bool changed = UpdateBinaryState(cnEnState, floatingToolbarState.cn_en);
    changed |= UpdateBinaryState(puncState, floatingToolbarState.punctuation);
    if (changed)
    {
        RenderFloatingToolbarState(webview.Get());
    }
}

/**
 * @brief 更新 floating toolbar 窗口的中英文切换状态和标点切换状态
 *
 * @param webview
 * @param cnEnState 1: 中文, 0: 英文
 * @param doubleSingleByteState 1: 全角, 0: 半角
 * @param puncState 1: 中文标点, 0: 英文标点
 */
void UpdateFtbCnEnAndDoubleSingleAndPuncState( //
    ComPtr<ICoreWebView2> webview,             //
    int cnEnState,                             //
    int doubleSingleByteState,                 //
    int puncState,                             //
    int capsLockState                          //
)
{
    bool changed = UpdateBinaryState(cnEnState, floatingToolbarState.cn_en);
    changed |= UpdateBinaryState(doubleSingleByteState, floatingToolbarState.double_single_byte);
    changed |= UpdateBinaryState(puncState, floatingToolbarState.punctuation);
    changed |= UpdateBinaryState(capsLockState, floatingToolbarState.caps_lock);
    if (changed)
    {
        RenderFloatingToolbarState(webview.Get());
    }
}

/**
 * @brief 更新 floating toolbar 窗口的标点切换状态
 *
 * @param webview
 * @param puncState 1: 中文标点, 0: 英文标点
 */
void UpdateFtbPuncState(ComPtr<ICoreWebView2> webview, int puncState)
{
    if (UpdateBinaryState(puncState, floatingToolbarState.punctuation))
    {
        RenderFloatingToolbarState(webview.Get());
    }
}

/**
 * @brief 更新 floating toolbar 窗口的全角和半角状态
 *
 * @param webview
 * @param doubleSingleByteState 0: 半角, 1: 全角
 */
void UpdateFtbDoubleSingleByteState(ComPtr<ICoreWebView2> webview, int doubleSingleByteState)
{
    if (UpdateBinaryState(doubleSingleByteState, floatingToolbarState.double_single_byte))
    {
        RenderFloatingToolbarState(webview.Get());
    }
}

void UpdateFtbEnglishInputModeState(ComPtr<ICoreWebView2> webview, int enabled)
{
    if (UpdateBinaryState(enabled, floatingToolbarState.english_input_mode))
    {
        RenderFloatingToolbarState(webview.Get());
    }
}

void UpdateFtbCapsLockState(ComPtr<ICoreWebView2> webview, int enabled)
{
    if (UpdateBinaryState(enabled, floatingToolbarState.caps_lock))
    {
        RenderFloatingToolbarState(webview.Get());
    }
}

void UpdateFtbCharacterSetState(ComPtr<ICoreWebView2> webview)
{
    RenderFloatingToolbarState(webview.Get());
}

void UpdateFtbInputModeState(ComPtr<ICoreWebView2> webview, int japaneseMode)
{
    if (japaneseMode != 0 && japaneseMode != 1)
    {
        return;
    }
    floatingToolbarState.japanese_input_mode = japaneseMode;
    RenderFloatingToolbarState(webview.Get());
}

void ApplyConfiguredFloatingToolbarItems()
{
    RenderFloatingToolbarState(::webviewFtbWnd.Get());
    ApplyConfiguredFloatingToolbarSize();
}
