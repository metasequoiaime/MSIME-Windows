#include "window/ui_backend_policy.h"
#include "ime_config.h"
#include <fmt/xchar.h>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <dwrite.h>
#include <wrl/client.h>
#include <winreg.h>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <vector>
#include "utils/common_utils.h"
#include "global/globals.h"
#include "clipboard/clipboard_history.h"
#include "defines/defines.h"
#include "ipc/ipc.h"
#include "MetasequoiaImeEngine/common/helpcode_utils.h"
#include "voice-input/voice_providers.h"

namespace
{
using Microsoft::WRL::ComPtr;

constexpr int kCandidateFontSizeMin = 12;
constexpr int kCandidateFontSizeMax = 32;
constexpr double kFloatingToolbarScaleMin = 0.75;
constexpr double kFloatingToolbarScaleMax = 1.5;
constexpr int kFloatingToolbarFontSizeMin = 16;
constexpr int kFloatingToolbarFontSizeMax = 28;
constexpr int kFloatingToolbarFontSizeDefault = 24;
constexpr int kEnglishMixedInputMinCharsMin = 1;
constexpr int kEnglishMixedInputMinCharsMax = 8;
constexpr int kEnglishMixedInputMinCharsDefault = 2;

std::string g_session_backend = "legacy";
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
std::string g_candidate_text_color = "auto";
std::string g_shuangpin_schema = "xiaohe";
std::string g_wubi_schema = "wubi86";
std::string g_shuangpin_preedit_mode = "quanpin";
std::string g_tsf_preedit_style = "raw";
bool g_shuangpin_helpcode_enabled = true;
bool g_quanpin_helpcode_enabled = true;
std::string g_shuangpin_helpcode_schema = "lantian";
std::string g_quanpin_helpcode_schema = "lantian";
bool g_show_shuangpin_helpcode_in_candidate_window = true;
bool g_show_quanpin_helpcode_in_candidate_window = true;
// The legacy single "quanpin.autocorrect" key is deliberately not read anymore:
// both correction types default to off and users opt in from the settings page.
bool g_quanpin_autocorrect_transposition = false;
bool g_quanpin_autocorrect_neighbor = false;
bool g_floating_toolbar_enabled = true;
FloatingToolbarItemsConfig g_floating_toolbar_items;
double g_floating_toolbar_scale = 1.0;
int g_floating_toolbar_font_size = kFloatingToolbarFontSizeDefault;
bool g_english_candidates_enabled = false;
bool g_candidate_translations_enabled = true;
int g_english_mixed_input_min_chars = kEnglishMixedInputMinCharsDefault;
bool g_cloud_candidates_enabled = true;
bool g_emoji_mixed_input_enabled = false;
bool g_kaomoji_mixed_input_enabled = false;
bool g_unicode_mode_enabled = true;
bool g_quick_phrase_enabled = true;
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
bool g_candidate_arrow_navigation_enabled = true;
bool g_word_to_character_enabled = false;
std::string g_word_to_character_keys = "brackets";
bool g_smart_punctuation_enabled = true;
bool g_smart_punctuation_repeat_to_chinese_enabled = true;
bool g_paired_punctuation_enabled = true;
std::string g_punctuation_lock = "follow";
std::string g_candidate_window_layout = "vertical";
bool g_candidate_window_follow_cursor = true;
std::string g_ui_backend = "d2d";
std::string g_ui_backend_active = "d2d";
std::string g_candidate_skin = "fluent";
std::string g_candidate_window_preedit_style = "pinyin";
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
bool g_persist_asr_token_slot = false;
bool g_persist_polish_token_slot = false;
AiAssistantConfig g_ai_assistant;
TencentTmtConfig g_tencent_tmt;
CustomTranslationConfig g_custom_translation;
FrequencyAdjustmentConfig g_frequency_adjustment;
std::filesystem::path g_config_path;
std::optional<std::filesystem::file_time_type> g_config_last_write_time;

VoiceInputConfig SnapshotVoiceInput()
{
    std::shared_lock<std::shared_mutex> lock(g_voice_input_mutex);
    return g_voice_input;
}

void PublishVoiceInput(VoiceInputConfig voice)
{
    std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
    g_voice_input = std::move(voice);
}

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

const std::vector<std::string_view> &AiAssistantProviders()
{
    static const std::vector<std::string_view> providers{"deepseek", "openai", "siliconflow", "groq"};
    return providers;
}

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

std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos)
    {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

std::string EscapeTomlBasicString(const std::string &value)
{
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (char ch : value)
    {
        if (ch == '\n')
        {
            result += "\\n";
            continue;
        }
        if (ch == '\r')
        {
            result += "\\r";
            continue;
        }
        if (ch == '\t')
        {
            result += "\\t";
            continue;
        }
        if (ch == '\\' || ch == '"')
        {
            result.push_back('\\');
        }
        result.push_back(ch);
    }
    result.push_back('"');
    return result;
}

size_t FindTomlValueEnd(const std::string &line, size_t value_begin)
{
    if (value_begin >= line.size())
    {
        return value_begin;
    }

    const char quote = line[value_begin];
    if (quote == '"' || quote == '\'')
    {
        bool escaped = false;
        for (size_t i = value_begin + 1; i < line.size(); ++i)
        {
            if (quote == '"' && line[i] == '\\' && !escaped)
            {
                escaped = true;
                continue;
            }
            if (line[i] == quote && !escaped)
            {
                return i + 1;
            }
            escaped = false;
        }
        return line.size();
    }

    size_t end = value_begin;
    while (end < line.size() && line[end] != '#' && line[end] != '\r')
    {
        ++end;
    }
    while (end > value_begin && (line[end - 1] == ' ' || line[end - 1] == '\t'))
    {
        --end;
    }
    return end;
}

size_t FindTomlValueEndInText(const std::string &text, size_t value_begin);

bool ReplaceTomlValuePreservingFormatting(std::string &text, const std::string &section, const std::string &key,
                                          const std::string &replacement)
{
    bool in_section = false;
    size_t line_begin = 0;
    while (line_begin <= text.size())
    {
        const size_t newline = text.find('\n', line_begin);
        const size_t line_end = newline == std::string::npos ? text.size() : newline;
        const std::string line = text.substr(line_begin, line_end - line_begin);
        const std::string trimmed = Trim(line);

        if (!trimmed.empty() && trimmed.front() == '[')
        {
            const size_t close = trimmed.find(']');
            in_section = close != std::string::npos && Trim(trimmed.substr(1, close - 1)) == section;
        }
        else if (in_section && !trimmed.empty() && trimmed.front() != '#')
        {
            const size_t equals = line.find('=');
            if (equals != std::string::npos && Trim(line.substr(0, equals)) == key)
            {
                const size_t value_begin = line.find_first_not_of(" \t", equals + 1);
                if (value_begin == std::string::npos)
                {
                    return false;
                }
                const size_t absolute_begin = line_begin + value_begin;
                const size_t value_end = FindTomlValueEndInText(text, absolute_begin);
                text.replace(absolute_begin, value_end - absolute_begin, replacement);
                return true;
            }
        }

        if (newline == std::string::npos)
        {
            break;
        }
        line_begin = newline + 1;
    }
    return false;
}

bool InsertTomlValuePreservingFormatting(std::string &text, const std::string &section, const std::string &key,
                                         const std::string &value)
{
    const std::string section_header = "[" + section + "]";
    const size_t section_begin = text.find(section_header);
    if (section_begin == std::string::npos)
    {
        // 出厂模板不带全部设置段（如 [quanpin]），首次安装后第一次写这类键不能失败，
        // 否则设置页报「保存失败」。把缺失的段追加到文件尾部，语义由末尾的 toml::parse 校验兜底。
        if (!text.empty() && text.back() != '\n')
        {
            text.push_back('\n');
        }
        text.append(section_header + "\n" + key + " = " + value + "\n");
        return true;
    }

    const size_t section_line_end = text.find('\n', section_begin + section_header.size());
    if (section_line_end == std::string::npos)
    {
        text.append("\n" + key + " = " + value + "\n");
        return true;
    }

    size_t insert_pos = text.find("\n[", section_line_end);
    if (insert_pos == std::string::npos)
    {
        insert_pos = text.size();
        if (!text.empty() && text.back() != '\n')
        {
            text.push_back('\n');
            insert_pos = text.size();
        }
    }
    else
    {
        ++insert_pos;
    }

    text.insert(insert_pos, key + " = " + value + "\n");
    return true;
}

// 安装包装入的本版出厂模板，以及上次合并时用的那份模板（升级基线）。
const char *const kConfigTemplateFileName = "config.default.toml";
const char *const kConfigBaselineFileName = "config.base.toml";

std::string ReadFileText(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void ClearReadOnlyAttribute(const std::filesystem::path &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_READONLY) == 0)
    {
        return;
    }
    SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
}

bool WriteFileBytes(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    return static_cast<bool>(output);
}

bool WriteFileTextAtomically(const std::filesystem::path &path, const std::string &text)
{
    std::filesystem::path temp_path = path;
    temp_path += L".tmp";
    ClearReadOnlyAttribute(path);
    ClearReadOnlyAttribute(temp_path);
    if (!WriteFileBytes(temp_path, text))
    {
        return false;
    }
    if (MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return true;
    }
    const bool replaced = WriteFileBytes(path, text);
    std::error_code error;
    std::filesystem::remove(temp_path, error);
    return replaced;
}

bool TomlTextIsParseable(const std::string &text)
{
    if (text.empty())
    {
        return false;
    }
    try
    {
        (void)toml::parse(text);
        return true;
    }
    catch (const toml::parse_error &)
    {
        return false;
    }
}

