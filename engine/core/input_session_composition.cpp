#include "input_session.h"
#include "../common/helpcode_utils.h"
#include "../quanpin/quanpin_utils.h"
#include "../shuangpin/shuangpin_query.h"
#include "../shuangpin/shuangpin_utils.h"
#include "../japanese/romaji_converter.h"
#include <algorithm>
#include <cctype>

namespace metasequoia
{
namespace
{
std::string remove_delimiters(const std::string &segmented)
{
    std::string normalized;
    normalized.reserve(segmented.size());
    for (const char ch : segmented)
    {
        if (ch != '\'')
        {
            normalized.push_back(ch);
        }
    }
    return normalized;
}

void remove_consumed_leading_separators(std::string &raw_input, std::string &raw_input_with_cases)
{
    size_t count = 0;
    while (count < raw_input.size() && raw_input[count] == '\'')
    {
        ++count;
    }
    raw_input.erase(0, count);

    count = 0;
    while (count < raw_input_with_cases.size() && raw_input_with_cases[count] == '\'')
    {
        ++count;
    }
    raw_input_with_cases.erase(0, count);
}

std::string normalize_canonical_pinyin_for_word(const std::string &pinyin, const std::string &word)
{
    if (pinyin.empty())
    {
        return {};
    }

    const auto segments = quanpin::split_segments(pinyin);
    if (segments.empty() || segments.size() != HelpcodeUtils::count_han_chars(word))
    {
        return {};
    }
    for (const auto &segment : segments)
    {
        if (segment.empty() || !quanpin::is_complete_pinyin_input(segment))
        {
            return {};
        }
    }
    return quanpin::join_segments(segments);
}

std::string append_canonical_pinyin(const std::string &prefix, const std::string &suffix)
{
    if (prefix.empty())
    {
        return suffix;
    }
    if (suffix.empty())
    {
        return {};
    }
    return prefix + "'" + suffix;
}

struct ShuangpinCompositionBase
{
    std::string raw_input;
    std::string raw_input_with_cases;
    std::string effective_raw_input;
    std::string effective_raw_input_with_cases;
    size_t helpcode_length = 0;
};

ShuangpinCompositionBase ResolveShuangpinCompositionBase(const QueryRequest &request, const ShuangpinProfile &profile)
{
    ShuangpinCompositionBase base{
        request.raw_input, request.raw_input_with_cases.empty() ? request.raw_input : request.raw_input_with_cases};
    base.effective_raw_input = shuangpin::remove_manual_delimiters(base.raw_input);
    base.effective_raw_input_with_cases = shuangpin::remove_manual_delimiters(base.raw_input_with_cases);

    if (!request.enable_shuangpin_helpcode || base.effective_raw_input.empty())
    {
        return base;
    }

    const auto has_complete_unseparated_base = [&](size_t helpcode_length) {
        if (base.effective_raw_input.size() <= helpcode_length)
        {
            return false;
        }
        const size_t pure_length = base.effective_raw_input.size() - helpcode_length;
        const size_t raw_prefix_length = shuangpin::raw_length_for_effective_prefix(base.raw_input, pure_length);
        // An apostrophe immediately before the suffix makes that suffix a
        // user-defined pinyin segment, not an auxiliary code.
        if (raw_prefix_length < base.raw_input.size() && base.raw_input[raw_prefix_length] == '\'')
        {
            return false;
        }
        return shuangpin::is_complete_input(base.raw_input.substr(0, raw_prefix_length), profile);
    };

    if (shuangpin::detect_active_double_helpcode_length(base.raw_input, base.raw_input_with_cases, profile) == 2)
    {
        base.helpcode_length = 2;
        return base;
    }

    if (base.effective_raw_input.size() % 2 == 1 && base.effective_raw_input.size() > 1)
    {
        if (has_complete_unseparated_base(1))
        {
            base.helpcode_length = 1;
        }
    }

    return base;
}

bool HasActiveQuanpinHelpcode(const QueryRequest &request)
{
    return request.enable_quanpin_helpcode &&
           quanpin::detect_active_helpcode_length(request.raw_input, request.raw_input_with_cases) > 0;
}

std::string ResolveShuangpinCloudCacheKey(const QueryRequest &request, const ShuangpinProfile &profile)
{
    const auto base = ResolveShuangpinCompositionBase(request, profile);
    if (base.helpcode_length > 0 && base.effective_raw_input.size() >= base.helpcode_length)
    {
        const size_t base_length = base.effective_raw_input.size() - base.helpcode_length;
        return base.raw_input.substr(0, shuangpin::raw_length_for_effective_prefix(base.raw_input, base_length));
    }
    return base.raw_input;
}

std::string ResolveQuanpinCloudCacheKey(const QueryRequest &request)
{
    return quanpin::strip_active_helpcodes(request.raw_input, request.raw_input_with_cases);
}

// Same four-bit mapping as autocorrect_types_from_request (engine.cpp): the
// request only carries the two legacy bools, and either one on also enables
// deletion and insertion so the preedit rebuild sees the same correction space
// as the query.
unsigned QuanpinAutocorrectTypes(const QueryRequest &request)
{
    const unsigned legacy =
        (request.enable_quanpin_autocorrect_transposition ? quanpin::kAutocorrectTransposition : 0u) |
        (request.enable_quanpin_autocorrect_neighbor ? quanpin::kAutocorrectNeighbor : 0u);
    return legacy == 0 ? 0u : legacy | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
}

std::string QuanpinLettersWithoutDelimiters(const std::string &text)
{
    std::string letters;
    letters.reserve(text.size());
    for (const char ch : text)
    {
        if (ch != '\'')
        {
            letters.push_back(ch);
        }
    }
    return letters;
}

// Folds letters for autocorrect comparisons: lowercases and strips manual
// delimiters, and maps the u-umlaut style 'v' spelling onto 'u'. The jv/nv
// normalisation is not a correction, so it must never make the scheme
// segmentation look rewritten.
std::string FoldQuanpinAutocorrectLetters(const std::string &text)
{
    std::string folded;
    folded.reserve(text.size());
    for (const char ch : text)
    {
        if (ch == '\'')
        {
            continue;
        }
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        folded.push_back(lower == 'v' ? 'u' : lower);
    }
    return folded;
}

// Folded letters of a cut's syllable sequence, for comparing the BFS reading
// against the scheme segmentation reading.
std::string CutSyllableLetters(const quanpin::AutocorrectCut &cut)
{
    std::string letters;
    for (const auto &segment : cut.segments)
    {
        letters += segment.syllable;
    }
    return FoldQuanpinAutocorrectLetters(letters);
}

// Rebuilds the preedit from the cased input letters with separators at the raw
// spans the correction BFS reports. Letters (case included) are preserved
// verbatim; manual delimiters are replaced by the actual cut positions.
std::string RebuildQuanpinDisplayFromCut(const std::string &cased_input, const quanpin::AutocorrectCut &cut)
{
    std::string display;
    display.reserve(cased_input.size() + cut.segments.size());
    size_t letter_index = 0;
    size_t boundary_index = 0;
    const size_t boundary_count = cut.segments.empty() ? 0 : cut.segments.size() - 1;
    for (const char ch : cased_input)
    {
        if (ch == '\'')
        {
            continue;
        }
        display.push_back(ch);
        ++letter_index;
        if (boundary_index < boundary_count &&
            letter_index == cut.segments[boundary_index].start + cut.segments[boundary_index].raw_text.size())
        {
            display.push_back('\'');
            ++boundary_index;
        }
    }
    return display;
}

// The preedit must always show the letters the user actually typed (PRD R5).
// Two layers can rewrite them into canonical pinyin: the scheme alias table
// (sahng -> shang, baked into raw_segmentation) and the dictionary correction
// BFS (shabg -> shang, which only re-separates). Both are rebuilt here from
// the raw letters with separators at the actual cut positions; when the BFS
// cannot explain a rewrite (length-changing aliases such as mihng -> ming) the
// raw letters are shown without separators. The rebuild is deliberately
// switch-independent: the alias layer rewrites letters regardless of the
// autocorrect switches, and AC1 only constrains the candidate list.
std::string BuildQuanpinAutocorrectDisplay(const QueryRequest &request)
{
    const std::string &cased = request.raw_input_with_cases.empty() ? request.raw_input : request.raw_input_with_cases;
    const std::string base = request.raw_segmentation.empty() ? cased : request.raw_segmentation;
    if (request.raw_input.empty() || cased.empty())
    {
        return base;
    }

    const unsigned types = QuanpinAutocorrectTypes(request);
    const std::string folded_input = FoldQuanpinAutocorrectLetters(cased);
    const bool letters_rewritten = FoldQuanpinAutocorrectLetters(QuanpinLettersWithoutDelimiters(base)) != folded_input;

    // Fast path: the scheme kept the typed letters and either no correction
    // type is enabled or the input is already a complete pinyin spelling, so
    // no correction interpretation exists to draw separators from.
    if (!letters_rewritten && (types == 0 || quanpin::is_complete_pinyin_input(request.raw_input)))
    {
        return base;
    }

    // Jianpin guard, mirroring resolve_series_query in the dictionary layer: a
    // legal syllable plus at most one trailing letter is user intent, never a
    // typo. The deletion table explains shapes like "zheg" -> zheng, so without
    // this guard the preedit would lose the scheme separators once the deletion
    // bit rides along with the legacy switches.
    if (quanpin::looks_like_syllable_with_jianpin_tail(request.raw_input))
    {
        return base;
    }

    const auto cut = quanpin::autocorrect_cut_detail(folded_input, types);
    // When the scheme rewrote the letters, the query resolved through the alias
    // layer, so the preedit may only draw separators from the BFS when both
    // layers explain the letters identically ("sahnghao" -> shang'hao).
    // Otherwise the deletion bit would re-separate "sahng" as sa'hng while the
    // candidates actually come from the alias reading shang -- the old code
    // never noticed because the cut happened to be empty without it.
    if (!cut.empty() && (!letters_rewritten || CutSyllableLetters(cut) == FoldQuanpinAutocorrectLetters(
                                                                              QuanpinLettersWithoutDelimiters(base))))
    {
        return RebuildQuanpinDisplayFromCut(cased, cut);
    }
    // The BFS cannot explain the input (e.g. a length-changing alias such as
    // mihng -> ming): fall back to the plain raw letters when the letters were
    // rewritten, otherwise keep the scheme segmentation untouched.
    return letters_rewritten ? QuanpinLettersWithoutDelimiters(cased) : base;
}
} // namespace

void InputSession::handle_engine_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    engine_.handle_key(vk, modifiers_down, wch);
    online_requests_.invalidate();
    update_mixed_candidates();
}

