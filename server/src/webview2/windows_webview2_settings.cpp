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
#include "skin/candidate_skin_catalog.h"
#include "settings/collocation_model.h"
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

// TSF 据此决定反引号、分号要不要当句中辅助码的编码键吃掉，两个键各走一个 opcode。
static void BroadcastMidSentenceHelpcodeTriggers()
{
    BroadcastToTsfWorkerThreadViaNamedpipe(Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeChanged,
                                           FormatMidSentenceHelpcodeWorkerPayload());
    BroadcastToTsfWorkerThreadViaNamedpipe(
        Global::DataFromServerMsgTypeToTsfWorkerThread::MidSentenceHelpcodeSemicolonChanged,
        FormatMidSentenceHelpcodeSemicolonWorkerPayload());
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

// 下面这组 Apply<段名>Subkey 是 configUpdate 消息的分发实现，按 config.toml 的段名分组，
// 一个段一个函数。分组的原因是链的长度而非可读性：合在一起是一条 109 个分支的
// else-if 链，MSVC 会以 C1061（块嵌套太深）拒绝编译——上限约 127 层，单条链加几个
// 配置项就会顶穿。拆开后每段最长 27 个分支，离上限足够远。
//
// 这里是纯机械分组，各段名互不重叠，所以调用点可以依次调用全部函数：至多一个会命中。
// 唯一例外是段内还有 rfind 前缀分支的地方，那里必须保持 else-if，见各函数内的说明。

// [input] 段：输入模式、输入方案、标点行为等
static void ApplyInputSubkey(const std::string &path, const json::object &data)
{
    if (path == "input.mode")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredInputMode(value))
        {
            ApplyConfiguredInputScheme();
            PostSettingsConfig();
        }
    }
    if (path == "input.schema")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredInputScheme(value))
        {
            ApplyConfiguredInputScheme();
            PostSettingsConfig();
        }
    }
    if (path == "input.character_set")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredCharacterSet(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "input.default_ime_mode")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredDefaultImeMode(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "input.ime_mode_scope")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredImeModeScope(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "input.shuangpin_schema")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredShuangpinSchema(value))
        {
            ApplyConfiguredShuangpinSchema();
            PostSettingsConfig();
        }
    }
    if (path == "input.wubi_schema")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredWubiSchema(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "input.wubi_mixed_pinyin")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredWubiMixedPinyin(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "input.escape_keeps_selected_word")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredEscapeKeepsSelectedWord(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "input.enter_learns_english_word")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredEnterLearnsEnglishWord(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "input.wubi_z_mode")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredWubiZMode(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "input.word_to_character")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        SetConfiguredWordToCharacterEnabled(value);
        PostSettingsConfig();
    }
    if (path == "input.word_to_character_keys")
    {
        SetConfiguredWordToCharacterKeys(json::value_to<std::string>(data.at("value")));
        PostSettingsConfig();
    }
    if (path == "input.smart_punctuation")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredSmartPunctuationEnabled(value))
        {
            BroadcastToTsfWorkerThreadViaNamedpipe(
                Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationChanged, value ? L"1" : L"0");
            PostSettingsConfig();
        }
    }
    if (path == "input.smart_punctuation_space_convert")
    {
        ApplySmartPunctuationSubkey(path, json::value_to<bool>(data.at("value")));
    }
    if (path == "input.smart_punctuation_direct_digit")
    {
        ApplySmartPunctuationSubkey(path, json::value_to<bool>(data.at("value")));
    }
    if (path == "input.smart_punctuation_direct_letter")
    {
        ApplySmartPunctuationSubkey(path, json::value_to<bool>(data.at("value")));
    }
    if (path == "input.smart_punctuation_repeat_to_chinese")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredSmartPunctuationRepeatToChineseEnabled(value))
        {
            BroadcastToTsfWorkerThreadViaNamedpipe(
                Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationRepeatToChineseChanged,
                value ? L"1" : L"0");
            PostSettingsConfig();
        }
    }
    if (path == "input.paired_punctuation")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredPairedPunctuationEnabled(value))
        {
            BroadcastToTsfWorkerThreadViaNamedpipe(
                Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged, value ? L"1" : L"0");
            PostSettingsConfig();
        }
    }
    if (path == "input.punctuation_lock")
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
    if (path == "input.japanese_schema")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredJapaneseSchema(value))
        {
            PostSettingsConfig();
        }
    }
}