// 与 FindTomlValueEnd 相同，但值可以跨行（ai_assistant.prompt 用的是 """ 多行字符串）。
size_t FindTomlValueEndInText(const std::string &text, size_t value_begin)
{
    if (value_begin < text.size() && text[value_begin] == '[')
    {
        int depth = 1;
        for (size_t i = value_begin + 1; i < text.size(); ++i)
        {
            if (text[i] == '"' || text[i] == '\'')
                i = FindTomlValueEndInText(text, i) - 1;
            else if (text[i] == '#')
            {
                i = text.find('\n', i);
                if (i == std::string::npos)
                    return text.size();
            }
            else if (text[i] == '[')
                ++depth;
            else if (text[i] == ']' && --depth == 0)
                return i + 1;
        }
        return text.size();
    }
    for (const char *delimiter : {"\"\"\"", "'''"})
    {
        if (value_begin + 3 > text.size() || text.compare(value_begin, 3, delimiter) != 0)
        {
            continue;
        }
        const bool escapable = delimiter[0] == '"';
        size_t i = value_begin + 3;
        while (i + 3 <= text.size())
        {
            if (escapable && text[i] == '\\')
            {
                i += 2;
                continue;
            }
            if (text.compare(i, 3, delimiter) == 0)
            {
                return i + 3;
            }
            ++i;
        }
        return text.size();
    }

    const size_t newline = text.find('\n', value_begin);
    const size_t line_end = newline == std::string::npos ? text.size() : newline;
    return value_begin + FindTomlValueEnd(text.substr(value_begin, line_end - value_begin), 0);
}

using TomlAssignmentVisitor =
    std::function<void(const std::string &section, const std::string &key, size_t value_begin, size_t value_end)>;

void ForEachTomlAssignment(const std::string &text, const TomlAssignmentVisitor &visit)
{
    std::string section;
    size_t line_begin = 0;
    while (line_begin < text.size())
    {
        const size_t newline = text.find('\n', line_begin);
        const size_t line_end = newline == std::string::npos ? text.size() : newline;
        const std::string line = text.substr(line_begin, line_end - line_begin);
        const std::string trimmed = Trim(line);
        size_t next_line_begin = newline == std::string::npos ? text.size() : newline + 1;

        if (!trimmed.empty() && trimmed.front() == '[')
        {
            const size_t close = trimmed.find(']');
            section = close == std::string::npos ? std::string() : Trim(trimmed.substr(1, close - 1));
        }
        else if (!trimmed.empty() && trimmed.front() != '#')
        {
            const size_t equals = line.find('=');
            const std::string key = equals == std::string::npos ? std::string() : Trim(line.substr(0, equals));
            const size_t value_offset =
                equals == std::string::npos ? std::string::npos : line.find_first_not_of(" \t", equals + 1);
            if (!key.empty() && value_offset != std::string::npos)
            {
                const size_t value_begin = line_begin + value_offset;
                const size_t value_end = FindTomlValueEndInText(text, value_begin);
                visit(section, key, value_begin, value_end);
                if (value_end > line_end)
                {
                    // 多行值：跳过它占用的所有行，避免把字符串内容当成新的键。
                    const size_t after = text.find('\n', value_end);
                    next_line_begin = after == std::string::npos ? text.size() : after + 1;
                }
            }
        }

        line_begin = next_line_begin;
    }
}

std::string MakeTomlAssignmentId(const std::string &section, const std::string &key)
{
    return section + '\x01' + key;
}

std::map<std::string, std::string> ParseTomlAssignments(const std::string &text)
{
    std::map<std::string, std::string> values;
    ForEachTomlAssignment(
        text, [&](const std::string &section, const std::string &key, size_t value_begin, size_t value_end) {
            values[MakeTomlAssignmentId(section, key)] = text.substr(value_begin, value_end - value_begin);
        });
    return values;
}

// 以新模板为骨架（注释、分节顺序、新增项都来自新版），只把用户改过的值填回去。
std::string MergeTomlIntoTemplate(const std::string &template_text, std::map<std::string, std::string> user_values,
                                  const std::map<std::string, std::string> &baseline_values)
{
    const auto fallback_id = MakeTomlAssignmentId("appearance", "fallback_fonts");
    if (!user_values.empty() && user_values.find(fallback_id) == user_values.end())
    {
        const auto font = user_values.find(MakeTomlAssignmentId("appearance", "font"));
        const auto default_font = user_values.find(MakeTomlAssignmentId("appearance", "default_font"));
        user_values[fallback_id] = "[" + (font == user_values.end() ? "\"Noto Sans SC\"" : font->second) + ", " +
                                   (default_font == user_values.end() ? "\"Microsoft YaHei\"" : default_font->second) +
                                   "]";
    }
    struct ValuePatch
    {
        size_t begin;
        size_t end;
        std::string value;
    };
    std::vector<ValuePatch> patches;

    ForEachTomlAssignment(
        template_text, [&](const std::string &section, const std::string &key, size_t value_begin, size_t value_end) {
            const std::string id = MakeTomlAssignmentId(section, key);
            const auto user = user_values.find(id);
            if (user == user_values.end())
            {
                return;
            }
            // 仍等于上一版默认值，说明用户没动过这一项，让新版默认值生效。
            const auto baseline = baseline_values.find(id);
            if (baseline != baseline_values.end() && baseline->second == user->second)
            {
                return;
            }
            patches.push_back({value_begin, value_end, user->second});
        });

    std::string merged = template_text;
    for (auto patch = patches.rbegin(); patch != patches.rend(); ++patch)
    {
        merged.replace(patch->begin, patch->end - patch->begin, patch->value);
    }
    return merged;
}

std::filesystem::path AcpDecodedUtf8Path(const std::filesystem::path &wide_path)
{
    try
    {
        return std::filesystem::path(wide_path.u8string());
    }
    catch (...)
    {
        return {};
    }
}

void RecoverLegacyAcpMangledConfig()
{
    const std::filesystem::path data_dir = g_config_path.parent_path();
    const std::filesystem::path mangled_dir = AcpDecodedUtf8Path(data_dir);
    if (mangled_dir.empty() || mangled_dir == data_dir)
    {
        return;
    }

    const std::filesystem::path mangled_config = mangled_dir / L"config.toml";
    std::error_code error;
    if (!std::filesystem::is_regular_file(mangled_config, error))
    {
        return;
    }

    const std::string mangled_text = ReadFileText(mangled_config);
    if (!TomlTextIsParseable(mangled_text))
    {
        return;
    }

    ConfigFileLock lock;
    if (!lock)
    {
        return;
    }

    const std::string real_text = ReadFileText(g_config_path);
    const std::string template_text = ReadFileText(data_dir / kConfigTemplateFileName);
    const bool real_unusable = !TomlTextIsParseable(real_text);
    const bool real_is_stock = !template_text.empty() && real_text == template_text;
    const bool real_never_saved = !std::filesystem::is_regular_file(data_dir / kConfigBaselineFileName, error);
    if (!real_unusable && !real_is_stock && !real_never_saved)
    {
        return;
    }

    if (!WriteFileTextAtomically(g_config_path, mangled_text))
    {
        return;
    }

    const std::filesystem::path mangled_baseline = mangled_dir / kConfigBaselineFileName;
    if (std::filesystem::is_regular_file(mangled_baseline, error))
    {
        const std::string baseline_text = ReadFileText(mangled_baseline);
        if (!baseline_text.empty())
        {
            WriteFileTextAtomically(data_dir / kConfigBaselineFileName, baseline_text);
        }
    }
}

void SyncConfigWithInstalledTemplate()
{
    const std::filesystem::path data_dir = g_config_path.parent_path();
    const std::filesystem::path template_path = data_dir / kConfigTemplateFileName;
    const std::string template_text = ReadFileText(template_path);
    if (template_text.empty())
    {
        return;
    }

    ConfigFileLock lock;
    if (!lock)
    {
        return;
    }

    const std::filesystem::path baseline_path = data_dir / kConfigBaselineFileName;
    std::error_code exists_error;
    const bool config_exists = std::filesystem::is_regular_file(g_config_path, exists_error);
    const auto config_size =
        config_exists ? std::filesystem::file_size(g_config_path, exists_error) : std::uintmax_t{0};
    const std::string user_text = ReadFileText(g_config_path);
    if (!TomlTextIsParseable(user_text))
    {
        if (config_exists && config_size > 0 && user_text.empty())
        {
            return;
        }
        if (WriteFileTextAtomically(g_config_path, template_text))
        {
            WriteFileTextAtomically(baseline_path, template_text);
        }
        return;
    }

    const std::string baseline_text = ReadFileText(baseline_path);
    if (baseline_text == template_text)
    {
        return;
    }

    const std::string merged =
        MergeTomlIntoTemplate(template_text, ParseTomlAssignments(user_text), ParseTomlAssignments(baseline_text));
    if (!TomlTextIsParseable(merged))
    {
        return;
    }

    if (WriteFileTextAtomically(g_config_path, merged))
    {
        WriteFileTextAtomically(baseline_path, template_text);
    }
}

