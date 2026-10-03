#pragma once

#include "engine/core/fuzzy_pinyin_options.h"
#include "engine/core/scheme_type.h"
#include "engine/shuangpin/shuangpin_profile.h"
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
    // Doubao console generation: api_key (new console, single API Key) | legacy (App ID + Access Token).
    // Empty means "infer from asr_app_key", which keeps configs written before this setting working.
    std::string doubao_auth_mode = "api_key";
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
    std::map<std::string, std::string> endpoints{{"deepseek", "https://api.deepseek.com/chat/completions"},
                                                 {"openai", "https://api.openai.com/v1/chat/completions"},
                                                 {"siliconflow", "https://api.siliconflow.cn/v1/chat/completions"},
                                                 {"groq", "https://api.groq.com/openai/v1/chat/completions"}};
    std::map<std::string, std::string> models{{"deepseek", "deepseek-v4-flash"},
                                              {"openai", "gpt-4o-mini"},
                                              {"siliconflow", "Qwen/Qwen3-8B"},
                                              {"groq", "llama-3.3-70b-versatile"}};
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

struct NiuTransConfig
{
    bool enabled = false;
    // 控制台->API应用 中的应用唯一标识与 apikey。
    std::string app_id;
    std::string apikey;
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
// 本机输入统计总开关，默认关闭：关闭时 Server 不创建也不写 stats.db。
bool GetConfiguredStatisticsEnabled();
bool SetConfiguredStatisticsEnabled(bool enabled);
// 统计保留策略：forever|30d|90d|180d|365d，默认 forever。缺键或非法值一律回落
// forever——坏配置绝不能导致自动清理误删数据；setter 校验枚举才落盘。
const std::string &GetConfiguredStatisticsRetention();
bool SetConfiguredStatisticsRetention(const std::string &retention);
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
// 当前双拼方案是否把 ing 放在分号键上（微软、搜狗、紫光），此时分号在音节第二位是输入键。
bool IsConfiguredShuangpinSemicolonFinal();
// 内置方案名，或 custom/<文件名> 形式的自定义方案；自定义方案要能解析通过才会写入。
bool SetConfiguredShuangpinSchema(const std::string &schema);
// 用户放在 <数据目录>/shuangpin/custom/*.toml 的自定义双拼，每次调用都重新扫描和解析。
// 解析失败的文件也列出来，error 写明原因，设置页据此把它显示成不可选。
struct CustomShuangpinSchemaInfo
{
    std::string schema;
    std::string name;
    std::string name_en;
    std::string error;
};
std::vector<CustomShuangpinSchemaInfo> GetCustomShuangpinSchemas();
// UTF-8 path of the folder GetCustomShuangpinSchemas scans.
std::string GetCustomShuangpinDirectory();
// 把一份自定义双拼 TOML 文本解析成 profile（profile.name 取 schema）。errors 为空才算成功。
struct ParsedCustomShuangpin
{
    std::string name;
    std::string name_en;
    ShuangpinProfile profile;
    std::vector<std::string> errors;
};
ParsedCustomShuangpin ParseCustomShuangpinSchema(const std::string &toml_text, const std::string &schema);
const std::string &GetConfiguredWubiSchema();
bool SetConfiguredWubiSchema(const std::string &schema);
// 五笔拼音混输：独立开关，同一串字母同时给五笔码表和全拼。
bool GetConfiguredWubiMixedPinyin();
bool SetConfiguredWubiMixedPinyin(bool enabled);
// 造词时 Esc 只清未选拼音、保留已选词（小狼毫风格）；关闭时 Esc 直接整体取消。
bool GetConfiguredEscapeKeepsSelectedWord();
bool SetConfiguredEscapeKeepsSelectedWord(bool enabled);
bool GetConfiguredEnterLearnsEnglishWord();
bool SetConfiguredEnterLearnsEnglishWord(bool enabled);
// 五笔 z 键角色："off" | "wildcard"。混输已拆成独立开关，这里只剩通配两态。
const std::string &GetConfiguredWubiZMode();
bool SetConfiguredWubiZMode(const std::string &mode);
const std::string &GetConfiguredShuangpinPreeditMode();
const std::string &GetConfiguredTsfPreeditStyle();
bool SetConfiguredTsfPreeditStyle(const std::string &style);
// 双拼方案下行内预编辑显示转换后的全拼：原始按键样式不带分词符号，拼音分词样式带。
// 开启后原始按键样式在双拼下由 Server 回包驱动，TSF 实际收到的样式见 GlobalSettings。
bool GetConfiguredTsfPreeditShuangpinQuanpin();
bool SetConfiguredTsfPreeditShuangpinQuanpin(bool enabled);
bool GetConfiguredShuangpinHelpcodeEnabled();
bool SetConfiguredShuangpinHelpcodeEnabled(bool enabled);
// 双拼句中辅助码（helpcode.shuangpin_mid_sentence_helpcode），默认关闭。
bool GetConfiguredShuangpinMidSentenceHelpcodeEnabled();
bool SetConfiguredShuangpinMidSentenceHelpcodeEnabled(bool enabled);
// 句中辅助码的触发键，可多选：反引号（helpcode.shuangpin_mid_sentence_helpcode_backtick，默认开）
// 和分号（helpcode.shuangpin_mid_sentence_helpcode_semicolon，默认关）。
bool GetConfiguredShuangpinMidSentenceHelpcodeBacktick();
bool SetConfiguredShuangpinMidSentenceHelpcodeBacktick(bool enabled);
bool GetConfiguredShuangpinMidSentenceHelpcodeSemicolon();
bool SetConfiguredShuangpinMidSentenceHelpcodeSemicolon(bool enabled);
// 字符 ch（'`' 或 ';'）此刻是不是一个开着的句中辅助码触发键：总开关开着且勾了这个键。
bool IsConfiguredMidSentenceHelpcodeTrigger(wchar_t ch);
// MidSentenceHelpcodeChanged 的载荷："1" 表示开关开着、勾了反引号且当前方案是双拼。
std::wstring FormatMidSentenceHelpcodeWorkerPayload();
// MidSentenceHelpcodeSemicolonChanged 的载荷：同上，换成分号。
std::wstring FormatMidSentenceHelpcodeSemicolonWorkerPayload();
const std::string &GetConfiguredShuangpinHelpcodeSchema();
bool SetConfiguredShuangpinHelpcodeSchema(const std::string &schema);
bool GetConfiguredQuanpinHelpcodeEnabled();
bool SetConfiguredQuanpinHelpcodeEnabled(bool enabled);
bool GetConfiguredQuanpinAutocorrectTransposition();
bool SetConfiguredQuanpinAutocorrectTransposition(bool enabled);
bool GetConfiguredQuanpinAutocorrectNeighbor();
bool SetConfiguredQuanpinAutocorrectNeighbor(bool enabled);
// Fuzzy pinyin rules are flat [input] booleans ("fuzzy_z_zh", ...) so the settings page can
// bind them per key and the upgrade merge keeps user choices per key. Callers get the
// synthesized engine options and never touch the bitmask themselves.
// The master switch ("fuzzy_pinyin") gates GetConfiguredFuzzyPinyinOptions only: sessions see
// all-zero rules while it is off, but the stored rule choices stay intact so flipping it back
// on restores them.
// First enable seeds: SetConfiguredFuzzyPinyinEnabled(true) while the internal marker key
// "fuzzy_seeded" is still false batch-writes all 11 rules true plus the marker in one atomic
// file write. Later toggles only touch the master switch, so a user's pruned selection
// survives disable/enable cycles; the marker also prevents re-seeding an intentionally empty
// selection. The marker is never sent to the settings page.
bool GetConfiguredFuzzyPinyinEnabled();
bool SetConfiguredFuzzyPinyinEnabled(bool enabled);
metasequoia::FuzzyPinyinOptions GetConfiguredFuzzyPinyinOptions();
// Rule bits without the master gate, for the settings snapshot: checkboxes must show the
// stored choices even while the master switch is off.
metasequoia::FuzzyPinyinOptions GetConfiguredFuzzyPinyinRuleStates();
// key is the config key without section, e.g. "fuzzy_n_l"; unknown keys return false.
bool SetConfiguredFuzzyPinyinRule(const std::string &key, bool enabled);
const std::string &GetConfiguredQuanpinHelpcodeSchema();
bool SetConfiguredQuanpinHelpcodeSchema(const std::string &schema);
// User helpcode tables under <data>/helpcodes/custom, rescanned on every call so a file dropped
// there shows up the next time the settings page asks. Names fall back to the file stem.
struct CustomHelpcodeSchemaInfo
{
    std::string schema;
    std::string name;
    std::string name_en;
};
std::vector<CustomHelpcodeSchemaInfo> GetCustomHelpcodeSchemas();
// UTF-8 path of the folder GetCustomHelpcodeSchemas scans.
std::string GetCustomHelpcodeDirectory();
bool GetConfiguredShowShuangpinHelpcodeInCandidateWindow();
bool SetConfiguredShowShuangpinHelpcodeInCandidateWindow(bool enabled);
bool GetConfiguredShowQuanpinHelpcodeInCandidateWindow();
bool SetConfiguredShowQuanpinHelpcodeInCandidateWindow(bool enabled);
bool GetConfiguredFloatingToolbarEnabled();
bool SetConfiguredFloatingToolbarEnabled(bool enabled);
bool GetConfiguredCaretStateIndicatorEnabled();
bool SetConfiguredCaretStateIndicatorEnabled(bool enabled);
// Also announce the current mode when focus moves into another text field,
// in the same window or another one. Only meaningful while the indicator
// itself is enabled.
bool GetConfiguredCaretStateIndicatorOnFocus();
bool SetConfiguredCaretStateIndicatorOnFocus(bool enabled);
const std::string &GetConfiguredCaretStateIndicatorPosition();
bool SetConfiguredCaretStateIndicatorPosition(const std::string &position);
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
// Fade the toolbar out after it has been shown this many seconds (1–60,
// default 5) without hover; a CN/EN-style state change shows it again.
bool GetConfiguredFloatingToolbarAutoHide();
bool SetConfiguredFloatingToolbarAutoHide(bool enabled);
int GetConfiguredFloatingToolbarAutoHideDelay();
bool SetConfiguredFloatingToolbarAutoHideDelay(int seconds);
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
// 整句候选来源与去重补位开关（[association] 段），默认全关。
bool GetConfiguredAssocSentenceWordLattice();
bool SetConfiguredAssocSentenceWordLattice(bool enabled);
bool GetConfiguredAssocSentenceGoogle();
bool SetConfiguredAssocSentenceGoogle(bool enabled);
bool GetConfiguredAssocSentenceNeuralDesktop();
bool SetConfiguredAssocSentenceNeuralDesktop(bool enabled);
bool GetConfiguredAssocSentenceNeuralKeyboard();
bool SetConfiguredAssocSentenceNeuralKeyboard(bool enabled);
bool GetConfiguredAssocSentenceShowNextOnDuplicate();
bool SetConfiguredAssocSentenceShowNextOnDuplicate(bool enabled);
bool GetConfiguredAssocSentenceSourceBadge();
bool SetConfiguredAssocSentenceSourceBadge(bool enabled);
// octagram 语法模型（八股文，[association] 段）：两个机制开关进设置页，模型 id 默认留空
// = 未选择任何模型（设置页单选激活后写入），权重是配置文件里的高级调参项，不进设置页。
bool GetConfiguredAssocSentenceCollocationEnabled();
bool SetConfiguredAssocSentenceCollocationEnabled(bool enabled);
bool GetConfiguredAssocSentenceCollocationRerankWeight();
std::string GetConfiguredAssocSentenceCollocationModel();
bool SetConfiguredAssocSentenceCollocationModel(const std::string &model_id);
double GetConfiguredAssocSentenceCollocationWeight();
bool GetConfiguredUnicodeModeEnabled();
bool SetConfiguredUnicodeModeEnabled(bool enabled);
// Shift+K 快捷短语模式。
bool GetConfiguredQuickPhraseEnabled();
bool SetConfiguredQuickPhraseEnabled(bool enabled);
// 全拼/双拼里把编码精确匹配的快捷短语混进候选，与 K 模式各自开关。
bool GetConfiguredQuickPhraseCandidatesEnabled();
bool SetConfiguredQuickPhraseCandidatesEnabled(bool enabled);
// 快捷短语参与调频：关掉后快捷短语组固定在首位，选择也不再学习。
bool GetConfiguredQuickPhraseFrequencyEnabled();
bool SetConfiguredQuickPhraseFrequencyEnabled(bool enabled);
// 候选混输总开关：关掉后英文、emoji、颜文字、快捷短语、日期时间都不再混进候选，各自的开关值保留。
bool GetConfiguredMixedCandidatesEnabled();
bool SetConfiguredMixedCandidatesEnabled(bool enabled);
// 全拼/双拼里输入 rq / sj / xq 等唤醒词时，把当前日期、时间、星期混进候选。
bool GetConfiguredDateTimeCandidatesEnabled();
bool SetConfiguredDateTimeCandidatesEnabled(bool enabled);
// 日期时间混入候选时，第三位的「📅日期」入口：选中后候选框换成全部日期格式。依附 date_time_candidates。
bool GetConfiguredDateTimeMenuEnabled();
bool SetConfiguredDateTimeMenuEnabled(bool enabled);
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
bool GetConfiguredPagingTabEnabled();
bool SetConfiguredPagingTabEnabled(bool enabled);
bool GetConfiguredPagingPageUpDownEnabled();
bool SetConfiguredPagingPageUpDownEnabled(bool enabled);
// Mouse wheel over the candidate window. Default off: the wheel only reaches a
// never-focused NOACTIVATE host when Windows' "scroll inactive windows when I
// hover over them" is enabled, so this cannot be promised unconditionally.
bool GetConfiguredPagingMouseWheelEnabled();
bool SetConfiguredPagingMouseWheelEnabled(bool enabled);
bool GetConfiguredCandidateArrowNavigationEnabled();
bool SetConfiguredCandidateArrowNavigationEnabled(bool enabled);
bool GetConfiguredWordToCharacterEnabled();
bool SetConfiguredWordToCharacterEnabled(bool enabled);
std::string GetConfiguredWordToCharacterKeys();
bool SetConfiguredWordToCharacterKeys(const std::string &keys);
bool GetConfiguredSmartPunctuationEnabled();
bool SetConfiguredSmartPunctuationEnabled(bool enabled);
bool GetConfiguredSmartPunctuationSpaceConvertEnabled();
bool SetConfiguredSmartPunctuationSpaceConvertEnabled(bool enabled);
bool GetConfiguredSmartPunctuationDirectDigitEnabled();
bool SetConfiguredSmartPunctuationDirectDigitEnabled(bool enabled);
bool GetConfiguredSmartPunctuationDirectLetterEnabled();
bool SetConfiguredSmartPunctuationDirectLetterEnabled(bool enabled);
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
// 固定排位候选词徽标：总开关 + 样式枚举（"paperclip" | "pushpin" | "dot"）。
bool GetConfiguredCandidateFixedBadge();
bool SetConfiguredCandidateFixedBadge(bool enabled);
const std::string &GetConfiguredCandidateFixedBadgeStyle();
bool SetConfiguredCandidateFixedBadgeStyle(const std::string &style);
// 设置窗口关闭后隐藏驻留多久再退出进程：
// "off" | "1m" | "5m" | "10m" | "30m" | "60m" | "forever"，默认 "10m"。
const std::string &GetConfiguredSettingsWindowLinger();
bool SetConfiguredSettingsWindowLinger(const std::string &linger);
// "d2d" | "webview2" — candidate, floating toolbar, and tray menu renderer.
// Changing this writes config immediately; the process snapshot does not switch
// until the IME server is restarted.
const std::string &GetConfiguredUiBackend();
bool SetConfiguredUiBackend(const std::string &backend);
// Process-lifetime renderer chosen at InitImeConfig. Independent of later reloads.
bool UseD2dSmallWindowUi();
// "fluent" | "wechat" | "graphite" | "willow_green" | "autumn_osmanthus" | "microsoft"
// — candidate-window and floating-toolbar skin.
const std::string &GetConfiguredCandidateSkin();
bool SetConfiguredCandidateSkin(const std::string &skin);
const std::string &GetConfiguredCandidateWindowPreeditStyle();
bool SetConfiguredCandidateWindowPreeditStyle(const std::string &style);
// 双拼方案下候选窗预编辑显示转换后的全拼分词。
bool GetConfiguredCandidateWindowPreeditShuangpinQuanpin();
bool SetConfiguredCandidateWindowPreeditShuangpinQuanpin(bool enabled);
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
const NiuTransConfig &GetConfiguredNiuTrans();
bool SetConfiguredNiuTransBool(const std::string &key, bool value);
bool SetConfiguredNiuTransString(const std::string &key, const std::string &value);
const FrequencyAdjustmentConfig &GetConfiguredFrequencyAdjustment();
bool SetConfiguredFrequencyAdjustmentString(const std::string &key, const std::string &value);
bool SetConfiguredFrequencyAdjustmentInt(const std::string &key, int value);
bool SetConfiguredAiAssistantString(const std::string &key, const std::string &value);
bool SetConfiguredAiAssistantBool(const std::string &key, bool value);
bool SetConfiguredAiAssistantInt(const std::string &key, int value);
