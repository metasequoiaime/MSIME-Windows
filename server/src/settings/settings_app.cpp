#include "engine/contracts/webview/validator.h"
#include "config/ime_config.h"
#include "global/globals.h"
#include "resource/resource.h"
#include "settings/settings_launcher.h"
#include "settings/settings_splash.h"
#include "settings/dictionary_manager.h"
#include "settings/serial_task_queue.h"
#include <memory>
#include <stdexcept>
#include "skin/candidate_skin_catalog.h"
#include "utils/common_utils.h"
#include "utils/single_instance.h"
#include "voice-input/voice_providers.h"

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <WebView2EnvironmentOptions.h>
#include <boost/json.hpp>
#include <dcomp.h>
#include <dwmapi.h>
#include <nlohmann/json.hpp>
#include <wil/com.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wrl.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
namespace json = boost::json;

namespace
{
constexpr wchar_t kWindowClass[] = L"MetasequoiaImeSettingsWindow";
constexpr wchar_t kWindowTitle[] = L"Metasequoia IME Settings";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\MetasequoiaImeSettings.SingleInstance";
constexpr UINT kActivateExistingWindow = WM_APP + 1;
constexpr UINT kOpenAboutSection = WM_APP + 2;
constexpr UINT kScanSkinCatalog = WM_APP + 3;
constexpr UINT kWorkerCompleted = WM_APP + 4;
constexpr UINT kQuitSettings = WM_APP + 5;
constexpr UINT_PTR kConfigReloadTimer = 1;
constexpr UINT_PTR kLingerTimer = 2;
// 关闭窗口先只隐藏，进程留着已导航完成的 WebView2；这段时间内重新打开就不用再冷启动一次。
constexpr UINT kLingerTimeoutMs = 10 * 60 * 1000;

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2CompositionController> g_composition_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<ICoreWebView2_3> g_webview3;
ComPtr<ICoreWebView2Controller2> g_controller2;
ComPtr<IDCompositionDevice> g_dcomp_device;
ComPtr<IDCompositionTarget> g_dcomp_target;
ComPtr<IDCompositionVisual> g_dcomp_root;
RECT g_maximize_button_rect{};
bool g_has_maximize_button_rect = false;
bool g_maximize_button_hover = false;
bool g_window_active = true;
bool g_window_minimized = false;
bool g_open_about_on_ready = false;
bool g_webview_content_started = false;
HWND g_settings_hwnd = nullptr;
std::optional<CandidateSkinCatalog::ScanResult> g_candidate_skin_catalog;
uint64_t g_candidate_skin_catalog_revision = 0;
std::unique_ptr<SerialTaskQueue> g_worker;
bool g_closing = false;
bool g_lingering = false;
bool g_reload_pending = false;
bool g_settings_light = false;
std::wstring g_last_config_message;
bool g_worker_com_initialized = false; // worker-only
void CloseSettings(HWND hwnd);
void HideSettings(HWND hwnd);

nlohmann::json CandidateColorsToJson(const CandidateSkinCatalog::CandidateColors &colors)
{
    nlohmann::json json = nlohmann::json::object();
    if (!colors.accent.empty())
        json["accent"] = colors.accent;
    if (!colors.selected.empty())
        json["selected"] = colors.selected;
    if (!colors.hover.empty())
        json["hover"] = colors.hover;
    if (!colors.surface.empty())
        json["surface"] = colors.surface;
    if (!colors.border.empty())
        json["border"] = colors.border;
    if (!colors.text.empty())
        json["text"] = colors.text;
    if (!colors.number.empty())
        json["number"] = colors.number;
    if (colors.showSelectedBar.has_value())
        json["showSelectedBar"] = *colors.showSelectedBar;
    return json;
}

void ShowAboutSection()
{
    if (!g_webview)
        return;
    g_webview->ExecuteScript(LR"JS((() => {
        const selectAbout = () => {
            const item = document.querySelector('.sidebar .item[data-target="about-settings"]');
            if (!item) return false;
            if (!item.classList.contains('active')) item.click();
            const section = document.getElementById('about-settings');
            if (!item.classList.contains('active') || !section || getComputedStyle(section).display === 'none')
                return false;
            item.scrollIntoView({ block: 'nearest' });
            return true;
        };
        if (selectAbout()) return;
        const retry = setInterval(() => {
            if (selectAbout()) clearInterval(retry);
        }, 50);
        setTimeout(() => clearInterval(retry), 10000);
    })())JS",
                             nullptr);
}

void ApplyWindowActivationAppearance()
{
    if (!g_webview)
        return;

    // Keep this effect in the native host so it follows the real top-level
    // activation state rather than DOM focus. Only title-bar foreground
    // elements are muted; the page and title-bar backgrounds remain intact.
    const wchar_t *script = g_window_active ? LR"JS((() => {
            document.getElementById('metasequoia-window-inactive-overlay')?.remove();
            const id = 'metasequoia-window-activation-style';
            if (!document.getElementById(id)) {
                const style = document.createElement('style');
                style.id = id;
                style.textContent = `
                    .titlebar-left, .window-button {
                        transition: opacity 140ms ease-out;
                    }
                    html.metasequoia-window-inactive .titlebar-left,
                    html.metasequoia-window-inactive .window-button {
                        opacity: 0.52 !important;
                    }
                    html.metasequoia-window-inactive .window-button:hover,
                    html.metasequoia-window-inactive .window-button.host-hover {
                        opacity: 1 !important;
                    }
                `;
                document.head.appendChild(style);
            }
            document.documentElement.classList.remove('metasequoia-window-inactive');
        })())JS"
                                            : LR"JS((() => {
            document.getElementById('metasequoia-window-inactive-overlay')?.remove();
            const id = 'metasequoia-window-activation-style';
            if (!document.getElementById(id)) {
                const style = document.createElement('style');
                style.id = id;
                style.textContent = `
                    .titlebar-left, .window-button {
                        transition: opacity 140ms ease-out;
                    }
                    html.metasequoia-window-inactive .titlebar-left,
                    html.metasequoia-window-inactive .window-button {
                        opacity: 0.52 !important;
                    }
                    html.metasequoia-window-inactive .window-button:hover,
                    html.metasequoia-window-inactive .window-button.host-hover {
                        opacity: 1 !important;
                    }
                `;
                document.head.appendChild(style);
            }
            document.documentElement.classList.add('metasequoia-window-inactive');
        })())JS";
    g_webview->ExecuteScript(script, nullptr);
}