void RememberConfigWriteTime()
{
    std::error_code error;
    const auto write_time = std::filesystem::last_write_time(g_config_path, error);
    if (!error)
    {
        g_config_last_write_time = write_time;
    }
}

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
        g_shuangpin_schema = tbl["input"]["shuangpin_schema"].value_or(std::string("xiaohe"));
        g_wubi_schema = tbl["input"]["wubi_schema"].value_or(std::string("wubi86"));
        g_shuangpin_preedit_mode = tbl["input"]["shuangpin_preedit_mode"].value_or(std::string("quanpin"));
        g_shuangpin_helpcode_enabled = tbl["helpcode"]["shuangpin_helpcode"].value_or(true);
        g_quanpin_helpcode_enabled = tbl["helpcode"]["quanpin_helpcode"].value_or(true);
        const std::string shuangpin_helpcode_schema =
            tbl["helpcode"]["shuangpin_helpcode_schema"].value_or(std::string("lantian"));
        g_shuangpin_helpcode_schema = HelpcodeUtils::is_supported_helpcode_schema(shuangpin_helpcode_schema)
                                          ? shuangpin_helpcode_schema
                                          : "lantian";
        const std::string quanpin_helpcode_schema =
            tbl["helpcode"]["quanpin_helpcode_schema"].value_or(std::string("lantian"));
        g_quanpin_helpcode_schema =
            HelpcodeUtils::is_supported_helpcode_schema(quanpin_helpcode_schema) ? quanpin_helpcode_schema : "lantian";
        g_show_shuangpin_helpcode_in_candidate_window =
            tbl["helpcode"]["show_sp_helpcode_in_candidate_window"].value_or(true);
        g_show_quanpin_helpcode_in_candidate_window =
            tbl["helpcode"]["show_qp_helpcode_in_candidate_window"].value_or(true);
        g_quanpin_autocorrect_transposition = tbl["quanpin"]["autocorrect_transposition"].value_or(false);
        g_quanpin_autocorrect_neighbor = tbl["quanpin"]["autocorrect_neighbor"].value_or(false);
        g_floating_toolbar_enabled = tbl["general"]["floating_toolbar"].value_or(true);
        // Read the old candidate-only key as a migration fallback. New writes
        // use the unified key.
        g_diagnostic_log_enabled.store(tbl["general"]["diagnostic_log"].value_or(
                                           tbl["general"]["candidate_window_diagnostic_log"].value_or(false)),
                                       std::memory_order_relaxed);
        g_tsf_diagnostic_log_enabled.store(tbl["general"]["tsf_diagnostic_log"].value_or(false),
                                           std::memory_order_relaxed);
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
        g_emoji_mixed_input_enabled = tbl["general"]["emoji_mixed_input"].value_or(false);
        g_kaomoji_mixed_input_enabled = tbl["general"]["kaomoji_mixed_input"].value_or(false);
        g_unicode_mode_enabled = tbl["utility"]["unicode_mode"].value_or(true);
        g_quick_phrase_enabled = tbl["utility"]["quick_phrase"].value_or(true);
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
        g_smart_punctuation_enabled = tbl["input"]["smart_punctuation"].value_or(true);
        g_smart_punctuation_repeat_to_chinese_enabled =
            tbl["input"]["smart_punctuation_repeat_to_chinese"].value_or(true);
        g_paired_punctuation_enabled = tbl["input"]["paired_punctuation"].value_or(true);
        {
            const std::string lock = tbl["input"]["punctuation_lock"].value_or(std::string("follow"));
            g_punctuation_lock = lock == "chinese" || lock == "english" ? lock : "follow";
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
            GlobalSettings::setTsfPreeditStyle(g_tsf_preedit_style);
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

struct ConfigValueUpdate
{
    std::string section;
    std::string key;
    std::string value;
};

bool WriteConfiguredValues(std::initializer_list<ConfigValueUpdate> updates)
{
    ConfigFileLock lock;
    if (!lock)
        return false;
    std::error_code exists_error;
    const bool config_exists = std::filesystem::is_regular_file(g_config_path, exists_error);
    const auto config_size =
        config_exists ? std::filesystem::file_size(g_config_path, exists_error) : std::uintmax_t{0};
    std::string text = ReadFileText(g_config_path);
    if (!TomlTextIsParseable(text))
    {
        if (config_exists && config_size > 0 && text.empty())
        {
            return false;
        }
        text = ReadFileText(g_config_path.parent_path() / kConfigTemplateFileName);
        if (!TomlTextIsParseable(text))
        {
            return false;
        }
    }

    for (const auto &update : updates)
    {
        if (!ReplaceTomlValuePreservingFormatting(text, update.section, update.key, update.value) &&
            !InsertTomlValuePreservingFormatting(text, update.section, update.key, update.value))
        {
            return false;
        }
    }

    try
    {
        (void)toml::parse(text);
    }
    catch (const toml::parse_error &)
    {
        return false;
    }

    // Write a temp file and rename it so a crash can never leave the user with a truncated config. This only makes the
    // directory entry swap atomic: there is no FlushFileBuffers, so a power loss can still lose the contents.
    if (!WriteFileTextAtomically(g_config_path, text))
    {
        return false;
    }

    RememberConfigWriteTime();
    NotifyImeServerConfigChanged();
    return true;
}

bool WriteConfiguredValue(const std::string &section, const std::string &key, const std::string &replacement)
{
    return WriteConfiguredValues({{section, key, replacement}});
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
} // namespace

void InvalidateImeConfigWriteTime()
{
    g_config_last_write_time.reset();
}

namespace
{
HWND FindImeServerCandidateWindow()
{
    // Candidate, tray menu, and floating toolbar share the same class. FindWindow
    // without a title often hits the toolbar, which does not apply config.
    if (const HWND hwnd = FindWindowW(L"metasequoiaime_windows", L"metaseuqoiaimecandwnd"))
    {
        return hwnd;
    }
    return FindWindowW(L"metasequoiaime_windows", nullptr);
}

bool SendAuxConfigNotification(const wchar_t *message)
{
    HANDLE pipe = CreateFileW(FANY_IME_AUX_NAMED_PIPE, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
        WaitNamedPipeW(FANY_IME_AUX_NAMED_PIPE, 200))
    {
        pipe = CreateFileW(FANY_IME_AUX_NAMED_PIPE, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD bytesWritten = 0;
    const DWORD byteCount = static_cast<DWORD>(wcslen(message) * sizeof(wchar_t));
    const bool sent = WriteFile(pipe, message, byteCount, &bytesWritten, nullptr) != FALSE && bytesWritten == byteCount;
    CloseHandle(pipe);
    return sent;
}

void NotifyImeServer(UINT windowMessage, const wchar_t *auxMessage, WPARAM wParam = 0)
{
    const HWND hwnd = FindImeServerCandidateWindow();
    DWORD serverProcessId = 0;
    if (hwnd)
    {
        GetWindowThreadProcessId(hwnd, &serverProcessId);
    }

    if (serverProcessId == GetCurrentProcessId())
    {
        PostMessageW(hwnd, windowMessage, wParam, 0);
        return;
    }

    // MetasequoiaImeServer runs with uiAccess while the standalone Settings
    // process does not. Cross-process WM_USER delivery can therefore be
    // rejected by UIPI. The session-less Aux pipe is already the supported
    // cross-integrity control path; use it for config invalidation as well.
    if (!SendAuxConfigNotification(auxMessage) && hwnd)
    {
        // Retain the old route as a best-effort fallback. The Server's periodic
        // file watcher remains the final recovery path if both transports are
        // temporarily unavailable during startup.
        PostMessageW(hwnd, windowMessage, wParam, 0);
    }
}
} // namespace

void NotifyImeServerConfigChanged()
{
    NotifyImeServer(WM_APPLY_IME_CONFIG, L"ConfigChanged");
}

void NotifyImeServerCandidateSkinRefresh()
{
    NotifyImeServer(WM_APPLY_IME_CONFIG, L"CandidateSkinRefresh", 1);
}

void NotifyImeServerInputSchemeChanged()
{
    NotifyImeServer(WM_APPLY_IME_INPUT_SCHEME, L"InputSchemeChanged");
}

std::string MergeConfigIntoTemplate(const std::string &template_text, const std::string &user_text,
                                    const std::string &baseline_text)
{
    return MergeTomlIntoTemplate(template_text, ParseTomlAssignments(user_text), ParseTomlAssignments(baseline_text));
}

void InitImeConfig()
{
    // Build the path from the wide accessor: std::filesystem::path(std::string) decodes with the
    // system ANSI code page, which corrupts a non-ASCII (e.g. Chinese) user profile path on a
    // non-UTF-8 ACP machine and makes every config read/write fail ("设置保存失败").
    g_config_path = std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"config.toml";
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

int GetConfiguredCandidatePageSize()
{
    return g_candidate_page_size;
}

bool SetConfiguredCandidatePageSize(int page_size)
{
    if (page_size < 3 || page_size > 9)
        return false;
    if (!WriteConfiguredValue("appearance", "page_size", std::to_string(page_size)))
        return false;
    g_candidate_page_size = page_size;
    return true;
}

const std::string &GetConfiguredCandidateFont()
{
    return g_candidate_font;
}

bool SetConfiguredCandidateFont(const std::string &font)
{
    if (font.empty() || font.size() > 64)
        return false;
    if (!WriteConfiguredValue("appearance", "font", EscapeTomlBasicString(font)))
        return false;
    g_candidate_font = font;
    return true;
}

const std::string &GetConfiguredCandidateEnglishFont()
{
    return g_candidate_english_font;
}

bool SetConfiguredCandidateEnglishFont(const std::string &font)
{
    if (font.empty() || font.size() > 64)
        return false;
    if (!WriteConfiguredValue("appearance", "english_font", EscapeTomlBasicString(font)))
        return false;
    g_candidate_english_font = font;
    return true;
}

const std::string &GetConfiguredCandidateDefaultFont()
{
    return g_candidate_default_font;
}

bool SetConfiguredCandidateDefaultFont(const std::string &font)
{
    if (font.empty() || font.size() > 64)
        return false;
    if (!WriteConfiguredValue("appearance", "default_font", EscapeTomlBasicString(font)))
        return false;
    g_candidate_default_font = font;
    return true;
}

int GetConfiguredCandidateFontSize()
{
    return g_candidate_font_size;
}

const std::vector<std::string> &GetConfiguredCandidateFallbackFonts()
{
    return g_candidate_fallback_fonts;
}

std::vector<std::string> GetConfiguredCandidateFallbackFontFamilies()
{
    std::vector<std::string> families;
    for (const auto &font : g_candidate_fallback_fonts)
        families.push_back(ResolveSystemFontFamilyForCss(font));
    return families;
}

bool SetConfiguredCandidateFallbackFonts(const std::vector<std::string> &fonts)
{
    if (fonts.size() > 32)
        return false;
    std::vector<std::string> normalized;
    std::string value = "[";
    for (const auto &font : fonts)
    {
        if (font.empty() || font.size() > 256 ||
            std::any_of(font.begin(), font.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }))
            return false;
        if (std::find(normalized.begin(), normalized.end(), font) != normalized.end())
            continue;
        if (!normalized.empty())
            value += ", ";
        value += EscapeTomlBasicString(font);
        normalized.push_back(font);
    }
    value += "]";
    if (!WriteConfiguredValue("appearance", "fallback_fonts", value))
        return false;
    g_candidate_fallback_fonts = std::move(normalized);
    return true;
}

bool SetConfiguredCandidateFontSize(int font_size)
{
    if (font_size < kCandidateFontSizeMin || font_size > kCandidateFontSizeMax)
        return false;
    if (!WriteConfiguredValue("appearance", "font_size", std::to_string(font_size)))
        return false;
    g_candidate_font_size = font_size;
    return true;
}

int GetConfiguredCandidateWindowPreeditFontSize()
{
    return g_candidate_window_preedit_font_size;
}

bool SetConfiguredCandidateWindowPreeditFontSize(int font_size)
{
    if (font_size < kCandidateFontSizeMin || font_size > kCandidateFontSizeMax)
        return false;
    if (!WriteConfiguredValue("appearance", "candidate_window_preedit_font_size", std::to_string(font_size)))
        return false;
    g_candidate_window_preedit_font_size = font_size;
    return true;
}

bool GetConfiguredDiagnosticLogEnabled()
{
    return g_diagnostic_log_enabled.load(std::memory_order_relaxed);
}

bool SetConfiguredDiagnosticLogEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "diagnostic_log", enabled ? "true" : "false"))
        return false;
    g_diagnostic_log_enabled.store(enabled, std::memory_order_relaxed);
    return true;
}

