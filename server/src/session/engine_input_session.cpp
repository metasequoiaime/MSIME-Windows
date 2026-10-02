#include "engine_input_session.h"
#include "config/ime_config.h"
#include "engine/common/helpcode_utils.h"
#include "engine/core/sentence_association_options.h"
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
    session_.set_mid_sentence_helpcode_enabled(GetConfiguredShuangpinMidSentenceHelpcodeEnabled());
    session_.set_quanpin_helpcode_enabled(GetConfiguredQuanpinHelpcodeEnabled());
    const unsigned autocorrect_types =
        (GetConfiguredQuanpinAutocorrectTransposition() ? quanpin::kAutocorrectTransposition : 0u) |
        (GetConfiguredQuanpinAutocorrectNeighbor() ? quanpin::kAutocorrectNeighbor : 0u);
    session_.set_quanpin_autocorrect_types(autocorrect_types);
    // Fuzzy pinyin applies to both quanpin and shuangpin; the engine fuzzes on the
    // converted quanpin syllables for shuangpin. Re-read on every key so setting
    // changes take effect immediately.
    session_.set_fuzzy_pinyin_options(GetConfiguredFuzzyPinyinOptions());
    // 整句候选来源与去重补位选项，每次击键重读，改设置立即生效。
    SentenceAssociationOptions association;
    association.word_lattice = GetConfiguredAssocSentenceWordLattice();
    association.google = GetConfiguredAssocSentenceGoogle();
    association.neural_desktop = GetConfiguredAssocSentenceNeuralDesktop();
    association.neural_keyboard = GetConfiguredAssocSentenceNeuralKeyboard();
    association.show_next_on_duplicate = GetConfiguredAssocSentenceShowNextOnDuplicate();
    session_.set_sentence_association(association);
    session_.set_shuangpin_preedit_uses_raw(GetConfiguredShuangpinPreeditMode() == "shuangpin");
    // 五笔拼音混输与 z 键角色是两个独立设置：混输是「同时给五笔和拼音候选」，z 键角色只管
    // z 是普通字母还是通配。每次击键重读，设置页改动对下一键立即生效。
    metasequoia::WubiInputOptions wubi_options;
    wubi_options.mixed_pinyin = GetConfiguredWubiMixedPinyin();
    wubi_options.z_wildcard = GetConfiguredWubiZMode() == "wildcard";
    session_.set_wubi_input_options(wubi_options);
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

void EngineInputSession::reset_sentence_cache()
{
    return session_.reset_sentence_cache();
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

std::vector<std::size_t> EngineInputSession::segment_raw_boundaries() const
{
    return session_.segment_raw_boundaries();
}

std::string EngineInputSession::get_quanpin() const
{
    return session_.get_quanpin();
}

bool EngineInputSession::is_all_complete_pure_pinyin() const
{
    return session_.is_all_complete_pure_pinyin();
}

bool EngineInputSession::reads_as_pinyin() const
{
    return session_.reads_as_pinyin();
}

bool EngineInputSession::wubi_unique_four_code() const
{
    return session_.wubi_unique_four_code();
}

bool EngineInputSession::wubi_four_code_is_complete() const
{
    return session_.wubi_four_code_is_complete();
}

bool EngineInputSession::has_active_helpcode() const
{
    return session_.has_active_helpcode();
}

bool EngineInputSession::accepts_mid_sentence_helpcode_marker(std::size_t caret) const
{
    // 开关在 ApplyConfiguration 里随每键重读；这里在吃键之前被问到，那一刻配置可能刚改过，
    // 所以直接按当前配置判断，不等下一次重读。
    return GetConfiguredShuangpinMidSentenceHelpcodeEnabled() &&
           session_.accepts_mid_sentence_helpcode_marker_at(caret);
}

bool EngineInputSession::has_mid_sentence_helpcode() const
{
    return session_.has_mid_sentence_helpcode();
}

std::vector<IInputSession::WordItem> EngineInputSession::candidates_without_mid_sentence_helpcode()
{
    return session_.candidates_without_mid_sentence_helpcode();
}

void EngineInputSession::set_rescoring_context(std::string context)
{
    return session_.set_rescoring_context(std::move(context));
}

void EngineInputSession::set_pinyin_sequence(const std::string &pinyin_sequence)
{
    return session_.set_pinyin_sequence(pinyin_sequence);
}

void EngineInputSession::set_pinyin_sequence_with_cases(const std::string &pinyin_sequence)
{
    return session_.set_pinyin_sequence_with_cases(pinyin_sequence);
}

void EngineInputSession::set_caret(std::optional<std::size_t> caret)
{
    return session_.set_caret(caret);
}

std::size_t EngineInputSession::prefix_end() const
{
    return session_.prefix_end();
}

std::string EngineInputSession::pending_suffix() const
{
    return session_.pending_suffix();
}

int EngineInputSession::store_user_phrase(std::string pinyin, std::string word)
{
    return session_.store_user_phrase(pinyin, word);
}

int EngineInputSession::store_user_phrase_from_canonical_pinyin(std::string pinyin, std::string word)
{
    return session_.store_user_phrase_from_canonical_pinyin(pinyin, word);
}

int EngineInputSession::remove_candidate(std::string pinyin, std::string word, SchemeType scheme)
{
    return session_.remove_candidate(pinyin, word, scheme);
}

int EngineInputSession::cache_dynamic_candidate(const std::string &pinyin, const std::string &word,
                                                CandidateSource source)
{
    return session_.cache_dynamic_candidate(pinyin, word, source);
}

IInputSession::SelectionTransition EngineInputSession::advance_composition_after_selection(
    const std::string &selected_pinyin, const std::string &selected_word, const std::string &selected_canonical_pinyin,
    SchemeType selected_scheme)
{
    return session_.advance_composition_after_selection(selected_pinyin, selected_word, selected_canonical_pinyin,
                                                        selected_scheme);
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
