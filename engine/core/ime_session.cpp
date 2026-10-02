#include "ime_session.h"
#include "../schemes/quanpin_scheme.h"
#include "../schemes/shuangpin_scheme.h"
#include "../schemes/wubi_scheme.h"
#include "../schemes/japanese_romaji_scheme.h"
#include "../quanpin/quanpin_utils.h"
#include "../shuangpin/shuangpin_query.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace
{
void ApplyShuangpinHelpcodeSegmentation(QueryRequest &request, const ShuangpinProfile &profile)
{
    if (request.scheme != SchemeType::Shuangpin || !request.enable_shuangpin_helpcode ||
        shuangpin::detect_active_double_helpcode_length(request.raw_input, request.raw_input_with_cases, profile) != 2)
    {
        return;
    }

    const size_t helpcode_length = 2;
    // The detector locates the help codes in delimiter-stripped space, so the split has to be made there too: slicing
    // the last raw bytes would push a pinyin letter into the base and a manual delimiter into the help codes.
    const std::string base_raw_input =
        shuangpin::trim_trailing_letters_preserve_delimiters(request.raw_input, helpcode_length);
    const std::string base_raw_input_with_cases =
        shuangpin::trim_trailing_letters_preserve_delimiters(request.raw_input_with_cases, helpcode_length);
    const std::string base_segmentation = shuangpin::segment_input(base_raw_input, profile);
    const std::string effective_input_with_cases = shuangpin::remove_manual_delimiters(request.raw_input_with_cases);
    const std::string help_codes =
        effective_input_with_cases.substr(effective_input_with_cases.size() - helpcode_length);

    request.raw_segmentation =
        shuangpin::apply_segmentation_cases(base_segmentation, base_raw_input_with_cases) + "'" + help_codes;
    request.normalized_segmentation = shuangpin::to_quanpin_segmentation(base_segmentation, profile) + "'" + help_codes;
    request.segmentation = request.normalized_segmentation;
}
} // namespace

ImeSession::ImeSession(SchemeType scheme_type, const ShuangpinProfile &shuangpin_profile,
                       metasequoia::RuntimePaths paths)
    : provider_registry_(shuangpin_profile, std::move(paths)), shuangpin_profile_(shuangpin_profile),
      scheme_(create_scheme(scheme_type))
{
    bind_wubi_scheme();
}

void ImeSession::bind_wubi_scheme()
{
    wubi_scheme_ = dynamic_cast<WubiScheme *>(scheme_.get());
    if (wubi_scheme_ != nullptr)
    {
        wubi_scheme_->set_mixed_pinyin_allowed(wubi_options_.mixed_pinyin);
        wubi_scheme_->set_z_wildcard(wubi_options_.z_wildcard);
    }
}

void ImeSession::set_wubi_input_options(metasequoia::WubiInputOptions options)
{
    wubi_options_ = options;
    if (wubi_scheme_ != nullptr)
    {
        wubi_scheme_->set_mixed_pinyin_allowed(wubi_options_.mixed_pinyin);
        wubi_scheme_->set_z_wildcard(wubi_options_.z_wildcard);
    }
}

void ImeSession::handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    scheme_->handle_key(vk, modifiers_down, wch);
    // 查询并更新候选词列表
    refresh_candidates();
}

void ImeSession::switch_scheme(SchemeType scheme_type)
{
    scheme_ = create_scheme(scheme_type);
    bind_wubi_scheme();
    state_ = CompositionState{};
}

void ImeSession::set_shuangpin_helpcode_enabled(bool enabled)
{
    enable_shuangpin_helpcode_ = enabled;
}

void ImeSession::set_quanpin_helpcode_enabled(bool enabled)
{
    enable_quanpin_helpcode_ = enabled;
}

void ImeSession::set_quanpin_autocorrect_types(unsigned autocorrect_types)
{
    quanpin_autocorrect_types_ = autocorrect_types;
}

void ImeSession::replace_shuangpin_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    if (scheme_->type() != SchemeType::Shuangpin)
    {
        return;
    }

    auto *shuangpin_scheme = dynamic_cast<ShuangpinScheme *>(scheme_.get());
    if (!shuangpin_scheme)
    {
        return;
    }

    shuangpin_scheme->set_raw_input(raw_input, raw_input_with_cases);
    refresh_candidates();
}

void ImeSession::replace_quanpin_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    if (scheme_->type() != SchemeType::Quanpin)
    {
        return;
    }

    auto *quanpin_scheme = dynamic_cast<QuanpinScheme *>(scheme_.get());
    if (!quanpin_scheme)
    {
        return;
    }

    quanpin_scheme->set_raw_input(raw_input, raw_input_with_cases);
    refresh_candidates();
}