// [appearance] 段：候选窗外观、颜色、字体、徽章等
static void ApplyAppearanceSubkey(const std::string &path, const json::object &data)
{
    if (path == "appearance.tsf_preedit_style")
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
    if (path == "appearance.tsf_preedit_shuangpin_quanpin")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredTsfPreeditShuangpinQuanpin(value))
        {
            BroadcastToTsfWorkerThreadViaNamedpipe(
                Global::DataFromServerMsgTypeToTsfWorkerThread::PagingCommaPeriodChanged,
                FormatPagingCommaPeriodWorkerPayload());
            PostSettingsConfig();
        }
    }
    if (path == "appearance.ui_backend")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredUiBackend(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "appearance.settings_window_linger")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredSettingsWindowLinger(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "appearance.candidate_window_layout")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredCandidateWindowLayout(value))
        {
            ApplyConfiguredCandidateWindowLayout();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.candidate_window_follow_cursor")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredCandidateWindowFollowCursor(value))
        {
            CAND_WEBVIEW_TRACE_LOGF(L"candidate-position config-update follow_cursor={}", value);
            PostSettingsConfig();
        }
    }
    if (path == "appearance.candidate_skin")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredCandidateSkin(value))
        {
            ApplyConfiguredUiThemes();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.candidate_window_preedit_style")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredCandidateWindowPreeditStyle(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "appearance.candidate_window_preedit_shuangpin_quanpin")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredCandidateWindowPreeditShuangpinQuanpin(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "appearance.candidate_fixed_badge")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredCandidateFixedBadge(value))
        {
            // 徽标在组页时拼进词条，不刷新的话要等下次上屏才看得见改动
            FanyNamedPipe::EnqueueRefreshCandidatePageTask();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.candidate_fixed_badge_style")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredCandidateFixedBadgeStyle(value))
        {
            FanyNamedPipe::EnqueueRefreshCandidatePageTask();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.page_size")
    {
        const int value = static_cast<int>(data.at("value").as_int64());
        if (SetConfiguredCandidatePageSize(value))
        {
            FanyNamedPipe::EnqueueApplyCandidatePageSizeTask();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.font")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredCandidateFont(value))
        {
            ApplyConfiguredCandidateAppearance();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.fallback_fonts")
    {
        // configUpdate accepts scalar values; structured settings use a JSON string.
        const auto fonts =
            nlohmann::json::parse(json::value_to<std::string>(data.at("value"))).get<std::vector<std::string>>();
        if (SetConfiguredCandidateFallbackFonts(fonts))
        {
            ApplyConfiguredCandidateAppearance();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.english_font")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredCandidateEnglishFont(value))
        {
            ApplyConfiguredCandidateAppearance();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.font_size")
    {
        const int value = static_cast<int>(data.at("value").as_int64());
        if (SetConfiguredCandidateFontSize(value))
        {
            ApplyConfiguredCandidateAppearance();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.candidate_window_preedit_font_size")
    {
        const int value = static_cast<int>(data.at("value").as_int64());
        if (SetConfiguredCandidateWindowPreeditFontSize(value))
        {
            ApplyConfiguredCandidateAppearance();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.cand_text_color")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredCandidateTextColor(value))
        {
            ApplyConfiguredCandidateAppearance();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.theme_mode")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredThemeMode(value))
        {
            ApplyConfiguredUiThemes();
            if (webviewController2SettingsWnd)
            {
                const bool settingsLight = ResolveConfiguredTheme(GetConfiguredThemeSettings()) == "light";
                COREWEBVIEW2_COLOR backgroundColor =
                    settingsLight ? COREWEBVIEW2_COLOR{255, 243, 243, 243} : COREWEBVIEW2_COLOR{255, 32, 32, 32};
                webviewController2SettingsWnd->put_DefaultBackgroundColor(backgroundColor);
            }
            PostSettingsConfig();
        }
    }
    if (path == "appearance.theme_settings")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredThemeSettings(value))
        {
            if (webviewController2SettingsWnd)
            {
                const bool settingsLight = ResolveConfiguredTheme(GetConfiguredThemeSettings()) == "light";
                COREWEBVIEW2_COLOR backgroundColor =
                    settingsLight ? COREWEBVIEW2_COLOR{255, 243, 243, 243} : COREWEBVIEW2_COLOR{255, 32, 32, 32};
                webviewController2SettingsWnd->put_DefaultBackgroundColor(backgroundColor);
            }
            PostSettingsConfig();
        }
    }
    if (path == "appearance.theme_cand")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredThemeCand(value))
        {
            ApplyConfiguredUiThemes();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.theme_ftb")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredThemeFtb(value))
        {
            ApplyConfiguredUiThemes();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.theme_menu")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredThemeMenu(value))
        {
            ApplyConfiguredUiThemes();
            PostSettingsConfig();
        }
    }
    if (path == "appearance.theme_emoji")
    {
        if (SetConfiguredThemeEmoji(json::value_to<std::string>(data.at("value"))))
            PostSettingsConfig();
    }
    if (path == "appearance.theme_screen_keyboard")
    {
        if (SetConfiguredThemeScreenKeyboard(json::value_to<std::string>(data.at("value"))))
            PostSettingsConfig();
    }
    if (path == "appearance.theme_handwriting")
    {
        if (SetConfiguredThemeHandwriting(json::value_to<std::string>(data.at("value"))))
            PostSettingsConfig();
    }
    if (path == "appearance.theme_voice")
    {
        if (SetConfiguredThemeVoice(json::value_to<std::string>(data.at("value"))))
            PostSettingsConfig();
    }
}

// [general] 段：通用设置，含悬浮工具栏
static void ApplyGeneralSubkey(const std::string &path, const json::object &data)
{
    if (path == "general.floating_toolbar")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredFloatingToolbarEnabled(value))
        {
            RestartFloatingToolbarAutoHide(L"settings-toggle");
            SyncMenuFloatingToolbarToggle();
            PostSettingsConfig();
        }
    }
    if (path == "general.caret_state_indicator")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredCaretStateIndicatorEnabled(value))
        {
            if (!value && ::global_hwnd_caret_state)
                PostMessage(::global_hwnd_caret_state, WM_HIDE_CARET_STATE, 0, 0);
            PostSettingsConfig();
        }
    }
    if (path == "general.caret_state_indicator_on_focus")
    {
        if (SetConfiguredCaretStateIndicatorOnFocus(json::value_to<bool>(data.at("value"))))
            PostSettingsConfig();
    }
    if (path == "general.caret_state_indicator_position")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredCaretStateIndicatorPosition(value))
            PostSettingsConfig();
    }
    // 这五条必须保持 else-if 链，不能各自独立 if：末位的 rfind 前缀分支覆盖了
    // scale/font_size/auto_hide/auto_hide_delay 这四个键。拆分前它们在同一条
    // else-if 链上，只有首个命中的分支会执行；拆成并列 if 后前缀分支会二次命中，
    // 让 general.floating_toolbar_scale 同时写进 SetConfiguredFloatingToolbarItemEnabled。
    if (path == "general.floating_toolbar_scale")
    {
        const double value = data.at("value").is_double() ? data.at("value").as_double()
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
        if (SetConfiguredFloatingToolbarAutoHideDelay(static_cast<int>(data.at("value").as_int64())))
        {
            RestartFloatingToolbarAutoHide(L"settings-auto-hide-delay");
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
    if (path == "general.cn_en_mixed_input")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredEnglishCandidatesEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "general.candidate_translations")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredCandidateTranslationsEnabled(value))
        {
            FanyNamedPipe::EnqueueRefreshCandidatePageTask();
            PostSettingsConfig();
        }
    }
    if (path == "general.diagnostic_log" || path == "general.candidate_window_diagnostic_log")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredDiagnosticLogEnabled(value))
        {
            CAND_DIAG_LOGF(L"diagnostic logging enabled from Settings");
            PostSettingsConfig();
        }
    }
    if (path == "general.tsf_diagnostic_log")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredTsfDiagnosticLogEnabled(value))
        {
            BroadcastToTsfWorkerThreadViaNamedpipe(
                Global::DataFromServerMsgTypeToTsfWorkerThread::TsfDiagnosticLogChanged, value ? L"1" : L"0");
            PostSettingsConfig();
        }
    }
    if (path == "general.cn_en_mixed_input_min_chars")
    {
        const int value = static_cast<int>(data.at("value").as_int64());
        if (SetConfiguredEnglishMixedInputMinChars(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "general.emoji_mixed_input")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredEmojiMixedInputEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "general.kaomoji_mixed_input")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredKaomojiMixedInputEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "general.cloud_candidates")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredCloudCandidatesEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "general.paging_minus_equal")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        SetConfiguredPagingMinusEqualEnabled(value);
        PostSettingsConfig();
    }
    if (path == "general.paging_tab")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredPagingTabEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "general.paging_comma_period")
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
    if (path == "general.paging_brackets")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        SetConfiguredPagingBracketsEnabled(value);
        PostSettingsConfig();
    }
    if (path == "general.paging_page_up_down")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredPagingPageUpDownEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "general.paging_mouse_wheel")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredPagingMouseWheelEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "general.candidate_arrow_navigation")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredCandidateArrowNavigationEnabled(value))
        {
            PostSettingsConfig();
        }
    }
}

