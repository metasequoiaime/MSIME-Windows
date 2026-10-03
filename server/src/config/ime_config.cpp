#include "window/ui_backend_policy.h"
#include "window/caret_state_indicator_policy.h"
#include "ime_config.h"
#include "config/ime_config_internal.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>
#include "utils/common_utils.h"
#include "global/globals.h"
#include "clipboard/clipboard_history.h"
#include "engine/common/helpcode_utils.h"
#include "engine/core/data_path.h"
#include "statistics/stats_store.h"
#include "voice-input/voice_providers.h"

using namespace ime_config_detail;

namespace ime_config_detail
{
// A custom schema whose file was removed falls back to the default instead of loading an empty table.
bool IsHelpcodeSchemaAvailable(const std::string &schema)
{
    return HelpcodeUtils::is_helpcode_schema_available(metasequoia::data_directory(), schema);
}
} // namespace ime_config_detail

namespace
{
std::string g_session_backend = "legacy";
} // namespace

namespace ime_config_detail
{
SchemeType g_input_scheme = SchemeType::Shuangpin;
std::string g_input_mode = "chinese";
std::string g_japanese_schema = "romaji";
std::string g_character_set = "simplified";
std::string g_default_ime_mode = "chinese";
std::string g_ime_mode_scope = "app";
bool g_switch_language_shift_enabled = true;
bool g_switch_language_ctrl_enabled = false;
bool g_switch_language_ctrl_alt_space_enabled = true;
bool g_character_set_shortcut_enabled = true;
int g_candidate_page_size = 8;
std::string g_candidate_font = "Noto Sans SC";
std::string g_candidate_english_font = "Segoe UI";
std::string g_candidate_default_font = "Microsoft YaHei";
std::vector<std::string> g_candidate_fallback_fonts = {"Noto Sans SC", "Microsoft YaHei"};
int g_candidate_font_size = 16;
int g_candidate_window_preedit_font_size = 16;
std::atomic_bool g_diagnostic_log_enabled{false};
std::atomic_bool g_tsf_diagnostic_log_enabled{false};
// 本机输入统计，默认关闭。关闭是整个统计链路的门控：线程收到帧也只丢弃，
// 不会创建 stats.db，这是隐私契约里可观察的那一半。
std::atomic_bool g_statistics_enabled{false};
// 统计保留策略（forever|30d|90d|180d|365d），默认永久保留。
std::string g_statistics_retention = "forever";
std::string g_candidate_text_color = "auto";
std::string g_shuangpin_schema = "xiaohe";
std::string g_wubi_schema = "wubi86";
// 五笔拼音混输是独立开关：同一串字母同时给五笔码表和全拼，五笔候选在前。与 z 键角色无关。
bool g_wubi_mixed_pinyin = false;
// 造词时 Esc 只清掉还没选的拼音、保留已选的词（小狼毫风格），第二次 Esc 才整体取消。默认关闭。
bool g_escape_keeps_selected_word = false;
// 回车上屏英文时顺带写入英文词库。默认开启，保持原有行为。
bool g_enter_learns_english_word = true;
// 五笔 z 键角色两态：off | wildcard。非法值回落 off（升级用户零变化）。
std::string g_wubi_z_mode = "off";
std::string g_shuangpin_preedit_mode = "quanpin";
std::string g_tsf_preedit_style = "raw";
// 双拼方案下行内 / 候选窗预编辑显示转换后的全拼（原始按键去掉分词符号），默认关闭。
bool g_tsf_preedit_shuangpin_quanpin = false;
bool g_candidate_window_preedit_shuangpin_quanpin = false;
bool g_shuangpin_helpcode_enabled = true;
bool g_shuangpin_mid_sentence_helpcode_enabled = false;
bool g_shuangpin_mid_sentence_helpcode_backtick = true;
bool g_shuangpin_mid_sentence_helpcode_semicolon = false;
bool g_quanpin_helpcode_enabled = true;
std::string g_shuangpin_helpcode_schema = "lantian";
std::string g_quanpin_helpcode_schema = "lantian";
bool g_show_shuangpin_helpcode_in_candidate_window = true;
bool g_show_quanpin_helpcode_in_candidate_window = true;
// The legacy single "quanpin.autocorrect" key is deliberately not read anymore:
// both correction types default to off and users opt in from the settings page.
bool g_quanpin_autocorrect_transposition = false;
bool g_quanpin_autocorrect_neighbor = false;
// Bitmask of FuzzyPinyinRule bits. All off by default so upgrades never change behavior.
std::uint32_t g_fuzzy_pinyin_rules = 0;
// Master switch, off by default. It gates GetConfiguredFuzzyPinyinOptions only; the rule
// bitmask above keeps its value so temporary disable/enable preserves the user's choices.
bool g_fuzzy_pinyin_enabled = false;
// First-enable seed marker: once set, toggling the master switch never rewrites the rule
// keys. Internal key — never sent to the settings page; if the template loses it, the
// upgrade merge drops it and every user gets re-seeded on the next master-switch flip.
bool g_fuzzy_seeded = false;
bool g_floating_toolbar_enabled = true;
bool g_caret_state_indicator_enabled = false;
bool g_caret_state_indicator_on_focus = false;
std::string g_caret_state_indicator_position = FanyImeUi::kDefaultCaretStatePosition;
FloatingToolbarItemsConfig g_floating_toolbar_items;
double g_floating_toolbar_scale = 1.0;
int g_floating_toolbar_font_size = kFloatingToolbarFontSizeDefault;
bool g_floating_toolbar_auto_hide = false;
int g_floating_toolbar_auto_hide_delay = kFloatingToolbarAutoHideDelayDefault;
bool g_english_candidates_enabled = false;
bool g_candidate_translations_enabled = true;
int g_english_mixed_input_min_chars = kEnglishMixedInputMinCharsDefault;
bool g_cloud_candidates_enabled = true;
// 整句候选来源与去重补位开关：词格、Google 和神经速度档默认开，神经效果档与去重补位默认关。
bool g_assoc_sentence_wordlattice = true;
bool g_assoc_sentence_google = true;
bool g_assoc_sentence_neural_desktop = false;
bool g_assoc_sentence_neural_keyboard = true;
bool g_assoc_sentence_show_next_on_duplicate = false;
// octagram 语法模型（八股文 .gram）：总开关默认关；模型 id 默认空（空 = 未选择任何模型）。
// 权重不进设置页，供配置文件高级调参。
bool g_assoc_sentence_collocation_enabled = false;
std::string g_assoc_sentence_collocation_model;
double g_assoc_sentence_collocation_weight = 0.1;
double g_assoc_sentence_collocation_rerank_weight = 0.05;
// 候选窗整句候选后的来源标签（〔Trigram〕〔神经K〕等），默认不显示。
bool g_assoc_sentence_source_badge = false;
bool g_emoji_mixed_input_enabled = false;
bool g_kaomoji_mixed_input_enabled = false;
bool g_unicode_mode_enabled = true;
bool g_quick_phrase_enabled = true;
bool g_quick_phrase_candidates_enabled = false;
bool g_quick_phrase_frequency_enabled = true;
bool g_mixed_candidates_enabled = true;
bool g_date_time_candidates_enabled = false;
bool g_date_time_menu_enabled = true;
bool g_date_time_mode_enabled = true;
bool g_emoji_mode_enabled = true;
bool g_kaomoji_mode_enabled = true;
bool g_jianpin_mode_enabled = true;
bool g_y_mode_enabled = true;
bool g_r_mode_enabled = true;
bool g_clipboard_history_enabled = false;
bool g_paging_minus_equal_enabled = true;
bool g_paging_comma_period_enabled = false;
bool g_paging_brackets_enabled = false;
bool g_paging_tab_enabled = true;
bool g_paging_page_up_down_enabled = true;
// Off by default: the wheel only reaches the candidate host when Windows'
// "scroll inactive windows on hover" is on, so the feature is opt-in.
bool g_paging_mouse_wheel_enabled = false;
bool g_candidate_arrow_navigation_enabled = true;
bool g_word_to_character_enabled = false;
std::string g_word_to_character_keys = "brackets";
bool g_smart_punctuation_enabled = false;
bool g_smart_punctuation_space_convert_enabled = false;
bool g_smart_punctuation_direct_digit_enabled = false;
bool g_smart_punctuation_direct_letter_enabled = false;
bool g_smart_punctuation_repeat_to_chinese_enabled = false;
bool g_paired_punctuation_enabled = true;
std::string g_punctuation_lock = "follow";
std::string g_candidate_window_layout = "vertical";
bool g_candidate_window_follow_cursor = true;
std::string g_ui_backend = "d2d";
std::string g_ui_backend_active = "d2d";
std::string g_candidate_skin = "fluent";
std::string g_candidate_window_preedit_style = "pinyin";
bool g_candidate_fixed_badge = true;
std::string g_candidate_fixed_badge_style = "paperclip";
std::string g_settings_window_linger = "off";
std::string g_theme_mode = "system";
std::string g_theme_settings = "follow";
std::string g_theme_cand = "follow";
std::string g_theme_ftb = "follow";
std::string g_theme_menu = "follow";
std::string g_theme_emoji = "follow";
std::string g_theme_screen_keyboard = "follow";
std::string g_theme_handwriting = "follow";
std::string g_theme_voice = "follow";
// LoadImeConfig rewrites g_voice_input from the IPC worker thread while the voice control thread and the low-level
// keyboard hook read it, so every access goes through this process-local lock. Unlike ConfigFileLock it is never held
// across file I/O or IPC: a hook that blocks on a cross-process wait would stall typing system-wide.
std::shared_mutex g_voice_input_mutex;
VoiceInputConfig g_voice_input; // guarded by g_voice_input_mutex
} // namespace ime_config_detail