void ApplySettingsChromeTheme(HWND hwnd = nullptr)
{
    const HWND target = hwnd ? hwnd : g_settings_hwnd;
    const bool light = g_settings_light;
    if (target)
    {
        BOOL dark = light ? FALSE : TRUE;
        DwmSetWindowAttribute(target, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
    if (g_controller2)
    {
        COREWEBVIEW2_COLOR background =
            light ? COREWEBVIEW2_COLOR{255, 243, 243, 243} : COREWEBVIEW2_COLOR{255, 32, 32, 32};
        g_controller2->put_DefaultBackgroundColor(background);
    }
}

void ResetTitlebarHoverAfterVisibilityChange()
{
    if (!g_webview)
        return;

    g_webview->ExecuteScript(LR"JS((() => {
        const root = document.documentElement;
        const suppressClass = 'metasequoia-suppress-titlebar-tooltip';
        const styleId = 'metasequoia-titlebar-tooltip-reset-style';
        if (!document.getElementById(styleId)) {
            const style = document.createElement('style');
            style.id = styleId;
            style.textContent = `
                html.${suppressClass} .window-button[data-tooltip]::after,
                html.${suppressClass} .window-button[data-tooltip]:hover::after {
                    opacity: 0 !important;
                    transform: translateX(-50%) !important;
                    transition-delay: 0s !important;
                }
            `;
            document.head.appendChild(style);
        }

        root.classList.add(suppressClass);
        document.querySelector('.window-controls')
            ?.classList.add('window-controls-click-reset');
        document.querySelectorAll('.window-button').forEach((button) => {
            button.classList.remove('host-hover', 'host-active');
            if (button instanceof HTMLElement) button.blur();
        });

        const previousRelease = window.__metasequoiaReleaseTooltipReset;
        if (typeof previousRelease === 'function') {
            window.removeEventListener('pointermove', previousRelease, true);
            window.removeEventListener('pointerdown', previousRelease, true);
        }
        const release = () => {
            root.classList.remove(suppressClass);
            window.removeEventListener('pointermove', release, true);
            window.removeEventListener('pointerdown', release, true);
            delete window.__metasequoiaReleaseTooltipReset;
        };
        window.__metasequoiaReleaseTooltipReset = release;
        window.addEventListener('pointermove', release, {capture: true, once: true});
        window.addEventListener('pointerdown', release, {capture: true, once: true});
    })())JS",
                             nullptr);
}

std::wstring BuildConfigMessage(bool refresh_skin_catalog)
{

    const VoiceInputConfig &voice = GetConfiguredVoiceInput();
    const AiAssistantConfig &ai = GetConfiguredAiAssistant();
    const TencentTmtConfig &tencent_tmt = GetConfiguredTencentTmt();
    const CustomTranslationConfig &custom_translation = GetConfiguredCustomTranslation();
    const FrequencyAdjustmentConfig &frequency = GetConfiguredFrequencyAdjustment();
    const FloatingToolbarItemsConfig &toolbar = GetConfiguredFloatingToolbarItems();
    const std::filesystem::path skins_root =
        std::filesystem::path(CommonUtils::get_local_appdata_path_w()) / GlobalIme::AppName / L"skins";
    if (refresh_skin_catalog)
    {
        g_candidate_skin_catalog = CandidateSkinCatalog::Scan(skins_root);
        ++g_candidate_skin_catalog_revision;
    }
    const std::string skin_layout = GetConfiguredCandidateWindowLayout();
    const std::string skin_theme = ResolveConfiguredTheme(GetConfiguredThemeCand());
    nlohmann::json external_skins = nlohmann::json::array();
    if (g_candidate_skin_catalog)
    {
        for (const auto &skin : g_candidate_skin_catalog->packages)
        {
            external_skins.push_back(
                {{"id", skin.id},
                 {"name", skin.name},
                 {"version", skin.version},
                 {"author", skin.author},
                 {"description", skin.description},
                 {"base", skin.base},
                 {"toolbarStylesheet", skin.toolbarStylesheet},
                 {"preview", skin.preview},
                 {"layouts", skin.layouts},
                 {"themes", skin.themes},
                 {"minWidthDip", skin.minWidthDip},
                 {"decorationTopDip", skin.decorationTopDip},
                 {"decorationWidthDip", skin.decorationWidthDip},
                 {"candidate",
                  {{"dark", CandidateColorsToJson(skin.dark)}, {"light", CandidateColorsToJson(skin.light)}}},
                 {"compatible", CandidateSkinCatalog::Supports(skin, skin_layout, skin_theme)}});
        }
    }
    nlohmann::json skin_issues = nlohmann::json::array();
    if (g_candidate_skin_catalog)
    {
        for (const auto &issue : g_candidate_skin_catalog->issues)
            skin_issues.push_back({{"folder", issue.folder}, {"reason", issue.reason}});
    }
    nlohmann::json polish_presets = nlohmann::json::array();
    for (const auto &preset : VoiceInput::BuiltinPolishPromptPresets())
    {
        polish_presets.push_back({{"id", std::string(preset.id)},
                                  {"name", std::string(preset.name)},
                                  {"prompt", std::string(preset.prompt)}});
    }
    // 规则位取不门控视图：总开关关闭时复选框仍要展示用户已存的勾选；
    // 总开关本身单独下发。
    const metasequoia::FuzzyPinyinOptions fuzzy_rules = GetConfiguredFuzzyPinyinRuleStates();
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
            {"punctuation_lock", GetConfiguredPunctuationLock()},
            {"fuzzy_pinyin", GetConfiguredFuzzyPinyinEnabled()},
            {"fuzzy_z_zh", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::Z_ZH)},
            {"fuzzy_c_ch", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::C_CH)},
            {"fuzzy_s_sh", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::S_SH)},
            {"fuzzy_n_l", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::N_L)},
            {"fuzzy_f_h", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::F_H)},
            {"fuzzy_r_l", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::R_L)},
            {"fuzzy_an_ang", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::AN_ANG)},
            {"fuzzy_en_eng", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::EN_ENG)},
            {"fuzzy_in_ing", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::IN_ING)},
            {"fuzzy_ian_iang", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::IAN_IANG)},
            {"fuzzy_uan_uang", fuzzy_rules.enabled(metasequoia::FuzzyPinyinRule::UAN_UANG)}}},
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
            {"paging_mouse_wheel", GetConfiguredPagingMouseWheelEnabled()},
            {"candidate_arrow_navigation", GetConfiguredCandidateArrowNavigationEnabled()}}},
          {"keybindings",
           {{"switch_language_shift", GetConfiguredSwitchLanguageShiftEnabled()},
            {"switch_language_ctrl", GetConfiguredSwitchLanguageCtrlEnabled()},
            {"switch_language_ctrl_alt_space", GetConfiguredSwitchLanguageCtrlAltSpaceEnabled()},
            {"toggle_character_set_ctrl_shift_f", GetConfiguredCharacterSetShortcutEnabled()}}},
          {"frequency_adjustment",
           {{"mode", frequency.mode},
            {"trigger_count", frequency.trigger_count},
            {"linear_step", frequency.linear_step}}},
          {"utility",
           {{"unicode_mode", GetConfiguredUnicodeModeEnabled()},
            {"quick_phrase", GetConfiguredQuickPhraseEnabled()},
            {"date_time_mode", GetConfiguredDateTimeModeEnabled()},
            {"emoji_mode", GetConfiguredEmojiModeEnabled()},
            {"kaomoji_mode", GetConfiguredKaomojiModeEnabled()},
            {"jianpin_mode", GetConfiguredJianpinModeEnabled()},
            {"y_mode", GetConfiguredYModeEnabled()},
            {"r_mode", GetConfiguredRModeEnabled()},
            {"clipboard_history", GetConfiguredClipboardHistoryEnabled()}}},
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
            {"system_fonts", GetSystemFontFamilies()},
            {"external_candidate_skins", std::move(external_skins)},
            {"candidate_skin_scan_issues", std::move(skin_issues)},
            {"candidate_skin_catalog_scanned", g_candidate_skin_catalog.has_value()},
            {"candidate_skin_catalog_revision", g_candidate_skin_catalog_revision},
            {"candidate_skin_directory", skins_root.u8string()}}},
          {"voice_input",
           {{"enabled", voice.enabled},
            {"hotkey_ralt", voice.hotkey_ralt},
            {"hotkey_ctrl_f9", voice.hotkey_ctrl_f9},
            {"hotkey_ctrl_win", voice.hotkey_ctrl_win},
            {"hotkey_rctrl_ralt", voice.hotkey_rctrl_ralt},
            {"hotkey_hold_space_lock", voice.hotkey_hold_space_lock},
            {"asr_provider", voice.asr_provider},
            {"asr_app_key", voice.asr_app_key},
            {"asr_token", voice.asr_token},
            {"asr_tokens", voice.asr_tokens},
            {"asr_endpoint", voice.asr_endpoint},
            {"asr_resource_id", voice.asr_resource_id},
            {"doubao_enable_itn", voice.doubao_enable_itn},
            {"doubao_enable_punc", voice.doubao_enable_punc},
            {"doubao_enable_ddc", voice.doubao_enable_ddc},
            {"doubao_boosting_table_id", voice.doubao_boosting_table_id},
            {"asr_model", voice.asr_model},
            {"polish_provider", voice.polish_provider},
            {"polish_token", voice.polish_token},
            {"polish_tokens", voice.polish_tokens},
            {"polish_endpoint", voice.polish_endpoint},
            {"polish_model", voice.polish_model},
            {"polish_prompt_id", voice.polish_prompt_id},
            {"polish_prompt", voice.polish_prompt},
            {"polish_prompt_custom_1", voice.polish_prompt_custom_1},
            {"polish_prompt_custom_2", voice.polish_prompt_custom_2},
            {"polish_prompt_custom_3", voice.polish_prompt_custom_3},
            {"language", voice.language},
            {"start_sound", voice.start_sound},
            {"end_sound", voice.end_sound},
            {"mute_system_audio", voice.mute_system_audio},
            {"notification_sound", voice.start_sound && voice.end_sound},
            {"polish_text", voice.polish_text},
            {"stream_inline_preedit", voice.stream_inline_preedit},
            {"commit_mode", voice.commit_mode}}},
          {"ai_assistant",
           {{"enabled", ai.enabled},
            {"provider", ai.provider},
            {"token", ai.token},
            {"tokens", ai.tokens},
            {"endpoint", ai.endpoint},
            {"model", ai.model},
            {"candidate_limit", ai.candidate_limit},
            {"prompt", ai.prompt},
            {"prompt_id", ai.prompt_id},
            {"prompt_custom_1", ai.prompt_custom_1},
            {"prompt_custom_2", ai.prompt_custom_2},
            {"prompt_custom_3", ai.prompt_custom_3}}},
          {"tencent_tmt",
           {{"secret_id", tencent_tmt.secret_id},
            {"secret_key", tencent_tmt.secret_key},
            {"region", tencent_tmt.region},
            {"target_language", tencent_tmt.target_language}}},
          {"custom_translation",
           {{"enabled", custom_translation.enabled},
            {"endpoint", custom_translation.endpoint},
            {"api_key", custom_translation.api_key}}},
          {"helpcode",
           {{"shuangpin_helpcode", GetConfiguredShuangpinHelpcodeEnabled()},
            {"shuangpin_helpcode_schema", GetConfiguredShuangpinHelpcodeSchema()},
            {"quanpin_helpcode", GetConfiguredQuanpinHelpcodeEnabled()},
            {"quanpin_helpcode_schema", GetConfiguredQuanpinHelpcodeSchema()},
            {"show_sp_helpcode_in_candidate_window", GetConfiguredShowShuangpinHelpcodeInCandidateWindow()},
            {"show_qp_helpcode_in_candidate_window", GetConfiguredShowQuanpinHelpcodeInCandidateWindow()}}},
          {"quanpin",
           {{"autocorrect_transposition", GetConfiguredQuanpinAutocorrectTransposition()},
            {"autocorrect_neighbor", GetConfiguredQuanpinAutocorrectNeighbor()}}}}}};
    payload["data"]["voice_input"]["polish_presets"] = std::move(polish_presets);
    payload["protocolVersion"] = metasequoia::webview::Version;
    const std::string serialized = payload.dump();
    if (!metasequoia::webview::Validate(json::parse(serialized), "server"))
        throw std::runtime_error("Invalid config snapshot");
    return string_to_wstring(serialized);
}