// [statistics] 段：使用统计开关
static void ApplyStatisticsSubkey(const std::string &path, const json::object &data)
{
    if (path == "statistics.enabled")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredStatisticsEnabled(value))
        {
            // The DLL gates capture on this: without the
            // broadcast an opt-out would keep classifying
            // and writing frames until the next connect.
            BroadcastToTsfWorkerThreadViaNamedpipe(
                Global::DataFromServerMsgTypeToTsfWorkerThread::StatisticsEnabledChanged, value ? L"1" : L"0");
            PostSettingsConfig();
        }
    }
    if (path == "statistics.retention")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredStatisticsRetention(value))
        {
            PostSettingsConfig();
        }
    }
}

// [tencent_tmt] 段：腾讯翻译开关
static void ApplyTencentTmtSubkey(const std::string &path, const json::object &data)
{
    if (path.rfind("tencent_tmt.", 0) == 0)
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredTencentTmtString(path.substr(std::string("tencent_tmt.").size()), value))
        {
            if (path == "tencent_tmt.target_language")
                FanyNamedPipe::EnqueueRefreshCandidatePageTask();
            PostSettingsConfig();
        }
    }
}

// [custom_translation] 段：自定义翻译的总开关与逐项文本
static void ApplyCustomTranslationSubkey(const std::string &path, const json::object &data)
{
    // 同上：rfind 前缀分支覆盖 "custom_translation.enabled"，必须保持 else-if 才能
    // 复现拆分前「只执行首个命中分支」的语义。
    if (path == "custom_translation.enabled")
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
        if (SetConfiguredCustomTranslationString(path.substr(std::string("custom_translation.").size()), value))
        {
            FanyNamedPipe::EnqueueRefreshCandidatePageTask();
            PostSettingsConfig();
        }
    }
}