namespace
{
bool g_persist_asr_token_slot = false;
bool g_persist_polish_token_slot = false;
} // namespace

namespace ime_config_detail
{
AiAssistantConfig g_ai_assistant;
TencentTmtConfig g_tencent_tmt;
CustomTranslationConfig g_custom_translation;
NiuTransConfig g_niutrans;
std::mutex g_network_proxy_mutex;
NetworkProxyConfig g_network_proxy;
FrequencyAdjustmentConfig g_frequency_adjustment;
std::filesystem::path g_config_path;
} // namespace ime_config_detail

namespace
{
std::optional<std::filesystem::file_time_type> g_config_last_write_time;
} // namespace

namespace ime_config_detail
{
VoiceInputConfig SnapshotVoiceInput()
{
    std::shared_lock<std::shared_mutex> lock(g_voice_input_mutex);
    return g_voice_input;
}
} // namespace ime_config_detail

namespace
{
void PublishVoiceInput(VoiceInputConfig voice)
{
    std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
    g_voice_input = std::move(voice);
}
} // namespace

namespace ime_config_detail
{
std::string NormalizeSmallWindowUiBackend(const std::string &value)
{
    if (UiBackendPolicy::Resolve(UiBackendPolicy::Surface::Candidate, value) == UiBackendPolicy::Backend::WebView2)
        return "webview2";
    return "d2d";
}

bool IsValidCandidateSkinId(const std::string &skin)
{
    if (skin.empty() || skin.size() > 64 || !std::isalnum(static_cast<unsigned char>(skin.front())))
    {
        return false;
    }
    return std::all_of(skin.begin(), skin.end(), [](unsigned char ch) {
        return std::islower(ch) || std::isdigit(ch) || ch == '.' || ch == '_' || ch == '-';
    });
}

// 固定排位徽标样式白名单。存枚举名而非字面 emoji：TOML/日志/diff 里不出现图形字符，
// 日后新增样式只改这里和展示层的映射。
bool IsValidCandidateFixedBadgeStyle(const std::string &style)
{
    return style == "paperclip" || style == "pushpin" || style == "dot";
}

bool IsValidSettingsWindowLinger(const std::string &linger)
{
    return linger == "off" || linger == "1m" || linger == "5m" || linger == "10m" || linger == "30m" ||
           linger == "60m" || linger == "forever";
}
} // namespace ime_config_detail

namespace
{
const std::vector<std::string_view> &AiAssistantProviders()
{
    static const std::vector<std::string_view> providers{"deepseek", "openai", "siliconflow", "groq"};
    return providers;
}
} // namespace

namespace ime_config_detail
{
std::string AiAssistantTokenSlotKey(std::string_view provider)
{
    const std::string id = VoiceInput::NormalizeProviderId(provider);
    for (const auto known : AiAssistantProviders())
    {
        if (id == known)
            return "token_" + id;
    }
    return {};
}
} // namespace ime_config_detail

namespace
{
template <typename Node> bool TomlFlexibleBool(const Node &node, bool fallback)
{
    if (const auto value = node.template value<bool>())
        return *value;
    if (const auto text = node.template value<std::string>())
    {
        std::string value = *text;
        for (char &ch : value)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (value == "true" || value == "1" || value == "yes" || value == "on")
            return true;
        if (value == "false" || value == "0" || value == "no" || value == "off")
            return false;
    }
    return fallback;
}
} // namespace

namespace ime_config_detail
{
void RememberConfigWriteTime()
{
    std::error_code error;
    const auto write_time = std::filesystem::last_write_time(g_config_path, error);
    if (!error)
    {
        g_config_last_write_time = write_time;
    }
}
} // namespace ime_config_detail