void InputSession::recompute_candidates()
{
    if (has_pending_pinyin_sequence_ || has_pending_pinyin_sequence_with_cases_)
    {
        apply_pending_sequence();
        return;
    }
    engine_.handle_key(0, 0, 0);
    update_mixed_candidates();
}

SchemeType InputSession::current_scheme_type() const
{
    return engine_.current_scheme_type();
}

void InputSession::reset_state()
{
    clear_pending_sequence();
    reset_composition();
}

void InputSession::reset_cache()
{
    engine_.reset_cache();
    if (canonical_phrase_engine_)
        canonical_phrase_engine_->reset_cache();
}

const std::vector<WordItem> &InputSession::get_candidates() const
{
    return candidates();
}

bool InputSession::expand_initial_candidates()
{
    return engine_.expand_initial_candidates();
}

std::optional<WordItem> InputSession::find_candidate(const std::string &key, const std::string &value)
{
    return engine_.find_candidate(key, value);
}

const QueryRequest &InputSession::request() const
{
    return engine_.get_request();
}

const std::string &InputSession::get_pinyin_sequence() const
{
    return request().raw_input;
}

const std::string &InputSession::get_pinyin_sequence_with_cases() const
{
    return request().raw_input_with_cases.empty() ? request().raw_input : request().raw_input_with_cases;
}

