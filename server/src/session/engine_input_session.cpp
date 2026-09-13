#include "engine_input_session.h"
#include "config/ime_config.h"
#include "engine/common/helpcode_utils.h"
#include "engine/quanpin/quanpin_utils.h"

EngineInputSession::EngineInputSession(SchemeType scheme, const ShuangpinProfile &profile)
    : paths_(metasequoia::RuntimePaths::legacy()), session_(scheme, profile, paths_)
{
    ApplyConfiguration();
}

void EngineInputSession::ApplyConfiguration()
{
    const auto scheme = session_.scheme();
    if (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin)
    {
        const auto &schema = scheme == SchemeType::Quanpin ? GetConfiguredQuanpinHelpcodeSchema()
                                                           : GetConfiguredShuangpinHelpcodeSchema();
        if (schema != helpcode_schema_)
        {
            // Keep filtering and annotations on this session's captured resource layout.
            // Applying unchanged settings on each key must not reload the tables.
            auto keymap = HelpcodeUtils::load_helpcode_keymap(paths_.resources, schema);
            if (session_.set_helpcode_schema(schema))
            {
                helpcode_schema_ = schema;
                helpcode_keymap_ = std::move(keymap);
            }
        }
    }
    session_.set_shuangpin_helpcode_enabled(GetConfiguredShuangpinHelpcodeEnabled());
    session_.set_quanpin_helpcode_enabled(GetConfiguredQuanpinHelpcodeEnabled());
    const unsigned autocorrect_types =
        (GetConfiguredQuanpinAutocorrectTransposition() ? quanpin::kAutocorrectTransposition : 0u) |
        (GetConfiguredQuanpinAutocorrectNeighbor() ? quanpin::kAutocorrectNeighbor : 0u);
    session_.set_quanpin_autocorrect_types(autocorrect_types);
    // Fuzzy pinyin applies to both quanpin and shuangpin; the engine fuzzes on the
    // converted quanpin syllables for shuangpin. Re-read on every key so setting
    // changes take effect immediately.
    session_.set_fuzzy_pinyin_options(GetConfiguredFuzzyPinyinOptions());
    session_.set_shuangpin_preedit_uses_raw(GetConfiguredShuangpinPreeditMode() == "shuangpin");
}

void EngineInputSession::handle_key(UINT vk, UINT modifiers_down, WCHAR wch)
{
    ApplyConfiguration();
    return session_.handle_engine_key(vk, modifiers_down, wch);
}

void EngineInputSession::recompute_candidates()
{
    ApplyConfiguration();
    return session_.recompute_candidates();
}

SchemeType EngineInputSession::current_scheme_type() const
{
    return session_.current_scheme_type();
}

void EngineInputSession::switch_scheme(SchemeType scheme_type)
{
    session_.switch_scheme(scheme_type);
    ApplyConfiguration();
}

void EngineInputSession::reset_state()
{
    return session_.reset_state();
}

void EngineInputSession::reset_cache()
{
    return session_.reset_cache();
}

const std::vector<IInputSession::WordItem> &EngineInputSession::get_candidates() const
{
    return session_.get_candidates();
}

bool EngineInputSession::expand_initial_candidates()
{
    return session_.expand_initial_candidates();
}

std::optional<WordItem> EngineInputSession::find_candidate(const std::string &key, const std::string &value)
{
    return session_.find_candidate(key, value);
}

const std::string &EngineInputSession::get_pinyin_sequence() const
{
    return session_.get_pinyin_sequence();
}

const std::string &EngineInputSession::get_pinyin_sequence_with_cases() const
{
    return session_.get_pinyin_sequence_with_cases();
}

const std::string &EngineInputSession::get_pure_pinyin_sequence() const
{
    return session_.get_pure_pinyin_sequence();
}

const std::string &EngineInputSession::get_pinyin_segmentation() const
{
    return session_.get_pinyin_segmentation();
}

std::string EngineInputSession::get_pinyin_segmentation_with_cases() const
{
    return session_.get_pinyin_segmentation_with_cases();
}

std::string EngineInputSession::get_quanpin() const
{
    return session_.get_quanpin();
}

bool EngineInputSession::is_all_complete_pure_pinyin() const
{
    return session_.is_all_complete_pure_pinyin();
}

bool EngineInputSession::has_active_helpcode() const
{
    return session_.has_active_helpcode();
}

void EngineInputSession::set_pinyin_sequence(const std::string &pinyin_sequence)
{
    return session_.set_pinyin_sequence(pinyin_sequence);
}

void EngineInputSession::set_pinyin_sequence_with_cases(const std::string &pinyin_sequence)
{
    return session_.set_pinyin_sequence_with_cases(pinyin_sequence);
}

int EngineInputSession::store_user_phrase(std::string pinyin, std::string word)
{
    return session_.store_user_phrase(pinyin, word);
}

int EngineInputSession::store_user_phrase_from_canonical_pinyin(std::string pinyin, std::string word)
{
    return session_.store_user_phrase_from_canonical_pinyin(pinyin, word);
}

int EngineInputSession::pin_candidate(std::string pinyin, std::string word)
{
    return session_.pin_candidate(pinyin, word);
}

int EngineInputSession::remove_candidate(std::string pinyin, std::string word)
{
    return session_.remove_candidate(pinyin, word);
}

int EngineInputSession::cache_dynamic_candidate(const std::string &pinyin, const std::string &word,
                                                CandidateSource source)
{
    return session_.cache_dynamic_candidate(pinyin, word, source);
}

IInputSession::SelectionTransition EngineInputSession::advance_composition_after_selection(
    const std::string &selected_pinyin, const std::string &selected_word, const std::string &selected_canonical_pinyin)
{
    return session_.advance_composition_after_selection(selected_pinyin, selected_word, selected_canonical_pinyin);
}

IInputSession::CloudQueryState EngineInputSession::get_cloud_query_state() const
{
    return session_.get_cloud_query_state();
}

std::optional<metasequoia::OnlineQuery> EngineInputSession::online_query() const
{
    return session_.online_query();
}

bool EngineInputSession::apply_online_candidate(const metasequoia::OnlineQuery &query, std::string candidate,
                                                CandidateSource source)
{
    return session_.apply_online_candidate(query, std::move(candidate), source);
}

IInputSession::CreatingWordProgress EngineInputSession::update_creating_word_progress(
    const std::string &current_pinyin, const std::string &current_word, const std::string &selected_word,
    const SelectionTransition &selection_transition) const
{
    return session_.update_creating_word_progress(current_pinyin, current_word, selected_word, selection_transition);
}

std::string EngineInputSession::get_helpcode_annotation(const std::string &word, bool uppercase_all) const
{
    const auto scheme = session_.scheme();
    if (!helpcode_keymap_ || (scheme != SchemeType::Quanpin && scheme != SchemeType::Shuangpin))
        return {};
    return HelpcodeUtils::compute_helpcodes(word, uppercase_all, helpcode_keymap_.get());
}