namespace
{
bool LoadImeConfig()
{
    ConfigFileLock lock;
    if (!lock)
        return false;
    try
    {
        // Read via the wide path and parse the text. toml::parse_file(g_config_path.string()) would run the
        // path through the ANSI code page; on a non-ASCII (e.g. Chinese) config path that corrupts it, and on
        // a code page that cannot represent the characters path::string() throws, crashing the process.
        std::ifstream input(g_config_path, std::ios::binary);
        if (!input)
            return false;
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        auto tbl = toml::parse(text);

        const int page_size = tbl["appearance"]["page_size"].value_or(6);
        g_candidate_page_size = page_size >= 3 && page_size <= 9 ? page_size : 6;
        g_candidate_font = tbl["appearance"]["font"].value_or(std::string("Noto Sans SC"));
        if (g_candidate_font.empty())
            g_candidate_font = "Noto Sans SC";
        g_candidate_english_font = tbl["appearance"]["english_font"].value_or(std::string("Segoe UI"));
        if (g_candidate_english_font.empty())
            g_candidate_english_font = "Segoe UI";
        g_candidate_default_font = tbl["appearance"]["default_font"].value_or(std::string("Microsoft YaHei"));
        if (g_candidate_default_font.empty())
            g_candidate_default_font = "Microsoft YaHei";
        // Absence means an old profile; an explicit empty array means system fallback only.
        g_candidate_fallback_fonts = {g_candidate_font, g_candidate_default_font};
        if (const auto *fonts = tbl["appearance"]["fallback_fonts"].as_array())
        {
            g_candidate_fallback_fonts.clear();
            for (const auto &node : *fonts)
            {
                if (auto font = node.value<std::string>();
                    font && !font->empty() && font->size() <= 256 &&
                    !std::any_of(font->begin(), font->end(), [](unsigned char ch) { return ch < 32 || ch == 127; }) &&
                    std::find(g_candidate_fallback_fonts.begin(), g_candidate_fallback_fonts.end(), *font) ==
                        g_candidate_fallback_fonts.end())
                {
                    g_candidate_fallback_fonts.push_back(*font);
                    if (g_candidate_fallback_fonts.size() == 32)
                        break;
                }
            }
        }
        {
            const int font_size = tbl["appearance"]["font_size"].value_or(16);
            g_candidate_font_size =
                font_size >= kCandidateFontSizeMin && font_size <= kCandidateFontSizeMax ? font_size : 16;
        }
        {
            const int font_size =
                tbl["appearance"]["candidate_window_preedit_font_size"].value_or(g_candidate_font_size);
            g_candidate_window_preedit_font_size =
                font_size >= kCandidateFontSizeMin && font_size <= kCandidateFontSizeMax ? font_size
                                                                                         : g_candidate_font_size;
        }
        {
            const std::string color = tbl["appearance"]["cand_text_color"].value_or(std::string("auto"));
            g_candidate_text_color = color.empty() ? "auto" : color;
        }
        g_session_backend = tbl["input"]["session_backend"].value_or(std::string("legacy"));
        g_input_scheme = ParseScheme(tbl["input"]["schema"].value_or(std::string("shuangpin")));
        {
            const std::string mode = tbl["input"]["mode"].value_or(std::string("chinese"));
            g_input_mode = mode == "japanese" ? "japanese" : "chinese";
        }
        {
            const std::string schema = tbl["input"]["japanese_schema"].value_or(std::string("romaji"));
            g_japanese_schema = schema == "romaji" ? schema : "romaji";
        }
        const std::string character_set = tbl["input"]["character_set"].value_or(std::string("simplified"));
        g_character_set = character_set == "traditional" ? "traditional" : "simplified";
        {
            const std::string mode = tbl["input"]["default_ime_mode"].value_or(std::string("chinese"));
            g_default_ime_mode = mode == "english" ? "english" : "chinese";
        }
        {
            const std::string scope = tbl["input"]["ime_mode_scope"].value_or(std::string("app"));
            g_ime_mode_scope = scope == "global" ? "global" : "app";
        }
        {
            // 自定义方案每次加载配置都重读文件；文件删了或写错就回落小鹤，而不是带着半张表输入。
            const std::string schema = tbl["input"]["shuangpin_schema"].value_or(std::string("xiaohe"));
            g_shuangpin_schema = LoadShuangpinSchema(schema) ? schema : "xiaohe";
        }
        g_wubi_schema = tbl["input"]["wubi_schema"].value_or(std::string("wubi86"));
        g_wubi_mixed_pinyin = tbl["input"]["wubi_mixed_pinyin"].value_or(false);
        g_escape_keeps_selected_word = tbl["input"]["escape_keeps_selected_word"].value_or(false);
        g_enter_learns_english_word = tbl["input"]["enter_learns_english_word"].value_or(true);
        const std::string z_mode = tbl["input"]["wubi_z_mode"].value_or(std::string("off"));
        g_wubi_z_mode = z_mode == "wildcard" ? "wildcard" : "off";
        g_shuangpin_preedit_mode = tbl["input"]["shuangpin_preedit_mode"].value_or(std::string("quanpin"));
        g_shuangpin_helpcode_enabled = tbl["helpcode"]["shuangpin_helpcode"].value_or(true);
        g_shuangpin_mid_sentence_helpcode_enabled = tbl["helpcode"]["shuangpin_mid_sentence_helpcode"].value_or(false);
        g_shuangpin_mid_sentence_helpcode_backtick =
            tbl["helpcode"]["shuangpin_mid_sentence_helpcode_backtick"].value_or(true);
        g_shuangpin_mid_sentence_helpcode_semicolon =
            tbl["helpcode"]["shuangpin_mid_sentence_helpcode_semicolon"].value_or(false);
        g_quanpin_helpcode_enabled = tbl["helpcode"]["quanpin_helpcode"].value_or(true);
        const std::string shuangpin_helpcode_schema =
            tbl["helpcode"]["shuangpin_helpcode_schema"].value_or(std::string("lantian"));
        g_shuangpin_helpcode_schema =
            IsHelpcodeSchemaAvailable(shuangpin_helpcode_schema) ? shuangpin_helpcode_schema : "lantian";
        const std::string quanpin_helpcode_schema =
            tbl["helpcode"]["quanpin_helpcode_schema"].value_or(std::string("lantian"));
        g_quanpin_helpcode_schema =
            IsHelpcodeSchemaAvailable(quanpin_helpcode_schema) ? quanpin_helpcode_schema : "lantian";
        g_show_shuangpin_helpcode_in_candidate_window =
            tbl["helpcode"]["show_sp_helpcode_in_candidate_window"].value_or(true);
        g_show_quanpin_helpcode_in_candidate_window =
            tbl["helpcode"]["show_qp_helpcode_in_candidate_window"].value_or(true);
        g_quanpin_autocorrect_transposition = tbl["quanpin"]["autocorrect_transposition"].value_or(false);
        g_quanpin_autocorrect_neighbor = tbl["quanpin"]["autocorrect_neighbor"].value_or(false);
        g_fuzzy_pinyin_enabled = tbl["input"]["fuzzy_pinyin"].value_or(false);
        g_fuzzy_seeded = tbl["input"]["fuzzy_seeded"].value_or(false);
        g_fuzzy_pinyin_rules = 0;
        for (const auto &entry : kFuzzyPinyinRuleKeys)
        {
            if (tbl["input"][entry.key].value_or(false))
                g_fuzzy_pinyin_rules |= static_cast<std::uint32_t>(entry.rule);
        }
        g_floating_toolbar_enabled = tbl["general"]["floating_toolbar"].value_or(true);
        g_caret_state_indicator_enabled = tbl["general"]["caret_state_indicator"].value_or(false);
        g_caret_state_indicator_on_focus = tbl["general"]["caret_state_indicator_on_focus"].value_or(false);
        g_caret_state_indicator_position = tbl["general"]["caret_state_indicator_position"].value_or(
            std::string(FanyImeUi::kDefaultCaretStatePosition));
        if (!FanyImeUi::IsValidCaretStatePosition(g_caret_state_indicator_position))
            g_caret_state_indicator_position = FanyImeUi::kDefaultCaretStatePosition;
        // Read the old candidate-only key as a migration fallback. New writes
        // use the unified key.
        g_diagnostic_log_enabled.store(tbl["general"]["diagnostic_log"].value_or(
                                           tbl["general"]["candidate_window_diagnostic_log"].value_or(false)),
                                       std::memory_order_relaxed);
        g_tsf_diagnostic_log_enabled.store(tbl["general"]["tsf_diagnostic_log"].value_or(false),
                                           std::memory_order_relaxed);
        g_statistics_enabled.store(tbl["statistics"]["enabled"].value_or(false), std::memory_order_relaxed);
        {
            // 读侧对缺键与非法值一律回落 forever：一个手写坏值不能让自动清理误删数据。
            const std::string retention = tbl["statistics"]["retention"].value_or(std::string("forever"));
            MsimeStats::Retention parsed = MsimeStats::Retention::Forever;
            g_statistics_retention = MsimeStats::ParseRetention(retention, parsed) ? retention : "forever";
        }
        g_floating_toolbar_items.fullwidth = tbl["general"]["floating_toolbar_fullwidth"].value_or(true);
        g_floating_toolbar_items.punctuation = tbl["general"]["floating_toolbar_punctuation"].value_or(true);
        g_floating_toolbar_items.character_set = tbl["general"]["floating_toolbar_character_set"].value_or(true);
        g_floating_toolbar_items.emoji = tbl["general"]["floating_toolbar_emoji"].value_or(true);
        g_floating_toolbar_items.screen_keyboard = tbl["general"]["floating_toolbar_screen_keyboard"].value_or(false);
        g_floating_toolbar_items.settings = tbl["general"]["floating_toolbar_settings"].value_or(true);
        {
            const double scale = tbl["general"]["floating_toolbar_scale"].value_or(1.0);
            g_floating_toolbar_scale =
                scale >= kFloatingToolbarScaleMin && scale <= kFloatingToolbarScaleMax ? scale : 1.0;
            const int font_size =
                tbl["general"]["floating_toolbar_font_size"].value_or(kFloatingToolbarFontSizeDefault);
            g_floating_toolbar_font_size =
                font_size >= kFloatingToolbarFontSizeMin && font_size <= kFloatingToolbarFontSizeMax
                    ? font_size
                    : kFloatingToolbarFontSizeDefault;
            g_floating_toolbar_auto_hide = tbl["general"]["floating_toolbar_auto_hide"].value_or(false);
            const int auto_hide_delay =
                tbl["general"]["floating_toolbar_auto_hide_delay"].value_or(kFloatingToolbarAutoHideDelayDefault);
            g_floating_toolbar_auto_hide_delay = auto_hide_delay >= kFloatingToolbarAutoHideDelayMin &&
                                                         auto_hide_delay <= kFloatingToolbarAutoHideDelayMax
                                                     ? auto_hide_delay
                                                     : kFloatingToolbarAutoHideDelayDefault;
        }
        g_english_candidates_enabled = tbl["general"]["cn_en_mixed_input"].value_or(false);
        g_candidate_translations_enabled = tbl["general"]["candidate_translations"].value_or(true);
        {
            const int min_chars =
                tbl["general"]["cn_en_mixed_input_min_chars"].value_or(kEnglishMixedInputMinCharsDefault);
            g_english_mixed_input_min_chars =
                min_chars >= kEnglishMixedInputMinCharsMin && min_chars <= kEnglishMixedInputMinCharsMax
                    ? min_chars
                    : kEnglishMixedInputMinCharsDefault;
        }
        g_cloud_candidates_enabled = tbl["general"]["cloud_candidates"].value_or(true);
        g_assoc_sentence_wordlattice = tbl["association"]["sentence_wordlattice"].value_or(true);
        g_assoc_sentence_google = tbl["association"]["sentence_google"].value_or(true);
        g_assoc_sentence_neural_desktop = tbl["association"]["sentence_neural_desktop"].value_or(false);
        g_assoc_sentence_neural_keyboard = tbl["association"]["sentence_neural_keyboard"].value_or(true);
        g_assoc_sentence_show_next_on_duplicate = tbl["association"]["sentence_show_next_on_duplicate"].value_or(false);
        g_assoc_sentence_collocation_enabled = tbl["association"]["sentence_collocation_enabled"].value_or(false);
        g_assoc_sentence_collocation_model = tbl["association"]["sentence_collocation_model"].value_or(std::string());
        g_assoc_sentence_collocation_weight = tbl["association"]["sentence_collocation_weight"].value_or(0.1);
        g_assoc_sentence_collocation_rerank_weight =
            tbl["association"]["sentence_collocation_rerank_weight"].value_or(0.05);
        g_assoc_sentence_source_badge = tbl["association"]["sentence_source_badge"].value_or(false);
        g_emoji_mixed_input_enabled = tbl["general"]["emoji_mixed_input"].value_or(false);
        g_kaomoji_mixed_input_enabled = tbl["general"]["kaomoji_mixed_input"].value_or(false);
        g_unicode_mode_enabled = tbl["utility"]["unicode_mode"].value_or(true);
        g_quick_phrase_enabled = tbl["utility"]["quick_phrase"].value_or(true);
        g_quick_phrase_candidates_enabled = tbl["utility"]["quick_phrase_candidates"].value_or(false);
        g_quick_phrase_frequency_enabled = tbl["utility"]["quick_phrase_frequency"].value_or(true);
        g_mixed_candidates_enabled = tbl["utility"]["mixed_candidates"].value_or(true);
        g_date_time_candidates_enabled = tbl["utility"]["date_time_candidates"].value_or(false);
        g_date_time_menu_enabled = tbl["utility"]["date_time_menu"].value_or(true);
        g_date_time_mode_enabled = tbl["utility"]["date_time_mode"].value_or(true);
        g_emoji_mode_enabled = tbl["utility"]["emoji_mode"].value_or(true);
        g_kaomoji_mode_enabled = tbl["utility"]["kaomoji_mode"].value_or(true);
        g_jianpin_mode_enabled = tbl["utility"]["jianpin_mode"].value_or(true);
        g_y_mode_enabled = tbl["utility"]["y_mode"].value_or(true);
        g_r_mode_enabled = tbl["utility"]["r_mode"].value_or(true);
        {
            const bool previous_clipboard_history = g_clipboard_history_enabled;
            static bool clipboard_history_loaded = false;
            g_clipboard_history_enabled = TomlFlexibleBool(tbl["utility"]["clipboard_history"], false);
            if (!g_clipboard_history_enabled && (previous_clipboard_history || !clipboard_history_loaded))
                ClipboardHistory::Clear();
            clipboard_history_loaded = true;
            ClipboardMonitor::Sync(g_clipboard_history_enabled);
        }
        const auto legacy_paging_mode = tbl["general"]["paging_mode"].value<std::string>();
        g_paging_minus_equal_enabled =
            tbl["general"]["paging_minus_equal"].value_or(!legacy_paging_mode || *legacy_paging_mode == "-/=");
        g_paging_comma_period_enabled =
            tbl["general"]["paging_comma_period"].value_or(legacy_paging_mode && *legacy_paging_mode == ",/.");
        g_paging_brackets_enabled = tbl["general"]["paging_brackets"].value_or(false);
        g_paging_tab_enabled =
            tbl["general"]["paging_tab"].value_or(legacy_paging_mode && *legacy_paging_mode == "Shift+Tab/Tab");
        g_paging_page_up_down_enabled = tbl["general"]["paging_page_up_down"].value_or(true);
        g_paging_mouse_wheel_enabled = tbl["general"]["paging_mouse_wheel"].value_or(false);
        g_candidate_arrow_navigation_enabled = tbl["general"]["candidate_arrow_navigation"].value_or(true);
        g_word_to_character_enabled = tbl["input"]["word_to_character"].value_or(false);
        g_word_to_character_keys = tbl["input"]["word_to_character_keys"].value_or(std::string("brackets"));
        if (g_word_to_character_keys != "minus_equal")
            g_word_to_character_keys = "brackets";
        if ((g_word_to_character_keys == "brackets" ? g_paging_brackets_enabled : g_paging_minus_equal_enabled) &&
            g_word_to_character_enabled)
        {
            g_word_to_character_enabled = false;
        }
        g_smart_punctuation_enabled = tbl["input"]["smart_punctuation"].value_or(false);
        g_smart_punctuation_space_convert_enabled = tbl["input"]["smart_punctuation_space_convert"].value_or(false);
        g_smart_punctuation_direct_digit_enabled = tbl["input"]["smart_punctuation_direct_digit"].value_or(false);
        g_smart_punctuation_direct_letter_enabled = tbl["input"]["smart_punctuation_direct_letter"].value_or(false);
        g_smart_punctuation_repeat_to_chinese_enabled =
            tbl["input"]["smart_punctuation_repeat_to_chinese"].value_or(false);
        g_paired_punctuation_enabled = tbl["input"]["paired_punctuation"].value_or(true);
        {
            const std::string punctuation_lock = tbl["input"]["punctuation_lock"].value_or(std::string("follow"));
            g_punctuation_lock =
                punctuation_lock == "chinese" || punctuation_lock == "english" ? punctuation_lock : "follow";
        }
        {
            // Prefer explicit bool keys; fall back to legacy switch_language array.
            const auto legacy = tbl["keybindings"]["switch_language"].as_array();
            bool legacy_shift = true;
            bool legacy_ctrl_alt_space = true;
            if (legacy)
            {
                legacy_shift = false;
                legacy_ctrl_alt_space = false;
                for (const auto &item : *legacy)
                {
                    const auto value = item.value<std::string>();
                    if (!value)
                        continue;
                    if (*value == "Shift")
                        legacy_shift = true;
                    else if (*value == "Ctrl+Alt+Space" || *value == "Ctrl+Space")
                        legacy_ctrl_alt_space = true;
                }
            }
            g_switch_language_shift_enabled = tbl["keybindings"]["switch_language_shift"].value_or(legacy_shift);
            g_switch_language_ctrl_enabled = tbl["keybindings"]["switch_language_ctrl"].value_or(false);
            g_switch_language_ctrl_alt_space_enabled =
                tbl["keybindings"]["switch_language_ctrl_alt_space"].value_or(legacy_ctrl_alt_space);
            g_character_set_shortcut_enabled = tbl["keybindings"]["toggle_character_set_ctrl_shift_f"].value_or(true);
        }
        {
            const std::string mode = tbl["frequency_adjustment"]["mode"].value_or(std::string("promote"));
            g_frequency_adjustment.mode =
                mode == "disabled" || mode == "pin" || mode == "halve" || mode == "linear" || mode == "promote"
                    ? mode
                    : "promote";
            const int trigger = tbl["frequency_adjustment"]["trigger_count"].value_or(1);
            const int step = tbl["frequency_adjustment"]["linear_step"].value_or(1);
            g_frequency_adjustment.trigger_count = trigger >= 1 && trigger <= 10 ? trigger : 1;
            g_frequency_adjustment.linear_step = step >= 1 && step <= 10 ? step : 1;
        }
        const std::string layout = tbl["appearance"]["candidate_window_layout"].value_or(std::string("vertical"));
        g_candidate_window_layout = layout == "horizontal" ? "horizontal" : "vertical";
        g_candidate_window_follow_cursor = tbl["appearance"]["candidate_window_follow_cursor"].value_or(true);
        g_ui_backend = NormalizeSmallWindowUiBackend(tbl["appearance"]["ui_backend"].value_or(std::string("d2d")));
        const std::string skin = tbl["appearance"]["candidate_skin"].value_or(std::string("fluent"));
        g_candidate_skin = IsValidCandidateSkinId(skin) ? skin : "fluent";
        {
            const std::string preedit_style =
                tbl["appearance"]["candidate_window_preedit_style"].value_or(std::string("pinyin"));
            g_candidate_window_preedit_style = preedit_style == "empty" ? "empty" : "pinyin";
        }
        g_candidate_window_preedit_shuangpin_quanpin =
            tbl["appearance"]["candidate_window_preedit_shuangpin_quanpin"].value_or(false);
        g_candidate_fixed_badge = tbl["appearance"]["candidate_fixed_badge"].value_or(true);
        {
            // 非法样式一律回退到默认徽标，避免手改配置后出现无法删除的畸形标记
            const std::string badge_style =
                tbl["appearance"]["candidate_fixed_badge_style"].value_or(std::string("paperclip"));
            g_candidate_fixed_badge_style = IsValidCandidateFixedBadgeStyle(badge_style) ? badge_style : "paperclip";
        }
        {
            const std::string linger = tbl["appearance"]["settings_window_linger"].value_or(std::string("off"));
            g_settings_window_linger = IsValidSettingsWindowLinger(linger) ? linger : "off";
        }
        {
            const std::string theme_mode = tbl["appearance"]["theme_mode"].value_or(std::string("system"));
            if (theme_mode == "light" || theme_mode == "system" || theme_mode == "auto")
                g_theme_mode = theme_mode == "auto" ? "system" : theme_mode;
            else
                g_theme_mode = "dark";
        }
        auto normalize_surface = [](const std::string &value) -> std::string {
            if (value == "light" || value == "dark" || value == "follow")
                return value;
            return "follow";
        };
        g_theme_settings = normalize_surface(tbl["appearance"]["theme_settings"].value_or(std::string("follow")));
        g_theme_cand = normalize_surface(tbl["appearance"]["theme_cand"].value_or(std::string("follow")));
        g_theme_ftb = normalize_surface(tbl["appearance"]["theme_ftb"].value_or(std::string("follow")));
        g_theme_menu = normalize_surface(tbl["appearance"]["theme_menu"].value_or(std::string("follow")));
        g_theme_emoji = normalize_surface(tbl["appearance"]["theme_emoji"].value_or(std::string("follow")));
        g_theme_screen_keyboard =
            normalize_surface(tbl["appearance"]["theme_screen_keyboard"].value_or(std::string("follow")));
        g_theme_handwriting = normalize_surface(tbl["appearance"]["theme_handwriting"].value_or(std::string("follow")));
        g_theme_voice = normalize_surface(tbl["appearance"]["theme_voice"].value_or(std::string("follow")));
        {
            const std::string tsf_preedit_style = tbl["appearance"]["tsf_preedit_style"].value_or(
                tbl["input"]["tsf_preedit_style"].value_or(std::string("raw")));
            g_tsf_preedit_style = GlobalSettings::normalizeTsfPreeditStyle(tsf_preedit_style);
            g_tsf_preedit_shuangpin_quanpin = tbl["appearance"]["tsf_preedit_shuangpin_quanpin"].value_or(false);
            RefreshEffectiveTsfPreeditStyle();
        }
        // Parse into a local and publish it in one step, so readers never observe half-rewritten strings or maps.
        VoiceInputConfig voice;
        voice.enabled = tbl["voice_input"]["voice_input"].value_or(true);
        voice.hotkey_ralt = tbl["voice_input"]["hotkey_ralt"].value_or(true);
        voice.hotkey_ctrl_f9 = tbl["voice_input"]["hotkey_ctrl_f9"].value_or(true);
        voice.hotkey_ctrl_win = tbl["voice_input"]["hotkey_ctrl_win"].value_or(false);
        voice.hotkey_rctrl_ralt = tbl["voice_input"]["hotkey_rctrl_ralt"].value_or(false);
        voice.hotkey_hold_space_lock = tbl["voice_input"]["hotkey_hold_space_lock"].value_or(true);
        voice.asr_provider = tbl["voice_input"]["asr_provider"].value_or(std::string("doubao"));
        voice.asr_app_key = tbl["voice_input"]["asr_app_key"].value_or(std::string());
        voice.doubao_auth_mode = VoiceInput::NormalizeDoubaoAuthMode(
            tbl["voice_input"]["doubao_auth_mode"].value_or(std::string()), voice.asr_app_key);
        voice.asr_token = tbl["voice_input"]["asr_token"].value_or(std::string());
        for (const auto provider : VoiceInput::AsrProviders())
        {
            const std::string id(provider);
            voice.asr_tokens[id] =
                VoiceInput::UsableToken(tbl["voice_input"][VoiceInput::AsrTokenSlotKey(id)].value_or(std::string()));
        }
        {
            const std::string provider = VoiceInput::NormalizeProviderId(voice.asr_provider);
            std::string &stored = voice.asr_tokens[provider];
            if (VoiceInput::IsPlaceholderToken(stored) && !VoiceInput::IsPlaceholderToken(voice.asr_token))
            {
                stored = voice.asr_token;
                g_persist_asr_token_slot = true;
            }
            voice.asr_token = stored;
        }
        voice.asr_endpoint = tbl["voice_input"]["asr_endpoint"].value_or(
            voice.asr_provider == "doubao" ? std::string("wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async")
                                           : std::string("https://api.siliconflow.cn/v1/audio/transcriptions"));
        if (voice.asr_provider == "doubao" &&
            voice.asr_endpoint == "https://api.siliconflow.cn/v1/audio/transcriptions")
        {
            voice.asr_endpoint = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";
        }
        voice.asr_resource_id =
            tbl["voice_input"]["asr_resource_id"].value_or(std::string("volc.seedasr.sauc.duration"));
        voice.doubao_enable_itn = tbl["voice_input"]["doubao_enable_itn"].value_or(true);
        voice.doubao_enable_punc = tbl["voice_input"]["doubao_enable_punc"].value_or(true);
        voice.doubao_enable_ddc = tbl["voice_input"]["doubao_enable_ddc"].value_or(false);
        voice.doubao_boosting_table_id = tbl["voice_input"]["doubao_boosting_table_id"].value_or(std::string());
        voice.asr_model = tbl["voice_input"]["asr_model"].value_or(std::string());
        voice.polish_provider = tbl["voice_input"]["polish_provider"].value_or(std::string("siliconflow"));
        voice.polish_token = tbl["voice_input"]["polish_token"].value_or(std::string());
        for (const auto provider : VoiceInput::PolishProviders())
        {
            const std::string id(provider);
            voice.polish_tokens[id] =
                VoiceInput::UsableToken(tbl["voice_input"][VoiceInput::PolishTokenSlotKey(id)].value_or(std::string()));
        }
        {
            const std::string provider = VoiceInput::NormalizeProviderId(voice.polish_provider);
            std::string &stored = voice.polish_tokens[provider];
            if (VoiceInput::IsPlaceholderToken(stored) && !VoiceInput::IsPlaceholderToken(voice.polish_token))
            {
                stored = voice.polish_token;
                g_persist_polish_token_slot = true;
            }
            voice.polish_token = stored;
        }
        voice.polish_endpoint = tbl["voice_input"]["polish_endpoint"].value_or(
            std::string("https://api.siliconflow.cn/v1/chat/completions"));
        voice.polish_model = tbl["voice_input"]["polish_model"].value_or(std::string());
        voice.polish_prompt = tbl["voice_input"]["polish_prompt"].value_or(std::string());
        voice.polish_prompt_id = tbl["voice_input"]["polish_prompt_id"].value_or(std::string("cleanup"));
        if (voice.polish_prompt_id == "custom")
            voice.polish_prompt_id = "custom_1";
        if (voice.polish_prompt_id != "cleanup" && voice.polish_prompt_id != "faithful" &&
            voice.polish_prompt_id != "zh2en" && voice.polish_prompt_id != "casual" &&
            voice.polish_prompt_id != "custom_1" && voice.polish_prompt_id != "custom_2" &&
            voice.polish_prompt_id != "custom_3")
        {
            voice.polish_prompt_id = "cleanup";
        }
        voice.polish_prompt_custom_1 = tbl["voice_input"]["polish_prompt_custom_1"].value_or(std::string());
        if (voice.polish_prompt_custom_1.empty())
            voice.polish_prompt_custom_1 = voice.polish_prompt;
        voice.polish_prompt_custom_2 = tbl["voice_input"]["polish_prompt_custom_2"].value_or(std::string());
        voice.polish_prompt_custom_3 = tbl["voice_input"]["polish_prompt_custom_3"].value_or(std::string());
        voice.language = tbl["voice_input"]["language"].value_or(std::string("zh-cn"));
        // notification_sound is retained as a fallback for configs written by older versions.
        const bool legacy_notification_sound = tbl["voice_input"]["notification_sound"].value_or(true);
        voice.start_sound = tbl["voice_input"]["start_sound"].value_or(legacy_notification_sound);
        voice.end_sound = tbl["voice_input"]["end_sound"].value_or(legacy_notification_sound);
        voice.mute_system_audio = tbl["voice_input"]["mute_system_audio"].value_or(false);
        voice.polish_text = tbl["voice_input"]["polish_text"].value_or(false);
        voice.stream_inline_preedit = tbl["voice_input"]["stream_inline_preedit"].value_or(false);
        voice.commit_mode = tbl["voice_input"]["commit_mode"].value_or(std::string("tsf"));
        if (voice.commit_mode != "tsf" && voice.commit_mode != "sendinput" && voice.commit_mode != "ctrl_v")
        {
            voice.commit_mode = "tsf";
        }
        PublishVoiceInput(std::move(voice));
        g_ai_assistant.enabled = tbl["ai_assistant"]["enabled"].value_or(false);
        g_ai_assistant.provider =
            VoiceInput::NormalizeProviderId(tbl["ai_assistant"]["provider"].value_or(std::string("deepseek")));
        if (AiAssistantTokenSlotKey(g_ai_assistant.provider).empty())
            g_ai_assistant.provider = "deepseek";
        g_ai_assistant.token = tbl["ai_assistant"]["token"].value_or(std::string());
        g_ai_assistant.tokens.clear();
        for (const auto provider : AiAssistantProviders())
        {
            const std::string id(provider);
            g_ai_assistant.tokens[id] =
                VoiceInput::UsableToken(tbl["ai_assistant"][AiAssistantTokenSlotKey(id)].value_or(std::string()));
        }
        {
            std::string &stored = g_ai_assistant.tokens[g_ai_assistant.provider];
            if (stored.empty())
                stored = VoiceInput::UsableToken(g_ai_assistant.token);
            g_ai_assistant.token = stored;
        }
        g_ai_assistant.endpoint =
            tbl["ai_assistant"]["endpoint"].value_or(std::string("https://api.deepseek.com/chat/completions"));
        g_ai_assistant.model = tbl["ai_assistant"]["model"].value_or(std::string("deepseek-v4-flash"));
        const AiAssistantConfig ai_defaults;
        g_ai_assistant.endpoints = ai_defaults.endpoints;
        g_ai_assistant.models = ai_defaults.models;
        for (const auto provider : AiAssistantProviders())
        {
            const std::string id(provider);
            const auto load_slot = [&](const std::string &key, const std::string &legacy, std::string &target) {
                const std::string stored = tbl["ai_assistant"][key + "_" + id].value_or(std::string());
                if (!stored.empty())
                    target = stored;
                else if (id == g_ai_assistant.provider)
                    target = legacy;
            };
            load_slot("endpoint", g_ai_assistant.endpoint, g_ai_assistant.endpoints[id]);
            load_slot("model", g_ai_assistant.model, g_ai_assistant.models[id]);
        }
        g_ai_assistant.endpoint = g_ai_assistant.endpoints[g_ai_assistant.provider];
        g_ai_assistant.model = g_ai_assistant.models[g_ai_assistant.provider];
        const int ai_limit = tbl["ai_assistant"]["candidate_limit"].value_or(3);
        g_ai_assistant.candidate_limit = ai_limit >= 1 && ai_limit <= 10 ? ai_limit : 3;
        const std::string legacy_ai_prompt = tbl["ai_assistant"]["prompt"].value_or(g_ai_assistant.prompt);
        g_ai_assistant.prompt_id = tbl["ai_assistant"]["prompt_id"].value_or(std::string("custom_1"));
        if (g_ai_assistant.prompt_id != "custom_1" && g_ai_assistant.prompt_id != "custom_2" &&
            g_ai_assistant.prompt_id != "custom_3")
            g_ai_assistant.prompt_id = "custom_1";
        g_ai_assistant.prompt_custom_1 = tbl["ai_assistant"]["prompt_custom_1"].value_or(std::string());
        if (g_ai_assistant.prompt_custom_1.empty())
            g_ai_assistant.prompt_custom_1 = legacy_ai_prompt;
        g_ai_assistant.prompt_custom_2 = tbl["ai_assistant"]["prompt_custom_2"].value_or(std::string());
        g_ai_assistant.prompt_custom_3 = tbl["ai_assistant"]["prompt_custom_3"].value_or(std::string());
        g_ai_assistant.prompt = g_ai_assistant.prompt_id == "custom_2"   ? g_ai_assistant.prompt_custom_2
                                : g_ai_assistant.prompt_id == "custom_3" ? g_ai_assistant.prompt_custom_3
                                                                         : g_ai_assistant.prompt_custom_1;
        g_tencent_tmt.enabled = tbl["tencent_tmt"]["enabled"].value_or(true);
        g_tencent_tmt.secret_id = tbl["tencent_tmt"]["secret_id"].value_or(std::string());
        g_tencent_tmt.secret_key = tbl["tencent_tmt"]["secret_key"].value_or(std::string());
        g_tencent_tmt.region = tbl["tencent_tmt"]["region"].value_or(std::string("ap-guangzhou"));
        if (g_tencent_tmt.region.empty())
            g_tencent_tmt.region = "ap-guangzhou";
        g_tencent_tmt.target_language = tbl["tencent_tmt"]["target_language"].value_or(std::string("en"));
        if (g_tencent_tmt.target_language != "en" && g_tencent_tmt.target_language != "fr" &&
            g_tencent_tmt.target_language != "ja" && g_tencent_tmt.target_language != "es" &&
            g_tencent_tmt.target_language != "ru" && g_tencent_tmt.target_language != "de" &&
            g_tencent_tmt.target_language != "ko")
            g_tencent_tmt.target_language = "en";
        g_custom_translation.enabled = tbl["custom_translation"]["enabled"].value_or(false);
        g_custom_translation.endpoint = tbl["custom_translation"]["endpoint"].value_or(std::string());
        g_custom_translation.api_key = tbl["custom_translation"]["api_key"].value_or(std::string());
        g_niutrans.enabled = tbl["niutrans"]["enabled"].value_or(false);
        g_niutrans.app_id = tbl["niutrans"]["app_id"].value_or(std::string());
        g_niutrans.apikey = tbl["niutrans"]["apikey"].value_or(std::string());
        {
            NetworkProxyConfig proxy;
            proxy.mode = tbl["network"]["proxy_mode"].value_or(std::string("system"));
            if (proxy.mode != "system" && proxy.mode != "none" && proxy.mode != "custom")
                proxy.mode = "system";
            proxy.server = NormalizeNetworkProxyServer(tbl["network"]["proxy_server"].value_or(std::string()));
            std::lock_guard<std::mutex> proxy_lock(g_network_proxy_mutex);
            g_network_proxy = std::move(proxy);
        }
        RememberConfigWriteTime();
        return true;
    }
    catch (const toml::parse_error &)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
        return false;
    }
}