const std::string &InputSession::get_pure_pinyin_sequence() const
{
    return request().normalized_input;
}

const std::string &InputSession::get_pinyin_segmentation() const
{
    return request().normalized_segmentation.empty() ? request().segmentation : request().normalized_segmentation;
}

std::string InputSession::get_pinyin_segmentation_with_cases() const
{
    if (is_wubi())
    {
        return request().raw_input;
    }
    if (is_japanese())
    {
        return request().raw_input_with_cases.empty() ? request().raw_input : request().raw_input_with_cases;
    }
    if (is_shuangpin() && shuangpin_preedit_uses_raw_)
    {
        std::string preedit = request().raw_segmentation.empty() ? request().raw_input : request().raw_segmentation;
        if (!request().raw_input_with_cases.empty() && request().raw_input_with_cases.back() == '\'' &&
            (preedit.empty() || preedit.back() != '\''))
        {
            preedit.push_back('\'');
        }
        return preedit;
    }
    if (current_scheme_type() == SchemeType::Quanpin)
    {
        return BuildQuanpinAutocorrectDisplay(request());
    }
    std::string preedit =
        request().normalized_segmentation.empty() ? request().segmentation : request().normalized_segmentation;
    if (!request().raw_input_with_cases.empty() && request().raw_input_with_cases.back() == '\'' &&
        (preedit.empty() || preedit.back() != '\''))
    {
        preedit.push_back('\'');
    }
    return preedit;
}