void ImeSession::replace_wubi_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    if (scheme_->type() != SchemeType::Wubi)
    {
        return;
    }

    if (wubi_scheme_ == nullptr)
    {
        return;
    }

    // A replacement is host-driven editing of text the composition already holds, so the length it
    // is clipped to comes from the setting rather than from whatever the previous query happened to
    // answer. Without this, moving the caret through a mixed composition silently drops everything
    // past the fourth letter. refresh_candidates puts the query-derived value back.
    wubi_scheme_->set_extended_length_allowed(wubi_options_.mixed_pinyin);
    wubi_scheme_->set_raw_input(raw_input, raw_input_with_cases);
    refresh_candidates();
}

void ImeSession::reset()
{
    scheme_->reset();
    state_ = CompositionState{};
}

void ImeSession::reset_cache()
{
    provider_registry_.reset_cache(current_scheme_type());
    // 混输组合同时查过拼音 provider，清缓存必须两边都清，否则选中调频后的重排会读到旧序。
    if (wubi_scheme_ != nullptr && wubi_options_.mixed_pinyin)
    {
        provider_registry_.reset_cache(SchemeType::Quanpin);
    }
    refresh_candidates();
}

void ImeSession::reset_sentence_cache()
{
    provider_registry_.reset_sentence_cache();
}

int ImeSession::create_word(std::string pinyin, std::string word)
{
    return provider_registry_.create_word(current_scheme_type(), std::move(pinyin), std::move(word));
}

int ImeSession::update_weight_by_pinyin_and_word(SchemeType scheme, std::string pinyin, std::string word)
{
    return provider_registry_.update_weight_by_pinyin_and_word(scheme, std::move(pinyin), std::move(word));
}

int ImeSession::delete_by_pinyin_and_word(SchemeType scheme, std::string pinyin, std::string word)
{
    return provider_registry_.delete_by_pinyin_and_word(scheme, std::move(pinyin), std::move(word));
}

int ImeSession::cache_dynamic_candidate(const std::string &pinyin, const std::string &word, CandidateSource source)
{
    return provider_registry_.cache_dynamic_candidate(current_scheme_type(), pinyin, word, source);
}

std::vector<WordItem> ImeSession::query_raw_candidates(const std::string &raw_input,
                                                       const std::string &raw_input_with_cases)
{
    // A throwaway scheme instance: set_raw_input on the live scheme would clobber its key
    // strokes, and a prefix query must leave the composition exactly as it found it.
    const std::unique_ptr<IInputScheme> query_scheme = create_scheme(scheme_->type());
    query_scheme->set_raw_input(raw_input, raw_input_with_cases);
    QueryRequest request = query_scheme->build_request();
    apply_request_options(request);
    ApplyShuangpinHelpcodeSegmentation(request, shuangpin_profile_);
    if (!request.valid)
    {
        return {};
    }
    return provider_registry_.resolve(request.scheme).query(request);
}

std::vector<WordItem> ImeSession::query_without_syllable_helpcodes()
{
    if (!state_.request.valid || state_.request.syllable_helpcodes.empty() ||
        !state_.request.enable_mid_sentence_helpcode)
    {
        return state_.candidates;
    }
    // 反引号段在请求里已换成分隔符，切分与不敲辅助码时一致；关掉开关，词典层就不再按约束筛。
    QueryRequest request = state_.request;
    request.enable_mid_sentence_helpcode = false;
    return provider_registry_.resolve(request.scheme).query(request);
}

void ImeSession::replace_japanese_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    if (scheme_->type() != SchemeType::JapaneseRomaji)
        return;
    auto *japanese_scheme = dynamic_cast<JapaneseRomajiScheme *>(scheme_.get());
    if (!japanese_scheme)
        return;
    japanese_scheme->set_raw_input(raw_input, raw_input_with_cases);
    refresh_candidates();
}

void ImeSession::replace_active_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    switch (scheme_->type())
    {
    case SchemeType::Quanpin:
        replace_quanpin_raw_input(raw_input, raw_input_with_cases);
        return;
    case SchemeType::Shuangpin:
        replace_shuangpin_raw_input(raw_input, raw_input_with_cases);
        return;
    case SchemeType::Wubi:
        replace_wubi_raw_input(raw_input, raw_input_with_cases);
        return;
    case SchemeType::JapaneseRomaji:
        replace_japanese_raw_input(raw_input, raw_input_with_cases);
        return;
    }
}

int ImeSession::cache_dynamic_candidate_for_current_request(const std::string &word, CandidateSource source)
{
    return provider_registry_.cache_dynamic_candidate_for_request(state_.request, word, source);
}

int ImeSession::apply_dynamic_candidate(const std::string &word, CandidateSource source)
{
    const int result = cache_dynamic_candidate_for_current_request(word, source);
    if (result == 0)
    {
        refresh_candidates();
    }
    return result;
}

SchemeType ImeSession::current_scheme_type() const
{
    return scheme_->type();
}

const std::string &ImeSession::get_preedit() const
{
    return state_.preedit;
}

const QueryRequest &ImeSession::get_request() const
{
    return state_.request;
}

const std::vector<WordItem> &ImeSession::get_candidates() const
{
    return state_.candidates;
}