bool GetConfiguredTsfDiagnosticLogEnabled()
{
    return g_tsf_diagnostic_log_enabled.load(std::memory_order_relaxed);
}

bool SetConfiguredTsfDiagnosticLogEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "tsf_diagnostic_log", enabled ? "true" : "false"))
        return false;
    g_tsf_diagnostic_log_enabled.store(enabled, std::memory_order_relaxed);
    return true;
}

namespace
{
bool IsValidCandidateTextColor(const std::string &color)
{
    if (color.empty() || color == "auto")
        return true;
    if (color.size() != 7 || color[0] != '#')
        return false;
    for (size_t i = 1; i < color.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(color[i]);
        if (!std::isxdigit(ch))
            return false;
    }
    return true;
}
} // namespace

const std::string &GetConfiguredCandidateTextColor()
{
    return g_candidate_text_color;
}

bool SetConfiguredCandidateTextColor(const std::string &color)
{
    const std::string normalized = color.empty() ? "auto" : color;
    if (!IsValidCandidateTextColor(normalized))
        return false;
    if (!WriteConfiguredValue("appearance", "cand_text_color", EscapeTomlBasicString(normalized)))
        return false;
    g_candidate_text_color = normalized;
    return true;
}

namespace
{
int CALLBACK EnumSystemFontFamExProc(const LOGFONTW *logfont, const TEXTMETRICW *, DWORD, LPARAM lparam)
{
    auto *families = reinterpret_cast<std::set<std::wstring> *>(lparam);
    if (!logfont || !families)
        return TRUE;
    if (logfont->lfFaceName[0] == L'\0' || logfont->lfFaceName[0] == L'@')
        return TRUE;
    families->insert(logfont->lfFaceName);
    return TRUE;
}

ComPtr<IDWriteFactory> GetSharedFontFactory()
{
    static const ComPtr<IDWriteFactory> factory = [] {
        ComPtr<IDWriteFactory> value;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(value.GetAddressOf()));
        return value;
    }();
    return factory;
}

std::wstring GetPreferredLocalizedFontName(IDWriteLocalizedStrings *names)
{
    if (!names || names->GetCount() == 0)
        return {};

    UINT32 index = 0;
    BOOL exists = FALSE;
    wchar_t user_locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(user_locale, LOCALE_NAME_MAX_LENGTH) > 0)
        names->FindLocaleName(user_locale, &index, &exists);
    if (!exists)
        names->FindLocaleName(L"zh-cn", &index, &exists);
    if (!exists)
        names->FindLocaleName(L"en-us", &index, &exists);
    if (!exists)
        index = 0;

    UINT32 length = 0;
    if (FAILED(names->GetStringLength(index, &length)))
        return {};
    std::wstring value(length + 1, L'\0');
    if (FAILED(names->GetString(index, value.data(), length + 1)))
        return {};
    value.resize(length);
    return value;
}
} // namespace

std::string ResolveSystemFontFamilyForCss(const std::string &font)
{
    if (font.empty())
        return font;

    static std::mutex cache_mutex;
    static std::map<std::string, std::string> cache;
    {
        const std::lock_guard<std::mutex> lock(cache_mutex);
        const auto cached = cache.find(font);
        if (cached != cache.end())
            return cached->second;
    }

    const auto remember = [&](std::string value) {
        const std::lock_guard<std::mutex> lock(cache_mutex);
        return cache.emplace(font, std::move(value)).first->second;
    };

    const ComPtr<IDWriteFactory> factory = GetSharedFontFactory();
    if (!factory)
        return remember(font);

    ComPtr<IDWriteGdiInterop> gdi_interop;
    if (FAILED(factory->GetGdiInterop(&gdi_interop)) || !gdi_interop)
        return remember(font);

    LOGFONTW logfont{};
    logfont.lfCharSet = DEFAULT_CHARSET;
    logfont.lfWeight = FW_NORMAL;
    const std::wstring requested = string_to_wstring(font);
    if (requested.size() >= LF_FACESIZE)
        return remember(font);
    wcscpy_s(logfont.lfFaceName, requested.c_str());

    ComPtr<IDWriteFont> resolved_font;
    if (FAILED(gdi_interop->CreateFontFromLOGFONT(&logfont, &resolved_font)) || !resolved_font)
        return remember(font);

    ComPtr<IDWriteLocalizedStrings> typographic_names;
    BOOL has_typographic_names = FALSE;
    if (SUCCEEDED(resolved_font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_TYPOGRAPHIC_FAMILY_NAMES,
                                                         &typographic_names, &has_typographic_names)) &&
        has_typographic_names)
    {
        const std::wstring name = GetPreferredLocalizedFontName(typographic_names.Get());
        if (!name.empty())
            return remember(wstring_to_string(name));
    }

    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteLocalizedStrings> family_names;
    if (SUCCEEDED(resolved_font->GetFontFamily(&family)) && family && SUCCEEDED(family->GetFamilyNames(&family_names)))
    {
        const std::wstring name = GetPreferredLocalizedFontName(family_names.Get());
        if (!name.empty())
            return remember(wstring_to_string(name));
    }
    return remember(font);
}

const std::vector<std::string> &GetSystemFontFamilies()
{
    static std::vector<std::string> cached;
    static bool loaded = false;
    if (loaded)
        return cached;

    std::set<std::wstring> families;
    const ComPtr<IDWriteFactory> factory = GetSharedFontFactory();
    ComPtr<IDWriteFontCollection> collection;
    if (factory && SUCCEEDED(factory->GetSystemFontCollection(&collection)) && collection)
    {
        const UINT32 count = collection->GetFontFamilyCount();
        for (UINT32 index = 0; index < count; ++index)
        {
            ComPtr<IDWriteFontFamily> family;
            ComPtr<IDWriteLocalizedStrings> names;
            if (SUCCEEDED(collection->GetFontFamily(index, &family)) && family &&
                SUCCEEDED(family->GetFamilyNames(&names)))
            {
                const std::wstring name = GetPreferredLocalizedFontName(names.Get());
                if (!name.empty() && name[0] != L'@')
                    families.insert(name);
            }
        }
    }

    // Older systems can fail DirectWrite collection creation. Keep the old GDI
    // enumeration as a fallback, but do not mix it into the normal list: GDI
    // face names often contain a weight suffix that Chromium cannot match as a
    // CSS font family.
    if (families.empty())
    {
        HDC hdc = GetDC(nullptr);
        if (hdc)
        {
            LOGFONTW probe{};
            probe.lfCharSet = DEFAULT_CHARSET;
            EnumFontFamiliesExW(hdc, &probe, EnumSystemFontFamExProc, reinterpret_cast<LPARAM>(&families), 0);
            ReleaseDC(nullptr, hdc);
        }
    }

    cached.reserve(families.size());
    for (const std::wstring &face : families)
    {
        cached.push_back(wstring_to_string(face));
    }
    loaded = true;
    return cached;
}

SchemeType GetConfiguredInputScheme()
{
    return g_input_scheme;
}

SchemeType GetConfiguredActiveInputScheme()
{
    return g_input_mode == "japanese" ? SchemeType::JapaneseRomaji : g_input_scheme;
}

std::string GetConfiguredInputSchemeName()
{
    switch (g_input_scheme)
    {
    case SchemeType::Quanpin:
        return "quanpin";
    case SchemeType::Shuangpin:
        return "shuangpin";
    case SchemeType::Wubi:
        return "wubi";
    default:
        return "shuangpin";
    }
}

bool SetConfiguredInputScheme(const std::string &scheme)
{
    if (scheme != "quanpin" && scheme != "shuangpin" && scheme != "wubi")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "schema", EscapeTomlBasicString(scheme)))
    {
        return false;
    }
    g_input_scheme = ParseScheme(scheme);
    NotifyImeServerInputSchemeChanged();
    return true;
}

const std::string &GetConfiguredCharacterSet()
{
    return g_character_set;
}

bool SetConfiguredCharacterSet(const std::string &character_set)
{
    if (character_set != "simplified" && character_set != "traditional")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "character_set", EscapeTomlBasicString(character_set)))
    {
        return false;
    }
    g_character_set = character_set;
    // Marshal WebView/native toolbar refreshes to their owner thread. Settings
    // in a separate process reaches the same refresh through ConfigChanged.
    NotifyImeServer(WM_REFRESH_CHARACTER_SET, L"ConfigChanged");
    return true;
}

bool GetConfiguredCharacterSetShortcutEnabled()
{
    return g_character_set_shortcut_enabled;
}

bool SetConfiguredCharacterSetShortcutEnabled(bool enabled)
{
    if (!WriteConfiguredValue("keybindings", "toggle_character_set_ctrl_shift_f", enabled ? "true" : "false"))
        return false;
    g_character_set_shortcut_enabled = enabled;
    return true;
}

const std::string &GetConfiguredDefaultImeMode()
{
    return g_default_ime_mode;
}

bool SetConfiguredDefaultImeMode(const std::string &mode)
{
    if (mode != "chinese" && mode != "english")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "default_ime_mode", EscapeTomlBasicString(mode)))
    {
        return false;
    }
    g_default_ime_mode = mode;
    return true;
}

const std::string &GetConfiguredImeModeScope()
{
    return g_ime_mode_scope;
}

bool SetConfiguredImeModeScope(const std::string &scope)
{
    if (scope != "app" && scope != "global")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "ime_mode_scope", EscapeTomlBasicString(scope)))
    {
        return false;
    }
    g_ime_mode_scope = scope;
    return true;
}

bool IsConfiguredImeModeScopeGlobal()
{
    return g_ime_mode_scope == "global";
}