void PersistSeededVoiceInputTokenSlots()
{
    VoiceInputConfig voice = SnapshotVoiceInput();
    if (g_persist_asr_token_slot)
    {
        const std::string id = VoiceInput::NormalizeProviderId(voice.asr_provider);
        const std::string key = VoiceInput::AsrTokenSlotKey(id);
        if (!key.empty())
            WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(voice.asr_tokens[id]));
        g_persist_asr_token_slot = false;
    }
    if (g_persist_polish_token_slot)
    {
        const std::string id = VoiceInput::NormalizeProviderId(voice.polish_provider);
        const std::string key = VoiceInput::PolishTokenSlotKey(id);
        if (!key.empty())
            WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(voice.polish_tokens[id]));
        g_persist_polish_token_slot = false;
    }
}

void MigrateLegacyVoiceInputConfig()
{
    VoiceInputConfig voice = SnapshotVoiceInput();
    if (!voice.asr_token.empty())
        return;
    const std::filesystem::path legacy_path =
        std::filesystem::path(CommonUtils::get_local_appdata_path_w()) / L"MetasequoiaVoiceInput" / L"config.toml";
    if (!std::filesystem::exists(legacy_path))
        return;
    try
    {
        std::ifstream legacy_input(legacy_path, std::ios::binary);
        const std::string legacy_text((std::istreambuf_iterator<char>(legacy_input)), std::istreambuf_iterator<char>());
        const toml::table legacy = toml::parse(legacy_text);
        const std::string asr_token = legacy["asr_api"]["token"].value_or(std::string());
        if (asr_token.empty())
            return;
        const auto migrate_string = [](const std::string &key, const std::string &value, std::string &target) {
            if (WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(value)))
                target = value;
        };
        const auto migrate_bool = [](const std::string &key, bool value, bool &target) {
            if (WriteConfiguredValue("voice_input", key, value ? "true" : "false"))
                target = value;
        };
        migrate_string("asr_provider", legacy["asr_api"]["provider"].value_or(std::string("siliconflow")),
                       voice.asr_provider);
        migrate_string("asr_token", asr_token, voice.asr_token);
        migrate_string("asr_endpoint", legacy["asr_api"]["endpoint"].value_or(voice.asr_endpoint), voice.asr_endpoint);
        migrate_string("polish_provider", legacy["polish_api"]["provider"].value_or(std::string("siliconflow")),
                       voice.polish_provider);
        migrate_string("polish_token", legacy["polish_api"]["token"].value_or(std::string()), voice.polish_token);
        migrate_string("polish_endpoint", legacy["polish_api"]["endpoint"].value_or(voice.polish_endpoint),
                       voice.polish_endpoint);
        migrate_string("language", legacy["settings"]["language"].value_or(std::string("zh-cn")), voice.language);
        const bool notification_sound = legacy["settings"]["notification_sound"].value_or(true);
        migrate_bool("start_sound", notification_sound, voice.start_sound);
        migrate_bool("end_sound", notification_sound, voice.end_sound);
        migrate_bool("polish_text", legacy["settings"]["polish_text"].value_or(false), voice.polish_text);
        PublishVoiceInput(std::move(voice));
    }
    catch (const toml::parse_error &)
    {
#ifdef FANY_DEBUG
        (void)0;
#endif
    }
}
} // namespace

