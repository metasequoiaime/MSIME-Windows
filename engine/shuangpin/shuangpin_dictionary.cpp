#include "shuangpin_dictionary.h"
#include "../user_dictionary/user_dictionary_journal.h"
#include "../common/helpcode_utils.h"
#include "../quanpin/quanpin_query.h"
#include "../quanpin/quanpin_utils.h"
#include "shuangpin_query.h"
#include "shuangpin_utils.h"
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <sqlite3.h>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <cstdlib>
#include <climits>
#include <boost/algorithm/string.hpp>
#include <fmt/xchar.h>
#include <utf8/cpp17.h>

using namespace std;

namespace
{
std::string remove_delimiters(const std::string &segmented)
{
    std::string normalized = segmented;
    normalized.erase(std::remove(normalized.begin(), normalized.end(), '\''), normalized.end());
    return normalized;
}

std::string escape_sql_text(std::string text)
{
    size_t pos = 0;
    while ((pos = text.find('\'', pos)) != std::string::npos)
    {
        text.insert(pos, 1, '\'');
        pos += 2;
    }
    return text;
}

} // namespace

ShuangpinDictionary::ShuangpinDictionary(const ShuangpinProfile &profile, metasequoia::RuntimePaths paths)
    : profile_(profile), paths_(std::move(paths)), decoder_(paths_.resource(metasequoia::assets::pinyin_model),
                                                            paths_.user(metasequoia::assets::pinyin_user_dictionary)),
      helpcodes_(HelpcodeUtils::load_helpcode_keymap(paths_.resources, HelpcodeUtils::selected_helpcode_schema())),
      _kb_input_sequence(100), _cached_buffer(128), _cached_buffer_sgl(128), _cached_buffer_sgl_reversed(128),
      _cached_buffer_dbl(128), _cached_buffer_series(128)
{
    // 最多可以输出 64 个汉字，拼音最多可以接受 128 个字符

    quanpin_db_path_ = metasequoia::path_to_utf8(paths_.dictionary(metasequoia::assets::main_dictionary));
    int exit = sqlite3_open(quanpin_db_path_.c_str(), &quanpin_db_);
    if (exit != SQLITE_OK)
    {
        (void)0;
    }
    else
    {
        quanpin::warm_up(quanpin_db_, quanpin_statement_cache_);
        reset_cache_if_database_changed();
    }
}

/**
 * @brief Generate candidate list when not in help mode
 *
 * @param pinyin_sequence
 * @param pinyin_segmentation
 * @return vector<ShuangpinDictionary::WordItem>
 */
vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::generate( //
    const string &pinyin_sequence,                                   //
    const string &pinyin_segmentation,                               //
    const string &cache_key                                          //
)
{
    // std::shared_lock lock(mutex_);
    vector<ShuangpinDictionary::WordItem> candidate_list;
    if (pinyin_sequence.size() == 0)
    {
        return candidate_list;
    }
    vector<string> code_list;
    if (pinyin_sequence.size() == 1)
    {
        generate_for_single_char(candidate_list, pinyin_sequence);
    }
    else
    {
        const std::string effective_cache_key = cache_key.empty() ? pinyin_sequence : cache_key;
        // Check cache first
        if (_cached_buffer.get(effective_cache_key))
        {
            reset_cache_if_database_changed();
            if (const auto cached = _cached_buffer.get(effective_cache_key))
            {
                return cached.value();
            }
        }

        candidate_list = query_from_quanpin_database(pinyin_sequence, pinyin_segmentation);
        _cached_buffer.insert(effective_cache_key, candidate_list);
    }
    return candidate_list;
}

/**
 * @brief 对于纯粹的拼音，除了完全匹配的汉字串，子串也要全部给出来，子串是为了给接下来可能会进行的造词使用的
 *
 * @param pinyin_sequence
 * @param pinyin_segmentation
 * @return vector<ShuangpinDictionary::WordItem>
 */
vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::generateSeries( //
    const string &pinyin_sequence,                                         //
    const string &pinyin_segmentation,                                     //
    const string &cache_key                                                //
)
{
    vector<ShuangpinDictionary::WordItem> candidate_list;
    if (pinyin_sequence.size() == 0)
    {
        return candidate_list;
    }
    vector<string> code_list;
    if (pinyin_sequence.size() == 1)
    {
        generate_for_single_char(candidate_list, pinyin_sequence);
    }
    else
    {
        const std::string effective_cache_key = cache_key.empty() ? pinyin_sequence : cache_key;
        // 先看一下缓存里有没有
        if (_cached_buffer_series.get(effective_cache_key))
        {
            reset_cache_if_database_changed();
            if (const auto cached = _cached_buffer_series.get(effective_cache_key))
            {
                return cached.value();
            }
        }

        // 查询当前的拼音严格对应的数据
        vector<ShuangpinDictionary::WordItem> cur_pinyin_cand =
            generate(pinyin_sequence, pinyin_segmentation, effective_cache_key);
        if (cur_pinyin_cand.size() > 0)
        {
            candidate_list.insert(candidate_list.end(), cur_pinyin_cand.begin(), cur_pinyin_cand.end());
        }
        else
        { /* 可能数据库查询的结果是空，这时就需要联想，这个只适合在此处联想 */
            if (candidate_list.size() == 0)
            {
                string quanpin_str =
                    ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(pinyin_segmentation, profile_);
                string res = search_sentence_from_ime_engine(quanpin_str);
                if (res.size() > 0)
                {
                    // 整句 fallback 必须带上 canonical quanpin，否则以它结尾的造词无法落库：
                    // update_creating_word_progress 依赖 canonical_pinyin 才能拼出完整读音。
                    candidate_list.emplace_back(_pinyin_sequence, res, 1, CandidateSource::Fallback, quanpin_str);
                }
            }
        }

        // 查询当前的拼音子串对应的数据
        string pure_pinyin = pinyin_sequence;
        string seg_pinyin = pinyin_segmentation;
        while (true)
        {
            size_t pos = seg_pinyin.rfind('\'');
            if (pos != string::npos)
            {
                seg_pinyin = seg_pinyin.substr(0, pos);
                pure_pinyin = boost::algorithm::replace_all_copy(seg_pinyin, "'", "");
                vector<ShuangpinDictionary::WordItem> sub_pinyin_cand = generate(pure_pinyin, seg_pinyin);
                candidate_list.insert(candidate_list.end(), sub_pinyin_cand.begin(), sub_pinyin_cand.end());
            }
            else
            {
                break;
            }
        }

        const std::string quanpin_segmentation =
            ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(pinyin_segmentation, profile_);
        // Prefer one local Google-Pinyin whole-sentence result, followed by
        // only the best dictionary-lattice path.  Multiple lattice paths tend
        // to crowd out useful candidates with near-duplicate sentences.
        std::string google_sentence;
        if (quanpin::split_segments(quanpin_segmentation).size() >= 3 &&
            quanpin_segmentation.find('\'') != std::string::npos)
        {
            google_sentence = search_sentence_from_ime_engine(quanpin_segmentation);
            const bool duplicate = std::any_of(candidate_list.begin(), candidate_list.end(),
                                               [&](const WordItem &item) { return item.word == google_sentence; });
            if (!google_sentence.empty() && !duplicate)
                // 同上：这条候选会被提到首位、成为空格默认提交的那个，必须可落库。
                candidate_list.insert(
                    candidate_list.begin(),
                    WordItem(_pinyin_sequence, google_sentence, 1, CandidateSource::Fallback, quanpin_segmentation));
        }
        quanpin::WordLatticeOptions lattice_options;
        lattice_options.nbest = 1;
        quanpin::merge_lattice_candidates(candidate_list, quanpin::split_segments(quanpin_segmentation),
                                          quanpin::make_lattice_db_lookup(quanpin_db_, quanpin_statement_cache_,
                                                                          quanpin::QuerySource::Shuangpin,
                                                                          lattice_options.span_limit),
                                          pinyin_sequence, lattice_options);
        if (!google_sentence.empty())
        {
            const auto google = std::find_if(candidate_list.begin(), candidate_list.end(), [&](const WordItem &item) {
                return item.word == google_sentence && item.source == CandidateSource::Fallback;
            });
            if (google != candidate_list.end() && google != candidate_list.begin())
            {
                WordItem preferred = std::move(*google);
                candidate_list.erase(google);
                candidate_list.insert(candidate_list.begin(), std::move(preferred));
            }
        }

        /* 缓存起来 */
        _cached_buffer_series.insert(effective_cache_key, candidate_list);
    }

    return candidate_list;
}

/**
 * @brief Filter with single help code
 *
 * Not only the first Hanzi part, but also the last one that will be considered.
 *   - For single Hanzi, we consider its first and last part
 *   - For Multi Hanzi, we consider first Hanzi's first part and last Hanzi's first part
 *
 * e.g. 阿: 阿's helpcode is ek, when we type aae or aak, 阿 will both be filtered.
 *      阿姨: 阿's helpcode is ek, 姨's helpcode is nr, when we type aayie or aayin, 阿姨 will both be filtered.
 *
 * 此外，单码辅助的情况，需要把原始拼音的候选列表加到辅助码模式的候选列表后面，这里的指的是不将最后一个字符看成是辅助码的情况下得到的候选项的结果
 *
 * @param candidate_list
 * @param filtered_list
 * @param help_code
 * @param pinyin_sequence 原始的拼音序列
 */
void ShuangpinDictionary::filter_with_single_helpcode(           //
    const vector<ShuangpinDictionary::WordItem> &candidate_list, //
    vector<ShuangpinDictionary::WordItem> &result_list,          //
    const string &help_code,                                     //
    const string &pinyin_sequence                                //
)
{
    if (candidate_list.empty() || help_code.size() != 1)
        return;
    const bool prefer_last_helpcode = help_code[0] >= 'A' && help_code[0] <= 'Z';
    const string normalized_help_code(1, static_cast<char>(std::tolower(static_cast<unsigned char>(help_code[0]))));
    vector<ShuangpinDictionary::WordItem> first_helpcode_matched_list;
    vector<ShuangpinDictionary::WordItem> last_helpcode_matched_list;
    vector<ShuangpinDictionary::WordItem> left_helpcode_matched_list; // 被筛完之后剩下的

    for (const auto &cand : candidate_list)
    {
        switch (HelpcodeUtils::match_single_helpcode(cand.word, normalized_help_code, helpcodes_.get()))
        {
        case HelpcodeUtils::SingleHelpcodeMatch::First:
            first_helpcode_matched_list.push_back(cand);
            break;
        case HelpcodeUtils::SingleHelpcodeMatch::Last:
            last_helpcode_matched_list.push_back(cand);
            break;
        case HelpcodeUtils::SingleHelpcodeMatch::Both:
            (prefer_last_helpcode ? last_helpcode_matched_list : first_helpcode_matched_list).push_back(cand);
            break;
        case HelpcodeUtils::SingleHelpcodeMatch::None:
            left_helpcode_matched_list.push_back(cand);
            break;
        }
    }

    /* 辅助码筛出来的候选列表 */
    if (prefer_last_helpcode)
    {
        result_list.insert(result_list.end(), last_helpcode_matched_list.begin(), last_helpcode_matched_list.end());
        result_list.insert(result_list.end(), first_helpcode_matched_list.begin(), first_helpcode_matched_list.end());
    }
    else
    {
        result_list.insert(result_list.end(), first_helpcode_matched_list.begin(), first_helpcode_matched_list.end());
        result_list.insert(result_list.end(), last_helpcode_matched_list.begin(), last_helpcode_matched_list.end());
    }
    /* 把原始拼音的候选列表加到辅助码模式的候选列表后面 */
    const std::string original_segmentation = ShuangpinUtil::pinyin_segmentation(pinyin_sequence, profile_);
    auto original_candidate_list = generateSeries(pinyin_sequence, original_segmentation);
    result_list.insert(result_list.end(), original_candidate_list.begin(), original_candidate_list.end());
    /* 把剩下的候选列表加到辅助码模式的候选列表后面 */
    result_list.insert(result_list.end(), left_helpcode_matched_list.begin(), left_helpcode_matched_list.end());
}