bool GetConfiguredSwitchLanguageShiftEnabled()
{
    return g_switch_language_shift_enabled;
}

bool SetConfiguredSwitchLanguageShiftEnabled(bool enabled)
{
    if (!WriteConfiguredValue("keybindings", "switch_language_shift", enabled ? "true" : "false"))
    {
        return false;
    }
    g_switch_language_shift_enabled = enabled;
    return true;
}

bool GetConfiguredSwitchLanguageCtrlEnabled()
{
    return g_switch_language_ctrl_enabled;
}

bool SetConfiguredSwitchLanguageCtrlEnabled(bool enabled)
{
    if (!WriteConfiguredValue("keybindings", "switch_language_ctrl", enabled ? "true" : "false"))
    {
        return false;
    }
    g_switch_language_ctrl_enabled = enabled;
    return true;
}

bool GetConfiguredSwitchLanguageCtrlAltSpaceEnabled()
{
    return g_switch_language_ctrl_alt_space_enabled;
}

bool SetConfiguredSwitchLanguageCtrlAltSpaceEnabled(bool enabled)
{
    if (!WriteConfiguredValue("keybindings", "switch_language_ctrl_alt_space", enabled ? "true" : "false"))
    {
        return false;
    }
    g_switch_language_ctrl_alt_space_enabled = enabled;
    return true;
}

const std::string &GetConfiguredShuangpinSchema()
{
    return g_shuangpin_schema;
}

bool SetConfiguredShuangpinSchema(const std::string &schema)
{
    if (schema != "xiaohe" && schema != "ziranma" && schema != "shoudao" && schema != "microsoft" && schema != "jiajia")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "shuangpin_schema", EscapeTomlBasicString(schema)))
    {
        return false;
    }
    g_shuangpin_schema = schema;
    return true;
}

const std::string &GetConfiguredWubiSchema()
{
    return g_wubi_schema;
}

bool SetConfiguredWubiSchema(const std::string &schema)
{
    if (schema != "wubi86")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "wubi_schema", EscapeTomlBasicString(schema)))
    {
        return false;
    }
    g_wubi_schema = schema;
    return true;
}

const std::string &GetConfiguredShuangpinPreeditMode()
{
    return g_shuangpin_preedit_mode;
}

const std::string &GetConfiguredTsfPreeditStyle()
{
    return g_tsf_preedit_style;
}

bool SetConfiguredTsfPreeditStyle(const std::string &style)
{
    if (!GlobalSettings::isKnownTsfPreeditStyle(style))
    {
        return false;
    }
    if (!WriteConfiguredValue("appearance", "tsf_preedit_style", EscapeTomlBasicString(style)))
    {
        return false;
    }
    g_tsf_preedit_style = style;
    GlobalSettings::setTsfPreeditStyle(style);
    return true;
}

bool GetConfiguredShuangpinHelpcodeEnabled()
{
    return g_shuangpin_helpcode_enabled;
}

bool SetConfiguredShuangpinHelpcodeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "shuangpin_helpcode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_shuangpin_helpcode_enabled = enabled;
    return true;
}

const std::string &GetConfiguredShuangpinHelpcodeSchema()
{
    return g_shuangpin_helpcode_schema;
}

bool SetConfiguredShuangpinHelpcodeSchema(const std::string &schema)
{
    if (!HelpcodeUtils::is_supported_helpcode_schema(schema))
        return false;
    if (!WriteConfiguredValue("helpcode", "shuangpin_helpcode_schema", EscapeTomlBasicString(schema)))
        return false;
    g_shuangpin_helpcode_schema = schema;
    return true;
}

bool GetConfiguredQuanpinHelpcodeEnabled()
{
    return g_quanpin_helpcode_enabled;
}

bool SetConfiguredQuanpinHelpcodeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "quanpin_helpcode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quanpin_helpcode_enabled = enabled;
    return true;
}

bool GetConfiguredQuanpinAutocorrectTransposition()
{
    return g_quanpin_autocorrect_transposition;
}

bool SetConfiguredQuanpinAutocorrectTransposition(bool enabled)
{
    if (!WriteConfiguredValue("quanpin", "autocorrect_transposition", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quanpin_autocorrect_transposition = enabled;
    return true;
}

bool GetConfiguredQuanpinAutocorrectNeighbor()
{
    return g_quanpin_autocorrect_neighbor;
}

bool SetConfiguredQuanpinAutocorrectNeighbor(bool enabled)
{
    if (!WriteConfiguredValue("quanpin", "autocorrect_neighbor", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quanpin_autocorrect_neighbor = enabled;
    return true;
}

const std::string &GetConfiguredQuanpinHelpcodeSchema()
{
    return g_quanpin_helpcode_schema;
}

bool SetConfiguredQuanpinHelpcodeSchema(const std::string &schema)
{
    if (!HelpcodeUtils::is_supported_helpcode_schema(schema))
        return false;
    if (!WriteConfiguredValue("helpcode", "quanpin_helpcode_schema", EscapeTomlBasicString(schema)))
        return false;
    g_quanpin_helpcode_schema = schema;
    return true;
}

bool GetConfiguredShowShuangpinHelpcodeInCandidateWindow()
{
    return g_show_shuangpin_helpcode_in_candidate_window;
}

bool SetConfiguredShowShuangpinHelpcodeInCandidateWindow(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "show_sp_helpcode_in_candidate_window", enabled ? "true" : "false"))
    {
        return false;
    }
    g_show_shuangpin_helpcode_in_candidate_window = enabled;
    return true;
}

bool GetConfiguredShowQuanpinHelpcodeInCandidateWindow()
{
    return g_show_quanpin_helpcode_in_candidate_window;
}

bool SetConfiguredShowQuanpinHelpcodeInCandidateWindow(bool enabled)
{
    if (!WriteConfiguredValue("helpcode", "show_qp_helpcode_in_candidate_window", enabled ? "true" : "false"))
    {
        return false;
    }
    g_show_quanpin_helpcode_in_candidate_window = enabled;
    return true;
}

bool GetConfiguredFloatingToolbarEnabled()
{
    return g_floating_toolbar_enabled;
}

bool SetConfiguredFloatingToolbarEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "floating_toolbar", enabled ? "true" : "false"))
    {
        return false;
    }
    g_floating_toolbar_enabled = enabled;
    return true;
}

const FloatingToolbarItemsConfig &GetConfiguredFloatingToolbarItems()
{
    return g_floating_toolbar_items;
}

bool SetConfiguredFloatingToolbarItemEnabled(const std::string &item, bool enabled)
{
    bool *target = nullptr;
    if (item == "fullwidth")
        target = &g_floating_toolbar_items.fullwidth;
    else if (item == "punctuation")
        target = &g_floating_toolbar_items.punctuation;
    else if (item == "character_set")
        target = &g_floating_toolbar_items.character_set;
    else if (item == "emoji")
        target = &g_floating_toolbar_items.emoji;
    else if (item == "screen_keyboard")
        target = &g_floating_toolbar_items.screen_keyboard;
    else if (item == "settings")
        target = &g_floating_toolbar_items.settings;
    else
        return false;

    if (!WriteConfiguredValue("general", "floating_toolbar_" + item, enabled ? "true" : "false"))
        return false;
    *target = enabled;
    return true;
}

namespace
{
double SnapFloatingToolbarScale(double scale)
{
    static const double kAllowed[] = {0.75, 1.0, 1.25, 1.5};
    double best = 1.0;
    double bestDelta = std::abs(scale - best);
    for (double candidate : kAllowed)
    {
        const double delta = std::abs(scale - candidate);
        if (delta < bestDelta)
        {
            best = candidate;
            bestDelta = delta;
        }
    }
    return best;
}

std::string FormatFloatingToolbarScale(double scale)
{
    if (std::abs(scale - 0.75) < 0.001)
        return "0.75";
    if (std::abs(scale - 1.25) < 0.001)
        return "1.25";
    if (std::abs(scale - 1.5) < 0.001)
        return "1.5";
    return "1.0";
}
} // namespace

double GetConfiguredFloatingToolbarScale()
{
    return g_floating_toolbar_scale;
}

bool SetConfiguredFloatingToolbarScale(double scale)
{
    if (scale < kFloatingToolbarScaleMin || scale > kFloatingToolbarScaleMax)
        return false;
    scale = SnapFloatingToolbarScale(scale);
    if (!WriteConfiguredValue("general", "floating_toolbar_scale", FormatFloatingToolbarScale(scale)))
        return false;
    g_floating_toolbar_scale = scale;
    return true;
}

int GetConfiguredFloatingToolbarFontSize()
{
    return g_floating_toolbar_font_size;
}

bool SetConfiguredFloatingToolbarFontSize(int font_size)
{
    if (font_size < kFloatingToolbarFontSizeMin || font_size > kFloatingToolbarFontSizeMax)
        return false;
    if (!WriteConfiguredValue("general", "floating_toolbar_font_size", std::to_string(font_size)))
        return false;
    g_floating_toolbar_font_size = font_size;
    return true;
}

bool GetConfiguredEnglishCandidatesEnabled()
{
    return g_english_candidates_enabled;
}

bool SetConfiguredEnglishCandidatesEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "cn_en_mixed_input", enabled ? "true" : "false"))
    {
        return false;
    }
    g_english_candidates_enabled = enabled;
    return true;
}

const std::string &GetConfiguredInputMode()
{
    return g_input_mode;
}

bool SetConfiguredInputMode(const std::string &mode)
{
    if (mode != "chinese" && mode != "japanese")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "mode", EscapeTomlBasicString(mode)))
    {
        return false;
    }
    g_input_mode = mode;
    NotifyImeServerInputSchemeChanged();
    return true;
}

const std::string &GetConfiguredJapaneseSchema()
{
    return g_japanese_schema;
}

bool SetConfiguredJapaneseSchema(const std::string &schema)
{
    if (schema != "romaji")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "japanese_schema", EscapeTomlBasicString(schema)))
    {
        return false;
    }
    g_japanese_schema = schema;
    return true;
}

bool GetConfiguredCandidateTranslationsEnabled()
{
    return g_candidate_translations_enabled;
}

bool SetConfiguredCandidateTranslationsEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "candidate_translations", enabled ? "true" : "false"))
        return false;
    g_candidate_translations_enabled = enabled;
    return true;
}

