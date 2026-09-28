#pragma once

#include "ime_session.h"
#include "input_session_types.h"
#include "candidate_queries.h"
#include "punctuation_policy.h"
#include "online_request_guard.h"
#include "../local_modes/date_time_query.h"
#include "../english/english_dictionary.h"
#include "word_item.h"
#include "../quanpin/engine.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace metasequoia
{
// Platform-neutral composition session shared by the native frontends. It owns an ImeSession and
// applies the key-handling and commit policy that each frontend would otherwise reimplement, so a
// frontend only has to translate platform key events into these calls.
class InputSession
{
  public:
    // Frontends pass their persisted options at session creation so every platform uses the same
    // engine configuration and commit policy.
    explicit InputSession(SchemeType scheme_type = SchemeType::Quanpin, unsigned quanpin_autocorrect_types = 0,
                          bool helpcode_enabled = true, bool chinese_punctuation_enabled = true,
                          bool candidate_learning_enabled = true, RuntimePaths paths = RuntimePaths::legacy());
    InputSession(SchemeType scheme_type, const ShuangpinProfile &shuangpin_profile,
                 RuntimePaths paths = RuntimePaths::legacy());

    // Feeds one lowercase ASCII letter or an in-composition apostrophe. Other input is rejected as
    // unhandled so the frontend can pass it through to the client application.
    KeyResult handle_character(char character, bool shift_only = false);
    // Applies a command. Every command is unhandled while no composition is active, which keeps
    // Backspace and Escape working normally in the client application.
    KeyResult handle_command(Command command);
    // Maps the visible 1-9 candidate keys and Chinese punctuation independently of platform UI.
    KeyResult handle_candidate_key(char character);
    KeyResult handle_punctuation(char character);
    // Commit the selected prefix and retain any unconsumed pinyin. Hosts insert
    // KeyResult::commit and then render the remaining preedit from this session.
    KeyResult select_candidate(std::size_t index);
    // Flush all remaining input for punctuation, scheme changes and host passthrough.
    KeyResult finish_composition(std::size_t first_index = 0);
    KeyResult select_candidate(const std::string &candidate);
    KeyResult select_candidate_edge(std::size_t index, CandidateEdge edge);
    KeyResult pin_candidate(std::size_t index);
    KeyResult remove_candidate(std::size_t index);
    KeyResult set_candidate_position(std::size_t index, int position);
    void enable_fixed_positions();
    void set_shuangpin_helpcode_enabled(bool enabled);
    void set_quanpin_helpcode_enabled(bool enabled);
    static bool is_supported_helpcode_schema(const std::string &schema);
    bool set_helpcode_schema(const std::string &schema);
    // Compatibility default for subsequently created sessions.
    static bool select_helpcode_schema(const std::string &schema);
    bool set_frequency_adjustment(FrequencyAdjustmentOptions options);
    const FrequencyAdjustmentOptions &frequency_adjustment() const;
    void set_local_mode_options(LocalModeOptions options);
    const LocalModeOptions &local_mode_options() const;
    bool set_english_input_options(EnglishInputOptions options);
    const EnglishInputOptions &english_input_options() const;
    void set_mixed_expressive_options(MixedExpressiveOptions options);
    void set_wubi_input_options(metasequoia::WubiInputOptions options);
    const MixedExpressiveOptions &mixed_expressive_options() const;
    void set_dedicated_english_mode(bool enabled);
    bool dedicated_english_mode() const;
    LocalInputMode local_input_mode() const;
    void set_local_date_time_provider(std::function<local_modes::LocalDateTime()> provider);
    std::optional<OnlineQuery> online_query() const;
    bool apply_online_candidate(const OnlineQuery &query, std::string candidate, CandidateSource source);

    SchemeType scheme_type() const;
    unsigned quanpin_autocorrect_types() const;
    bool helpcode_enabled() const;
    bool chinese_punctuation_enabled() const;
    bool candidate_learning_enabled() const;
    // Switching schemes discards the current composition. A frontend that promises to preserve
    // typed text must commit it before calling this method.
    void switch_scheme(SchemeType scheme_type);
    // Japanese-only: cycle the top candidate kana form. to_katakana true forces Katakana,
    // false forces Hiragana. Only acts while a Japanese composition is active; otherwise
    // unhandled so the key passes through to the host.
    KeyResult cycle_japanese_kana_form(bool to_katakana);
    SchemeType scheme() const;

    bool has_composition() const;
    const std::string &preedit() const;
    std::string editing_text() const;
    std::size_t caret_position() const;
    const std::string &raw_segmentation() const;
    const std::string &normalized_segmentation() const;
    const std::vector<WordItem> &candidates() const;
    // True while the current composition is answered by the wubi mixed-pinyin fallback.
    bool answered_by_pinyin_fallback() const;

    // Advanced composition operations for hosts with their own asynchronous text insertion.
    // They share the same engine/configuration as the portable character/command API.
    struct SelectionTransition
    {
        bool continues_composition = false;
        std::string full_pure_pinyin;
        std::string current_segmentation;
        std::string current_segmentation_with_cases;
        std::string selected_canonical_pinyin;
        // Raw spelling the selection consumed from the active scheme's input,
        // in the exact form the user typed it. A host that lets the user retract
        // a selected segment must replay this spelling, not the pre-selection
        // raw: the pre-selection raw also contains suffix characters the user
        // may have deleted since the selection. Sources that consume no input
        // (cloud, associative, whole-word commits) leave it empty.
        std::string consumed_raw_input_with_cases;
    };

    struct CloudQueryState
    {
        bool should_query = false;
        std::string query_text;
        std::string cache_key;
        std::string committed_pinyin;
    };

    struct CreatingWordProgress
    {
        std::string pinyin;
        std::string word;
        std::string preedit;
        bool completed = false;
        bool can_store = false;
    };

    void handle_engine_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch);
    void recompute_candidates();
    // Moves the caret without editing the raw string. nullopt = end of string (full-string
    // decoding, the default). The value is clamped to [0, editing_text().size()]. Setting it
    // only updates state; candidates re-decode by the new boundary on the next
    // recompute_candidates() or key handling.
    void set_caret(std::optional<std::size_t> caret);
    // Raw length consumed by the current decode: the caret moved onto the last complete
    // syllable-unit boundary at or before it (floor). The caret being unset, or a scheme
    // without the unit model (segment_raw_boundaries() empty), decodes the whole string and
    // this equals the raw length.
    std::size_t prefix_end() const;
    // raw[prefix_end, size) with its original casing: the pending input this decode did not
    // consume. Empty unless the caret prefix is strictly shorter than the raw string.
    std::string pending_suffix() const;
    SchemeType current_scheme_type() const;

    void reset_state();
    void reset_cache();

    const std::vector<WordItem> &get_candidates() const;
    bool expand_initial_candidates();
    std::optional<WordItem> find_candidate(const std::string &key, const std::string &value);

    const std::string &get_pinyin_sequence() const;
    const std::string &get_pinyin_sequence_with_cases() const;
    const std::string &get_pure_pinyin_sequence() const;
    const std::string &get_pinyin_segmentation() const;
    std::string get_pinyin_segmentation_with_cases() const;
    // Offsets in get_pinyin_sequence_with_cases() where one input unit starts,
    // always including 0 (when non-empty) and raw.size(). A unit is one syllable:
    // the `ma` of ni'hao'ma, one 1-2 key syllable in shuangpin. Schemes and
    // local modes without the unit model (wubi, japanese, U/K/E/M/J/Y/R and the
    // dedicated English scheme) return an empty vector, and hosts then fall back
    // to single-character editing. An active autocorrect/helpcode display is
    // mapped back to the raw spans the engine actually cut, so a deleted unit
    // can never leave half a segment or a stray separator behind. Consumed by
    // segment deletion (Ctrl+Backspace), so any later caret-movement feature
    // must use this same boundary set instead of re-deriving one.
    std::vector<std::size_t> segment_raw_boundaries() const;
    std::string get_quanpin() const;
    bool is_all_complete_pure_pinyin() const;
    // The current composition is a complete four-letter wubi code answered by the wubi table with
    // exactly one candidate. Hosts decide whether to auto-commit on this; the engine only reports
    // the fact. A four-letter spelling answered by the pinyin fallback is deliberately not one:
    // session.h's answered_by_pinyin_fallback comment explains that a code the table did not answer
    // is not a unique wubi code, and committing it would take away the fifth letter mixed input
    // exists to allow.
    bool wubi_unique_four_code() const;
    // The current composition is a complete four-letter wubi code the wubi table answered (not a
    // pinyin fallback), regardless of how many candidates it has. Hosts use it to commit the first
    // candidate when the user types past the fourth letter: a complete code that keeps growing must
    // not silently swallow the extra letters. See wubi_unique_four_code for the uniqueness part.
    bool wubi_four_code_is_complete() const;
    bool has_active_helpcode() const;

    void set_pinyin_sequence(const std::string &pinyin_sequence);
    void set_pinyin_sequence_with_cases(const std::string &pinyin_sequence);

    int store_user_phrase(std::string pinyin, std::string word);
    int store_user_phrase_from_canonical_pinyin(std::string pinyin, std::string word);
    int pin_candidate(std::string pinyin, std::string word);
    int remove_candidate(std::string pinyin, std::string word);
    int cache_dynamic_candidate(const std::string &pinyin, const std::string &word, CandidateSource source);
    SelectionTransition advance_composition_after_selection(const std::string &selected_pinyin,
                                                            const std::string &selected_word,
                                                            const std::string &selected_canonical_pinyin);
    CloudQueryState get_cloud_query_state() const;
    CreatingWordProgress update_creating_word_progress(const std::string &current_pinyin,
                                                       const std::string &current_word,
                                                       const std::string &selected_word,
                                                       const SelectionTransition &selection_transition) const;

    void set_quanpin_autocorrect_types(unsigned autocorrect_types);
    void set_fuzzy_pinyin_options(metasequoia::FuzzyPinyinOptions options)
    {
        engine_.set_fuzzy_pinyin_options(options);
    }
    void set_sentence_association(const SentenceAssociationOptions &options)
    {
        engine_.set_sentence_association(options);
    }
    void set_rescoring_context(std::string context)
    {
        engine_.set_rescoring_context(std::move(context));
    }
    void set_chinese_punctuation_enabled(bool enabled)
    {
        chinese_punctuation_enabled_ = enabled;
    }
    void set_candidate_learning_enabled(bool enabled)
    {
        candidate_learning_enabled_ = enabled;
    }
    void set_shuangpin_preedit_uses_raw(bool enabled)
    {
        shuangpin_preedit_uses_raw_ = enabled;
    }

  private:
    const QueryRequest &request() const;
    bool is_shuangpin() const;
    bool is_wubi() const;
    // Wubi whose candidates came from the wubi table. A code answered by the quanpin
    // fallback carries pinyin words, so ranking, fixed positions and removal have to key
    // off the pinyin rather than off the code that produced them.
    bool wubi_candidates_are_native() const;
    // The candidates on offer behave like pinyin: quanpin, shuangpin, or a wubi code the
    // table could not answer. Committing one of these commits a spelling out of a longer
    // one, so the rest of the composition has to survive the selection.
    bool candidates_follow_pinyin() const;
    bool is_japanese() const;
    void clear_pending_sequence();
    void apply_pending_sequence();

    KeyResult commit(std::size_t index);
    KeyResult handle_local_character(char character);
    KeyResult insert_at_caret(char character);
    KeyResult edit_at_caret(Command command);
    KeyResult replace_editing_text(std::string text, std::size_t caret);
    std::optional<std::size_t> caret_;
    // Last complete unit boundary at or before the caret; only consumes segment_raw_boundaries()
    // (segmentation contract #187). caret unset or no unit model yields the full raw length.
    std::size_t quantized_prefix_end() const;
    // Decodes the quantized caret prefix into prefix_candidates_ when it is strictly shorter
    // than the raw string, caching by prefix so unchanged keystrokes skip the extra query.
    void refresh_prefix_candidates();
    std::vector<WordItem> prefix_candidates_;
    // Lowercased prefix the cache was built from; also feeds mixed-candidate association so
    // English/emoji suggestions follow the string being converted.
    std::string prefix_query_input_;
    // True while candidates()/mixed assembly must read prefix_candidates_ instead of engine_.
    bool prefix_candidates_active_ = false;
    std::optional<std::string> update_local_candidates();
    void update_mixed_candidates();
    void apply_candidate_positions(std::vector<WordItem> &items);
    std::string position_context(bool english) const;
    bool fixed_positions_enabled_ = false;
    void update_dedicated_english_candidates();
    void reset_composition();
    void discard_abandoned_phrase_progress();
    std::optional<std::string> learn_candidate(std::size_t index);
    // 词格 / Google 解码器猜出来的整句在词库里没有对应行，选中后落成一条用户词组。
    std::optional<std::string> learn_sentence_candidate(const WordItem &selected);
    std::optional<std::string> adjust_candidate_frequency(std::size_t index, FrequencyAdjustmentOptions options,
                                                          bool force_top);

    CreatingWordProgress immediate_phrase_progress_;
    bool shuangpin_preedit_uses_raw_ = true;
    std::unique_ptr<QuanpinEngine> canonical_phrase_engine_;
    std::string pending_pinyin_sequence_;
    std::string pending_pinyin_sequence_with_cases_;
    bool has_pending_pinyin_sequence_ = false;
    bool has_pending_pinyin_sequence_with_cases_ = false;

    RuntimePaths paths_;
    CandidateQueries candidate_queries_;
    ImeSession engine_;
    // 位掩码（quanpin::kAutocorrect* 位），不是 bool：bool 会把邻键位截断丢失。
    unsigned quanpin_autocorrect_types_ = 0;
    bool quanpin_helpcode_enabled_ = true;
    bool shuangpin_helpcode_enabled_ = true;
    bool chinese_punctuation_enabled_ = true;
    bool candidate_learning_enabled_ = true;
    PunctuationPolicy punctuation_;
    const ShuangpinProfile shuangpin_profile_;
    FrequencyAdjustmentOptions frequency_adjustment_;
    bool frequency_adjustment_configured_ = false;
    LocalModeOptions local_mode_options_;
    EnglishInputOptions english_input_options_;
    MixedExpressiveOptions mixed_expressive_options_;
    bool dedicated_english_mode_ = false;
    std::string dedicated_english_preedit_;
    std::vector<WordItem> dedicated_english_candidates_;
    std::vector<WordItem> mixed_candidates_;
    LocalInputMode local_input_mode_ = LocalInputMode::None;
    std::optional<SchemeType> temporary_original_scheme_;
    std::string local_preedit_;
    std::vector<WordItem> local_candidates_;
    std::function<local_modes::LocalDateTime()> local_date_time_provider_;
    OnlineRequestGuard online_requests_;
};
} // namespace metasequoia
