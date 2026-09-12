#pragma once

#include "engine/core/scheme_type.h"
#include <toml++/toml.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct VoiceInputConfig
{
    bool enabled = true;
    bool hotkey_ralt = true;
    bool hotkey_ctrl_f9 = true;
    bool hotkey_ctrl_win = false;
    bool hotkey_rctrl_ralt = false;
    bool hotkey_hold_space_lock = true;
    // doubao | openai | siliconflow | groq
    std::string asr_provider = "doubao";
    std::string asr_app_key;
    std::string asr_token;
    // Per-provider tokens so switching ASR/polish providers restores the matching key.
    std::map<std::string, std::string> asr_tokens;
    std::string asr_endpoint = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";
    std::string asr_resource_id = "volc.seedasr.sauc.duration";
    bool doubao_enable_itn = true;
    bool doubao_enable_punc = true;
    bool doubao_enable_ddc = false;
    std::string doubao_boosting_table_id;
    // OpenAI-compatible transcriptions model. Unused for doubao streaming.
    std::string asr_model;
    // siliconflow | openai | deepseek | groq
    std::string polish_provider = "siliconflow";
    std::string polish_token;
    std::map<std::string, std::string> polish_tokens;
    std::string polish_endpoint = "https://api.siliconflow.cn/v1/chat/completions";
    std::string polish_model;
    // cleanup | faithful | zh2en | casual | custom_1 | custom_2 | custom_3
    std::string polish_prompt_id = "cleanup";
    // Legacy single custom prompt. Retained for config compatibility.
    std::string polish_prompt;
    std::string polish_prompt_custom_1;
    std::string polish_prompt_custom_2;
    std::string polish_prompt_custom_3;
    std::string language = "zh-cn";
    bool start_sound = true;
    bool end_sound = true;
    // Temporarily mute other apps' playback while the microphone is recording.
    bool mute_system_audio = false;
    bool polish_text = false;
    // When true, Doubao streaming partials are shown as TSF inline preedit.
    bool stream_inline_preedit = false;
    // tsf | sendinput | ctrl_v
    std::string commit_mode = "tsf";
};

struct AiAssistantConfig
{
    bool enabled = false;
    // deepseek | openai | siliconflow | groq (all use Chat Completions)
    std::string provider = "deepseek";
    std::string token;
    std::map<std::string, std::string> tokens;
    std::string endpoint = "https://api.deepseek.com/chat/completions";
    std::string model = "deepseek-v4-flash";
    int candidate_limit = 3;
    // custom_1 | custom_2 | custom_3
    std::string prompt_id = "custom_1";
    std::string prompt_custom_1;
    std::string prompt_custom_2;
    std::string prompt_custom_3;
    // Resolved active prompt. `prompt` is also retained as the legacy slot-one key.
    std::string prompt =
        R"PROMPT(你是一个中文全拼输入法联想引擎。输入为已经切分好的拼音数组、前文上下文和候选数量。

优先生成与拼音严格对应的中文候选：若有 N 段拼音，首选必须尽量为 N 个汉字，每段拼音对应一个汉字，不得随意增删或改变读音。结合上下文、常用程度、语义完整性和固定搭配排序。

若去掉分词后能明显组成更合理的英文单词、缩写、产品名或技术术语，如 `deep + seek → DeepSeek`、`git + hub → GitHub`，可优先返回英文；不要生造英文或做牵强匹配。

只输出合法 JSON，不要解释或输出 Markdown：

{
"candidates": [
{
"text": "候选内容",
"type": "chinese或english",
"confidence": 0.98
}
]
}

候选按推荐程度降序排列，数量不超过指定上限；没有合理结果时返回空数组。)PROMPT";
};

struct TencentTmtConfig
{
    bool enabled = true;
    std::string secret_id;
    std::string secret_key;
    std::string region = "ap-guangzhou";
    // Language shown beside Chinese candidates. Tencent TMT language codes:
    // en / fr / ja / es / ru / de / ko.
    std::string target_language = "en";
};

struct CustomTranslationConfig
{
    bool enabled = false;
    // Full URL of a DeepLX-compatible POST endpoint.
    std::string endpoint;
    // Optional bearer token used by private or protected deployments.
    std::string api_key;
};

struct FrequencyAdjustmentConfig
{
    std::string mode = "promote"; // disabled | pin | halve | linear | promote
    int trigger_count = 1;
    int linear_step = 1;
};

void InitImeConfig();
void InvalidateImeConfigWriteTime();
void NotifyImeServerConfigChanged();
void NotifyImeServerCandidateSkinRefresh();
void NotifyImeServerInputSchemeChanged();
// 请求输入法后台进程重启（由 Watchdog 拉起）。返回是否成功送达。
bool NotifyImeServerRestart();
// 升级用：以新版模板为骨架重建配置。用户改过的值（与 baseline 中的旧默认值不同）保留，
// 其余跟随新默认值；模板里没有的旧键被丢弃。baseline 为空时一律保留用户值。
std::string MergeConfigIntoTemplate(const std::string &template_text, const std::string &user_text,
                                    const std::string &baseline_text);