/**
 * @brief Filter with double help codes
 *
 * Rules:
 *   - For single Hanzi, we consider its first and last part
 *   - For Multi Hanzi, we consider first Hanzi's first part and last Hanzi's first part
 *
 * e.g. 阿: 阿's helpcode is ek, when we type aaek, 阿 will be filtered.
 *      阿姨: 阿's helpcode is ek, 姨's helpcode is nr, when we type aayien, 阿姨 will be filtered.
 *
 * @param candidate_list
 * @param result_list
 * @param help_codes
 */
void ShuangpinDictionary::filter_with_double_helpcodes(               //
    const std::vector<ShuangpinDictionary::WordItem> &candidate_list, //
    std::vector<ShuangpinDictionary::WordItem> &result_list,          //
    const std::string &help_codes                                     //
)
{
    if (candidate_list.empty())
        return;

    for (const auto &cand : candidate_list)
    {
        if (HelpcodeUtils::matches_double_helpcodes(cand.word, help_codes, helpcodes_.get()))
        {
            result_list.push_back(cand);
        }
    }
}

/**
 * @brief
 *
 * Note: Use the help code only in standard cases—that is, when the shuangpin part is complete.
 *
 * @param pure_pinyin
 * @param pure_pinyin_segmentation
 * @param pinyin_sequence
 * @param help_codes
 * @return vector<ShuangpinDictionary::WordItem>
 */
vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::generate_with_helpcodes( //
    const string &pure_pinyin,                                                      //
    const string &pure_pinyin_segmentation,                                         //
    const string &pinyin_sequence,                                                  //
    const string &help_codes                                                        //
)
{
    vector<WordItem> candidate_list;
    const bool reversed_single_helpcode = help_codes.size() == 1 && help_codes[0] >= 'A' && help_codes[0] <= 'Z';
    // Check cache first
    if (help_codes.size() == 1)
    {
        auto &single_helpcode_cache = reversed_single_helpcode ? _cached_buffer_sgl_reversed : _cached_buffer_sgl;
        if (const auto cached = single_helpcode_cache.get(pinyin_sequence))
        {
            reset_cache_if_database_changed();
            if (const auto refreshed = single_helpcode_cache.get(pinyin_sequence))
            {
                return refreshed.value();
            }
        }
    }
    else if (help_codes.size() == 2)
    {
        if (_cached_buffer_dbl.get(pinyin_sequence))
        {
            reset_cache_if_database_changed();
            if (const auto cached = _cached_buffer_dbl.get(pinyin_sequence))
            {
                return cached.value();
            }
        }
    }

    candidate_list = generateSeries(pure_pinyin, pure_pinyin_segmentation);
    vector<WordItem> result_list;
    // Filter with help codes
    if (help_codes.size() == 1)
    {
        filter_with_single_helpcode( //
            candidate_list,          //
            result_list,             //
            help_codes,              //
            pinyin_sequence          //
        );
        auto &single_helpcode_cache = reversed_single_helpcode ? _cached_buffer_sgl_reversed : _cached_buffer_sgl;
        single_helpcode_cache.insert(pinyin_sequence, result_list);
    }
    else if (help_codes.size() == 2)
    {
        filter_with_double_helpcodes( //
            candidate_list,           //
            result_list,              //
            help_codes                //
        );
        _cached_buffer_dbl.insert(pinyin_sequence, result_list);
    }
    return result_list;
}

std::string VkCodeToChar(ImeKeyCode vk)
{
    if (vk >= 'A' && vk <= 'Z')
    {
        return std::string(1, char(vk + ('a' - 'A')));
    }
    if (vk >= '0' && vk <= '9')
    {
        return std::string(1, char(vk));
    }
    switch (vk)
    {
    case ImeKey::Space:
        return " ";
    case ImeKey::Tab:
        return "\t";
    case ImeKey::Return:
        return "\n";
    default:
        return "";
    }
}

std::string VkSequenceToString(const ImeKeyCode *vk_codes, size_t count)
{
    std::string result;
    for (size_t i = 0; i < count; ++i)
    {
        result += VkCodeToChar(vk_codes[i]);
    }
    return result;
}

void ShuangpinDictionary::generate_for_single_char(vector<ShuangpinDictionary::WordItem> &candidate_list, string code)
{
    constexpr int kInitialCandidateLimit = 24;
    candidate_list = query_initial_from_quanpin_database(code, kInitialCandidateLimit);
}

bool ShuangpinDictionary::expand_initial_candidates()
{
    return expand_initial_candidates(_pinyin_sequence, _cur_candidate_list);
}