std::string InputSession::get_quanpin() const
{
    return request().normalized_input;
}

bool InputSession::is_all_complete_pure_pinyin() const
{
    if (is_wubi())
    {
        return request().valid;
    }
    if (is_japanese())
    {
        return japanese::ConvertRomaji(request().raw_input).complete;
    }
    if (is_shuangpin())
    {
        const auto base = ResolveShuangpinCompositionBase(request(), shuangpin_profile_);
        if (base.helpcode_length > 0 && base.effective_raw_input.size() >= base.helpcode_length)
        {
            const size_t base_length = base.effective_raw_input.size() - base.helpcode_length;
            return shuangpin::is_complete_input(
                base.raw_input.substr(0, shuangpin::raw_length_for_effective_prefix(base.raw_input, base_length)),
                shuangpin_profile_);
        }
        return shuangpin::is_complete_input(base.raw_input, shuangpin_profile_);
    }
    const auto &segmentation =
        request().normalized_segmentation.empty() ? request().segmentation : request().normalized_segmentation;
    return !segmentation.empty() && quanpin::is_complete_pinyin_input(segmentation);
}

bool InputSession::has_active_helpcode() const
{
    if (is_wubi() || is_japanese())
    {
        return false;
    }
    if (is_shuangpin())
    {
        return ResolveShuangpinCompositionBase(request(), shuangpin_profile_).helpcode_length > 0;
    }
    return HasActiveQuanpinHelpcode(request());
}

void InputSession::set_pinyin_sequence(const std::string &pinyin_sequence)
{
    pending_pinyin_sequence_ = pinyin_sequence;
    has_pending_pinyin_sequence_ = true;
}

void InputSession::set_pinyin_sequence_with_cases(const std::string &pinyin_sequence)
{
    pending_pinyin_sequence_with_cases_ = pinyin_sequence;
    has_pending_pinyin_sequence_with_cases_ = true;
}

int InputSession::store_user_phrase(std::string pinyin, std::string word)
{
    return engine_.create_word(std::move(pinyin), std::move(word));
}

