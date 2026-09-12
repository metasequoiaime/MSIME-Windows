#include "engine.h"
#include "../common/helpcode_utils.h"
#include "quanpin_query.h"
#include "quanpin_utils.h"

namespace
{ // The request carries one bool per correction type; the dictionary layer gates
// the whole feature with a single mask, so bridge the two here. Design D2: any
// correction switch on also enables the deletion bit -- the user's intent to
// have typos fixed covers dropped letters -- while both off stays 0. The same
// linkage carries the insertion bit (design D4 of the insertion task).
unsigned autocorrect_types_from_request(const QueryRequest &request)
{
    const unsigned legacy =
        (request.enable_quanpin_autocorrect_transposition ? quanpin::kAutocorrectTransposition : 0u) |
        (request.enable_quanpin_autocorrect_neighbor ? quanpin::kAutocorrectNeighbor : 0u);
    return legacy == 0 ? 0u : legacy | quanpin::kAutocorrectDeletion | quanpin::kAutocorrectInsertion;
}
} // namespace

QuanpinEngine::QuanpinEngine(metasequoia::RuntimePaths paths)
    : dictionary_({}, paths),
      helpcodes_(HelpcodeUtils::load_helpcode_keymap(paths.resources, HelpcodeUtils::selected_helpcode_schema()))
{
}

QuanpinEngine::~QuanpinEngine() = default;

std::optional<WordItem> QuanpinEngine::find_candidate(const std::string &key, const std::string &value)
{
    return dictionary_.find_candidate(key, value);
}

std::vector<WordItem> QuanpinEngine::query(const QueryRequest &request)
{
    if (!request.valid)
    {
        return {};
    }

    const size_t helpcode_length =
        request.enable_quanpin_helpcode
            ? quanpin::detect_active_helpcode_length(request.raw_input, request.raw_input_with_cases)
            : 0;

    if (helpcode_length == 2)
    {
        const std::string base_raw_input =
            quanpin::strip_active_helpcodes(request.raw_input, request.raw_input_with_cases);
        const auto cuts = quanpin::cut_pinyin_by_mode(base_raw_input, "correction");
        const std::string base_segmentation = quanpin::join_segments(cuts.front());
        const std::string help_codes = request.raw_input.substr(request.raw_input.size() - 2, 2);
        const auto base_candidates = dictionary_.query(base_raw_input, base_segmentation,
                                                       autocorrect_types_from_request(request), request.fuzzy_pinyin);
        return HelpcodeUtils::filter_candidates_with_double_helpcodes(base_candidates, help_codes, helpcodes_.get());
    }

    if (helpcode_length == 1)
    {
        const std::string base_raw_input =
            quanpin::strip_active_helpcodes(request.raw_input, request.raw_input_with_cases);
        const auto cuts = quanpin::cut_pinyin_by_mode(base_raw_input, "correction");
        const std::string base_segmentation = quanpin::join_segments(cuts.front());
        const std::string help_code = request.raw_input.substr(request.raw_input.size() - 1, 1);
        const auto base_candidates = dictionary_.query(base_raw_input, base_segmentation,
                                                       autocorrect_types_from_request(request), request.fuzzy_pinyin);
        return HelpcodeUtils::reorder_candidates_with_single_helpcode(base_candidates, help_code, helpcodes_.get());
    }

    return dictionary_.query(request.raw_input, request.segmentation, autocorrect_types_from_request(request),
                             request.fuzzy_pinyin);
}

bool QuanpinEngine::expand_initial_candidates(const QueryRequest &request, std::vector<WordItem> &candidates)
{
    if (request.scheme != SchemeType::Quanpin ||
        (request.enable_quanpin_helpcode &&
         quanpin::detect_active_helpcode_length(request.raw_input, request.raw_input_with_cases) > 0))
    {
        return false;
    }

    const auto segments = quanpin::split_segments(request.segmentation);
    if (segments.empty() || segments.front().size() != 1)
    {
        return false;
    }
    return dictionary_.expand_initial_candidates(segments.front(), candidates);
}

int QuanpinEngine::handleVkCode(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    return dictionary_.handleVkCode(vk, modifiers_down, wch);
}

int QuanpinEngine::create_word(std::string pinyin, std::string word)
{
    return dictionary_.create_word(std::move(pinyin), std::move(word));
}

int QuanpinEngine::create_word_from_canonical_pinyin(std::string pinyin, std::string word)
{
    return dictionary_.create_word_from_canonical_pinyin(std::move(pinyin), std::move(word));
}

int QuanpinEngine::update_weight_by_word(std::string word)
{
    return dictionary_.update_weight_by_word(std::move(word));
}

int QuanpinEngine::update_weight_by_pinyin_and_word(std::string pinyin, std::string word)
{
    return dictionary_.update_weight_by_pinyin_and_word(std::move(pinyin), std::move(word));
}

int QuanpinEngine::delete_by_pinyin_and_word(std::string pinyin, std::string word)
{
    return dictionary_.delete_by_pinyin_and_word(std::move(pinyin), std::move(word));
}

int QuanpinEngine::insert_word_to_series_cache(const std::string &pinyin, const std::string &word,
                                               CandidateSource source)
{
    return dictionary_.insert_word_to_series_cache(pinyin, word, source);
}

int QuanpinEngine::insert_word_to_series_cache(const QueryRequest &request, const std::string &word,
                                               CandidateSource source)
{
    return dictionary_.insert_word_to_series_cache(request.raw_input, request.segmentation,
                                                   autocorrect_types_from_request(request), word, source);
}

std::string QuanpinEngine::search_sentence_from_ime_engine(const std::string &user_pinyin)
{
    return dictionary_.search_sentence_from_ime_engine(user_pinyin);
}

void QuanpinEngine::reset_state()
{
    dictionary_.reset_state();
}

void QuanpinEngine::reset_cache()
{
    dictionary_.reset_cache();
}