bool ShuangpinDictionary::expand_initial_candidates(const std::string &code, std::vector<WordItem> &candidates,
                                                    const std::string &series_cache_key)
{
    if (code.size() != 1)
    {
        return false;
    }

    const auto is_limited_initial = [&](const WordItem &item) {
        return item.source == CandidateSource::Database && item.pinyin == code;
    };
    const size_t limited_count =
        static_cast<size_t>(std::count_if(candidates.begin(), candidates.end(), is_limited_initial));
    constexpr size_t kInitialCandidateLimit = 24;
    if (limited_count != kInitialCandidateLimit)
    {
        return false;
    }

    auto expanded = query_initial_from_quanpin_database(code, INT_MAX);
    if (expanded.size() <= limited_count)
    {
        return false;
    }

    std::vector<WordItem> merged;
    merged.reserve(candidates.size() - limited_count + expanded.size());
    bool inserted = false;
    for (auto &item : candidates)
    {
        if (is_limited_initial(item))
        {
            if (!inserted)
            {
                merged.insert(merged.end(), expanded.begin(), expanded.end());
                inserted = true;
            }
            continue;
        }
        merged.push_back(std::move(item));
    }

    candidates = std::move(merged);
    if (!series_cache_key.empty())
    {
        _cached_buffer_series.insert(series_cache_key, candidates);
    }
    return true;
}

/**
 * @brief
 *
 * @param vk
 * @return int
 */
int ShuangpinDictionary::handleVkCode(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    if (vk != 0)
    { /* 0 是造词过程中的 dummy code */
        _kb_input_sequence.push_back(vk);
        if (vk >= 'A' && vk <= 'Z')
        {
            const char lowerAlpha = static_cast<char>(vk + ('a' - 'A'));
            _pinyin_sequence += lowerAlpha;

            // Prefer the real typed character from TSF side so CapsLock/Shift combinations are preserved.
            if (wch >= u'A' && wch <= u'Z')
            {
                _pinyin_sequence_with_cases += static_cast<char>(wch);
            }
            else if (wch >= u'a' && wch <= u'z')
            {
                _pinyin_sequence_with_cases += static_cast<char>(wch);
            }
            else if (modifiers_down >> 0 & 1u)
            {
                // Fallback for callers that don't provide wch.
                _pinyin_sequence_with_cases += static_cast<char>(vk);
            }
            else
            {
                _pinyin_sequence_with_cases += lowerAlpha;
            }
        }
        else if (profile_.name == "microsoft" && vk == ImeKey::Semicolon && wch == u';' &&
                 _pinyin_sequence.size() % 2 == 1)
        {
            _pinyin_sequence += ';';
            _pinyin_sequence_with_cases += ';';
        }
        else if (vk == ImeKey::Space || (vk >= '0' && vk <= '9') || vk == ImeKey::Return || vk == ImeKey::Shift ||
                 vk == ImeKey::Escape)
        {
            if (vk == ImeKey::Return || vk == ImeKey::Shift || vk == ImeKey::Escape)
            { /* 空格键和数字键不要清理状态，因为可能会触发造词 */
                // Clear state
                reset_state();
            }
            return 0;
        }
        else if (vk == ImeKey::Tab)
        {
            return 0;
        }
        else if (vk == ImeKey::Backspace)
        {
            if (_pinyin_sequence.size() > 0)
            {
                _pinyin_sequence = _pinyin_sequence.substr(0, _pinyin_sequence.size() - 1);
                _pinyin_sequence_with_cases =
                    _pinyin_sequence_with_cases.substr(0, _pinyin_sequence_with_cases.size() - 1);
            }
        }
    }

    //
    // We do not handle other keys currently
    //

    /* 初始状态 */
    _pure_pinyin_sequence = _pinyin_sequence;

    /* Whether in full help mode */
    _is_full_help_mode = ShuangpinUtil::IsFullHelpMode(_pinyin_sequence_with_cases, profile_);
    if (_is_full_help_mode)
    {
        _help_mode_raw_pos = _pinyin_sequence.size() - 2;
    }
    else
    {
        _help_mode_raw_pos = 0;
    }

    /* Generate candidate list */
    if (_is_full_help_mode)
    { // 全码辅助，结果只包含根据辅助码筛出来的候选词部分
        _pure_pinyin_sequence = _pinyin_sequence.substr(0, _help_mode_raw_pos);
        _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pure_pinyin_sequence, profile_);
        _pinyin_helpcodes = ShuangpinUtil::GetFullHelpCodes(_pinyin_sequence_with_cases);
        _cur_candidate_list = generate_with_helpcodes( //
            _pure_pinyin_sequence,                     //
            _pinyin_segmentation,                      //
            _pinyin_sequence,                          //
            _pinyin_helpcodes                          //
        );
    }
    else
    {
        // 不是全码辅助的情况：
        //   1. 奇数长度拼音序列，且双拼部分是完整的拼音，需要触发辅助码
        //   2. 偶数长度拼音序列，不需要触发辅助码
        if (_pinyin_sequence.size() % 2 == 1 && _pinyin_sequence.size() > 1)
        { /* 1. 奇数长度拼音序列 */
            _pure_pinyin_sequence = _pinyin_sequence.substr(0, _pinyin_sequence.size() - 1);
            _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pure_pinyin_sequence, profile_);
            if (ShuangpinUtil::is_all_complete_pinyin(_pure_pinyin_sequence, _pinyin_segmentation))
            { /* 双拼部分是完整的拼音，需要触发辅助码 */
                _pure_pinyin_sequence = _pinyin_sequence.substr(0, _pinyin_sequence.size() - 1);
                _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pure_pinyin_sequence, profile_);
                _pinyin_helpcodes = _pinyin_sequence_with_cases.substr(_pinyin_sequence_with_cases.size() - 1, 1);
                _cur_candidate_list = generate_with_helpcodes( //
                    _pure_pinyin_sequence,                     //
                    _pinyin_segmentation,                      //
                    _pinyin_sequence,                          //
                    _pinyin_helpcodes                          //
                );
            }
            else
            { /* 依然使用纯拼音，不触发辅助码模式 */
                _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pinyin_sequence, profile_);
                _cur_candidate_list = generateSeries(_pinyin_sequence, _pinyin_segmentation);
            }
        }
        else
        { /* 偶数长度拼音序列，不需要触发辅助码 */
            _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pinyin_sequence, profile_);
            _cur_candidate_list = generateSeries(_pinyin_sequence, _pinyin_segmentation);
        }
    }

    _pinyin_segmentation = ShuangpinUtil::pinyin_segmentation(_pinyin_sequence, profile_);

    return 0;
}