int InputSession::store_user_phrase_from_canonical_pinyin(std::string pinyin, std::string word)
{
    // Both quanpin and shuangpin ultimately share the canonical quanpin
    // dictionary.  Do not feed a complete quanpin key back through the active
    // shuangpin profile a second time.
    if (!canonical_phrase_engine_)
        canonical_phrase_engine_ = std::make_unique<QuanpinEngine>(paths_);
    return canonical_phrase_engine_->create_word_from_canonical_pinyin(std::move(pinyin), std::move(word));
}

int InputSession::pin_candidate(std::string pinyin, std::string word)
{
    return engine_.update_weight_by_pinyin_and_word(std::move(pinyin), std::move(word));
}

int InputSession::remove_candidate(std::string pinyin, std::string word)
{
    if (!is_wubi() && remove_delimiters(request().raw_input).size() == 1)
    {
        return -1;
    }
    return engine_.delete_by_pinyin_and_word(std::move(pinyin), std::move(word));
}

int InputSession::cache_dynamic_candidate(const std::string &pinyin, const std::string &word, CandidateSource source)
{
    const int cache_result = engine_.cache_dynamic_candidate(pinyin, word, source);
    (void)engine_.cache_dynamic_candidate_for_current_request(word, source);
    return cache_result;
}

InputSession::SelectionTransition InputSession::advance_composition_after_selection(
    const std::string &selected_pinyin, const std::string &selected_word, const std::string &selected_canonical_pinyin)
{
    SelectionTransition transition;
    transition.selected_canonical_pinyin = selected_canonical_pinyin;
    if (is_japanese())
    {
        transition.full_pure_pinyin = request().raw_input;
        transition.current_segmentation = request().segmentation;
        transition.current_segmentation_with_cases = request().raw_input_with_cases;
        return transition;
    }
    if (wubi_candidates_are_native())
    {
        transition.full_pure_pinyin = request().normalized_input;
        transition.current_segmentation = request().normalized_input;
        transition.current_segmentation_with_cases = request().raw_input;
        return transition;
    }
    if (is_shuangpin())
    {
        const auto base = ResolveShuangpinCompositionBase(request(), shuangpin_profile_);
        const size_t word_pinyin_length = HelpcodeUtils::count_han_chars(selected_word) * 2;
        const size_t total_input_length = base.effective_raw_input.size();

        transition.full_pure_pinyin =
            base.helpcode_length > 0 && total_input_length >= base.helpcode_length
                ? base.effective_raw_input.substr(0, total_input_length - base.helpcode_length)
                : base.effective_raw_input;

        size_t consumed_length = remove_delimiters(selected_pinyin).size();
        if (base.helpcode_length > 0)
        {
            const size_t required_length = word_pinyin_length + base.helpcode_length;
            transition.continues_composition =
                required_length < total_input_length && word_pinyin_length < total_input_length;

            if (transition.continues_composition)
            {
                const size_t rest_start =
                    shuangpin::raw_length_for_effective_prefix(base.raw_input_with_cases, word_pinyin_length);
                const size_t rest_end = shuangpin::raw_length_for_effective_prefix(
                    base.raw_input_with_cases, total_input_length - base.helpcode_length);
                const std::string rest_pinyin_sequence = base.raw_input.substr(rest_start, rest_end - rest_start);
                std::string normalized_rest = rest_pinyin_sequence;
                std::string cased_rest = base.raw_input_with_cases.substr(rest_start, rest_end - rest_start);
                remove_consumed_leading_separators(normalized_rest, cased_rest);
                engine_.replace_shuangpin_raw_input(normalized_rest, cased_rest);
                online_requests_.invalidate();
                update_mixed_candidates();
            }
        }
        else
        {
            if (consumed_length == 0 || consumed_length > base.effective_raw_input.size())
            {
                consumed_length = (std::min)(word_pinyin_length, base.effective_raw_input.size());
            }

            transition.continues_composition = consumed_length < transition.full_pure_pinyin.size();

            if (transition.continues_composition)
            {
                const size_t consumed_raw_length =
                    shuangpin::raw_length_for_effective_prefix(base.raw_input_with_cases, consumed_length);
                const std::string rest_pinyin_sequence =
                    base.raw_input.substr(consumed_raw_length, base.raw_input.size() - consumed_raw_length);
                const std::string rest_pinyin_sequence_with_cases = base.raw_input_with_cases.substr(
                    consumed_raw_length, base.raw_input_with_cases.size() - consumed_raw_length);
                std::string normalized_rest = rest_pinyin_sequence;
                std::string cased_rest = rest_pinyin_sequence_with_cases;
                remove_consumed_leading_separators(normalized_rest, cased_rest);
                engine_.replace_shuangpin_raw_input(normalized_rest, cased_rest);
                online_requests_.invalidate();
                update_mixed_candidates();
            }
        }

        transition.current_segmentation = get_pinyin_segmentation();
        transition.current_segmentation_with_cases = get_pinyin_segmentation_with_cases();
        return transition;
    }

    transition.full_pure_pinyin = request().normalized_input;
    const std::string current_segmentation =
        request().normalized_segmentation.empty() ? request().segmentation : request().normalized_segmentation;
    const std::string current_segmentation_with_cases = get_pinyin_segmentation_with_cases();
    const std::string selected_pure_pinyin = remove_delimiters(selected_pinyin);
    const std::string raw_input_without_helpcodes =
        quanpin::strip_active_helpcodes(request().raw_input, request().raw_input_with_cases);
    const std::string raw_input_with_cases_without_helpcodes =
        quanpin::strip_active_helpcodes_with_cases(request().raw_input, request().raw_input_with_cases);

    size_t consumed_raw_length =
        shuangpin::raw_length_for_effective_prefix(raw_input_with_cases_without_helpcodes, selected_pure_pinyin.size());

    transition.continues_composition = !selected_pure_pinyin.empty() &&
                                       selected_pure_pinyin.size() < transition.full_pure_pinyin.size() &&
                                       consumed_raw_length < raw_input_without_helpcodes.size();

    if (transition.continues_composition)
    {
        std::string rest_raw_input = raw_input_without_helpcodes.substr(consumed_raw_length);
        std::string rest_raw_input_with_cases = raw_input_with_cases_without_helpcodes.substr(consumed_raw_length);
        remove_consumed_leading_separators(rest_raw_input, rest_raw_input_with_cases);
        engine_.replace_active_raw_input(rest_raw_input, rest_raw_input_with_cases);
        online_requests_.invalidate();
        update_mixed_candidates();
        transition.current_segmentation = get_pinyin_segmentation();
        transition.current_segmentation_with_cases = get_pinyin_segmentation_with_cases();
        return transition;
    }

    transition.current_segmentation = current_segmentation;
    transition.current_segmentation_with_cases = current_segmentation_with_cases;
    return transition;
}