SerialTaskQueue::Completion ConfigCompletion(bool refresh_skin_catalog = false)
{
    auto message = BuildConfigMessage(refresh_skin_catalog);
    const bool light = ResolveConfiguredTheme(GetConfiguredThemeSettings()) == "light";
    return [message = std::move(message), light] {
        g_settings_light = light;
        ApplySettingsChromeTheme();
        SettingsSplash::SetTheme(light);
        if (g_webview && message != g_last_config_message)
        {
            if (SUCCEEDED(g_webview->PostWebMessageAsJson(message.c_str())))
                g_last_config_message = message;
        }
    };
}

void PostConfig(bool refresh_skin_catalog = false)
{
    if (!g_worker || g_closing)
        return;
    g_worker->Submit([refresh_skin_catalog] { return ConfigCompletion(refresh_skin_catalog); });
}

void PostValidatedServerMessage(json::object payload)
{
    if (!g_webview)
        return;
    payload["protocolVersion"] = metasequoia::webview::Version;
    if (!metasequoia::webview::Validate(payload, "server"))
        return;
    const std::wstring message = string_to_wstring(json::serialize(payload));
    g_webview->PostWebMessageAsJson(message.c_str());
}

void PostWindowState(HWND hwnd)
{
    PostValidatedServerMessage({
        {"type", "windowState"},
        {"data", {{"isMaximized", IsZoomed(hwnd) != FALSE}}},
    });
}

void PostMaximizeButtonEvent(const char *event_name)
{
    PostValidatedServerMessage({
        {"type", "maxButtonEvent"},
        {"data", {{"event", event_name}}},
    });
}