std::string ShuangpinDictionary::get_quanpin() const
{

    string quanpin_str = ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(_pinyin_segmentation, profile_);
    quanpin_str.erase(std::remove(quanpin_str.begin(), quanpin_str.end(), '\''), quanpin_str.end());
    return quanpin_str;
}

std::string ShuangpinDictionary::get_quanpin_seg() const
{
    string quanpin_str = ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(_pinyin_segmentation, profile_);
    return quanpin_str;
}

vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::generate_for_creating_word(const string code)
{
    return select_complete_data(quanpin_db_, build_quanpin_sql_for_creating_word(code));
}

int ShuangpinDictionary::create_word(string pinyin, string word)
{
    return create_word_from_quanpin(shuangpin::normalize_input_with_delimiters(pinyin, profile_), std::move(word));
}

int ShuangpinDictionary::create_word_from_quanpin(string pinyin, string word)
{
    const auto segments = quanpin::split_segments(pinyin);
    const size_t han_count = HelpcodeUtils::count_han_chars(word);
    if (segments.empty() || segments.size() != han_count ||
        std::any_of(segments.begin(), segments.end(), [](const std::string &segment) {
            return segment.empty() || !quanpin::is_complete_pinyin_input(segment);
        }))
    {
        return ERROR_CODE;
    }

    pinyin = quanpin::join_segments(segments);
    const string jp = quanpin::segments_to_jianpin(segments);
    if (!do_validate(pinyin, jp, word))
    {
        return ERROR_CODE;
    }
    if (check_data(quanpin_db_, build_quanpin_sql_for_checking_word(pinyin, word)))
    {
        return OK;
    }
    if (insert_data(quanpin_db_, build_quanpin_sql_for_inserting_word(pinyin, jp, word)) != OK)
    {
        return ERROR_CODE;
    }
    (void)user_dictionary::record_user_insert(metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)),
                                              user_dictionary::DictionaryKind::Pinyin, pinyin, word, 10000);
    /* 插入新词之后要清理缓存 */
    reset_cache();
    return OK;
}

int ShuangpinDictionary::update_data(sqlite3 *target_db, const std::string &sql_str)
{
    if (target_db == nullptr)
    {
        return ERROR_CODE;
    }
    char *errmsg = nullptr;
    int exit = sqlite3_exec(target_db, sql_str.c_str(), nullptr, nullptr, &errmsg);
    if (exit != SQLITE_OK)
    {
        (void)0;
        sqlite3_free(errmsg);
        return ERROR_CODE;
    }
    return OK;
}

int ShuangpinDictionary::delete_data(sqlite3 *target_db, const std::string &sql_str)
{
    if (target_db == nullptr)
    {
        return ERROR_CODE;
    }
    char *errmsg = nullptr;
    int exit = sqlite3_exec(target_db, sql_str.c_str(), nullptr, nullptr, &errmsg);
    if (exit != SQLITE_OK)
    {
        (void)0;
        sqlite3_free(errmsg);
        return ERROR_CODE;
    }
    return OK;
}

int ShuangpinDictionary::update_weight_by_word(string word)
{
    return update_weight_by_pinyin_and_word(get_quanpin(), std::move(word));
}

int ShuangpinDictionary::update_weight_by_pinyin_and_word(string pinyin, string word)
{
    const auto direct_cuts = quanpin::cut_pinyin_by_mode(remove_delimiters(pinyin), "correction");
    // 只有“完整合法拼读”的直切才可信：correction 模式的 greedy 后备允许单声母
    // 等不完整段，会把双拼原始串（如 qbtmuo → q'b't'mu'o）切成恰好能拼回原串
    // 的碎片，导致归一化被跳过、置顶更新落在错误 key 上静默失败（0 行更新
    // 仍返回 OK）。完整拼读检查与 delete 路径的段数-字数比对语义一致。
    if (direct_cuts.empty() || !quanpin::has_only_complete_pinyin_segments(direct_cuts.front()) ||
        remove_delimiters(quanpin::join_segments(direct_cuts.front())) != remove_delimiters(pinyin))
    {
        pinyin = normalize_shuangpin_to_quanpin_input(pinyin);
    }
    const auto cuts = quanpin::cut_pinyin_by_mode(remove_delimiters(pinyin), "correction");
    if (cuts.empty())
        return ERROR_CODE;
    auto segments = cuts.front();
    const size_t han_count = HelpcodeUtils::count_han_chars(word);
    if (segments.size() > han_count)
        segments.resize(han_count);
    const std::string normalized = quanpin::join_segments(segments);
    if (update_data(quanpin_db_, build_quanpin_sql_for_updating_word(normalized, word)) != OK)
    {
        return ERROR_CODE;
    }
    (void)user_dictionary::record_pinyin_upsert_from_database(
        quanpin_db_path_, normalized, word, metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)));
    reset_cache();
    return OK;
}

