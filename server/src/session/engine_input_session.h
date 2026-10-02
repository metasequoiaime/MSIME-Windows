#pragma once

#include "input_session.h"
#include "engine/common/helpcode_utils.h"
#include "engine/core/input_session.h"

class EngineInputSession : public IInputSession
{
  public:
    explicit EngineInputSession(SchemeType scheme_type = SchemeType::Shuangpin,
                                const ShuangpinProfile &shuangpin_profile = GetXiaoheShuangpinProfile());

    void handle_key(UINT vk, UINT modifiers_down, WCHAR wch) override;
    void recompute_candidates() override;
    SchemeType current_scheme_type() const override;
    void switch_scheme(SchemeType scheme_type) override;

    void reset_state() override;
    void reset_cache() override;
    void reset_sentence_cache() override;

    const std::vector<WordItem> &get_candidates() const override;
    bool expand_initial_candidates() override;
    std::string get_helpcode_annotation(const std::string &word, bool uppercase_all) const override;
    std::optional<WordItem> find_candidate(const std::string &key, const std::string &value) override;

    const std::string &get_pinyin_sequence() const override;
    const std::string &get_pinyin_sequence_with_cases() const override;
    const std::string &get_pure_pinyin_sequence() const override;
    const std::string &get_pinyin_segmentation() const override;
    std::string get_pinyin_segmentation_with_cases() const override;
    std::vector<std::size_t> segment_raw_boundaries() const override;
    std::string get_quanpin() const override;
    bool is_all_complete_pure_pinyin() const override;
    bool reads_as_pinyin() const override;
    bool wubi_unique_four_code() const override;
    bool wubi_four_code_is_complete() const override;
    bool has_active_helpcode() const override;
    bool accepts_mid_sentence_helpcode_marker(std::size_t caret) const override;
    bool has_mid_sentence_helpcode() const override;
    std::vector<WordItem> candidates_without_mid_sentence_helpcode() override;

    void set_rescoring_context(std::string context) override;
    void set_pinyin_sequence(const std::string &pinyin_sequence) override;
    void set_pinyin_sequence_with_cases(const std::string &pinyin_sequence) override;

    void set_caret(std::optional<std::size_t> caret) override;
    std::size_t prefix_end() const override;
    std::string pending_suffix() const override;

    int store_user_phrase(std::string pinyin, std::string word) override;
    int store_user_phrase_from_canonical_pinyin(std::string pinyin, std::string word) override;
    int remove_candidate(std::string pinyin, std::string word, SchemeType scheme) override;
    int cache_dynamic_candidate(const std::string &pinyin, const std::string &word, CandidateSource source) override;
    SelectionTransition advance_composition_after_selection(const std::string &selected_pinyin,
                                                            const std::string &selected_word,
                                                            const std::string &selected_canonical_pinyin,
                                                            SchemeType selected_scheme = SchemeType::Quanpin) override;
    CloudQueryState get_cloud_query_state() const override;
    std::optional<metasequoia::OnlineQuery> online_query() const override;
    bool apply_online_candidate(const metasequoia::OnlineQuery &query, std::string candidate,
                                CandidateSource source) override;
    CreatingWordProgress update_creating_word_progress(const std::string &current_pinyin,
                                                       const std::string &current_word,
                                                       const std::string &selected_word,
                                                       const SelectionTransition &selection_transition) const override;

  private:
    void ApplyConfiguration();
    const metasequoia::RuntimePaths paths_;
    metasequoia::InputSession session_;
    std::string helpcode_schema_;
    HelpcodeUtils::SharedKeymap helpcode_keymap_;
};