bool ReloadImeConfigIfChanged();
const std::filesystem::path &GetImeConfigPath();
const std::string &GetConfiguredSessionBackend();
int GetConfiguredCandidatePageSize();
bool SetConfiguredCandidatePageSize(int page_size);
const std::string &GetConfiguredCandidateFont();
bool SetConfiguredCandidateFont(const std::string &font);
const std::string &GetConfiguredCandidateEnglishFont();
bool SetConfiguredCandidateEnglishFont(const std::string &font);
const std::string &GetConfiguredCandidateDefaultFont();
const std::vector<std::string> &GetConfiguredCandidateFallbackFonts();
bool SetConfiguredCandidateFallbackFonts(const std::vector<std::string> &fonts);
std::vector<std::string> GetConfiguredCandidateFallbackFontFamilies();
bool SetConfiguredCandidateDefaultFont(const std::string &font);
int GetConfiguredCandidateFontSize();
bool SetConfiguredCandidateFontSize(int font_size);
int GetConfiguredCandidateWindowPreeditFontSize();
bool SetConfiguredCandidateWindowPreeditFontSize(int font_size);
bool GetConfiguredDiagnosticLogEnabled();
bool SetConfiguredDiagnosticLogEnabled(bool enabled);
bool GetConfiguredTsfDiagnosticLogEnabled();
bool SetConfiguredTsfDiagnosticLogEnabled(bool enabled);
const std::string &GetConfiguredCandidateTextColor();
bool SetConfiguredCandidateTextColor(const std::string &color);
// Installed font family names for settings dropdowns (cached after first call).
const std::vector<std::string> &GetSystemFontFamilies();
// Resolve a GDI/legacy face name (for example "Family W03") to the
// typographic family name Chromium expects in CSS. Returns the input on failure.
std::string ResolveSystemFontFamilyForCss(const std::string &font);
SchemeType GetConfiguredInputScheme();
// The Chinese scheme is preserved while Japanese mode is active.
SchemeType GetConfiguredActiveInputScheme();
std::string GetConfiguredInputSchemeName();
bool SetConfiguredInputScheme(const std::string &scheme);
const std::string &GetConfiguredInputMode();
bool SetConfiguredInputMode(const std::string &mode);
const std::string &GetConfiguredJapaneseSchema();
bool SetConfiguredJapaneseSchema(const std::string &schema);
const std::string &GetConfiguredCharacterSet();
bool SetConfiguredCharacterSet(const std::string &character_set);
bool GetConfiguredCharacterSetShortcutEnabled();
bool SetConfiguredCharacterSetShortcutEnabled(bool enabled);
// "chinese" | "english" — default CN/EN when activating this IME.
const std::string &GetConfiguredDefaultImeMode();
bool SetConfiguredDefaultImeMode(const std::string &mode);
// "app" | "global" — per-app memory vs unified CN/EN across apps.
const std::string &GetConfiguredImeModeScope();
bool SetConfiguredImeModeScope(const std::string &scope);
bool IsConfiguredImeModeScopeGlobal();
bool GetConfiguredSwitchLanguageShiftEnabled();
bool SetConfiguredSwitchLanguageShiftEnabled(bool enabled);
bool GetConfiguredSwitchLanguageCtrlEnabled();
bool SetConfiguredSwitchLanguageCtrlEnabled(bool enabled);
bool GetConfiguredSwitchLanguageCtrlAltSpaceEnabled();
bool SetConfiguredSwitchLanguageCtrlAltSpaceEnabled(bool enabled);
const std::string &GetConfiguredShuangpinSchema();
bool SetConfiguredShuangpinSchema(const std::string &schema);
const std::string &GetConfiguredWubiSchema();
bool SetConfiguredWubiSchema(const std::string &schema);
const std::string &GetConfiguredShuangpinPreeditMode();
const std::string &GetConfiguredTsfPreeditStyle();
bool SetConfiguredTsfPreeditStyle(const std::string &style);
bool GetConfiguredShuangpinHelpcodeEnabled();
bool SetConfiguredShuangpinHelpcodeEnabled(bool enabled);
const std::string &GetConfiguredShuangpinHelpcodeSchema();
bool SetConfiguredShuangpinHelpcodeSchema(const std::string &schema);
bool GetConfiguredQuanpinHelpcodeEnabled();
bool SetConfiguredQuanpinHelpcodeEnabled(bool enabled);
bool GetConfiguredQuanpinAutocorrectTransposition();
bool SetConfiguredQuanpinAutocorrectTransposition(bool enabled);
bool GetConfiguredQuanpinAutocorrectNeighbor();
bool SetConfiguredQuanpinAutocorrectNeighbor(bool enabled);
const std::string &GetConfiguredQuanpinHelpcodeSchema();
bool SetConfiguredQuanpinHelpcodeSchema(const std::string &schema);
bool GetConfiguredShowShuangpinHelpcodeInCandidateWindow();
bool SetConfiguredShowShuangpinHelpcodeInCandidateWindow(bool enabled);
bool GetConfiguredShowQuanpinHelpcodeInCandidateWindow();
bool SetConfiguredShowQuanpinHelpcodeInCandidateWindow(bool enabled);
bool GetConfiguredFloatingToolbarEnabled();
bool SetConfiguredFloatingToolbarEnabled(bool enabled);
struct FloatingToolbarItemsConfig
{
    bool fullwidth = true;
    bool punctuation = true;
    bool character_set = true;
    bool emoji = true;
    bool screen_keyboard = false;
    bool settings = true;
};
const FloatingToolbarItemsConfig &GetConfiguredFloatingToolbarItems();
bool SetConfiguredFloatingToolbarItemEnabled(const std::string &item, bool enabled);
// User scale independent of system DPI (0.75 / 1.0 / 1.25 / 1.5).
double GetConfiguredFloatingToolbarScale();
bool SetConfiguredFloatingToolbarScale(double scale);
// Icon box size in CSS px before user scale (16–28, default 24).
int GetConfiguredFloatingToolbarFontSize();
bool SetConfiguredFloatingToolbarFontSize(int font_size);
bool GetConfiguredEnglishCandidatesEnabled();
bool SetConfiguredEnglishCandidatesEnabled(bool enabled);
bool GetConfiguredCandidateTranslationsEnabled();
bool SetConfiguredCandidateTranslationsEnabled(bool enabled);
int GetConfiguredEnglishMixedInputMinChars();
bool SetConfiguredEnglishMixedInputMinChars(int min_chars);
bool GetConfiguredEmojiMixedInputEnabled();
bool SetConfiguredEmojiMixedInputEnabled(bool enabled);
bool GetConfiguredKaomojiMixedInputEnabled();
bool SetConfiguredKaomojiMixedInputEnabled(bool enabled);
bool GetConfiguredCloudCandidatesEnabled();
bool SetConfiguredCloudCandidatesEnabled(bool enabled);
bool GetConfiguredUnicodeModeEnabled();
bool SetConfiguredUnicodeModeEnabled(bool enabled);
bool GetConfiguredQuickPhraseEnabled();
bool SetConfiguredQuickPhraseEnabled(bool enabled);
bool GetConfiguredDateTimeModeEnabled();
bool SetConfiguredDateTimeModeEnabled(bool enabled);
bool GetConfiguredEmojiModeEnabled();
bool SetConfiguredEmojiModeEnabled(bool enabled);
bool GetConfiguredKaomojiModeEnabled();
bool SetConfiguredKaomojiModeEnabled(bool enabled);
bool GetConfiguredJianpinModeEnabled();
bool SetConfiguredJianpinModeEnabled(bool enabled);
bool GetConfiguredYModeEnabled();
bool SetConfiguredYModeEnabled(bool enabled);
bool GetConfiguredNumberModeEnabled();
bool SetConfiguredNumberModeEnabled(bool enabled);
bool GetConfiguredRModeEnabled();
bool SetConfiguredRModeEnabled(bool enabled);
bool GetConfiguredClipboardHistoryEnabled();
bool SetConfiguredClipboardHistoryEnabled(bool enabled);
bool GetConfiguredPagingMinusEqualEnabled();
bool SetConfiguredPagingMinusEqualEnabled(bool enabled);
bool GetConfiguredPagingCommaPeriodEnabled();
bool SetConfiguredPagingCommaPeriodEnabled(bool enabled);
bool GetConfiguredPagingBracketsEnabled();
bool SetConfiguredPagingBracketsEnabled(bool enabled);
// Worker payload for PagingCommaPeriodChanged: "0|raw" / "1|pinyin" / "0|empty".
// Legacy TSF only reads data[0] as the paging flag and ignores the rest.
std::wstring FormatPagingCommaPeriodWorkerPayload();
// Worker payload for LowercaseNumberModeChanged: "1" when a leading lowercase v
// starts number mode. Only quanpin qualifies; shuangpin uses v as an initial.
std::wstring FormatLowercaseNumberModeWorkerPayload();
bool GetConfiguredPagingTabEnabled();
bool SetConfiguredPagingTabEnabled(bool enabled);
bool GetConfiguredPagingPageUpDownEnabled();
bool SetConfiguredPagingPageUpDownEnabled(bool enabled);
bool GetConfiguredCandidateArrowNavigationEnabled();
bool SetConfiguredCandidateArrowNavigationEnabled(bool enabled);
bool GetConfiguredWordToCharacterEnabled();
bool SetConfiguredWordToCharacterEnabled(bool enabled);
std::string GetConfiguredWordToCharacterKeys();
bool SetConfiguredWordToCharacterKeys(const std::string &keys);
bool GetConfiguredSmartPunctuationEnabled();
bool SetConfiguredSmartPunctuationEnabled(bool enabled);
bool GetConfiguredSmartPunctuationRepeatToChineseEnabled();
bool SetConfiguredSmartPunctuationRepeatToChineseEnabled(bool enabled);
bool GetConfiguredPairedPunctuationEnabled();
bool SetConfiguredPairedPunctuationEnabled(bool enabled);
// "follow" | "chinese" | "english" — punctuation stays put when switching CN/EN.
const std::string &GetConfiguredPunctuationLock();
bool SetConfiguredPunctuationLock(const std::string &lock);
// Worker payload for PunctuationLockChanged: "0" follow / "1" Chinese / "2" English.
std::wstring FormatPunctuationLockWorkerPayload();
const std::string &GetConfiguredCandidateWindowLayout();
bool SetConfiguredCandidateWindowLayout(const std::string &layout);
bool GetConfiguredCandidateWindowFollowCursor();
bool SetConfiguredCandidateWindowFollowCursor(bool enabled);
// "d2d" | "webview2" — candidate, floating toolbar, and tray menu renderer.
// Changing this writes config immediately; the process snapshot does not switch
// until the IME server is restarted.
const std::string &GetConfiguredUiBackend();
bool SetConfiguredUiBackend(const std::string &backend);
// Process-lifetime renderer chosen at InitImeConfig. Independent of later reloads.
bool UseD2dSmallWindowUi();
// "fluent" | "wechat" | "graphite" | "willow_green" — candidate-window and floating-toolbar skin.
const std::string &GetConfiguredCandidateSkin();
bool SetConfiguredCandidateSkin(const std::string &skin);
const std::string &GetConfiguredCandidateWindowPreeditStyle();
bool SetConfiguredCandidateWindowPreeditStyle(const std::string &style);
const std::string &GetConfiguredThemeMode();
bool SetConfiguredThemeMode(const std::string &mode);
const std::string &GetConfiguredThemeSettings();
bool SetConfiguredThemeSettings(const std::string &theme);
const std::string &GetConfiguredThemeCand();
bool SetConfiguredThemeCand(const std::string &theme);
const std::string &GetConfiguredThemeFtb();
bool SetConfiguredThemeFtb(const std::string &theme);
const std::string &GetConfiguredThemeMenu();
bool SetConfiguredThemeMenu(const std::string &theme);
const std::string &GetConfiguredThemeEmoji();
bool SetConfiguredThemeEmoji(const std::string &theme);
const std::string &GetConfiguredThemeScreenKeyboard();
bool SetConfiguredThemeScreenKeyboard(const std::string &theme);
const std::string &GetConfiguredThemeHandwriting();
bool SetConfiguredThemeHandwriting(const std::string &theme);
const std::string &GetConfiguredThemeVoice();
bool SetConfiguredThemeVoice(const std::string &theme);
// Resolve effective "dark" / "light" for a surface override ("follow" | "dark" | "light").
std::string ResolveConfiguredTheme(const std::string &surface_theme);
bool IsSystemAppsLightTheme();
// Returns a snapshot by value: the backing config is rewritten by the IPC worker thread while the voice control thread
// and the low-level keyboard hook read it, so callers must never hold a reference into it.
VoiceInputConfig GetConfiguredVoiceInput();
bool SetConfiguredVoiceInputString(const std::string &key, const std::string &value);
bool SetConfiguredVoiceInputBool(const std::string &key, bool value);
const AiAssistantConfig &GetConfiguredAiAssistant();
const TencentTmtConfig &GetConfiguredTencentTmt();
bool SetConfiguredTencentTmtString(const std::string &key, const std::string &value);
const CustomTranslationConfig &GetConfiguredCustomTranslation();
bool SetConfiguredCustomTranslationBool(const std::string &key, bool value);
bool SetConfiguredCustomTranslationString(const std::string &key, const std::string &value);
const FrequencyAdjustmentConfig &GetConfiguredFrequencyAdjustment();
bool SetConfiguredFrequencyAdjustmentString(const std::string &key, const std::string &value);
bool SetConfiguredFrequencyAdjustmentInt(const std::string &key, int value);
bool SetConfiguredAiAssistantString(const std::string &key, const std::string &value);
bool SetConfiguredAiAssistantBool(const std::string &key, bool value);
bool SetConfiguredAiAssistantInt(const std::string &key, int value);
