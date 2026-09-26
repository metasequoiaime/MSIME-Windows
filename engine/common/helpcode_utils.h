#pragma once

#include <string>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace HelpcodeUtils
{

using Keymap = std::unordered_map<std::string, std::string>;
using SharedKeymap = std::shared_ptr<const Keymap>;
SharedKeymap load_helpcode_keymap(const std::filesystem::path &resources, const std::string &schema);
std::string selected_helpcode_schema();
const Keymap &helpcode_keymap();
bool is_supported_helpcode_schema(const std::string &schema);
bool select_helpcode_schema(const std::string &schema);

// User tables dropped into <resources>/helpcodes/custom/*.txt are schemas named
// "custom/<file stem>". Leading "# name: ..." / "# name_en: ..." comment lines give display names.
inline constexpr char kCustomHelpcodeSchemaPrefix[] = "custom/";
struct CustomHelpcodeSchema
{
    std::string schema;
    std::string name;    // Chinese display name; empty when the file does not declare one
    std::string name_en; // English display name; empty when the file does not declare one
    std::string file_stem;
};
std::filesystem::path custom_helpcode_directory(const std::filesystem::path &resources);
std::vector<CustomHelpcodeSchema> list_custom_helpcode_schemas(const std::filesystem::path &resources);
// Built-in schemas are always available; a custom schema only while its file exists.
bool is_helpcode_schema_available(const std::filesystem::path &resources, const std::string &schema);

std::string get_first_han_char(const std::string &words);
std::string get_last_han_char(const std::string &words);
std::string::size_type count_han_chars(const std::string &words);
std::string::size_type count_utf8_chars(const std::string &text);
std::string compute_helpcodes(const std::string &words, bool uppercase_all = false, const Keymap *keymap = nullptr);

bool is_quanpin_single_help_mode(const std::string &pinyin_with_cases);
bool is_quanpin_double_help_mode(const std::string &pinyin_with_cases);

enum class SingleHelpcodeMatch
{
    None,
    First,
    Last,
    Both,
};

SingleHelpcodeMatch match_single_helpcode(const std::string &word, const std::string &help_code,
                                          const Keymap *keymap = nullptr);
bool matches_double_helpcodes(const std::string &word, const std::string &help_codes, const Keymap *keymap = nullptr);

template <typename TWordItem>
std::vector<TWordItem> reorder_candidates_with_single_helpcode(const std::vector<TWordItem> &candidate_list,
                                                               const std::string &help_code,
                                                               const Keymap *keymap = nullptr)
{
    if (help_code.size() != 1)
    {
        return candidate_list;
    }

    std::vector<TWordItem> first_helpcode_matched_list;
    std::vector<TWordItem> last_helpcode_matched_list;
    std::vector<TWordItem> left_helpcode_matched_list;
    std::vector<TWordItem> result_list;

    for (const auto &cand : candidate_list)
    {
        switch (match_single_helpcode(cand.word, help_code, keymap))
        {
        case SingleHelpcodeMatch::First:
            first_helpcode_matched_list.push_back(cand);
            break;
        case SingleHelpcodeMatch::Last:
            last_helpcode_matched_list.push_back(cand);
            break;
        case SingleHelpcodeMatch::Both:
            first_helpcode_matched_list.push_back(cand);
            break;
        case SingleHelpcodeMatch::None:
            left_helpcode_matched_list.push_back(cand);
            break;
        }
    }

    result_list.insert(result_list.end(), first_helpcode_matched_list.begin(), first_helpcode_matched_list.end());
    result_list.insert(result_list.end(), last_helpcode_matched_list.begin(), last_helpcode_matched_list.end());
    result_list.insert(result_list.end(), left_helpcode_matched_list.begin(), left_helpcode_matched_list.end());
    return result_list;
}

template <typename TWordItem>
std::vector<TWordItem> filter_candidates_with_double_helpcodes(const std::vector<TWordItem> &candidate_list,
                                                               const std::string &help_codes,
                                                               const Keymap *keymap = nullptr)
{
    std::vector<TWordItem> filtered_list;
    if (help_codes.size() != 2)
    {
        return filtered_list;
    }

    for (const auto &cand : candidate_list)
    {
        if (matches_double_helpcodes(cand.word, help_codes, keymap))
        {
            filtered_list.push_back(cand);
        }
    }

    return filtered_list;
}

} // namespace HelpcodeUtils
