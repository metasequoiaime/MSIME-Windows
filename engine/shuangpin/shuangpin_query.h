#pragma once

#include "shuangpin_profile.h"
#include "../contracts/mid_sentence_helpcode.h"
#include "../core/syllable_helpcode.h"
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace shuangpin
{

std::string segment_input(const std::string &raw_input, const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
// Raw offsets where one shuangpin syllable starts, always including 0 (when
// non-empty) and raw_input.size(). A unit is one syllable: 1-2 keys decided by
// the same forward-greedy acceptance pinyin_segmentation uses, so the Microsoft
// ';' final and the profile's syllable set are honored. Manual delimiters start
// a new chunk. Empty input yields an empty vector.
std::vector<std::size_t> segment_raw_boundaries(const std::string &raw_input,
                                                const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
std::string to_quanpin_segmentation(const std::string &segmented_input,
                                    const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
std::string normalize_input(const std::string &raw_input,
                            const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
std::string normalize_input_with_delimiters(const std::string &raw_input,
                                            const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
std::string remove_manual_delimiters(const std::string &text);
size_t effective_input_length(const std::string &raw_input);
// Maps a length measured in delimiter-stripped space back onto the raw input, so callers can slice a raw prefix that
// holds exactly that many effective characters.
size_t raw_length_for_effective_prefix(const std::string &raw_input, size_t effective_length);
// Drops the trailing `letter_count` effective characters while keeping every delimiter that precedes them.
std::string trim_trailing_letters_preserve_delimiters(const std::string &raw_input, size_t letter_count);
size_t detect_active_double_helpcode_length(const std::string &raw_input, const std::string &raw_input_with_cases,
                                            const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
bool is_complete_input(const std::string &raw_input, const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
std::string apply_segmentation_cases(const std::string &segmented_input, const std::string &raw_input_with_cases);
std::string get_first_han_char(const std::string &words);
std::string get_last_han_char(const std::string &words);
std::string::size_type count_utf8_chars(const std::string &text);
std::string::size_type count_han_chars(const std::string &text);

// 句中辅助码，规则见 core/syllable_helpcode.h，输入形状与 TSF 共用 contracts/mid_sentence_helpcode.h。
inline constexpr char kMidSentenceHelpcodeMarker = FanyImeMidSentenceHelpcode::kMarker;

struct MidSentenceHelpcodeInput
{
    // 每段换成一个 ' 之后的输入串，保留大小写。下游把它当作带手动分隔的普通双拼。
    std::string input;
    // input[i] 在原串里的下标，末尾多一项等于原串长度。
    std::vector<std::size_t> source_index;
    SyllableHelpcodes helpcodes;
    // 每段原样的文本（含还没敲码的光杆反引号）及它挂在哪个音节后面，预编辑据此还原显示。
    std::vector<std::pair<std::size_t, std::string>> decorations;
};

bool has_mid_sentence_helpcode(const std::string &raw_input);
MidSentenceHelpcodeInput parse_mid_sentence_helpcodes(const std::string &raw_input_with_cases,
                                                      const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
// 把每段反引号段按原样接回切分串里对应音节的后面：ul'pb'ih'fa → ul'pb`x'ih'fa。切分串必须是
// 由 parse_mid_sentence_helpcodes(...).input 切出来的（音节序号才对得上）。
std::string decorate_mid_sentence_segmentation(const std::string &segmentation, const std::string &raw_input_with_cases,
                                               const ShuangpinProfile &profile = GetXiaoheShuangpinProfile());
// 输入串末尾能否接一个反引号，即 FanyImeMidSentenceHelpcode::AcceptsMarker。
bool accepts_mid_sentence_helpcode_marker(const std::string &raw_input);
// 光标停在 raw_input[caret] 时能否插入一个反引号，即 FanyImeMidSentenceHelpcode::AcceptsMarkerAt。
bool accepts_mid_sentence_helpcode_marker_at(const std::string &raw_input, std::size_t caret);
// 输入串末尾能否接 ch 作为第二码：紧跟在「反引号 + 第一码」之后的大写字母。
bool accepts_mid_sentence_second_code(const std::string &raw_input, char ch);

} // namespace shuangpin