int GetConfiguredEnglishMixedInputMinChars()
{
    return g_english_mixed_input_min_chars;
}

bool SetConfiguredEnglishMixedInputMinChars(int min_chars)
{
    if (min_chars < kEnglishMixedInputMinCharsMin || min_chars > kEnglishMixedInputMinCharsMax)
        return false;
    if (!WriteConfiguredValue("general", "cn_en_mixed_input_min_chars", std::to_string(min_chars)))
        return false;
    g_english_mixed_input_min_chars = min_chars;
    return true;
}

bool GetConfiguredEmojiMixedInputEnabled()
{
    return g_emoji_mixed_input_enabled;
}

bool SetConfiguredEmojiMixedInputEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "emoji_mixed_input", enabled ? "true" : "false"))
    {
        return false;
    }
    g_emoji_mixed_input_enabled = enabled;
    return true;
}

bool GetConfiguredKaomojiMixedInputEnabled()
{
    return g_kaomoji_mixed_input_enabled;
}

bool SetConfiguredKaomojiMixedInputEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "kaomoji_mixed_input", enabled ? "true" : "false"))
    {
        return false;
    }
    g_kaomoji_mixed_input_enabled = enabled;
    return true;
}

bool GetConfiguredPagingMinusEqualEnabled()
{
    return g_paging_minus_equal_enabled;
}

bool SetConfiguredPagingMinusEqualEnabled(bool enabled)
{
    const bool word_enabled = g_word_to_character_enabled && !(enabled && g_word_to_character_keys == "minus_equal");
    if (!WriteConfiguredValues({{"general", "paging_minus_equal", enabled ? "true" : "false"},
                                {"input", "word_to_character", word_enabled ? "true" : "false"}}))
    {
        return false;
    }
    g_paging_minus_equal_enabled = enabled;
    g_word_to_character_enabled = word_enabled;
    return true;
}

bool GetConfiguredPagingTabEnabled()
{
    return g_paging_tab_enabled;
}

bool SetConfiguredPagingTabEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "paging_tab", enabled ? "true" : "false"))
    {
        return false;
    }
    g_paging_tab_enabled = enabled;
    return true;
}

bool GetConfiguredPagingCommaPeriodEnabled()
{
    return g_paging_comma_period_enabled;
}

bool SetConfiguredPagingCommaPeriodEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "paging_comma_period", enabled ? "true" : "false"))
    {
        return false;
    }
    g_paging_comma_period_enabled = enabled;
    return true;
}

bool GetConfiguredPagingBracketsEnabled()
{
    return g_paging_brackets_enabled;
}

bool SetConfiguredPagingBracketsEnabled(bool enabled)
{
    const bool word_enabled = g_word_to_character_enabled && !(enabled && g_word_to_character_keys == "brackets");
    if (!WriteConfiguredValues({{"general", "paging_brackets", enabled ? "true" : "false"},
                                {"input", "word_to_character", word_enabled ? "true" : "false"}}))
    {
        return false;
    }
    g_paging_brackets_enabled = enabled;
    g_word_to_character_enabled = word_enabled;
    return true;
}

std::wstring FormatPagingCommaPeriodWorkerPayload()
{
    // data[0] = paging flag for legacy clients; "|style" is ignored by old TSF.
    return (g_paging_comma_period_enabled ? L"1|" : L"0|") + string_to_wstring(g_tsf_preedit_style);
}

bool GetConfiguredPagingPageUpDownEnabled()
{
    return g_paging_page_up_down_enabled;
}

bool SetConfiguredPagingPageUpDownEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "paging_page_up_down", enabled ? "true" : "false"))
    {
        return false;
    }
    g_paging_page_up_down_enabled = enabled;
    return true;
}

bool GetConfiguredCandidateArrowNavigationEnabled()
{
    return g_candidate_arrow_navigation_enabled;
}

bool SetConfiguredCandidateArrowNavigationEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "candidate_arrow_navigation", enabled ? "true" : "false"))
    {
        return false;
    }
    g_candidate_arrow_navigation_enabled = enabled;
    return true;
}

bool GetConfiguredWordToCharacterEnabled()
{
    return g_word_to_character_enabled;
}

std::string GetConfiguredWordToCharacterKeys()
{
    return g_word_to_character_keys;
}

static bool SetWordToCharacterConfig(bool enabled, const std::string &keys)
{
    if (keys != "brackets" && keys != "minus_equal")
        return false;
    const bool brackets = g_paging_brackets_enabled && !(enabled && keys == "brackets");
    const bool minus_equal = g_paging_minus_equal_enabled && !(enabled && keys == "minus_equal");
    if (!WriteConfiguredValues({{"input", "word_to_character", enabled ? "true" : "false"},
                                {"input", "word_to_character_keys", EscapeTomlBasicString(keys)},
                                {"general", "paging_brackets", brackets ? "true" : "false"},
                                {"general", "paging_minus_equal", minus_equal ? "true" : "false"}}))
    {
        return false;
    }
    g_word_to_character_enabled = enabled;
    g_word_to_character_keys = keys;
    g_paging_brackets_enabled = brackets;
    g_paging_minus_equal_enabled = minus_equal;
    return true;
}

bool SetConfiguredWordToCharacterEnabled(bool enabled)
{
    return SetWordToCharacterConfig(enabled, g_word_to_character_keys);
}

bool SetConfiguredWordToCharacterKeys(const std::string &keys)
{
    return SetWordToCharacterConfig(g_word_to_character_enabled, keys);
}

bool GetConfiguredSmartPunctuationEnabled()
{
    return g_smart_punctuation_enabled;
}

bool SetConfiguredSmartPunctuationEnabled(bool enabled)
{
    if (!WriteConfiguredValue("input", "smart_punctuation", enabled ? "true" : "false"))
    {
        return false;
    }
    g_smart_punctuation_enabled = enabled;
    return true;
}

bool GetConfiguredSmartPunctuationRepeatToChineseEnabled()
{
    return g_smart_punctuation_repeat_to_chinese_enabled;
}

bool SetConfiguredSmartPunctuationRepeatToChineseEnabled(bool enabled)
{
    if (!WriteConfiguredValue("input", "smart_punctuation_repeat_to_chinese", enabled ? "true" : "false"))
    {
        return false;
    }
    g_smart_punctuation_repeat_to_chinese_enabled = enabled;
    return true;
}

bool GetConfiguredPairedPunctuationEnabled()
{
    return g_paired_punctuation_enabled;
}

bool SetConfiguredPairedPunctuationEnabled(bool enabled)
{
    if (!WriteConfiguredValue("input", "paired_punctuation", enabled ? "true" : "false"))
    {
        return false;
    }
    g_paired_punctuation_enabled = enabled;
    return true;
}

const std::string &GetConfiguredPunctuationLock()
{
    return g_punctuation_lock;
}

bool SetConfiguredPunctuationLock(const std::string &lock)
{
    if (lock != "follow" && lock != "chinese" && lock != "english")
    {
        return false;
    }
    if (!WriteConfiguredValue("input", "punctuation_lock", EscapeTomlBasicString(lock)))
    {
        return false;
    }
    g_punctuation_lock = lock;
    return true;
}

std::wstring FormatPunctuationLockWorkerPayload()
{
    if (g_punctuation_lock == "chinese")
    {
        return L"1";
    }
    if (g_punctuation_lock == "english")
    {
        return L"2";
    }
    return L"0";
}

const std::string &GetConfiguredCandidateWindowLayout()
{
    return g_candidate_window_layout;
}

bool SetConfiguredCandidateWindowLayout(const std::string &layout)
{
    if (layout != "vertical" && layout != "horizontal")
    {
        return false;
    }

    if (!WriteConfiguredValue("appearance", "candidate_window_layout", EscapeTomlBasicString(layout)))
    {
        return false;
    }
    g_candidate_window_layout = layout;
    return true;
}

bool GetConfiguredCandidateWindowFollowCursor()
{
    return g_candidate_window_follow_cursor;
}

bool SetConfiguredCandidateWindowFollowCursor(bool enabled)
{
    if (!WriteConfiguredValue("appearance", "candidate_window_follow_cursor", enabled ? "true" : "false"))
    {
        return false;
    }
    g_candidate_window_follow_cursor = enabled;
    return true;
}

const std::string &GetConfiguredUiBackend()
{
    return g_ui_backend;
}

bool SetConfiguredUiBackend(const std::string &backend)
{
    if (!UiBackendPolicy::IsSupported(backend))
        return false;
    const std::string normalized = NormalizeSmallWindowUiBackend(backend);
    if (!WriteConfiguredValue("appearance", "ui_backend", EscapeTomlBasicString(normalized)))
    {
        return false;
    }
    g_ui_backend = normalized;
    return true;
}

bool UseD2dSmallWindowUi()
{
    return UiBackendPolicy::Resolve(UiBackendPolicy::Surface::Candidate, g_ui_backend_active) ==
           UiBackendPolicy::Backend::Native;
}

const std::string &GetConfiguredCandidateSkin()
{
    return g_candidate_skin;
}

bool SetConfiguredCandidateSkin(const std::string &skin)
{
    if (!IsValidCandidateSkinId(skin))
    {
        return false;
    }
    if (!WriteConfiguredValue("appearance", "candidate_skin", EscapeTomlBasicString(skin)))
    {
        return false;
    }
    g_candidate_skin = skin;
    return true;
}

const std::string &GetConfiguredCandidateWindowPreeditStyle()
{
    return g_candidate_window_preedit_style;
}

bool SetConfiguredCandidateWindowPreeditStyle(const std::string &style)
{
    if (style != "pinyin" && style != "empty")
    {
        return false;
    }
    if (!WriteConfiguredValue("appearance", "candidate_window_preedit_style", EscapeTomlBasicString(style)))
    {
        return false;
    }
    g_candidate_window_preedit_style = style;
    return true;
}

