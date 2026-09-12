#include "candidate_queries.h"
#include "data_path.h"
#include "../contracts/assets/assets.h"
#include "../local_modes/emoji_query.h"
#include "../local_modes/jianpin_query.h"
#include "../local_modes/kaomoji_query.h"
#include "../local_modes/number_query.h"
#include "../local_modes/quick_phrase_query.h"
#include "../local_modes/unicode_query.h"
#include <algorithm>
#include <cctype>
#include <iterator>
#include <unordered_set>
namespace metasequoia
{
local_modes::LocalQueryResult CandidateQueries::local(LocalInputMode mode, const std::string &preedit,
                                                      SchemeType scheme,
                                                      const std::function<local_modes::LocalDateTime()> &clock,
                                                      const std::vector<WordItem> &engine_candidates)
{
    local_modes::LocalQueryResult result;

    switch (mode)
    {
    case LocalInputMode::Unicode:
        result.candidates = local_modes::query_unicode(preedit.substr(1));
        return result;
    case LocalInputMode::Number:
        result.candidates = local_modes::query_number(preedit.substr(1));
        return result;
    case LocalInputMode::DateTime: {
        const local_modes::LocalDateTime now = clock ? clock() : local_modes::current_local_date_time();
        result.candidates = local_modes::query_date_time(preedit.substr(1), &now);
        return result;
    }
    case LocalInputMode::QuickPhrase: {
        local_modes::QuickPhraseQueryResult query =
            local_modes::query_quick_phrases(preedit.substr(1), paths_.dictionary(assets::main_dictionary));
        result.candidates = std::move(query.candidates);
        result.diagnostic = std::move(query.diagnostic);
        return result;
    }
    case LocalInputMode::Emoji: {
        local_modes::LocalQueryResult query = local_modes::query_emoji(
            preedit.substr(1), scheme, paths_.resource(assets::other_dictionary), 10, shuangpin_profile_);
        result.candidates = std::move(query.candidates);
        result.diagnostic = std::move(query.diagnostic);
        return result;
    }
    case LocalInputMode::Kaomoji: {
        local_modes::LocalQueryResult query = local_modes::query_kaomoji(
            preedit.substr(1), scheme, paths_.resource(assets::other_dictionary), 10, shuangpin_profile_);
        result.candidates = std::move(query.candidates);
        result.diagnostic = std::move(query.diagnostic);
        return result;
    }
    case LocalInputMode::SuperJianpin: {
        const std::string code = preedit.substr(1);
        const int limit = code.size() == 1 ? 24 : 100;
        local_modes::LocalQueryResult query = local_modes::query_jianpin(
            code, scheme, paths_.dictionary(assets::main_dictionary), limit, shuangpin_profile_);
        result.candidates = std::move(query.candidates);
        result.diagnostic = std::move(query.diagnostic);
        return result;
    }
    case LocalInputMode::TemporaryEnglish: {
        const std::string raw = preedit.substr(1);
        result.candidates.clear();
        if (raw.empty())
        {
            return result;
        }
        result.candidates.emplace_back("", raw, 0, CandidateSource::Generated);
        std::string prefix = raw;
        std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        auto completions = english_dictionary().query_prefix(prefix, 1000);
        completions.erase(std::remove_if(completions.begin(), completions.end(),
                                         [&](const WordItem &candidate) {
                                             if (candidate.word.size() != raw.size())
                                             {
                                                 return false;
                                             }
                                             return std::equal(candidate.word.begin(), candidate.word.end(),
                                                               raw.begin(),
                                                               [](unsigned char left, unsigned char right) {
                                                                   return std::tolower(left) == std::tolower(right);
                                                               });
                                         }),
                          completions.end());
        result.candidates.insert(result.candidates.end(), std::make_move_iterator(completions.begin()),
                                 std::make_move_iterator(completions.end()));
        return result;
    }
    case LocalInputMode::TemporaryJapanese:
        result.candidates = engine_candidates;
        return result;
    case LocalInputMode::None:
        result.candidates.clear();
        return result;
    }
    return result;
}

std::vector<WordItem> CandidateQueries::mixed(std::vector<WordItem> candidates, const std::string &prefix,
                                              SchemeType scheme, EnglishInputOptions english_options,
                                              MixedExpressiveOptions expressive_options, bool dedicated_english,
                                              LocalInputMode local_mode)
{

    if ((!english_options.mixed_candidates && !expressive_options.emoji_candidates &&
         !expressive_options.kaomoji_candidates) ||
        dedicated_english || local_mode != LocalInputMode::None ||
        (scheme != SchemeType::Quanpin && scheme != SchemeType::Shuangpin))
    {
        return candidates;
    }

    if (prefix.empty())
    {
        return candidates;
    }

    std::unordered_set<std::string> seen;
    for (const auto &candidate : candidates)
    {
        seen.insert(candidate.word);
    }

    const auto collect_unique = [&](std::vector<WordItem> candidates) {
        std::vector<WordItem> unique;
        unique.reserve(candidates.size());
        for (auto &candidate : candidates)
        {
            if (seen.insert(candidate.word).second)
            {
                unique.push_back(std::move(candidate));
            }
        }
        return unique;
    };

    std::vector<WordItem> english_candidates;
    const bool lower_ascii_prefix = std::all_of(
        prefix.begin(), prefix.end(), [](unsigned char character) { return character >= 'a' && character <= 'z'; });
    if (english_options.mixed_candidates && prefix.size() >= english_options.minimum_prefix && lower_ascii_prefix)
    {
        english_candidates = collect_unique(english_dictionary().query_prefix(prefix, 5));
    }

    std::vector<WordItem> emoji_candidates;
    if (expressive_options.emoji_candidates && prefix.size() >= 2)
    {
        emoji_candidates = collect_unique(
            local_modes::query_emoji(prefix, scheme, paths_.resource(assets::other_dictionary), 3, shuangpin_profile_)
                .candidates);
    }

    std::vector<WordItem> kaomoji_candidates;
    if (expressive_options.kaomoji_candidates && prefix.size() >= 2)
    {
        kaomoji_candidates = collect_unique(
            local_modes::query_kaomoji(prefix, scheme, paths_.resource(assets::other_dictionary), 3, shuangpin_profile_)
                .candidates);
    }

    std::size_t priority_slot = std::min<std::size_t>(1, candidates.size());
    const auto has_source = [&](CandidateSource source) {
        return std::any_of(candidates.begin(), candidates.end(),
                           [source](const WordItem &candidate) { return candidate.source == source; });
    };
    if (has_source(CandidateSource::CloudSuggestion))
    {
        priority_slot = std::min<std::size_t>(2, candidates.size());
    }
    if (has_source(CandidateSource::AiSuggestion))
    {
        priority_slot = std::min<std::size_t>(3, candidates.size());
    }
    const auto insert_leading = [&](std::vector<WordItem> &source) {
        if (source.empty())
        {
            return;
        }
        candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(priority_slot), std::move(source.front()));
        source.erase(source.begin());
        ++priority_slot;
    };
    insert_leading(english_candidates);
    insert_leading(emoji_candidates);
    insert_leading(kaomoji_candidates);

    for (auto *source : {&english_candidates, &emoji_candidates, &kaomoji_candidates})
    {
        candidates.insert(candidates.end(), std::make_move_iterator(source->begin()),
                          std::make_move_iterator(source->end()));
    }
    return candidates;
}

EnglishDictionary &CandidateQueries::english_dictionary()
{
    if (!english_dictionary_)
    {
        english_dictionary_ =
            std::make_unique<EnglishDictionary>(path_to_utf8(paths_.dictionary(assets::english_dictionary)), false,
                                                path_to_utf8(paths_.resource(assets::translations)));
    }
    return *english_dictionary_;
}

} // namespace metasequoia