// [network] 段：联网功能共用的代理
static void ApplyNetworkSubkey(const std::string &path, const json::object &data)
{
    if (path.rfind("network.", 0) == 0)
    {
        const json::value &value = data.at("value");
        if (value.is_string() &&
            SetConfiguredNetworkString(path.substr(std::string("network.").size()), json::value_to<std::string>(value)))
        {
            PostSettingsConfig();
        }
    }
}

// [association] 段：联想、词格、搭配模型等
static void ApplyAssociationSubkey(const std::string &path, const json::object &data)
{
    if (path == "association.sentence_wordlattice")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredAssocSentenceWordLattice(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "association.sentence_google")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredAssocSentenceGoogle(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "association.sentence_neural_desktop")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredAssocSentenceNeuralDesktop(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "association.sentence_neural_keyboard")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredAssocSentenceNeuralKeyboard(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "association.sentence_show_next_on_duplicate")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredAssocSentenceShowNextOnDuplicate(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "association.sentence_source_badge")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredAssocSentenceSourceBadge(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "association.sentence_collocation_enabled")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredAssocSentenceCollocationEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "association.sentence_collocation_model")
    {
        const std::string model_id = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredAssocSentenceCollocationModel(model_id))
        {
            PostSettingsConfig();
        }
    }
}