InputSession::CloudQueryState InputSession::get_cloud_query_state() const
{
    CloudQueryState state;

    if (is_japanese())
    {
        state.cache_key = request().raw_input;
        state.committed_pinyin = request().raw_input;
        state.should_query = !request().raw_input.empty();
        state.query_text = state.should_query ? request().raw_input : std::string{};
        return state;
    }

    if (is_wubi())
    {
        state.cache_key = request().normalized_input;
        state.committed_pinyin = request().normalized_input;
        return state;
    }

    if (is_shuangpin())
    {
        const auto base = ResolveShuangpinCompositionBase(request(), shuangpin_profile_);
        state.cache_key = ResolveShuangpinCloudCacheKey(request(), shuangpin_profile_);
        state.committed_pinyin = shuangpin::remove_manual_delimiters(state.cache_key);

        if (has_active_helpcode())
        {
            return state;
        }

        const char last =
            base.effective_raw_input_with_cases.empty() ? '\0' : base.effective_raw_input_with_cases.back();
        const bool ends_with_input_key = (last >= 'a' && last <= 'z') || last == ';';
        state.should_query =
            ends_with_input_key && shuangpin::is_complete_input(base.effective_raw_input, shuangpin_profile_);

        if (state.should_query)
        {
            state.query_text = shuangpin::normalize_input_with_delimiters(state.cache_key, shuangpin_profile_);
        }
        return state;
    }

    state.committed_pinyin = request().normalized_input;
    state.cache_key = ResolveQuanpinCloudCacheKey(request());

    if (has_active_helpcode())
    {
        return state;
    }

    state.should_query = !request().normalized_input.empty();
    state.query_text = request().normalized_input;
    return state;
}