namespace ime_config_detail
{
SchemeType ParseScheme(const std::string &value)
{
    if (value == "quanpin")
    {
        return SchemeType::Quanpin;
    }
    if (value == "wubi")
    {
        return SchemeType::Wubi;
    }
    return SchemeType::Shuangpin;
}
} // namespace ime_config_detail

void InvalidateImeConfigWriteTime()
{
    g_config_last_write_time.reset();
}

void InitImeConfig()
{
    // Build the path from the wide accessor: std::filesystem::path(std::string) decodes with the
    // system ANSI code page, which corrupts a non-ASCII (e.g. Chinese) user profile path on a
    // non-UTF-8 ACP machine and makes every config read/write fail ("设置保存失败").
    g_config_path = std::filesystem::path(CommonUtils::get_ime_config_dir_w()) / L"config.toml";
    std::error_code create_error;
    std::filesystem::create_directories(g_config_path.parent_path(), create_error);
    CommonUtils::ensure_ime_data_writable();
    RecoverLegacyAcpMangledConfig();
    SyncConfigWithInstalledTemplate();
    if (LoadImeConfig())
    {
        MigrateLegacyVoiceInputConfig();
        VoiceInputConfig voice = SnapshotVoiceInput();
        if (VoiceInput::NormalizeProviderId(voice.asr_provider) == "siliconflow" &&
            voice.asr_model == "TeleAI/TeleSpeechASR")
        {
            const std::string model = VoiceInput::DefaultAsrModel("siliconflow");
            if (WriteConfiguredValue("voice_input", "asr_model", EscapeTomlBasicString(model)))
                voice.asr_model = model;
        }
        {
            const std::string asr_id = VoiceInput::NormalizeProviderId(voice.asr_provider);
            if (VoiceInput::IsPlaceholderToken(voice.asr_tokens[asr_id]) &&
                !VoiceInput::IsPlaceholderToken(voice.asr_token))
            {
                voice.asr_tokens[asr_id] = voice.asr_token;
                g_persist_asr_token_slot = true;
            }
            const std::string polish_id = VoiceInput::NormalizeProviderId(voice.polish_provider);
            if (VoiceInput::IsPlaceholderToken(voice.polish_tokens[polish_id]) &&
                !VoiceInput::IsPlaceholderToken(voice.polish_token))
            {
                voice.polish_tokens[polish_id] = voice.polish_token;
                g_persist_polish_token_slot = true;
            }
        }
        PublishVoiceInput(std::move(voice));
        PersistSeededVoiceInputTokenSlots();
#ifdef FANY_DEBUG
        (void)0;
#endif
#ifdef FANY_DEBUG
        (void)0;
#endif
#ifdef FANY_DEBUG
        (void)0;
#endif
#ifdef FANY_DEBUG
        (void)0;
#endif
    }
    g_ui_backend_active = g_ui_backend;
}

bool ReloadImeConfigIfChanged()
{
    std::error_code error;
    const auto write_time = std::filesystem::last_write_time(g_config_path, error);
    if (error || (g_config_last_write_time && write_time == *g_config_last_write_time))
    {
        return false;
    }
    return LoadImeConfig();
}

const std::filesystem::path &GetImeConfigPath()
{
    return g_config_path;
}

const std::string &GetConfiguredSessionBackend()
{
    return g_session_backend;
}