namespace
{
std::string NormalizeThemeMode(const std::string &mode)
{
    if (mode == "light" || mode == "system")
        return mode;
    if (mode == "auto")
        return "system";
    return "dark";
}

std::string NormalizeSurfaceTheme(const std::string &theme)
{
    if (theme == "light" || theme == "dark" || theme == "follow")
        return theme;
    return "follow";
}

bool SetSurfaceThemeValue(const char *key, const std::string &theme, std::string &target)
{
    const std::string normalized = NormalizeSurfaceTheme(theme);
    if (theme != "light" && theme != "dark" && theme != "follow")
        return false;
    if (!WriteConfiguredValue("appearance", key, EscapeTomlBasicString(normalized)))
        return false;
    target = normalized;
    return true;
}
} // namespace

bool IsSystemAppsLightTheme()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
    {
        return false;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LONG result =
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && value != 0;
}

std::string ResolveConfiguredTheme(const std::string &surface_theme)
{
    const std::string surface = NormalizeSurfaceTheme(surface_theme);
    if (surface == "light" || surface == "dark")
        return surface;
    if (g_theme_mode == "light")
        return "light";
    if (g_theme_mode == "system" || g_theme_mode == "auto")
        return IsSystemAppsLightTheme() ? "light" : "dark";
    return "dark";
}

const std::string &GetConfiguredThemeMode()
{
    return g_theme_mode;
}

bool SetConfiguredThemeMode(const std::string &mode)
{
    if (mode != "dark" && mode != "light" && mode != "system" && mode != "auto")
        return false;
    const std::string normalized = NormalizeThemeMode(mode);
    if (!WriteConfiguredValue("appearance", "theme_mode", EscapeTomlBasicString(normalized)))
        return false;
    g_theme_mode = normalized;
    return true;
}

const std::string &GetConfiguredThemeSettings()
{
    return g_theme_settings;
}

bool SetConfiguredThemeSettings(const std::string &theme)
{
    return SetSurfaceThemeValue("theme_settings", theme, g_theme_settings);
}

const std::string &GetConfiguredThemeCand()
{
    return g_theme_cand;
}

bool SetConfiguredThemeCand(const std::string &theme)
{
    return SetSurfaceThemeValue("theme_cand", theme, g_theme_cand);
}

const std::string &GetConfiguredThemeFtb()
{
    return g_theme_ftb;
}

bool SetConfiguredThemeFtb(const std::string &theme)
{
    return SetSurfaceThemeValue("theme_ftb", theme, g_theme_ftb);
}

const std::string &GetConfiguredThemeMenu()
{
    return g_theme_menu;
}

bool SetConfiguredThemeMenu(const std::string &theme)
{
    return SetSurfaceThemeValue("theme_menu", theme, g_theme_menu);
}

const std::string &GetConfiguredThemeEmoji()
{
    return g_theme_emoji;
}

bool SetConfiguredThemeEmoji(const std::string &theme)
{
    return SetSurfaceThemeValue("theme_emoji", theme, g_theme_emoji);
}

const std::string &GetConfiguredThemeScreenKeyboard()
{
    return g_theme_screen_keyboard;
}

bool SetConfiguredThemeScreenKeyboard(const std::string &theme)
{
    return SetSurfaceThemeValue("theme_screen_keyboard", theme, g_theme_screen_keyboard);
}

const std::string &GetConfiguredThemeHandwriting()
{
    return g_theme_handwriting;
}

bool SetConfiguredThemeHandwriting(const std::string &theme)
{
    return SetSurfaceThemeValue("theme_handwriting", theme, g_theme_handwriting);
}

const std::string &GetConfiguredThemeVoice()
{
    return g_theme_voice;
}

bool SetConfiguredThemeVoice(const std::string &theme)
{
    return SetSurfaceThemeValue("theme_voice", theme, g_theme_voice);
}

VoiceInputConfig GetConfiguredVoiceInput()
{
    return SnapshotVoiceInput();
}

bool SetConfiguredVoiceInputString(const std::string &key, const std::string &value)
{
    if (key == "language" && value != "zh-cn" && value != "en" && value != "auto")
        return false;
    if (key == "commit_mode" && value != "tsf" && value != "sendinput" && value != "ctrl_v")
        return false;
    if (key == "polish_prompt_id" && value != "cleanup" && value != "faithful" && value != "zh2en" &&
        value != "casual" && value != "custom_1" && value != "custom_2" && value != "custom_3")
        return false;

    // Fields are addressed by pointer-to-member rather than by a bare reference because the store has to happen under
    // the writer lock, while WriteConfiguredValue does file I/O and a cross-process wait that must never run with that
    // lock held.
    const auto persist = [](const std::string &toml_key, const std::string &toml_value,
                            std::string VoiceInputConfig::*field) {
        if (!WriteConfiguredValue("voice_input", toml_key, EscapeTomlBasicString(toml_value)))
            return false;
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.*field = toml_value;
        return true;
    };

    if (key.rfind("asr_token_", 0) == 0)
    {
        const std::string provider = key.substr(std::string("asr_token_").size());
        if (VoiceInput::AsrTokenSlotKey(provider) != key)
            return false;
        const std::string id = VoiceInput::NormalizeProviderId(provider);
        if (!WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(value)))
            return false;
        std::string asr_provider;
        {
            std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
            g_voice_input.asr_tokens[id] = value;
            asr_provider = g_voice_input.asr_provider;
        }
        if (VoiceInput::NormalizeProviderId(asr_provider) == id)
            persist("asr_token", value, &VoiceInputConfig::asr_token);
        return true;
    }
    if (key.rfind("polish_token_", 0) == 0)
    {
        const std::string provider = key.substr(std::string("polish_token_").size());
        if (VoiceInput::PolishTokenSlotKey(provider) != key)
            return false;
        const std::string id = VoiceInput::NormalizeProviderId(provider);
        if (!WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(value)))
            return false;
        std::string polish_provider;
        {
            std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
            g_voice_input.polish_tokens[id] = value;
            polish_provider = g_voice_input.polish_provider;
        }
        if (VoiceInput::NormalizeProviderId(polish_provider) == id)
            persist("polish_token", value, &VoiceInputConfig::polish_token);
        return true;
    }

    std::string VoiceInputConfig::*target = nullptr;
    if (key == "asr_provider")
        target = &VoiceInputConfig::asr_provider;
    else if (key == "asr_app_key")
        target = &VoiceInputConfig::asr_app_key;
    else if (key == "asr_token")
        target = &VoiceInputConfig::asr_token;
    else if (key == "asr_endpoint")
        target = &VoiceInputConfig::asr_endpoint;
    else if (key == "asr_resource_id")
        target = &VoiceInputConfig::asr_resource_id;
    else if (key == "doubao_boosting_table_id")
        target = &VoiceInputConfig::doubao_boosting_table_id;
    else if (key == "asr_model")
        target = &VoiceInputConfig::asr_model;
    else if (key == "polish_provider")
        target = &VoiceInputConfig::polish_provider;
    else if (key == "polish_token")
        target = &VoiceInputConfig::polish_token;
    else if (key == "polish_endpoint")
        target = &VoiceInputConfig::polish_endpoint;
    else if (key == "polish_model")
        target = &VoiceInputConfig::polish_model;
    else if (key == "polish_prompt_id")
        target = &VoiceInputConfig::polish_prompt_id;
    else if (key == "polish_prompt")
        target = &VoiceInputConfig::polish_prompt;
    else if (key == "polish_prompt_custom_1")
        target = &VoiceInputConfig::polish_prompt_custom_1;
    else if (key == "polish_prompt_custom_2")
        target = &VoiceInputConfig::polish_prompt_custom_2;
    else if (key == "polish_prompt_custom_3")
        target = &VoiceInputConfig::polish_prompt_custom_3;
    else if (key == "language")
        target = &VoiceInputConfig::language;
    else if (key == "commit_mode")
        target = &VoiceInputConfig::commit_mode;
    if (!target || !WriteConfiguredValue("voice_input", key, EscapeTomlBasicString(value)))
        return false;
    {
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.*target = value;
    }
    if (key == "polish_prompt_custom_1")
    {
        WriteConfiguredValue("voice_input", "polish_prompt", EscapeTomlBasicString(""));
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.polish_prompt.clear();
    }
    if (key == "asr_token")
    {
        const std::string asr_provider = SnapshotVoiceInput().asr_provider;
        const std::string slot = VoiceInput::AsrTokenSlotKey(asr_provider);
        if (!slot.empty())
        {
            const std::string id = VoiceInput::NormalizeProviderId(asr_provider);
            {
                std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
                g_voice_input.asr_tokens[id] = value;
            }
            WriteConfiguredValue("voice_input", slot, EscapeTomlBasicString(value));
        }
    }
    else if (key == "polish_token")
    {
        const std::string polish_provider = SnapshotVoiceInput().polish_provider;
        const std::string slot = VoiceInput::PolishTokenSlotKey(polish_provider);
        if (!slot.empty())
        {
            const std::string id = VoiceInput::NormalizeProviderId(polish_provider);
            {
                std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
                g_voice_input.polish_tokens[id] = value;
            }
            WriteConfiguredValue("voice_input", slot, EscapeTomlBasicString(value));
        }
    }
    else if (key == "asr_provider")
    {
        persist("asr_token", VoiceInput::ResolveAsrToken(SnapshotVoiceInput()), &VoiceInputConfig::asr_token);
    }
    else if (key == "polish_provider")
    {
        persist("polish_token", VoiceInput::ResolvePolishToken(SnapshotVoiceInput()), &VoiceInputConfig::polish_token);
    }
    return true;
}

bool GetConfiguredCloudCandidatesEnabled()
{
    return g_cloud_candidates_enabled;
}

bool SetConfiguredCloudCandidatesEnabled(bool enabled)
{
    if (!WriteConfiguredValue("general", "cloud_candidates", enabled ? "true" : "false"))
    {
        return false;
    }
    g_cloud_candidates_enabled = enabled;
    return true;
}

bool GetConfiguredUnicodeModeEnabled()
{
    return g_unicode_mode_enabled;
}

bool SetConfiguredUnicodeModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "unicode_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_unicode_mode_enabled = enabled;
    return true;
}

bool GetConfiguredQuickPhraseEnabled()
{
    return g_quick_phrase_enabled;
}

bool SetConfiguredQuickPhraseEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "quick_phrase", enabled ? "true" : "false"))
    {
        return false;
    }
    g_quick_phrase_enabled = enabled;
    return true;
}

bool GetConfiguredDateTimeModeEnabled()
{
    return g_date_time_mode_enabled;
}

bool SetConfiguredDateTimeModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "date_time_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_date_time_mode_enabled = enabled;
    return true;
}

bool GetConfiguredEmojiModeEnabled()
{
    return g_emoji_mode_enabled;
}

bool SetConfiguredEmojiModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "emoji_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_emoji_mode_enabled = enabled;
    return true;
}

bool GetConfiguredKaomojiModeEnabled()
{
    return g_kaomoji_mode_enabled;
}

bool SetConfiguredKaomojiModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "kaomoji_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_kaomoji_mode_enabled = enabled;
    return true;
}

bool GetConfiguredJianpinModeEnabled()
{
    return g_jianpin_mode_enabled;
}

bool SetConfiguredJianpinModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "jianpin_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_jianpin_mode_enabled = enabled;
    return true;
}

bool GetConfiguredYModeEnabled()
{
    return g_y_mode_enabled;
}

bool SetConfiguredYModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "y_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_y_mode_enabled = enabled;
    return true;
}

bool GetConfiguredRModeEnabled()
{
    return g_r_mode_enabled;
}

bool SetConfiguredRModeEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "r_mode", enabled ? "true" : "false"))
    {
        return false;
    }
    g_r_mode_enabled = enabled;
    return true;
}

bool GetConfiguredClipboardHistoryEnabled()
{
    return g_clipboard_history_enabled;
}

bool SetConfiguredClipboardHistoryEnabled(bool enabled)
{
    if (!WriteConfiguredValue("utility", "clipboard_history", enabled ? "true" : "false"))
    {
        return false;
    }
    const bool was_enabled = g_clipboard_history_enabled;
    g_clipboard_history_enabled = enabled;
    if (was_enabled && !enabled)
        ClipboardHistory::Clear();
    ClipboardMonitor::Sync(enabled);
    return true;
}

bool SetConfiguredVoiceInputBool(const std::string &key, bool value)
{
    bool VoiceInputConfig::*target = nullptr;
    if (key == "voice_input")
        target = &VoiceInputConfig::enabled;
    else if (key == "hotkey_ralt")
        target = &VoiceInputConfig::hotkey_ralt;
    else if (key == "hotkey_ctrl_f9")
        target = &VoiceInputConfig::hotkey_ctrl_f9;
    else if (key == "hotkey_ctrl_win")
        target = &VoiceInputConfig::hotkey_ctrl_win;
    else if (key == "hotkey_rctrl_ralt")
        target = &VoiceInputConfig::hotkey_rctrl_ralt;
    else if (key == "hotkey_hold_space_lock")
        target = &VoiceInputConfig::hotkey_hold_space_lock;
    else if (key == "start_sound")
        target = &VoiceInputConfig::start_sound;
    else if (key == "end_sound")
        target = &VoiceInputConfig::end_sound;
    else if (key == "mute_system_audio")
        target = &VoiceInputConfig::mute_system_audio;
    else if (key == "doubao_enable_itn")
        target = &VoiceInputConfig::doubao_enable_itn;
    else if (key == "doubao_enable_punc")
        target = &VoiceInputConfig::doubao_enable_punc;
    else if (key == "doubao_enable_ddc")
        target = &VoiceInputConfig::doubao_enable_ddc;
    else if (key == "notification_sound")
    {
        // Accept settings pages from older installations during a rolling update.
        if (!WriteConfiguredValue("voice_input", key, value ? "true" : "false"))
            return false;
        std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
        g_voice_input.start_sound = value;
        g_voice_input.end_sound = value;
        return true;
    }
    else if (key == "polish_text")
        target = &VoiceInputConfig::polish_text;
    else if (key == "stream_inline_preedit")
        target = &VoiceInputConfig::stream_inline_preedit;
    if (!target || !WriteConfiguredValue("voice_input", key, value ? "true" : "false"))
        return false;
    std::unique_lock<std::shared_mutex> lock(g_voice_input_mutex);
    g_voice_input.*target = value;
    return true;
}

const AiAssistantConfig &GetConfiguredAiAssistant()
{
    return g_ai_assistant;
}

const TencentTmtConfig &GetConfiguredTencentTmt()
{
    return g_tencent_tmt;
}

bool SetConfiguredTencentTmtString(const std::string &key, const std::string &value)
{
    std::string *target = nullptr;
    if (key == "secret_id")
        target = &g_tencent_tmt.secret_id;
    else if (key == "secret_key")
        target = &g_tencent_tmt.secret_key;
    else if (key == "region")
        target = &g_tencent_tmt.region;
    else if (key == "target_language" && (value == "en" || value == "fr" || value == "ja" || value == "es" ||
                                          value == "ru" || value == "de" || value == "ko"))
        target = &g_tencent_tmt.target_language;
    if (!target || !WriteConfiguredValue("tencent_tmt", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    return true;
}

const CustomTranslationConfig &GetConfiguredCustomTranslation()
{
    return g_custom_translation;
}

bool SetConfiguredCustomTranslationBool(const std::string &key, bool value)
{
    if (key != "enabled" || !WriteConfiguredValue("custom_translation", key, value ? "true" : "false"))
        return false;
    g_custom_translation.enabled = value;
    return true;
}

bool SetConfiguredCustomTranslationString(const std::string &key, const std::string &value)
{
    std::string *target = nullptr;
    if (key == "endpoint")
        target = &g_custom_translation.endpoint;
    else if (key == "api_key")
        target = &g_custom_translation.api_key;
    if (!target || !WriteConfiguredValue("custom_translation", key, EscapeTomlBasicString(value)))
        return false;
    *target = value;
    return true;
}

const FrequencyAdjustmentConfig &GetConfiguredFrequencyAdjustment()
{
    return g_frequency_adjustment;
}

bool SetConfiguredFrequencyAdjustmentString(const std::string &key, const std::string &value)
{
    if (key != "mode" ||
        (value != "disabled" && value != "pin" && value != "halve" && value != "linear" && value != "promote") ||
        !WriteConfiguredValue("frequency_adjustment", key, EscapeTomlBasicString(value)))
        return false;
    g_frequency_adjustment.mode = value;
    return true;
}

bool SetConfiguredFrequencyAdjustmentInt(const std::string &key, int value)
{
    if ((key != "trigger_count" && key != "linear_step") || value < 1 || value > 10 ||
        !WriteConfiguredValue("frequency_adjustment", key, std::to_string(value)))
        return false;
    if (key == "trigger_count")
        g_frequency_adjustment.trigger_count = value;
    else
        g_frequency_adjustment.linear_step = value;
    return true;
}

bool SetConfiguredAiAssistantString(const std::string &key, const std::string &value)
{
    if (key == "prompt_id" && value != "custom_1" && value != "custom_2" && value != "custom_3")
        return false;
    const auto persist = [](const std::string &toml_key, const std::string &toml_value, std::string &target) {
        if (!WriteConfiguredValue("ai_assistant", toml_key, EscapeTomlBasicString(toml_value)))
            return false;
        target = toml_value;
        return true;
    };

    if (key.rfind("token_", 0) == 0)
    {
        const std::string provider = key.substr(std::string("token_").size());
        if (AiAssistantTokenSlotKey(provider) != key)
            return false;
        if (!WriteConfiguredValue("ai_assistant", key, EscapeTomlBasicString(value)))
            return false;
        const std::string id = VoiceInput::NormalizeProviderId(provider);
        g_ai_assistant.tokens[id] = value;
        if (g_ai_assistant.provider == id)
            persist("token", value, g_ai_assistant.token);
        return true;
    }

    std::string *target = nullptr;
    if (key == "provider")
    {
        if (AiAssistantTokenSlotKey(value).empty())
            return false;
        target = &g_ai_assistant.provider;
    }
    else if (key == "token")
        target = &g_ai_assistant.token;
    else if (key == "endpoint")
        target = &g_ai_assistant.endpoint;
    else if (key == "model")
        target = &g_ai_assistant.model;
    else if (key == "prompt_id")
        target = &g_ai_assistant.prompt_id;
    else if (key == "prompt_custom_1")
        target = &g_ai_assistant.prompt_custom_1;
    else if (key == "prompt_custom_2")
        target = &g_ai_assistant.prompt_custom_2;
    else if (key == "prompt_custom_3")
        target = &g_ai_assistant.prompt_custom_3;
    else if (key == "prompt")
        target = &g_ai_assistant.prompt;
    if (!target || !WriteConfiguredValue("ai_assistant", key, EscapeTomlBasicString(value)))
        return false;
    *target = key == "provider" ? VoiceInput::NormalizeProviderId(value) : value;
    if (key == "prompt_custom_1")
        WriteConfiguredValue("ai_assistant", "prompt", EscapeTomlBasicString(""));
    if (key == "prompt")
        g_ai_assistant.prompt_custom_1 = value;
    if (key == "prompt" || key == "prompt_id" || key.rfind("prompt_custom_", 0) == 0)
    {
        g_ai_assistant.prompt = g_ai_assistant.prompt_id == "custom_2"   ? g_ai_assistant.prompt_custom_2
                                : g_ai_assistant.prompt_id == "custom_3" ? g_ai_assistant.prompt_custom_3
                                                                         : g_ai_assistant.prompt_custom_1;
    }
    if (key == "provider")
    {
        persist("token", g_ai_assistant.tokens[g_ai_assistant.provider], g_ai_assistant.token);
    }
    else if (key == "token")
    {
        const std::string slot = AiAssistantTokenSlotKey(g_ai_assistant.provider);
        g_ai_assistant.tokens[g_ai_assistant.provider] = value;
        WriteConfiguredValue("ai_assistant", slot, EscapeTomlBasicString(value));
    }
    return true;
}

bool SetConfiguredAiAssistantBool(const std::string &key, bool value)
{
    if (key != "enabled" || !WriteConfiguredValue("ai_assistant", key, value ? "true" : "false"))
        return false;
    g_ai_assistant.enabled = value;
    return true;
}

bool SetConfiguredAiAssistantInt(const std::string &key, int value)
{
    if (key != "candidate_limit" || value < 1 || value > 10 ||
        !WriteConfiguredValue("ai_assistant", key, std::to_string(value)))
        return false;
    g_ai_assistant.candidate_limit = value;
    return true;
}