InputSession::CreatingWordProgress InputSession::update_creating_word_progress(
    const std::string &current_pinyin, const std::string &current_word, const std::string &selected_word,
    const SelectionTransition &selection_transition) const
{
    CreatingWordProgress progress;
    if (wubi_candidates_are_native())
    {
        progress.pinyin = current_pinyin.empty() ? selection_transition.full_pure_pinyin : current_pinyin;
        progress.word = current_word + selected_word;
        progress.preedit = progress.word;
        progress.completed = true;
        progress.can_store = false;
        return progress;
    }

    const std::string selected_canonical =
        normalize_canonical_pinyin_for_word(selection_transition.selected_canonical_pinyin, selected_word);
    const bool prior_parts_are_storeable = current_word.empty() || !current_pinyin.empty();
    if (prior_parts_are_storeable && !selected_canonical.empty())
    {
        progress.pinyin = append_canonical_pinyin(current_pinyin, selected_canonical);
    }
    progress.word = current_word + selected_word;
    progress.preedit = progress.word + selection_transition.current_segmentation_with_cases;
    progress.completed = !selection_transition.continues_composition;
    progress.can_store =
        progress.completed && !normalize_canonical_pinyin_for_word(progress.pinyin, progress.word).empty();
    return progress;
}

bool InputSession::is_shuangpin() const
{
    return current_scheme_type() == SchemeType::Shuangpin;
}

bool InputSession::is_wubi() const
{
    return current_scheme_type() == SchemeType::Wubi;
}

bool InputSession::answered_by_pinyin_fallback() const
{
    return engine_.answered_by_pinyin_fallback();
}

bool InputSession::wubi_candidates_are_native() const
{
    return is_wubi() && !engine_.answered_by_pinyin_fallback();
}

bool InputSession::candidates_follow_pinyin() const
{
    return current_scheme_type() == SchemeType::Quanpin || current_scheme_type() == SchemeType::Shuangpin ||
           engine_.answered_by_pinyin_fallback();
}

bool InputSession::is_japanese() const
{
    return current_scheme_type() == SchemeType::JapaneseRomaji;
}

void InputSession::clear_pending_sequence()
{
    pending_pinyin_sequence_.clear();
    pending_pinyin_sequence_with_cases_.clear();
    has_pending_pinyin_sequence_ = false;
    has_pending_pinyin_sequence_with_cases_ = false;
}

void InputSession::apply_pending_sequence()
{
    caret_.reset();
    const std::string raw_input = has_pending_pinyin_sequence_ ? pending_pinyin_sequence_ : request().raw_input;
    const std::string raw_input_with_cases =
        has_pending_pinyin_sequence_with_cases_ ? pending_pinyin_sequence_with_cases_ : raw_input;

    switch (current_scheme_type())
    {
    case SchemeType::Shuangpin:
        engine_.replace_shuangpin_raw_input(raw_input, raw_input_with_cases);
        break;
    case SchemeType::Quanpin:
        engine_.replace_quanpin_raw_input(raw_input, raw_input_with_cases);
        break;
    case SchemeType::Wubi:
        engine_.replace_wubi_raw_input(raw_input, raw_input_with_cases);
        break;
    case SchemeType::JapaneseRomaji:
        engine_.replace_japanese_raw_input(raw_input, raw_input_with_cases);
        break;
    }
    clear_pending_sequence();
    online_requests_.invalidate();
    update_mixed_candidates();
}
} // namespace metasequoia