int ShuangpinDictionary::delete_by_pinyin_and_word(string pinyin, string word)
{
    const auto direct_cuts = quanpin::cut_pinyin_by_mode(remove_delimiters(pinyin), "correction");
    const auto normalized_shuangpin = normalize_shuangpin_to_quanpin_input(pinyin);
    const auto shuangpin_cuts = quanpin::cut_pinyin_by_mode(normalized_shuangpin, "correction");
    const size_t han_count = HelpcodeUtils::count_han_chars(word);
    const bool direct_key_matches_word = !direct_cuts.empty() && direct_cuts.front().size() == han_count;
    const bool shuangpin_key_matches_word = !shuangpin_cuts.empty() && shuangpin_cuts.front().size() == han_count;
    if ((!direct_key_matches_word && shuangpin_key_matches_word) ||
        (direct_cuts.empty() ||
         remove_delimiters(quanpin::join_segments(direct_cuts.front())) != remove_delimiters(pinyin)))
    {
        pinyin = normalized_shuangpin;
    }
    const auto cuts = quanpin::cut_pinyin_by_mode(remove_delimiters(pinyin), "correction");
    if (cuts.empty())
        return ERROR_CODE;
    const std::string normalized = quanpin::join_segments(cuts.front());
    if (!user_dictionary::delete_dictionary_candidate(
            quanpin_db_path_, metasequoia::path_to_utf8(paths_.user(metasequoia::assets::user_journal)),
            user_dictionary::DictionaryKind::Pinyin, normalized, word))
        return ERROR_CODE;
    reset_cache();
    return OK;
}

// generate_with_seg_pinyin

ShuangpinDictionary::~ShuangpinDictionary()
{
    for (auto &[sql, stmt] : quanpin_statement_cache_)
    {
        if (stmt != nullptr)
        {
            sqlite3_finalize(stmt);
        }
    }
    if (quanpin_db_)
    {
        sqlite3_close(quanpin_db_);
    }
}

vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::query_from_quanpin_database(
    const std::string &pinyin_sequence, const std::string &pinyin_segmentation)
{
    if (quanpin_db_ == nullptr || pinyin_segmentation.empty())
    {
        return {};
    }

    const std::string quanpin_segmentation =
        ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(pinyin_segmentation, profile_);
    const auto segments = quanpin::split_segments(quanpin_segmentation);
    if (segments.empty())
    {
        return {};
    }

    std::vector<WordItem> candidate_list;
    try
    {
        const auto flat_items = quanpin::query_segments_keyed_flat(segments, quanpin_db_, quanpin_statement_cache_,
                                                                   INT_MAX, quanpin::QuerySource::Shuangpin);
        candidate_list.reserve(flat_items.size());
        for (const auto &item : flat_items)
        {
            candidate_list.emplace_back(pinyin_sequence, item.value, item.weight, CandidateSource::Database, item.key);
        }
    }
    catch (const std::exception &)
    {
        (void)0;
    }

    return candidate_list;
}

std::optional<WordItem> ShuangpinDictionary::find_candidate(const std::string &key, const std::string &value)
{
    const std::string table = quanpin::build_table_name(quanpin::split_segments(key));
    if (!quanpin_db_ || table.empty())
        return std::nullopt;
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "SELECT weight FROM \"" + table + "\" WHERE key=?1 AND value=?2 LIMIT 1";
    if (sqlite3_prepare_v2(quanpin_db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    if (sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW)
        return std::nullopt;
    return WordItem(key, value, sqlite3_column_int64(stmt, 0), CandidateSource::Database, key);
}

vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::query_initial_from_quanpin_database(const std::string &code,
                                                                                               int limit)
{
    if (quanpin_db_ == nullptr || code.size() != 1 || code.front() < 'a' || code.front() > 'z')
    {
        return {};
    }

    const std::string initial = ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(code, profile_);
    const auto rows = quanpin::query_initial(quanpin_db_, initial, limit);

    vector<WordItem> candidate_list;
    candidate_list.reserve(rows.size());
    for (const auto &item : rows)
    {
        candidate_list.emplace_back(code, item.value, item.weight, CandidateSource::Database, item.key);
    }
    return candidate_list;
}

vector<ShuangpinDictionary::WordItem> ShuangpinDictionary::select_complete_data(sqlite3 *target_db,
                                                                                const std::string &sql_str)
{
    vector<ShuangpinDictionary::WordItem> candidateList;
    if (target_db == nullptr)
    {
        return candidateList;
    }
    sqlite3_stmt *stmt;
    int exit = sqlite3_prepare_v2(target_db, sql_str.c_str(), -1, &stmt, 0);
    if (exit != SQLITE_OK)
    {
        (void)0;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        candidateList.emplace_back(                                               //
            string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0))), // key
            string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2))), // value
            sqlite3_column_int64(stmt, 3), CandidateSource::Database,
            string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)))); // canonical key
    }
    sqlite3_finalize(stmt);
    return candidateList;
}

int ShuangpinDictionary::check_data(sqlite3 *target_db, const std::string &sql_str)
{
    if (target_db == nullptr)
    {
        return false;
    }
    sqlite3_stmt *stmt;
    int exit = sqlite3_prepare_v2(target_db, sql_str.c_str(), -1, &stmt, 0);
    if (exit != SQLITE_OK)
    {
        (void)0;
    }
    bool exists = false;
    exit = sqlite3_step(stmt);
    if (exit == SQLITE_ROW)
    {
        exists = true;
    }
    sqlite3_finalize(stmt);
    return exists;
}

int ShuangpinDictionary::insert_data(sqlite3 *target_db, const std::string &sql_str)
{
    if (target_db == nullptr)
    {
        return ERROR_CODE;
    }
    char *errmsg = nullptr;
    int exit = sqlite3_exec(target_db, sql_str.c_str(), nullptr, nullptr, &errmsg);
    if (exit != SQLITE_OK)
    {
        (void)0;
        sqlite3_free(errmsg);
        return ERROR_CODE;
    }
    return OK;
}