bool ApplyConfigUpdate(const json::object &data)
{
    const std::string path = json::value_to<std::string>(data.at("path"));
    if (path == "input.mode")
        return SetConfiguredInputMode(json::value_to<std::string>(data.at("value")));
    if (path == "input.schema")
        return SetConfiguredInputScheme(json::value_to<std::string>(data.at("value")));
    if (path == "input.japanese_schema")
        return SetConfiguredJapaneseSchema(json::value_to<std::string>(data.at("value")));
    if (path == "input.character_set")
        return SetConfiguredCharacterSet(json::value_to<std::string>(data.at("value")));
    if (path == "input.default_ime_mode")
        return SetConfiguredDefaultImeMode(json::value_to<std::string>(data.at("value")));
    if (path == "input.ime_mode_scope")
        return SetConfiguredImeModeScope(json::value_to<std::string>(data.at("value")));
    if (path == "input.shuangpin_schema")
        return SetConfiguredShuangpinSchema(json::value_to<std::string>(data.at("value")));
    if (path == "input.wubi_schema")
        return SetConfiguredWubiSchema(json::value_to<std::string>(data.at("value")));
    if (path == "input.word_to_character")
        return SetConfiguredWordToCharacterEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.word_to_character_keys")
        return SetConfiguredWordToCharacterKeys(json::value_to<std::string>(data.at("value")));
    if (path == "input.smart_punctuation")
        return SetConfiguredSmartPunctuationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.smart_punctuation_repeat_to_chinese")
        return SetConfiguredSmartPunctuationRepeatToChineseEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.paired_punctuation")
        return SetConfiguredPairedPunctuationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "input.punctuation_lock")
        return SetConfiguredPunctuationLock(json::value_to<std::string>(data.at("value")));
    constexpr std::string_view input_section_prefix = "input.";
    constexpr std::string_view fuzzy_prefix = "input.fuzzy_";
    // 精确分支必须在前：总开关键同样命中 fuzzy_ 前缀，不能靠 prefix setter 拒绝它。
    if (path == "input.fuzzy_pinyin")
        return SetConfiguredFuzzyPinyinEnabled(json::value_to<bool>(data.at("value")));
    if (path.rfind(fuzzy_prefix, 0) == 0)
        return SetConfiguredFuzzyPinyinRule(path.substr(input_section_prefix.size()),
                                            json::value_to<bool>(data.at("value")));
    if (path == "appearance.tsf_preedit_style")
        return SetConfiguredTsfPreeditStyle(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.ui_backend")
        return SetConfiguredUiBackend(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_window_layout")
        return SetConfiguredCandidateWindowLayout(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_window_follow_cursor")
        return SetConfiguredCandidateWindowFollowCursor(json::value_to<bool>(data.at("value")));
    if (path == "appearance.candidate_skin")
        return SetConfiguredCandidateSkin(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.candidate_window_preedit_style")
        return SetConfiguredCandidateWindowPreeditStyle(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.page_size")
        return SetConfiguredCandidatePageSize(static_cast<int>(data.at("value").as_int64()));
    if (path == "appearance.font")
        return SetConfiguredCandidateFont(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.fallback_fonts")
        return SetConfiguredCandidateFallbackFonts(
            json::value_to<std::vector<std::string>>(json::parse(json::value_to<std::string>(data.at("value")))));
    if (path == "appearance.english_font")
        return SetConfiguredCandidateEnglishFont(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.font_size")
        return SetConfiguredCandidateFontSize(static_cast<int>(data.at("value").as_int64()));
    if (path == "appearance.candidate_window_preedit_font_size")
        return SetConfiguredCandidateWindowPreeditFontSize(static_cast<int>(data.at("value").as_int64()));
    if (path == "appearance.cand_text_color")
        return SetConfiguredCandidateTextColor(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_mode")
        return SetConfiguredThemeMode(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_settings")
        return SetConfiguredThemeSettings(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_cand")
        return SetConfiguredThemeCand(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_ftb")
        return SetConfiguredThemeFtb(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_menu")
        return SetConfiguredThemeMenu(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_emoji")
        return SetConfiguredThemeEmoji(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_screen_keyboard")
        return SetConfiguredThemeScreenKeyboard(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_handwriting")
        return SetConfiguredThemeHandwriting(json::value_to<std::string>(data.at("value")));
    if (path == "appearance.theme_voice")
        return SetConfiguredThemeVoice(json::value_to<std::string>(data.at("value")));
    if (path == "general.floating_toolbar")
        return SetConfiguredFloatingToolbarEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.diagnostic_log" || path == "general.candidate_window_diagnostic_log")
        return SetConfiguredDiagnosticLogEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.tsf_diagnostic_log")
        return SetConfiguredTsfDiagnosticLogEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.floating_toolbar_scale")
    {
        const json::value &value = data.at("value");
        const double scale = value.is_double() ? value.as_double() : static_cast<double>(value.as_int64());
        return SetConfiguredFloatingToolbarScale(scale);
    }
    if (path == "general.floating_toolbar_font_size")
        return SetConfiguredFloatingToolbarFontSize(static_cast<int>(data.at("value").as_int64()));
    constexpr std::string_view toolbar_prefix = "general.floating_toolbar_";
    if (path.rfind(toolbar_prefix, 0) == 0)
        return SetConfiguredFloatingToolbarItemEnabled(path.substr(toolbar_prefix.size()),
                                                       json::value_to<bool>(data.at("value")));
    if (path == "general.cn_en_mixed_input")
        return SetConfiguredEnglishCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_translations")
        return SetConfiguredCandidateTranslationsEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.cn_en_mixed_input_min_chars")
        return SetConfiguredEnglishMixedInputMinChars(static_cast<int>(data.at("value").as_int64()));
    if (path == "general.emoji_mixed_input")
        return SetConfiguredEmojiMixedInputEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.kaomoji_mixed_input")
        return SetConfiguredKaomojiMixedInputEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.cloud_candidates")
        return SetConfiguredCloudCandidatesEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.unicode_mode")
        return SetConfiguredUnicodeModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.quick_phrase")
        return SetConfiguredQuickPhraseEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.date_time_mode")
        return SetConfiguredDateTimeModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.emoji_mode")
        return SetConfiguredEmojiModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.kaomoji_mode")
        return SetConfiguredKaomojiModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.jianpin_mode")
        return SetConfiguredJianpinModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.y_mode")
        return SetConfiguredYModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.r_mode")
        return SetConfiguredRModeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "utility.clipboard_history")
        return SetConfiguredClipboardHistoryEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_minus_equal")
        return SetConfiguredPagingMinusEqualEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_tab")
        return SetConfiguredPagingTabEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_comma_period")
        return SetConfiguredPagingCommaPeriodEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_brackets")
        return SetConfiguredPagingBracketsEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_page_up_down")
        return SetConfiguredPagingPageUpDownEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.paging_mouse_wheel")
        return SetConfiguredPagingMouseWheelEnabled(json::value_to<bool>(data.at("value")));
    if (path == "general.candidate_arrow_navigation")
        return SetConfiguredCandidateArrowNavigationEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.switch_language_shift")
        return SetConfiguredSwitchLanguageShiftEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.switch_language_ctrl")
        return SetConfiguredSwitchLanguageCtrlEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.switch_language_ctrl_alt_space")
        return SetConfiguredSwitchLanguageCtrlAltSpaceEnabled(json::value_to<bool>(data.at("value")));
    if (path == "keybindings.toggle_character_set_ctrl_shift_f")
        return SetConfiguredCharacterSetShortcutEnabled(json::value_to<bool>(data.at("value")));
    if (path.rfind("frequency_adjustment.", 0) == 0)
    {
        const std::string key = path.substr(21);
        const json::value &value = data.at("value");
        if (value.is_string())
            return SetConfiguredFrequencyAdjustmentString(key, json::value_to<std::string>(value));
        if (value.is_int64())
            return SetConfiguredFrequencyAdjustmentInt(key, static_cast<int>(value.as_int64()));
        return false;
    }
    if (path.rfind("voice_input.", 0) == 0)
    {
        const std::string key = path.substr(12);
        const json::value &value = data.at("value");
        if (value.is_bool())
        {
            return SetConfiguredVoiceInputBool(key, json::value_to<bool>(value));
        }
        if (value.is_string())
            return SetConfiguredVoiceInputString(key, json::value_to<std::string>(value));
        return false;
    }
    if (path.rfind("ai_assistant.", 0) == 0)
    {
        const std::string key = path.substr(13);
        const json::value &value = data.at("value");
        if (value.is_bool())
            return SetConfiguredAiAssistantBool(key, json::value_to<bool>(value));
        if (value.is_string())
            return SetConfiguredAiAssistantString(key, json::value_to<std::string>(value));
        if (value.is_int64())
            return SetConfiguredAiAssistantInt(key, static_cast<int>(value.as_int64()));
        return false;
    }
    if (path.rfind("tencent_tmt.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() && SetConfiguredTencentTmtString(path.substr(std::string("tencent_tmt.").size()),
                                                                  json::value_to<std::string>(value));
    }
    if (path == "custom_translation.enabled")
        return SetConfiguredCustomTranslationBool("enabled", json::value_to<bool>(data.at("value")));
    if (path.rfind("custom_translation.", 0) == 0)
    {
        const json::value &value = data.at("value");
        return value.is_string() &&
               SetConfiguredCustomTranslationString(path.substr(std::string("custom_translation.").size()),
                                                    json::value_to<std::string>(value));
    }
    if (path == "helpcode.show_sp_helpcode_in_candidate_window")
        return SetConfiguredShowShuangpinHelpcodeInCandidateWindow(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_helpcode")
        return SetConfiguredShuangpinHelpcodeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.shuangpin_helpcode_schema")
        return SetConfiguredShuangpinHelpcodeSchema(json::value_to<std::string>(data.at("value")));
    if (path == "helpcode.quanpin_helpcode")
        return SetConfiguredQuanpinHelpcodeEnabled(json::value_to<bool>(data.at("value")));
    if (path == "helpcode.quanpin_helpcode_schema")
        return SetConfiguredQuanpinHelpcodeSchema(json::value_to<std::string>(data.at("value")));
    if (path == "helpcode.show_qp_helpcode_in_candidate_window")
        return SetConfiguredShowQuanpinHelpcodeInCandidateWindow(json::value_to<bool>(data.at("value")));
    if (path == "quanpin.autocorrect_transposition")
        return SetConfiguredQuanpinAutocorrectTransposition(json::value_to<bool>(data.at("value")));
    if (path == "quanpin.autocorrect_neighbor")
        return SetConfiguredQuanpinAutocorrectNeighbor(json::value_to<bool>(data.at("value")));
    return false;
}

int HitTestName(const std::string &name)
{
    if (name == "left")
        return HTLEFT;
    if (name == "right")
        return HTRIGHT;
    if (name == "top")
        return HTTOP;
    if (name == "bottom")
        return HTBOTTOM;
    if (name == "left-top")
        return HTTOPLEFT;
    if (name == "right-top")
        return HTTOPRIGHT;
    if (name == "left-bottom")
        return HTBOTTOMLEFT;
    if (name == "right-bottom")
        return HTBOTTOMRIGHT;
    return HTCLIENT;
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring &text)
{
    if (!OpenClipboard(hwnd))
        return false;
    EmptyClipboard();

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
    {
        CloseClipboard();
        return false;
    }

    void *destination = GlobalLock(memory);
    if (!destination)
    {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory))
    {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

void HandleWebMessage(HWND hwnd, ICoreWebView2WebMessageReceivedEventArgs *args)
{
    if (g_closing || !g_worker)
        return;
    wil::unique_cotaskmem_string raw;
    if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw)
        return;

    try
    {
        json::value value = json::parse(wstring_to_string(raw.get()));
        if (!metasequoia::webview::Validate(value, "client", "settings"))
            return;
        const std::string type = json::value_to<std::string>(value.at("type"));
        if (type == "dragStart")
        {
            ReleaseCapture();
            PostMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        else if (type == "resizeHitTest" || type == "resizeStart")
        {
            const int hit_test = HitTestName(json::value_to<std::string>(value.at("data")));
            if (hit_test != HTCLIENT)
            {
                ReleaseCapture();
                PostMessageW(hwnd, WM_NCLBUTTONDOWN, hit_test, 0);
            }
        }
        else if (type == "focus")
        {
            SetFocus(hwnd);
        }
        else if (type == "windowControl")
        {
            const std::string command = json::value_to<std::string>(value.at("data"));
            if (command == "minimize")
                ShowWindow(hwnd, SW_MINIMIZE);
            else if (command == "maximize")
                ShowWindow(hwnd, SW_MAXIMIZE);
            else if (command == "restore")
                ShowWindow(hwnd, SW_RESTORE);
            else if (command == "close")
                HideSettings(hwnd);
        }
        else if (type == "maximizeButtonRect")
        {
            const auto &data = value.at("data").as_object();
            const double x = data.if_contains("x") ? json::value_to<double>(*data.if_contains("x")) : 0.0;
            const double y = data.if_contains("y") ? json::value_to<double>(*data.if_contains("y")) : 0.0;
            const double width = data.if_contains("width") ? json::value_to<double>(*data.if_contains("width")) : 0.0;
            const double height =
                data.if_contains("height") ? json::value_to<double>(*data.if_contains("height")) : 0.0;
            double scale = data.if_contains("dpr") ? json::value_to<double>(*data.if_contains("dpr")) : 0.0;
            if (scale <= 0.0)
                scale = static_cast<double>(GetDpiForWindow(hwnd)) / 96.0;
            g_maximize_button_rect = {static_cast<LONG>(std::lround(x * scale)),
                                      static_cast<LONG>(std::lround(y * scale)),
                                      static_cast<LONG>(std::lround((x + width) * scale)),
                                      static_cast<LONG>(std::lround((y + height) * scale))};
            g_has_maximize_button_rect = true;
        }
        else if (type == "configRequest")
        {
            // A snapshot sent before the new document installed its listener may be lost.
            g_last_config_message.clear();
            PostConfig(false);
            PostMessageW(hwnd, kScanSkinCatalog, 0, 0);
        }
        else if (type == "skinCatalogRequest")
        {
            g_worker->Submit([] {
                auto completion = ConfigCompletion(true);
                NotifyImeServerCandidateSkinRefresh();
                return completion;
            });
        }
        else if (type == "openSkinDirectory")
        {
            const std::filesystem::path skins =
                std::filesystem::path(CommonUtils::get_local_appdata_path_w()) / GlobalIme::AppName / L"skins";
            std::error_code ec;
            std::filesystem::create_directories(skins, ec);
            if (!ec)
                ShellExecuteW(hwnd, L"open", skins.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "configUpdate")
        {
            const auto data = value.at("data").as_object();
            g_worker->Submit([data] {
                const bool saved = ApplyConfigUpdate(data);
                auto completion = ConfigCompletion();
                return [saved, completion = std::move(completion)] {
                    completion();
                    if (!saved)
                        MessageBoxW(g_settings_hwnd, L"设置保存失败，请重试。", L"水杉输入法", MB_OK | MB_ICONWARNING);
                };
            });
        }
        else if (type == "openKeyboardPanel")
        {
            OpenKeyboardPanelApplication();
        }
        else if (type == "openHandwritingPanel")
        {
            OpenHandwritingPanelApplication();
        }
        else if (type == "restartServer")
        {
            if (!NotifyImeServerRestart())
            {
                MessageBoxW(g_settings_hwnd, L"未能重启输入法进程，请稍后重试。", L"水杉输入法",
                            MB_OK | MB_ICONWARNING);
            }
        }
        else if (type == "openExternalUrl")
        {
            const std::string url = json::value_to<std::string>(value.at("data"));
            if (url.rfind("https://", 0) == 0)
                ShellExecuteW(hwnd, L"open", string_to_wstring(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (type == "copyText")
        {
            CopyTextToClipboard(hwnd, string_to_wstring(json::value_to<std::string>(value.at("data"))));
        }
        else if (type == "dictionaryRequest")
        {
            const auto data = value.at("data").as_object();
            g_worker->Submit([data]() -> SerialTaskQueue::Completion {
                json::value response;
                try
                {
                    response = SettingsDictionary::HandleRequest(data);
                }
                catch (...)
                {
                    response =
                        json::object{{"ok", false}, {"message", "词库操作失败，请重试"}, {"rows", json::array{}}};
                }
                auto &object = response.as_object();
                object["type"] = "dictionaryResponse";
                object["requestId"] = data.at("requestId");
                object["action"] = data.at("action");
                object["dictionary"] = data.at("dictionary");
                object["protocolVersion"] = metasequoia::webview::Version;
                if (!metasequoia::webview::Validate(response, "server"))
                    throw std::runtime_error("Invalid dictionary response");
                auto message = string_to_wstring(json::serialize(response));
                return [message = std::move(message)] {
                    if (g_webview)
                        g_webview->PostWebMessageAsJson(message.c_str());
                };
            });
        }
    }
    catch (const std::exception &)
    {
        // Ignore malformed messages from the page; the next snapshot restores its state.
    }
}

HRESULT EnsureCompositionTree(HWND hwnd)
{
    HRESULT hr = DCompositionCreateDevice(nullptr, __uuidof(IDCompositionDevice),
                                          reinterpret_cast<void **>(g_dcomp_device.GetAddressOf()));
    if (FAILED(hr))
        return hr;
    hr = g_dcomp_device->CreateTargetForHwnd(hwnd, TRUE, &g_dcomp_target);
    if (FAILED(hr))
        return hr;
    hr = g_dcomp_device->CreateVisual(&g_dcomp_root);
    if (FAILED(hr))
        return hr;
    hr = g_dcomp_target->SetRoot(g_dcomp_root.Get());
    if (FAILED(hr))
        return hr;
    return g_dcomp_device->Commit();
}

HRESULT OnControllerCreated(HWND hwnd, HRESULT result, ICoreWebView2CompositionController *controller)
{
    if (FAILED(result) || !controller)
    {
        SettingsSplash::Dismiss();
        return FAILED(result) ? result : E_FAIL;
    }
    g_composition_controller = controller;
    if (FAILED(g_composition_controller.As(&g_controller)))
        return E_NOINTERFACE;
    if (FAILED(g_controller->get_CoreWebView2(&g_webview)) || !g_webview)
        return E_FAIL;

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(g_webview->get_Settings(&settings)))
    {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreHostObjectsAllowed(TRUE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
    }
    g_controller->put_ZoomFactor(1.0);

    if (SUCCEEDED(g_webview.As(&g_webview3)))
    {
        const std::filesystem::path assets = std::filesystem::path(CommonUtils::get_local_appdata_path_w()) /
                                             GlobalIme::AppName / "html/webview2/settings/ime-settings/dist";
        g_webview3->SetVirtualHostNameToFolderMapping(L"imesettings", assets.c_str(),
                                                      COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        const std::filesystem::path skins =
            std::filesystem::path(CommonUtils::get_local_appdata_path_w()) / GlobalIme::AppName / L"skins";
        std::error_code ec;
        std::filesystem::create_directories(skins, ec);
        g_webview3->SetVirtualHostNameToFolderMapping(L"candidate-skins", skins.c_str(),
                                                      COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }
    if (SUCCEEDED(g_controller.As(&g_controller2)))
    {
        ApplySettingsChromeTheme(hwnd);
    }

    HRESULT hr = EnsureCompositionTree(hwnd);
    if (FAILED(hr))
        return hr;
    hr = g_composition_controller->put_RootVisualTarget(g_dcomp_root.Get());
    if (FAILED(hr))
        return hr;
    g_dcomp_device->Commit();

    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    g_controller->put_Bounds(bounds);

    EventRegistrationToken token{};
    g_webview->add_ContentLoading(
        Callback<ICoreWebView2ContentLoadingEventHandler>([](ICoreWebView2 *,
                                                             ICoreWebView2ContentLoadingEventArgs *) -> HRESULT {
            g_last_config_message.clear();
            g_webview_content_started = true;
            SettingsSplash::Dismiss();
            return S_OK;
        }).Get(),
        &token);
    g_webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>([hwnd](ICoreWebView2 *,
                                                                      ICoreWebView2NavigationCompletedEventArgs *args)
                                                                   -> HRESULT {
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            SettingsSplash::Dismiss();
            if (success)
            {
                PostWindowState(hwnd);
                PostConfig();
                ApplyWindowActivationAppearance();
                SetFocus(hwnd);
                if (g_open_about_on_ready)
                {
                    ShowAboutSection();
                    g_open_about_on_ready = false;
                }
            }
            return S_OK;
        }).Get(),
        &token);
    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>([hwnd](ICoreWebView2 *,
                                                                     ICoreWebView2WebMessageReceivedEventArgs *args)
                                                                  -> HRESULT {
            HandleWebMessage(hwnd, args);
            return S_OK;
        }).Get(),
        &token);
    return g_webview->Navigate(L"https://imesettings/index.html");
}

void InitWebView(HWND hwnd)
{
    std::filesystem::path user_data = CommonUtils::get_webview2_user_data_path(L"webview2-settings");
    std::error_code ec;
    std::filesystem::create_directories(user_data, ec);
    auto options = Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--disable-features=TranslateUI "
                                            L"--disable-background-networking "
                                            L"--disable-default-apps "
                                            L"--disable-sync "
                                            L"--disable-prompt-on-repost "
                                            L"--no-first-run");
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                if (FAILED(result) || !environment)
                {
                    SettingsSplash::Dismiss();
                    return FAILED(result) ? result : E_FAIL;
                }
                ComPtr<ICoreWebView2Environment3> environment3;
                if (FAILED(environment->QueryInterface(IID_PPV_ARGS(&environment3))))
                {
                    SettingsSplash::Dismiss();
                    return E_NOINTERFACE;
                }
                return environment3->CreateCoreWebView2CompositionController(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                        [hwnd](HRESULT controller_result, ICoreWebView2CompositionController *controller) -> HRESULT {
                            return OnControllerCreated(hwnd, controller_result, controller);
                        })
                        .Get());
            })
            .Get());
}

UINT32 MouseKeys(WPARAM w_param)
{
    UINT32 keys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
    if (w_param & MK_LBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
    if (w_param & MK_RBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_RIGHT_BUTTON;
    if (w_param & MK_MBUTTON)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_MIDDLE_BUTTON;
    if (w_param & MK_SHIFT)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_SHIFT;
    if (w_param & MK_CONTROL)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_CONTROL;
    if (w_param & MK_XBUTTON1)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON1;
    if (w_param & MK_XBUTTON2)
        keys |= COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_X_BUTTON2;
    return keys;
}

bool ForwardMouse(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    if (!g_composition_controller)
        return false;
    COREWEBVIEW2_MOUSE_EVENT_KIND kind{};
    UINT32 data = 0;
    switch (message)
    {
    case WM_MOUSEMOVE:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
        break;
    case WM_LBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
        SetFocus(hwnd);
        break;
    case WM_LBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
        break;
    case WM_LBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK;
        break;
    case WM_RBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
        break;
    case WM_RBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
        break;
    case WM_RBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOUBLE_CLICK;
        break;
    case WM_MBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN;
        break;
    case WM_MBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP;
        break;
    case WM_MBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOUBLE_CLICK;
        break;
    case WM_XBUTTONDOWN:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOWN;
        data = GET_XBUTTON_WPARAM(w_param);
        break;
    case WM_XBUTTONUP:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_UP;
        data = GET_XBUTTON_WPARAM(w_param);
        break;
    case WM_XBUTTONDBLCLK:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_X_BUTTON_DOUBLE_CLICK;
        data = GET_XBUTTON_WPARAM(w_param);
        break;
    case WM_MOUSEWHEEL:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
        data = GET_WHEEL_DELTA_WPARAM(w_param);
        break;
    case WM_MOUSEHWHEEL:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_HORIZONTAL_WHEEL;
        data = GET_WHEEL_DELTA_WPARAM(w_param);
        break;
    case WM_MOUSELEAVE:
        kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE;
        break;
    default:
        return false;
    }
    POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
        ScreenToClient(hwnd, &point);
    if (message == WM_MOUSEMOVE)
    {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
    }
    g_composition_controller->SendMouseInput(
        kind, static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(MouseKeys(w_param)), data, point);
    return true;
}

bool IsMaximizeButton(HWND hwnd, POINT screen_point)
{
    if (!g_has_maximize_button_rect)
        return false;
    ScreenToClient(hwnd, &screen_point);
    return PtInRect(&g_maximize_button_rect, screen_point) != FALSE;
}

int TopNonClientInset(HWND hwnd)
{
    const UINT dpi = GetDpiForWindow(hwnd);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    RECT caption{}, no_caption{};
    AdjustWindowRectExForDpi(&caption, style, FALSE, ex_style, dpi);
    AdjustWindowRectExForDpi(&no_caption, style & ~WS_CAPTION, FALSE, ex_style, dpi);
    return no_caption.top - caption.top + GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// 首帧之前隐藏 WebView2 会卡住 raster 初始化，预热阶段不要碰可见性。
void SetWebViewVisible(bool visible)
{
    if (g_controller && g_webview_content_started)
        g_controller->put_IsVisible(visible ? TRUE : FALSE);
}

// 重新露面：取消延迟退出，恢复渲染与配置轮询。
void CancelLinger(HWND hwnd)
{
    if (!g_lingering)
        return;
    g_lingering = false;
    KillTimer(hwnd, kLingerTimer);
    SetWebViewVisible(true);
    if (g_webview3)
        g_webview3->Resume();
    SetTimer(hwnd, kConfigReloadTimer, 300, nullptr);
}

void ActivateWindow(HWND hwnd)
{
    if (g_closing)
        return;
    CancelLinger(hwnd);
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    else
        ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

// 用户关闭窗口时走这里：只隐藏，进程继续持有导航完成的 WebView2，kLingerTimeoutMs
// 之后才真正退出。隐藏期间停掉配置轮询并让 controller 不可见，避免白拿 CPU。
// 首帧还没出来就被关掉，说明用户不想等这次冷启动，直接退出，别留一个半初始化的进程。
void HideSettings(HWND hwnd)
{
    if (g_closing)
        return;
    if (!g_webview_content_started)
    {
        CloseSettings(hwnd);
        return;
    }
    if (g_lingering)
    {
        ShowWindow(hwnd, SW_HIDE);
        return;
    }
    g_lingering = true;
    // 窗口隐藏时保留的那一帧会在下次打开时先显示出来，带着高亮的标题栏按钮就会闪一下。
    // 页面自己的关闭按钮已经在点击时清过，这里覆盖 Alt+F4、启动图的关闭按钮等不经过页面的路径。
    ResetTitlebarHoverAfterVisibilityChange();
    // 隐藏的窗口收不到 WM_NCMOUSELEAVE，本地这份悬停状态要手动清掉，
    // 否则下次真的移进去时不会再发 enter。
    g_maximize_button_hover = false;
    KillTimer(hwnd, kConfigReloadTimer);
    SettingsSplash::Dismiss();
    ShowWindow(hwnd, SW_HIDE);
    SetWebViewVisible(false);
    SetTimer(hwnd, kLingerTimer, kLingerTimeoutMs, nullptr);
}

void CloseSettings(HWND hwnd)
{
    if (g_closing)
        return;
    g_closing = true;
    g_lingering = false;
    KillTimer(hwnd, kLingerTimer);
    KillTimer(hwnd, kConfigReloadTimer);
    SettingsSplash::Dismiss();
    ShowWindow(hwnd, SW_HIDE);
    if (g_worker)
        g_worker->Stop();
    else
        DestroyWindow(hwnd);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message)
    {
    case kActivateExistingWindow:
        ActivateWindow(hwnd);
        return 0;
    case kOpenAboutSection:
        ActivateWindow(hwnd);
        ShowAboutSection();
        return 0;
    case kScanSkinCatalog:
        PostConfig(true);
        return 0;
    case WM_NCCALCSIZE:
        if (w_param)
        {
            auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(l_param);
            const LRESULT result = DefWindowProcW(hwnd, message, w_param, l_param);
            params->rgrc[0].top -= IsZoomed(hwnd) ? GetSystemMetrics(SM_CYCAPTION) : TopNonClientInset(hwnd);
            return result;
        }
        break;
    case WM_NCHITTEST: {
        const LRESULT result = DefWindowProcW(hwnd, message, w_param, l_param);
        POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        return result == HTCLIENT && IsMaximizeButton(hwnd, point) ? HTMAXBUTTON : result;
    }
    case WM_MOVE:
    case WM_MOVING:
        if (g_controller)
            g_controller->NotifyParentWindowPositionChanged();
        if (SettingsSplash::IsVisible())
            SettingsSplash::SyncToOwner();
        break;
    case WM_SETFOCUS:
        if (g_controller)
            g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        break;
    case WM_ACTIVATE: {
        const bool active = LOWORD(w_param) != WA_INACTIVE;
        if (g_window_active != active)
        {
            g_window_active = active;
            ApplyWindowActivationAppearance();
        }
        if (active)
        {
            if (g_webview3)
                g_webview3->Resume();
            if (g_controller)
                g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        break;
    }
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (ForwardMouse(hwnd, message, w_param, l_param))
            return (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK) ? TRUE : 0;
        break;
    case WM_SETCURSOR:
        if (g_composition_controller && LOWORD(l_param) == HTCLIENT)
        {
            UINT32 cursor = 0;
            if (SUCCEEDED(g_composition_controller->get_SystemCursorId(&cursor)) && cursor)
            {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(cursor)));
                return TRUE;
            }
        }
        break;
    case WM_NCMOUSEMOVE: {
        POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        if (IsMaximizeButton(hwnd, point))
        {
            if (!g_maximize_button_hover)
            {
                g_maximize_button_hover = true;
                PostMaximizeButtonEvent("enter");
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        break;
    }
    case WM_NCMOUSELEAVE:
        if (g_maximize_button_hover)
        {
            g_maximize_button_hover = false;
            PostMaximizeButtonEvent("leave");
        }
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP: {
        POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        if (IsMaximizeButton(hwnd, point))
        {
            PostMaximizeButtonEvent(message == WM_NCLBUTTONDOWN ? "down" : "up");
            return 0;
        }
        break;
    }
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(hwnd, &client);
        const bool light = g_settings_light;
        HBRUSH brush = CreateSolidBrush(light ? RGB(243, 243, 243) : RGB(32, 32, 32));
        FillRect(reinterpret_cast<HDC>(w_param), &client, brush);
        DeleteObject(brush);
        return 1;
    }
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(l_param);
        const UINT dpi = GetDpiForWindow(hwnd);
        info->ptMinTrackSize = {MulDiv(600, dpi, 96), MulDiv(400, dpi, 96)};
        return 0;
    }
    case WM_SIZE: {
        const bool minimized = w_param == SIZE_MINIMIZED;
        if (minimized || g_window_minimized)
            ResetTitlebarHoverAfterVisibilityChange();
        g_window_minimized = minimized;
        if (g_controller)
        {
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            g_controller->put_Bounds(bounds);
            PostWindowState(hwnd);
        }
        if (SettingsSplash::IsVisible())
            SettingsSplash::SyncToOwner();
        break;
    }
    case WM_TIMER:
        if (w_param == kLingerTimer)
        {
            CloseSettings(hwnd);
            return 0;
        }
        if (w_param == kConfigReloadTimer && g_worker && !g_closing && !g_reload_pending)
        {
            g_reload_pending = true;
            g_worker->Submit([] {
                SerialTaskQueue::Completion completion;
                try
                {
                    if (ReloadImeConfigIfChanged())
                        completion = ConfigCompletion();
                }
                catch (...)
                { /* Retry on the next timer tick; do not poison the queue. */
                }
                return [completion = std::move(completion)] {
                    g_reload_pending = false;
                    if (completion)
                        completion();
                };
            });
        }
        return 0;
    case kWorkerCompleted:
        if (g_worker)
        {
            g_worker->Drain(!g_closing);
            if (g_closing && g_worker->Stopped())
                DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        HideSettings(hwnd);
        return 0;
    case kQuitSettings:
        CloseSettings(hwnd);
        return 0;
    case WM_DESTROY:
        g_closing = true;
        g_lingering = false;
        if (g_worker)
            g_worker->Stop();
        KillTimer(hwnd, kLingerTimer);
        KillTimer(hwnd, kConfigReloadTimer);
        SettingsSplash::Dismiss();
        g_webview.Reset();
        g_controller2.Reset();
        g_controller.Reset();
        g_composition_controller.Reset();
        g_dcomp_root.Reset();
        g_dcomp_target.Reset();
        g_dcomp_device.Reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, w_param, l_param);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show_command)
{
    g_open_about_on_ready = command_line && std::wstring(command_line).find(L"--about") != std::wstring::npos;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result))
        return 1;

    CommonUtils::SingleInstanceGuard single_instance(kSingleInstanceMutex);
    if (!single_instance.is_valid())
        return 1;
    if (single_instance.already_running())
    {
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing)
        {
            PostMessageW(existing, g_open_about_on_ready ? kOpenAboutSection : kActivateExistingWindow, 0, 0);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_IME_ICON));
    window_class.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SETTINGS_ICON));
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class))
        return 1;

    const UINT dpi = GetDpiForSystem();
    const int width = MulDiv(900, dpi, 96);
    const int height = MulDiv(710, dpi, 96);
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr,
                                nullptr, instance, nullptr);
    if (!hwnd)
        return 1;
    g_settings_hwnd = hwnd;
    g_worker = std::make_unique<SerialTaskQueue>(
        [hwnd] { PostMessageW(hwnd, kWorkerCompleted, 0, 0); },
        [] { MessageBoxW(g_settings_hwnd, L"设置操作失败，请重试。", L"水杉输入法", MB_OK | MB_ICONWARNING); },
        [] {
            if (g_worker_com_initialized)
                CoUninitialize();
        });
    g_worker->Submit([] {
        g_worker_com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        InitImeConfig();
        return ConfigCompletion();
    });

    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_NONE;
    ApplySettingsChromeTheme(hwnd);
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    MARGINS margins{};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    // CreateWindow sends the initial WM_NCCALCSIZE before the DWM/custom-frame
    // attributes above are installed. Force a second frame calculation now;
    // otherwise the native caption remains until the first minimize/restore.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    SetTimer(hwnd, kConfigReloadTimer, 300, nullptr);
    InitWebView(hwnd);
    ShowWindow(hwnd, show_command == SW_HIDE ? SW_SHOWNORMAL : show_command);
    UpdateWindow(hwnd);
    SettingsSplash::Show(hwnd, g_settings_light);
    if (g_webview_content_started)
        SettingsSplash::Dismiss();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_worker.reset(); // Drain accepted writes even if the window was destroyed externally.
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