// [utility] 段：导入导出、剪贴板等工具类设置
static void ApplyUtilitySubkey(const std::string &path, const json::object &data)
{
    if (path == "utility.unicode_mode")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredUnicodeModeEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.quick_phrase")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredQuickPhraseEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.quick_phrase_candidates")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredQuickPhraseCandidatesEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.quick_phrase_frequency")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredQuickPhraseFrequencyEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.mixed_candidates")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredMixedCandidatesEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.date_time_candidates")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredDateTimeCandidatesEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.date_time_menu")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredDateTimeMenuEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.date_time_mode")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredDateTimeModeEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.emoji_mode")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredEmojiModeEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.kaomoji_mode")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredKaomojiModeEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.jianpin_mode")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredJianpinModeEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.y_mode")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredYModeEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.r_mode")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredRModeEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "utility.clipboard_history")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredClipboardHistoryEnabled(value))
        {
            PostSettingsConfig();
        }
    }
}

// [keybindings] 段：快捷键
static void ApplyKeybindingsSubkey(const std::string &path, const json::object &data)
{
    if (path == "keybindings.toggle_character_set_ctrl_shift_f")
    {
        SetConfiguredCharacterSetShortcutEnabled(json::value_to<bool>(data.at("value")));
        PostSettingsConfig();
    }
    if (path == "keybindings.switch_language_shift")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredSwitchLanguageShiftEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "keybindings.switch_language_ctrl")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredSwitchLanguageCtrlEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "keybindings.switch_language_ctrl_alt_space")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredSwitchLanguageCtrlAltSpaceEnabled(value))
        {
            PostSettingsConfig();
        }
    }
}