std::string ShuangpinDictionary::normalize_shuangpin_to_quanpin_segmentation(const std::string &pinyin) const
{
    if (pinyin.empty())
    {
        return {};
    }

    return shuangpin::normalize_input_with_delimiters(pinyin, profile_);
}

std::string ShuangpinDictionary::normalize_shuangpin_to_quanpin_input(const std::string &pinyin) const
{
    return remove_delimiters(normalize_shuangpin_to_quanpin_segmentation(pinyin));
}

std::string ShuangpinDictionary::build_quanpin_sql_for_creating_word(const std::string &pinyin) const
{
    const std::string normalized = normalize_shuangpin_to_quanpin_input(pinyin);
    const auto cuts = quanpin::cut_pinyin_by_mode(normalized, "correction");
    if (cuts.empty())
    {
        return "";
    }

    std::string sql;
    for (size_t i = 1; i <= cuts.front().size(); ++i)
    {
        std::vector<std::string> partial(cuts.front().begin(), cuts.front().begin() + i);
        const std::string key = quanpin::join_segments(partial);
        const std::string table = quanpin::build_table_name(partial);
        const std::string each =
            fmt::format("select * from(select * from {} where key = '{}' order by weight desc)", table, key);
        sql = sql.empty() ? each : each + " union all " + sql;
    }
    return sql;
}

std::string ShuangpinDictionary::build_quanpin_sql_for_checking_word(const std::string &key,
                                                                     const std::string &value) const
{
    const auto cuts = quanpin::cut_pinyin_by_mode(key, "correction");
    if (cuts.empty())
    {
        return "";
    }
    const std::string table = quanpin::build_table_name(cuts.front());
    return fmt::format("select 1 from {} where key = '{}' and value = '{}';", table, escape_sql_text(key),
                       escape_sql_text(value));
}

std::string ShuangpinDictionary::build_quanpin_sql_for_inserting_word(const std::string &key, const std::string &jp,
                                                                      const std::string &value) const
{
    const auto cuts = quanpin::cut_pinyin_by_mode(key, "correction");
    if (cuts.empty())
    {
        return "";
    }
    const std::string table = quanpin::build_table_name(cuts.front());
    return fmt::format("insert into {} (key, jp, value, weight) values ('{}', '{}', '{}', '{}');", table,
                       escape_sql_text(key), escape_sql_text(jp), escape_sql_text(value), 10000);
}

std::string ShuangpinDictionary::build_quanpin_sql_for_updating_word(const std::string &word) const
{
    return build_quanpin_sql_for_updating_word(get_quanpin(), word);
}

std::string ShuangpinDictionary::build_quanpin_sql_for_updating_word(std::string pinyin, const std::string &word) const
{
    // 调用方约定传入 canonical 全拼（update_weight 已归一化；get_quanpin 本身就
    // 是全拼）。这里不能再过 normalize_shuangpin_to_quanpin_input：双拼 profile
    // 会把它当作双拼码重切，qin'tian'shuo 被搧成 qi'n'ti'an'sang'shuo，UPDATE
    // 永远落在错误 key 上静默失败（0 行更新仍返回 OK）。
    const auto cuts = quanpin::cut_pinyin_by_mode(remove_delimiters(pinyin), "correction");
    if (cuts.empty())
    {
        return "";
    }

    size_t han_cnt = HelpcodeUtils::count_han_chars(word);
    auto segments = cuts.front();
    if (segments.size() > han_cnt)
    {
        segments.resize(han_cnt);
    }

    pinyin = quanpin::join_segments(segments);
    const std::string jp = quanpin::segments_to_jianpin(segments);
    if (!do_validate(pinyin, jp, word))
    {
        return "";
    }

    const std::string table = quanpin::build_table_name(segments);
    return fmt::format("update {0} set weight = ( select MAX(weight) + 1 from {0} AS sub where sub.key = '{1}') "
                       "where key = '{1}' and value = '{2}';",
                       table, escape_sql_text(pinyin), escape_sql_text(word));
}

std::string ShuangpinDictionary::build_quanpin_sql_for_deleting_canonical_word(const std::string &canonical_pinyin,
                                                                               const std::string &word) const
{
    const auto cuts = quanpin::cut_pinyin_by_mode(canonical_pinyin, "correction");
    if (cuts.empty())
    {
        return "";
    }

    const std::string normalized = quanpin::join_segments(cuts.front());
    const std::string jp = quanpin::segments_to_jianpin(cuts.front());
    if (!do_validate(normalized, jp, word))
    {
        return "";
    }

    return fmt::format("delete from {} where key = '{}' and value = '{}';", quanpin::build_table_name(cuts.front()),
                       escape_sql_text(normalized), escape_sql_text(word));
}

bool ShuangpinDictionary::do_validate(string key, string jp, string value) const
{
    const std::string pure_key = remove_delimiters(key);
    if (pure_key.empty())
    {
        return false;
    }

    const size_t han_count = HelpcodeUtils::count_han_chars(value);
    if (jp.size() != han_count)
    {
        return false;
    }

    const auto cuts = quanpin::cut_pinyin_by_mode(pure_key, "correction");
    if (!cuts.empty())
    {
        return cuts.front().size() == han_count;
    }

    return pure_key.size() % 2 == 0 && pure_key.size() == han_count * 2;
}

string ShuangpinDictionary::search_sentence_from_ime_engine(const string &user_pinyin)
{
    return decoder_.sentence(user_pinyin);
}

void ShuangpinDictionary::reset_state()
{
    _is_full_help_mode = false;
    _help_mode_raw_pos = 0;
    _kb_input_sequence.clear();
    _pinyin_sequence = "";
    _pinyin_sequence_with_cases = "";
    _pure_pinyin_sequence = "";
    _pinyin_segmentation = "";
    _help_codes_sequence.fill(0);
    _cur_candidate_list.clear();
    _cur_page_candidate_list.clear();
}

