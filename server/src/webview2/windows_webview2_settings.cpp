// 设置窗口 WebView：DirectComposition 视觉树、composition controller 的创建、设置页消息与
// configUpdate 分发，以及 PostSettingsWindowState / PostSettingsConfig / InitWebviewSettingsWnd。
#include "webview2/windows_webview2_internal.h"
#include "engine/contracts/webview/validator.h"
#include "engine/core/data_path.h"
#include "config/ime_config.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include "global/globals.h"
#include "ipc/ipc.h"
#include "ipc/event_listener.h"
#include "utils/common_utils.h"
#include <dwmapi.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <string>

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

// 死宿主（InitWebviewSettingsWnd 无调用者）的智能标点子键分发。单独成函数，避免在
// OnControllerCreatedSettingsWnd 的深层 if/else 链里插入折行——那条链一旦出现折行，
// clang-format 会连带重排整个 lambda 的缩进（行宽 120 的罚分择优）。
static void ApplySmartPunctuationSubkey(const std::string &path, bool value)
{
    if (path == "input.smart_punctuation_space_convert")
    {
        if (SetConfiguredSmartPunctuationSpaceConvertEnabled(value))
        {
            BroadcastToTsfWorkerThreadViaNamedpipe(
                Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationSpaceConvertChanged,
                value ? L"1" : L"0");
            PostSettingsConfig();
        }
    }
    else if (path == "input.smart_punctuation_direct_digit")
    {
        if (SetConfiguredSmartPunctuationDirectDigitEnabled(value))
        {
            BroadcastToTsfWorkerThreadViaNamedpipe(
                Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectDigitChanged,
                value ? L"1" : L"0");
            PostSettingsConfig();
        }
    }
    else if (path == "input.smart_punctuation_direct_letter")
    {
        if (SetConfiguredSmartPunctuationDirectLetterEnabled(value))
        {
            BroadcastToTsfWorkerThreadViaNamedpipe(
                Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectLetterChanged,
                value ? L"1" : L"0");
            PostSettingsConfig();
        }
    }
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
        const std::wstring assetPath = fmt::format(              //
            L"{}\\html\\webview2\\settings\\ime-settings\\dist", //
            CommonUtils::get_ime_data_path_w()                   //
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
                    else if (type == "openHelpcodeDirectory")
                    {
                        const std::filesystem::path directory =
                            metasequoia::path_from_utf8(GetCustomHelpcodeDirectory().c_str());
                        std::error_code ec;
                        std::filesystem::create_directories(directory, ec);
                        if (!ec)
                            ShellExecuteW(hwnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
                            else if (path == "appearance.settings_window_linger")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredSettingsWindowLinger(value))
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
                            else if (path == "appearance.candidate_fixed_badge")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredCandidateFixedBadge(value))
                                {
                                    // 徽标在组页时拼进词条，不刷新的话要等下次上屏才看得见改动
                                    FanyNamedPipe::EnqueueRefreshCandidatePageTask();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "appearance.candidate_fixed_badge_style")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredCandidateFixedBadgeStyle(value))
                                {
                                    FanyNamedPipe::EnqueueRefreshCandidatePageTask();
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
                                    RestartFloatingToolbarAutoHide(L"settings-toggle");
                                    SyncMenuFloatingToolbarToggle();
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.caret_state_indicator")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredCaretStateIndicatorEnabled(value))
                                {
                                    if (!value && ::global_hwnd_caret_state)
                                        PostMessage(::global_hwnd_caret_state, WM_HIDE_CARET_STATE, 0, 0);
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.caret_state_indicator_on_focus")
                            {
                                if (SetConfiguredCaretStateIndicatorOnFocus(json::value_to<bool>(data.at("value"))))
                                    PostSettingsConfig();
                            }
                            else if (path == "general.caret_state_indicator_position")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredCaretStateIndicatorPosition(value))
                                    PostSettingsConfig();
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
                            // Must precede the general.floating_toolbar_ item prefix below.
                            else if (path == "general.floating_toolbar_auto_hide")
                            {
                                if (SetConfiguredFloatingToolbarAutoHide(json::value_to<bool>(data.at("value"))))
                                {
                                    RestartFloatingToolbarAutoHide(L"settings-auto-hide");
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "general.floating_toolbar_auto_hide_delay")
                            {
                                if (SetConfiguredFloatingToolbarAutoHideDelay(
                                        static_cast<int>(data.at("value").as_int64())))
                                {
                                    RestartFloatingToolbarAutoHide(L"settings-auto-hide-delay");
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
                            else if (path == "input.smart_punctuation_space_convert")
                            {
                                ApplySmartPunctuationSubkey(path, json::value_to<bool>(data.at("value")));
                            }
                            else if (path == "input.smart_punctuation_direct_digit")
                            {
                                ApplySmartPunctuationSubkey(path, json::value_to<bool>(data.at("value")));
                            }
                            else if (path == "input.smart_punctuation_direct_letter")
                            {
                                ApplySmartPunctuationSubkey(path, json::value_to<bool>(data.at("value")));
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
                            else if (path == "input.japanese_punctuation")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredJapanesePunctuation(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "input.japanese_katakana_fkey")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredJapaneseKatakanaFkey(value))
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
                            else if (path == "statistics.enabled")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredStatisticsEnabled(value))
                                {
                                    // The DLL gates capture on this: without the
                                    // broadcast an opt-out would keep classifying
                                    // and writing frames until the next connect.
                                    BroadcastToTsfWorkerThreadViaNamedpipe(
                                        Global::DataFromServerMsgTypeToTsfWorkerThread::StatisticsEnabledChanged,
                                        value ? L"1" : L"0");
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "statistics.retention")
                            {
                                const std::string value = json::value_to<std::string>(data.at("value"));
                                if (SetConfiguredStatisticsRetention(value))
                                {
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
                            else if (path == "association.sentence_wordlattice")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredAssocSentenceWordLattice(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "association.sentence_google")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredAssocSentenceGoogle(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "association.sentence_neural_desktop")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredAssocSentenceNeuralDesktop(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "association.sentence_neural_keyboard")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredAssocSentenceNeuralKeyboard(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "association.sentence_show_next_on_duplicate")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredAssocSentenceShowNextOnDuplicate(value))
                                {
                                    PostSettingsConfig();
                                }
                            }
                            else if (path == "association.sentence_source_badge")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredAssocSentenceSourceBadge(value))
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
                            else if (path == "general.paging_mouse_wheel")
                            {
                                const bool value = json::value_to<bool>(data.at("value"));
                                if (SetConfiguredPagingMouseWheelEnabled(value))
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

static nlohmann::json CustomHelpcodeSchemasJson()
{
    nlohmann::json schemas = nlohmann::json::array();
    for (const auto &schema : GetCustomHelpcodeSchemas())
        schemas.push_back({{"id", schema.schema}, {"name", schema.name}, {"name_en", schema.name_en}});
    return schemas;
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
            {"japanese_punctuation", GetConfiguredJapanesePunctuation()},
            {"japanese_katakana_fkey", GetConfiguredJapaneseKatakanaFkey()},
            {"character_set", GetConfiguredCharacterSet()},
            {"default_ime_mode", GetConfiguredDefaultImeMode()},
            {"ime_mode_scope", GetConfiguredImeModeScope()},
            {"shuangpin_schema", GetConfiguredShuangpinSchema()},
            {"wubi_schema", GetConfiguredWubiSchema()},
            {"word_to_character", GetConfiguredWordToCharacterEnabled()},
            {"word_to_character_keys", GetConfiguredWordToCharacterKeys()},
            {"smart_punctuation", GetConfiguredSmartPunctuationEnabled()},
            {"smart_punctuation_space_convert", GetConfiguredSmartPunctuationSpaceConvertEnabled()},
            {"smart_punctuation_direct_digit", GetConfiguredSmartPunctuationDirectDigitEnabled()},
            {"smart_punctuation_direct_letter", GetConfiguredSmartPunctuationDirectLetterEnabled()},
            {"smart_punctuation_repeat_to_chinese", GetConfiguredSmartPunctuationRepeatToChineseEnabled()},
            {"paired_punctuation", GetConfiguredPairedPunctuationEnabled()},
            {"punctuation_lock", GetConfiguredPunctuationLock()}}},
          {"general",
           {{"diagnostic_log", GetConfiguredDiagnosticLogEnabled()},
            {"candidate_window_diagnostic_log", GetConfiguredDiagnosticLogEnabled()},
            {"tsf_diagnostic_log", GetConfiguredTsfDiagnosticLogEnabled()},
            {"floating_toolbar", GetConfiguredFloatingToolbarEnabled()},
            {"caret_state_indicator", GetConfiguredCaretStateIndicatorEnabled()},
            {"caret_state_indicator_on_focus", GetConfiguredCaretStateIndicatorOnFocus()},
            {"caret_state_indicator_position", GetConfiguredCaretStateIndicatorPosition()},
            {"floating_toolbar_fullwidth", toolbar.fullwidth},
            {"floating_toolbar_punctuation", toolbar.punctuation},
            {"floating_toolbar_character_set", toolbar.character_set},
            {"floating_toolbar_emoji", toolbar.emoji},
            {"floating_toolbar_screen_keyboard", toolbar.screen_keyboard},
            {"floating_toolbar_settings", toolbar.settings},
            {"floating_toolbar_scale", GetConfiguredFloatingToolbarScale()},
            {"floating_toolbar_font_size", GetConfiguredFloatingToolbarFontSize()},
            {"floating_toolbar_auto_hide", GetConfiguredFloatingToolbarAutoHide()},
            {"floating_toolbar_auto_hide_delay", GetConfiguredFloatingToolbarAutoHideDelay()},
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
            {"paging_mouse_wheel", GetConfiguredPagingMouseWheelEnabled()},
            {"candidate_arrow_navigation", GetConfiguredCandidateArrowNavigationEnabled()}}},
          {"association",
           {{"sentence_wordlattice", GetConfiguredAssocSentenceWordLattice()},
            {"sentence_google", GetConfiguredAssocSentenceGoogle()},
            {"sentence_neural_desktop", GetConfiguredAssocSentenceNeuralDesktop()},
            {"sentence_neural_keyboard", GetConfiguredAssocSentenceNeuralKeyboard()},
            {"sentence_show_next_on_duplicate", GetConfiguredAssocSentenceShowNextOnDuplicate()},
            {"sentence_source_badge", GetConfiguredAssocSentenceSourceBadge()}}},
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
            {"r_mode", GetConfiguredRModeEnabled()}}},
          {"appearance",
           {{"ui_backend", GetConfiguredUiBackend()},
            {"settings_window_linger", GetConfiguredSettingsWindowLinger()},
            {"candidate_window_layout", GetConfiguredCandidateWindowLayout()},
            {"candidate_window_follow_cursor", GetConfiguredCandidateWindowFollowCursor()},
            {"candidate_skin", GetConfiguredCandidateSkin()},
            {"candidate_window_preedit_style", GetConfiguredCandidateWindowPreeditStyle()},
            {"candidate_fixed_badge", GetConfiguredCandidateFixedBadge()},
            {"candidate_fixed_badge_style", GetConfiguredCandidateFixedBadgeStyle()},
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
            {"show_qp_helpcode_in_candidate_window", GetConfiguredShowQuanpinHelpcodeInCandidateWindow()}}},
          {"statistics",
           {{"enabled", GetConfiguredStatisticsEnabled()}, {"retention", GetConfiguredStatisticsRetention()}}}}}};
    payload["data"]["helpcode"]["custom_schemas"] = CustomHelpcodeSchemasJson();
    payload["data"]["helpcode"]["custom_directory"] = GetCustomHelpcodeDirectory();
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