std::optional<WordItem> ImeSession::find_candidate(SchemeType scheme, const std::string &key, const std::string &value)
{
    return provider_registry_.find_candidate(scheme, key, value);
}

bool ImeSession::expand_initial_candidates()
{
    return provider_registry_.expand_initial_candidates(state_.request, state_.candidates);
}

void ImeSession::apply_request_options(QueryRequest &request) const
{
    request.enable_shuangpin_helpcode = enable_shuangpin_helpcode_;
    request.enable_mid_sentence_helpcode = enable_mid_sentence_helpcode_;
    request.enable_quanpin_helpcode = enable_quanpin_helpcode_;
    request.enable_quanpin_autocorrect_transposition =
        (quanpin_autocorrect_types_ & quanpin::kAutocorrectTransposition) != 0;
    request.enable_quanpin_autocorrect_neighbor = (quanpin_autocorrect_types_ & quanpin::kAutocorrectNeighbor) != 0;
    request.fuzzy_pinyin = fuzzy_pinyin_;
    request.sentence_association = sentence_association_;
    request.rescoring_context = rescoring_context_;
}

void ImeSession::refresh_candidates()
{
    state_.preedit = scheme_->get_preedit();
    state_.request = scheme_->build_request();
    apply_request_options(state_.request);
    ApplyShuangpinHelpcodeSegmentation(state_.request, shuangpin_profile_);

    if (!state_.request.valid)
    {
        state_.candidates.clear();
        return;
    }

    state_.candidates = provider_registry_.resolve(state_.request.scheme).query(state_.request);
    if (wubi_scheme_ == nullptr)
    {
        return;
    }

    // The wubi provider also returns longer codes that start with the input (per-key hints), and a
    // one-to-three letter prefix almost always has some. Only a row for exactly this code means the
    // table answered it; hints alone must not keep mixed input from offering pinyin.
    const bool wubi_table_answered =
        std::any_of(state_.candidates.begin(), state_.candidates.end(),
                    [this](const WordItem &item) { return item.pinyin == state_.request.normalized_input; });

    // Once the table has failed the code in hand, mixed input lets the composition grow past four
    // letters so a full spelling can be finished. A code the table answers keeps the four-letter
    // limit, so a fluent wubi typist is not interrupted; the fifth letter is handled by the host's
    // top-commit path instead.
    wubi_scheme_->set_extended_length_allowed(wubi_options_.mixed_pinyin && !wubi_table_answered);

    if (!wubi_options_.mixed_pinyin)
    {
        return;
    }

    // 五笔拼音混输：同一串字母同时交给五笔码表和全拼，五笔候选在前、拼音候选按权重追加在后，
    // 两个表都答得出的词只留五笔那份。这是业界惯例的「候选词优先五笔，兼容拼音」，而不是
    // 只在码表失手时才转拼音的兜底。
    QuanpinScheme pinyin;
    pinyin.set_raw_input(state_.request.raw_input, state_.request.raw_input_with_cases);
    QueryRequest pinyin_request = pinyin.build_request();
    apply_request_options(pinyin_request);
    // The same physical keys produced these letters, so the strokes carry over rather than
    // reaching the provider empty.
    pinyin_request.key_strokes = state_.request.key_strokes;
    if (!pinyin_request.valid)
    {
        return;
    }

    std::vector<WordItem> pinyin_candidates = provider_registry_.resolve(pinyin_request.scheme).query(pinyin_request);
    if (pinyin_candidates.empty())
    {
        return;
    }

    // 通配与混输同时开启时，含 z 的编码既可能是通配猜码也可能是拼音拼写（zi、zai、zhong）。
    // 通配查询按权重返回整页无关码行，放在前面会把拼音的正解挤到后几页，所以这种组合里拼音
    // 在前、通配结果在后；同词只留拼音那份。
    std::vector<WordItem> leading = std::move(state_.candidates);
    std::vector<WordItem> trailing = std::move(pinyin_candidates);
    if (state_.request.wubi_z_wildcard)
    {
        std::swap(leading, trailing);
    }

    std::unordered_set<std::string> seen_words;
    seen_words.reserve(leading.size() + trailing.size());
    for (const WordItem &item : leading)
    {
        seen_words.insert(item.word);
    }
    for (WordItem &item : trailing)
    {
        if (seen_words.insert(item.word).second)
        {
            leading.push_back(std::move(item));
        }
    }
    state_.candidates = std::move(leading);
}

std::unique_ptr<IInputScheme> ImeSession::create_scheme(SchemeType scheme_type) const
{
    switch (scheme_type)
    {
    case SchemeType::Shuangpin:
        return std::make_unique<ShuangpinScheme>(shuangpin_profile_);
    case SchemeType::Quanpin:
        return std::make_unique<QuanpinScheme>();
    case SchemeType::Wubi:
        return std::make_unique<WubiScheme>();
    case SchemeType::JapaneseRomaji:
        return std::make_unique<JapaneseRomajiScheme>();
    default:
        throw std::runtime_error("Unknown scheme type.");
    }
}