// [helpcode] 段：辅助码方案与开关
static void ApplyHelpcodeSubkey(const std::string &path, const json::object &data)
{
    if (path == "helpcode.show_sp_helpcode_in_candidate_window")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredShowShuangpinHelpcodeInCandidateWindow(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "helpcode.shuangpin_helpcode")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredShuangpinHelpcodeEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "helpcode.shuangpin_mid_sentence_helpcode")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredShuangpinMidSentenceHelpcodeEnabled(value))
        {
            BroadcastMidSentenceHelpcodeTriggers();
            PostSettingsConfig();
        }
    }
    if (path == "helpcode.shuangpin_mid_sentence_helpcode_backtick" ||
        path == "helpcode.shuangpin_mid_sentence_helpcode_semicolon")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        const bool saved = path == "helpcode.shuangpin_mid_sentence_helpcode_backtick"
                               ? SetConfiguredShuangpinMidSentenceHelpcodeBacktick(value)
                               : SetConfiguredShuangpinMidSentenceHelpcodeSemicolon(value);
        if (saved)
        {
            BroadcastMidSentenceHelpcodeTriggers();
            PostSettingsConfig();
        }
    }
    if (path == "helpcode.shuangpin_helpcode_schema")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredShuangpinHelpcodeSchema(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "helpcode.quanpin_helpcode")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredQuanpinHelpcodeEnabled(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "helpcode.quanpin_helpcode_schema")
    {
        const std::string value = json::value_to<std::string>(data.at("value"));
        if (SetConfiguredQuanpinHelpcodeSchema(value))
        {
            PostSettingsConfig();
        }
    }
    if (path == "helpcode.show_qp_helpcode_in_candidate_window")
    {
        const bool value = json::value_to<bool>(data.at("value"));
        if (SetConfiguredShowQuanpinHelpcodeInCandidateWindow(value))
        {
            PostSettingsConfig();
        }
    }
}

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
                    else if (type == "openShuangpinDirectory")
                    {
                        const std::filesystem::path directory =
                            metasequoia::path_from_utf8(GetCustomShuangpinDirectory().c_str());
                        std::error_code ec;
                        std::filesystem::create_directories(directory, ec);
                        if (!ec)
                            ShellExecuteW(hwnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    else if (type == "collocationModelDownload")
                    {
                        // 后台线程下载，进度随下一次配置快照回给页面；页面在下载态轮询 configRequest。
                        // 忙碌（其他模型在下载）时 StartDownload 拒绝，页面在下载态禁用其余下载按钮。
                        collocation::StartDownload(json::value_to<std::string>(val.at("data").at("modelId")));
                        PostSettingsConfig();
                    }
                    else if (type == "collocationModelDelete")
                    {
                        // 下载中与当前生效解析的 id 在 DeleteModel 里拒绝；无论成败都回快照刷新列表。
                        collocation::DeleteModel(json::value_to<std::string>(val.at("data").at("modelId")));
                        PostSettingsConfig();
                    }
                    else if (type == "collocationModelImport")
                    {
                        // 浏览器下载后的本地导入：对话框在 UI 线程模态弹出，复制与校验在后台线程。
                        const std::string modelId = json::value_to<std::string>(val.at("data").at("modelId"));
                        const std::filesystem::path source = collocation::PromptForModelFile(hwnd);
                        if (!source.empty())
                            collocation::ImportModel(modelId, source);
                        PostSettingsConfig();
                    }
                    else if (type == "collocationModelStatusRequest")
                    {
                        PostSettingsConfig();
                    }
                    else if (type == "openExternalUrl")
                    {
                        const std::string url = json::value_to<std::string>(val.at("data"));
                        if (url.rfind("https://", 0) == 0)
                            ShellExecuteW(hwnd, L"open", string_to_wstring(url).c_str(), nullptr, nullptr,
                                          SW_SHOWNORMAL);
                    }
                    else if (type == "configUpdate")
                    {
                        try
                        {
                            const auto &data = val.at("data").as_object();
                            const std::string path = json::value_to<std::string>(data.at("path"));
                            ApplyInputSubkey(path, data);
                            ApplyAppearanceSubkey(path, data);
                            ApplyGeneralSubkey(path, data);
                            ApplyStatisticsSubkey(path, data);
                            ApplyTencentTmtSubkey(path, data);
                            ApplyCustomTranslationSubkey(path, data);
                            ApplyNetworkSubkey(path, data);
                            ApplyAssociationSubkey(path, data);
                            ApplyUtilitySubkey(path, data);
                            ApplyKeybindingsSubkey(path, data);
                            ApplyHelpcodeSubkey(path, data);
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

static nlohmann::json CustomShuangpinSchemasJson()
{
    nlohmann::json schemas = nlohmann::json::array();
    for (const auto &schema : GetCustomShuangpinSchemas())
    {
        schemas.push_back(
            {{"id", schema.schema}, {"name", schema.name}, {"name_en", schema.name_en}, {"error", schema.error}});
    }
    return schemas;
}

// 与 settings_app.cpp 同一字段：内置皮肤的翻页箭头开关，供外观与皮肤页的预览使用。
static nlohmann::json BuiltinSkinPageArrowsJson()
{
    const std::filesystem::path skinsRoot = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins";
    nlohmann::json result = nlohmann::json::object();
    for (const auto &id : CandidateSkinCatalog::BuiltInIds())
        result[id] = CandidateSkinCatalog::ResolvePageArrows(skinsRoot, id, nullptr);
    return result;
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
    const NetworkProxyConfig network_proxy = GetConfiguredNetworkProxy();
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
            {"wubi_mixed_pinyin", GetConfiguredWubiMixedPinyin()},
            {"escape_keeps_selected_word", GetConfiguredEscapeKeepsSelectedWord()},
            {"enter_learns_english_word", GetConfiguredEnterLearnsEnglishWord()},
            {"wubi_z_mode", GetConfiguredWubiZMode()},
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
            {"sentence_source_badge", GetConfiguredAssocSentenceSourceBadge()},
            {"sentence_collocation_enabled", GetConfiguredAssocSentenceCollocationEnabled()},
            {"sentence_collocation_model_status",
             [] {
                 nlohmann::json statuses = nlohmann::json::object();
                 for (const auto &[model_id, status] : collocation::GetModelStatuses())
                 {
                     statuses[model_id] = nlohmann::json{
                         {"state", status.state}, {"progress", status.progress}, {"error", status.error}};
                 }
                 return statuses;
             }()},
            {"sentence_collocation_model", GetConfiguredAssocSentenceCollocationModel()},
            {"sentence_collocation_catalog",
             [] {
                 // 页面不自持目录副本：id 与下载 URL 留在 Server 侧，只下发展示字段。
                 nlohmann::json catalog = nlohmann::json::array();
                 for (const auto &entry : collocation::Catalog())
                 {
                     catalog.push_back(nlohmann::json{{"id", entry.id},
                                                      {"displayName", entry.display_name},
                                                      {"sizeHint", entry.size_hint},
                                                      {"license", entry.license},
                                                      {"url", collocation::SourceUrl(entry)}});
                 }
                 return catalog;
             }()}}},
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
          {"network", {{"proxy_mode", network_proxy.mode}, {"proxy_server", network_proxy.server}}},
          {"utility",
           {{"unicode_mode", GetConfiguredUnicodeModeEnabled()},
            {"quick_phrase", GetConfiguredQuickPhraseEnabled()},
            {"quick_phrase_candidates", GetConfiguredQuickPhraseCandidatesEnabled()},
            {"quick_phrase_frequency", GetConfiguredQuickPhraseFrequencyEnabled()},
            {"mixed_candidates", GetConfiguredMixedCandidatesEnabled()},
            {"date_time_candidates", GetConfiguredDateTimeCandidatesEnabled()},
            {"date_time_menu", GetConfiguredDateTimeMenuEnabled()},
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
            {"candidate_window_preedit_shuangpin_quanpin", GetConfiguredCandidateWindowPreeditShuangpinQuanpin()},
            {"candidate_fixed_badge", GetConfiguredCandidateFixedBadge()},
            {"candidate_fixed_badge_style", GetConfiguredCandidateFixedBadgeStyle()},
            {"tsf_preedit_style", GetConfiguredTsfPreeditStyle()},
            {"tsf_preedit_shuangpin_quanpin", GetConfiguredTsfPreeditShuangpinQuanpin()},
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
            {"system_fonts", GetSystemFontFamilies()},
            {"builtin_skin_page_arrows", BuiltinSkinPageArrowsJson()}}},
          {"helpcode",
           {{"shuangpin_helpcode", GetConfiguredShuangpinHelpcodeEnabled()},
            {"shuangpin_mid_sentence_helpcode", GetConfiguredShuangpinMidSentenceHelpcodeEnabled()},
            {"shuangpin_mid_sentence_helpcode_backtick", GetConfiguredShuangpinMidSentenceHelpcodeBacktick()},
            {"shuangpin_mid_sentence_helpcode_semicolon", GetConfiguredShuangpinMidSentenceHelpcodeSemicolon()},
            {"shuangpin_helpcode_schema", GetConfiguredShuangpinHelpcodeSchema()},
            {"quanpin_helpcode", GetConfiguredQuanpinHelpcodeEnabled()},
            {"quanpin_helpcode_schema", GetConfiguredQuanpinHelpcodeSchema()},
            {"show_sp_helpcode_in_candidate_window", GetConfiguredShowShuangpinHelpcodeInCandidateWindow()},
            {"show_qp_helpcode_in_candidate_window", GetConfiguredShowQuanpinHelpcodeInCandidateWindow()}}},
          {"statistics",
           {{"enabled", GetConfiguredStatisticsEnabled()}, {"retention", GetConfiguredStatisticsRetention()}}}}}};
    payload["data"]["helpcode"]["custom_schemas"] = CustomHelpcodeSchemasJson();
    payload["data"]["helpcode"]["custom_directory"] = GetCustomHelpcodeDirectory();
    payload["data"]["input"]["custom_shuangpin_schemas"] = CustomShuangpinSchemasJson();
    payload["data"]["input"]["custom_shuangpin_directory"] = GetCustomShuangpinDirectory();
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