void ShuangpinDictionary::reset_cache()
{
    _cached_buffer.clear();
    _cached_buffer_sgl.clear();
    _cached_buffer_sgl_reversed.clear();
    _cached_buffer_dbl.clear();
    _cached_buffer_series.clear();
}

void ShuangpinDictionary::reset_cache_if_database_changed()
{
    if (quanpin_db_ == nullptr)
    {
        return;
    }
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(quanpin_db_, "PRAGMA data_version", -1, &statement, nullptr) != SQLITE_OK)
    {
        return;
    }
    if (sqlite3_step(statement) != SQLITE_ROW)
    {
        sqlite3_finalize(statement);
        return;
    }
    const sqlite3_int64 current_version = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    if (data_version_ >= 0 && current_version != data_version_)
    {
        reset_cache();
    }
    data_version_ = current_version;
}

int ShuangpinDictionary::insert_word_to_cached_buffer_series(const std::string &pinyin, const std::string &word,
                                                             CandidateSource source)
{
    (void)0;
    if (pinyin.empty() || word.empty())
    {
        return -1;
    }

    auto list = _cached_buffer_series.get(pinyin).value_or(std::vector<WordItem>{});

    // Keep at most one cloud/AI suggestion in the series cache for this key.
    if (source == CandidateSource::AiSuggestion || source == CandidateSource::CloudSuggestion)
    {
        list.erase(
            std::remove_if(list.begin(), list.end(), [source](const WordItem &item) { return item.source == source; }),
            list.end());
    }

    const auto exists = std::find_if(list.begin(), list.end(), [&](const WordItem &item) { return item.word == word; });
    if (exists == list.end())
    {
        if (list.empty())
        {
            list.emplace_back(pinyin, word, 1, source);
        }
        else
        {
            const size_t index = source == CandidateSource::AiSuggestion ? std::min<size_t>(2, list.size()) : 1;
            list.insert(list.begin() + index, WordItem(pinyin, word, 1, source));
        }
    }

    if (source == CandidateSource::CloudSuggestion)
    {
        const auto ai = std::find_if(list.begin(), list.end(),
                                     [](const WordItem &item) { return item.source == CandidateSource::AiSuggestion; });
        if (ai != list.end())
        {
            WordItem ai_item = std::move(*ai);
            list.erase(ai);
            list.insert(list.begin() + std::min<size_t>(2, list.size()), std::move(ai_item));
        }
    }

    _cached_buffer_series.insert(pinyin, list);
    return 0;
}

int ShuangpinDictionary::insert_word_to_active_helpcode_cache(const std::string &pinyin, const std::string &word,
                                                              CandidateSource source)
{
    auto insert_into_cache = [&](auto &cache) {
        if (auto opt = cache.get(pinyin))
        {
            auto list = opt.value();
            if (source == CandidateSource::AiSuggestion || source == CandidateSource::CloudSuggestion)
            {
                list.erase(std::remove_if(list.begin(), list.end(),
                                          [source](const WordItem &item) { return item.source == source; }),
                           list.end());
            }
            const auto exists =
                std::find_if(list.begin(), list.end(), [&](const WordItem &item) { return item.word == word; });
            if (exists == list.end())
            {
                if (list.size() >= 1)
                {
                    const size_t index = source == CandidateSource::AiSuggestion ? std::min<size_t>(2, list.size()) : 1;
                    list.insert(list.begin() + index, WordItem(pinyin, word, 1, source));
                }
                else
                {
                    list.emplace_back(pinyin, word, 1, source);
                }
            }
            cache.insert(pinyin, list);
            return true;
        }
        return false;
    };

    const bool updated_single = insert_into_cache(_cached_buffer_sgl);
    const bool updated_reversed_single = insert_into_cache(_cached_buffer_sgl_reversed);
    const bool updated_double = insert_into_cache(_cached_buffer_dbl);
    return updated_single || updated_reversed_single || updated_double ? 0 : -1;
}

bool ShuangpinDictionary::is_all_complete_pinyin()
{
    bool res = ShuangpinUtil::is_all_complete_pinyin(_pinyin_sequence, _pinyin_segmentation);
    return res;
}

bool ShuangpinDictionary::is_all_complete_pure_pinyin()
{
    bool res = ShuangpinUtil::is_all_complete_pinyin( //
        _pure_pinyin_sequence,                        //
        ShuangpinUtil::pinyin_segmentation(_pure_pinyin_sequence, profile_));
    return res;
}

std::string ShuangpinDictionary::get_pinyin_segmentation_with_cases()
{
    string res;
    int index = 0;

    if (_pinyin_segmentation.empty() || _pinyin_sequence_with_cases.empty())
        return res;

    string extracted_pinyin = "";
    for (size_t i = 0; i < _pinyin_segmentation.size(); ++i)
    {
        if (_pinyin_segmentation[i] == '\'')
        {
            continue;
        }
        else
        {
            extracted_pinyin += _pinyin_segmentation[i];
        }
    }

    if (extracted_pinyin != boost::algorithm::to_lower_copy(_pinyin_sequence_with_cases))
    {
        return res;
    }

    for (size_t i = 0; i < _pinyin_segmentation.size(); ++i)
    {
        if (_pinyin_segmentation[i] == '\'')
        {
            res += _pinyin_segmentation[i];
            continue;
        }
        else
        {
            if (_pinyin_segmentation[i] == _pinyin_sequence_with_cases[index])
            {
                res += _pinyin_segmentation[i];
            }
            else if (_pinyin_segmentation[i] == _pinyin_sequence_with_cases[index] + ('a' - 'A'))
            {
                res += _pinyin_sequence_with_cases[index];
            }
        }
        index += 1;
    }

    return res;
}
