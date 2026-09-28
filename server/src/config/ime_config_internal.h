#pragma once

// ime_config*.cpp 之间共享的内部声明：拆分前同在 ime_config.cpp 匿名命名空间里、
// 现在跨文件使用的配置状态、常量与辅助函数。只给 server/src/config/ime_config*.cpp 包含，
// 其他地方不要引用。

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>
#include "config/ime_config.h"

namespace ime_config_detail
{
constexpr int kCandidateFontSizeMin = 12;
constexpr int kCandidateFontSizeMax = 32;
constexpr double kFloatingToolbarScaleMin = 0.75;
constexpr double kFloatingToolbarScaleMax = 1.5;
constexpr int kFloatingToolbarFontSizeMin = 16;
constexpr int kFloatingToolbarFontSizeMax = 28;
constexpr int kFloatingToolbarFontSizeDefault = 24;
constexpr int kFloatingToolbarAutoHideDelayMin = 1;
constexpr int kFloatingToolbarAutoHideDelayMax = 60;
constexpr int kFloatingToolbarAutoHideDelayDefault = 5;
constexpr int kEnglishMixedInputMinCharsMin = 1;
constexpr int kEnglishMixedInputMinCharsMax = 8;
constexpr int kEnglishMixedInputMinCharsDefault = 2;

// Config key ↔ engine rule bit. The key mirrors the enumerator name lowercased, so the
// only mapping to review is this table; the bit position lives in the enum itself.
struct FuzzyPinyinRuleKey
{
    const char *key;
    metasequoia::FuzzyPinyinRule rule;
};
constexpr FuzzyPinyinRuleKey kFuzzyPinyinRuleKeys[] = {
    {"fuzzy_z_zh", metasequoia::FuzzyPinyinRule::Z_ZH},
    {"fuzzy_c_ch", metasequoia::FuzzyPinyinRule::C_CH},
    {"fuzzy_s_sh", metasequoia::FuzzyPinyinRule::S_SH},
    {"fuzzy_n_l", metasequoia::FuzzyPinyinRule::N_L},
    {"fuzzy_f_h", metasequoia::FuzzyPinyinRule::F_H},
    {"fuzzy_r_l", metasequoia::FuzzyPinyinRule::R_L},
    {"fuzzy_an_ang", metasequoia::FuzzyPinyinRule::AN_ANG},
    {"fuzzy_en_eng", metasequoia::FuzzyPinyinRule::EN_ENG},
    {"fuzzy_in_ing", metasequoia::FuzzyPinyinRule::IN_ING},
    {"fuzzy_ian_iang", metasequoia::FuzzyPinyinRule::IAN_IANG},
    {"fuzzy_uan_uang", metasequoia::FuzzyPinyinRule::UAN_UANG},
};

class ConfigFileLock
{
  public:
    ConfigFileLock()
    {
        handle_ = CreateMutexW(nullptr, FALSE, L"Local\\MetasequoiaIme.ConfigFile");
        if (handle_)
        {
            const DWORD result = WaitForSingleObject(handle_, 5000);
            locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }

    ~ConfigFileLock()
    {
        if (locked_)
            ReleaseMutex(handle_);
        if (handle_)
            CloseHandle(handle_);
    }

    explicit operator bool() const
    {
        return locked_;
    }

  private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

SchemeType ParseScheme(const std::string &value);

// 安装包装入的本版出厂模板，以及上次合并时用的那份模板（升级基线）。
const char *const kConfigTemplateFileName = "config.default.toml";
const char *const kConfigBaselineFileName = "config.base.toml";

using TomlAssignmentVisitor =
    std::function<void(const std::string &section, const std::string &key, size_t value_begin, size_t value_end)>;

struct ConfigValueUpdate
{
    std::string section;
    std::string key;
    std::string value;
};

// 配置状态：定义与初始化顺序都在 ime_config.cpp。
extern SchemeType g_input_scheme;
extern std::string g_input_mode;
extern std::string g_japanese_schema;
extern bool g_japanese_punctuation;
extern bool g_japanese_katakana_fkey;
extern std::string g_character_set;
extern std::string g_default_ime_mode;
extern std::string g_ime_mode_scope;
extern bool g_switch_language_shift_enabled;
extern bool g_switch_language_ctrl_enabled;
extern bool g_switch_language_ctrl_alt_space_enabled;
extern bool g_character_set_shortcut_enabled;
extern int g_candidate_page_size;
extern std::string g_candidate_font;
extern std::string g_candidate_english_font;
extern std::string g_candidate_default_font;
extern std::vector<std::string> g_candidate_fallback_fonts;
extern int g_candidate_font_size;
extern int g_candidate_window_preedit_font_size;
extern std::atomic_bool g_diagnostic_log_enabled;
extern std::atomic_bool g_tsf_diagnostic_log_enabled;
extern std::atomic_bool g_statistics_enabled;
extern std::string g_statistics_retention;
extern std::string g_candidate_text_color;
extern std::string g_shuangpin_schema;
extern std::string g_wubi_schema;
extern std::string g_shuangpin_preedit_mode;
extern std::string g_tsf_preedit_style;
extern bool g_shuangpin_helpcode_enabled;
extern bool g_quanpin_helpcode_enabled;
extern std::string g_shuangpin_helpcode_schema;
extern std::string g_quanpin_helpcode_schema;
extern bool g_show_shuangpin_helpcode_in_candidate_window;
extern bool g_show_quanpin_helpcode_in_candidate_window;
extern bool g_quanpin_autocorrect_transposition;
extern bool g_quanpin_autocorrect_neighbor;
extern std::uint32_t g_fuzzy_pinyin_rules;
extern bool g_fuzzy_pinyin_enabled;
extern bool g_fuzzy_seeded;
extern bool g_floating_toolbar_enabled;
extern bool g_caret_state_indicator_enabled;
extern bool g_caret_state_indicator_on_focus;
extern std::string g_caret_state_indicator_position;
extern FloatingToolbarItemsConfig g_floating_toolbar_items;
extern double g_floating_toolbar_scale;
extern int g_floating_toolbar_font_size;
extern bool g_floating_toolbar_auto_hide;
extern int g_floating_toolbar_auto_hide_delay;
extern bool g_english_candidates_enabled;
extern bool g_candidate_translations_enabled;
extern int g_english_mixed_input_min_chars;
extern bool g_cloud_candidates_enabled;
extern bool g_assoc_sentence_wordlattice;
extern bool g_assoc_sentence_google;
extern bool g_assoc_sentence_neural_desktop;
extern bool g_assoc_sentence_neural_keyboard;
extern bool g_assoc_sentence_show_next_on_duplicate;
extern bool g_assoc_sentence_source_badge;
extern bool g_emoji_mixed_input_enabled;
extern bool g_kaomoji_mixed_input_enabled;
extern bool g_unicode_mode_enabled;
extern bool g_quick_phrase_enabled;
extern bool g_date_time_mode_enabled;
extern bool g_emoji_mode_enabled;
extern bool g_kaomoji_mode_enabled;
extern bool g_jianpin_mode_enabled;
extern bool g_y_mode_enabled;
extern bool g_r_mode_enabled;
extern bool g_clipboard_history_enabled;
extern bool g_paging_minus_equal_enabled;
extern bool g_paging_comma_period_enabled;
extern bool g_paging_brackets_enabled;
extern bool g_paging_tab_enabled;
extern bool g_paging_page_up_down_enabled;
extern bool g_paging_mouse_wheel_enabled;
extern bool g_candidate_arrow_navigation_enabled;
extern bool g_word_to_character_enabled;
extern std::string g_word_to_character_keys;
extern bool g_smart_punctuation_enabled;
extern bool g_smart_punctuation_space_convert_enabled;
extern bool g_smart_punctuation_direct_digit_enabled;
extern bool g_smart_punctuation_direct_letter_enabled;
extern bool g_smart_punctuation_repeat_to_chinese_enabled;
extern bool g_paired_punctuation_enabled;
extern std::string g_punctuation_lock;
extern std::string g_candidate_window_layout;
extern bool g_candidate_window_follow_cursor;
extern std::string g_ui_backend;
extern std::string g_ui_backend_active;
extern std::string g_candidate_skin;
extern std::string g_candidate_window_preedit_style;
extern bool g_candidate_fixed_badge;
extern std::string g_candidate_fixed_badge_style;
extern std::string g_settings_window_linger;
extern std::string g_theme_mode;
extern std::string g_theme_settings;
extern std::string g_theme_cand;
extern std::string g_theme_ftb;
extern std::string g_theme_menu;
extern std::string g_theme_emoji;
extern std::string g_theme_screen_keyboard;
extern std::string g_theme_handwriting;
extern std::string g_theme_voice;
extern std::shared_mutex g_voice_input_mutex;
extern VoiceInputConfig g_voice_input;
extern AiAssistantConfig g_ai_assistant;
extern TencentTmtConfig g_tencent_tmt;
extern CustomTranslationConfig g_custom_translation;
extern NiuTransConfig g_niutrans;
extern FrequencyAdjustmentConfig g_frequency_adjustment;
extern std::filesystem::path g_config_path;

bool IsHelpcodeSchemaAvailable(const std::string &schema);
VoiceInputConfig SnapshotVoiceInput();
std::string NormalizeSmallWindowUiBackend(const std::string &value);
bool IsValidCandidateSkinId(const std::string &skin);
bool IsValidCandidateFixedBadgeStyle(const std::string &style);
bool IsValidSettingsWindowLinger(const std::string &linger);
std::string AiAssistantTokenSlotKey(std::string_view provider);
void RememberConfigWriteTime();

// ime_config_toml.cpp
std::string EscapeTomlBasicString(const std::string &value);
bool ReplaceTomlValuePreservingFormatting(std::string &text, const std::string &section, const std::string &key,
                                          const std::string &replacement);
bool InsertTomlValuePreservingFormatting(std::string &text, const std::string &section, const std::string &key,
                                         const std::string &value);
std::string ReadFileText(const std::filesystem::path &path);
bool WriteFileBytes(const std::filesystem::path &path, const std::string &text);
bool WriteFileTextAtomically(const std::filesystem::path &path, const std::string &text);
bool TomlTextIsParseable(const std::string &text);
void ForEachTomlAssignment(const std::string &text, const TomlAssignmentVisitor &visit);
std::string MakeTomlAssignmentId(const std::string &section, const std::string &key);
std::map<std::string, std::string> ParseTomlAssignments(const std::string &text);

// ime_config_template.cpp
void RecoverLegacyAcpMangledConfig();
void SyncConfigWithInstalledTemplate();

// ime_config_write.cpp
bool WriteConfiguredValues(const std::vector<ConfigValueUpdate> &updates);
bool WriteConfiguredValue(const std::string &section, const std::string &key, const std::string &replacement);
void NotifyImeServer(UINT windowMessage, const wchar_t *auxMessage, WPARAM wParam = 0);
} // namespace ime_config_detail
