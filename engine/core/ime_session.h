#pragma once

#include "composition_state.h"
#include "input_session_types.h"
#include "scheme_type.h"
#include "sentence_association_options.h"
#include "../providers/provider_registry.h"
#include "../schemes/input_scheme.h"
#include "../schemes/wubi_scheme.h"
#include "../shuangpin/shuangpin_profile.h"
#include <memory>

class ImeSession
{
  public:
    explicit ImeSession(SchemeType scheme_type = SchemeType::Shuangpin,
                        const ShuangpinProfile &shuangpin_profile = GetXiaoheShuangpinProfile(),
                        metasequoia::RuntimePaths paths = metasequoia::RuntimePaths::legacy());

    void handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down = 0, ImeCharacter wch = 0);
    void switch_scheme(SchemeType scheme_type);
    void set_shuangpin_helpcode_enabled(bool enabled);
    void set_quanpin_helpcode_enabled(bool enabled);
    void set_quanpin_autocorrect_types(unsigned autocorrect_types);
    void set_fuzzy_pinyin_options(metasequoia::FuzzyPinyinOptions options)
    {
        fuzzy_pinyin_ = options;
    }
    void set_sentence_association(const SentenceAssociationOptions &options)
    {
        sentence_association_ = options;
    }
    // 本会话最近上屏的文本，随查询下发给神经重排当前文。
    void set_rescoring_context(std::string context)
    {
        rescoring_context_ = std::move(context);
    }
    void set_wubi_input_options(metasequoia::WubiInputOptions options);
    // Japanese-only: pins the top candidate kana form for the current
    // composition. Reset to Auto on scheme switch / reset.
    void set_japanese_kana_form(JapaneseKanaForm form)
    {
        japanese_kana_form_ = form;
        refresh_candidates();
    }
    JapaneseKanaForm japanese_kana_form() const
    {
        return japanese_kana_form_;
    }
    void replace_shuangpin_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases);
    void replace_quanpin_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases);
    void replace_wubi_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases);
    void replace_japanese_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases);
    // Writes back to whichever scheme is composing. Committing a spelling out of a longer one
    // has to shorten the live composition, and under the wubi fallback the pinyin-shaped
    // caller would otherwise address a scheme that is not the active one and be ignored.
    void replace_active_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases);
    // Runs one standalone candidate query for the given raw input without touching the live
    // composition: the active scheme's raw/key strokes and state_'s request/candidates stay put.
    // Query options (helpcode, autocorrect, fuzzy) match refresh_candidates exactly. The caret
    // driven prefix decoding in InputSession uses this to decode a prefix while the composition
    // still owns the full raw string.
    std::vector<WordItem> query_raw_candidates(const std::string &raw_input, const std::string &raw_input_with_cases);
    void reset();
    void reset_cache();
    int create_word(std::string pinyin, std::string word);
    int update_weight_by_pinyin_and_word(std::string pinyin, std::string word);
    int delete_by_pinyin_and_word(std::string pinyin, std::string word);
    int cache_dynamic_candidate(const std::string &pinyin, const std::string &word, CandidateSource source);
    int cache_dynamic_candidate_for_current_request(const std::string &word, CandidateSource source);
    int apply_dynamic_candidate(const std::string &word, CandidateSource source);
    std::optional<WordItem> find_candidate(const std::string &key, const std::string &value);

    SchemeType current_scheme_type() const;
    const std::string &get_preedit() const;
    const QueryRequest &get_request() const;
    bool answered_by_pinyin_fallback() const
    {
        return state_.answered_by_pinyin_fallback;
    }
    // Forwarded from the live wubi scheme so a session with no wubi scheme answers false. The
    // composition knows whether its raw input is a full four-letter code; only the scheme holds it.
    bool wubi_code_is_complete() const
    {
        return wubi_scheme_ != nullptr && wubi_scheme_->has_complete_code();
    }
    const std::vector<WordItem> &get_candidates() const;
    bool expand_initial_candidates();

    void set_helpcode_keymap(HelpcodeUtils::SharedKeymap table)
    {
        provider_registry_.set_helpcode_keymap(std::move(table));
        refresh_candidates();
    }

  private:
    // Shared option injection for refresh_candidates() and query_raw_candidates(); the two must
    // not drift or a prefix query would answer with different candidates than the live pipeline.
    void apply_request_options(QueryRequest &request) const;
    void refresh_candidates();
    void bind_wubi_scheme();
    SchemeType candidate_scheme() const;
    std::unique_ptr<IInputScheme> create_scheme(SchemeType scheme_type) const;

  private:
    ProviderRegistry provider_registry_;
    const ShuangpinProfile shuangpin_profile_;
    std::unique_ptr<IInputScheme> scheme_;
    CompositionState state_;
    bool enable_shuangpin_helpcode_ = false;
    bool enable_quanpin_helpcode_ = false;
    unsigned quanpin_autocorrect_types_ = 0;
    metasequoia::FuzzyPinyinOptions fuzzy_pinyin_;
    SentenceAssociationOptions sentence_association_;
    std::string rescoring_context_;
    metasequoia::WubiInputOptions wubi_options_;
    JapaneseKanaForm japanese_kana_form_ = JapaneseKanaForm::Auto;
    // Resolved when the scheme changes rather than on every keystroke.
    WubiScheme *wubi_scheme_ = nullptr;
    // Once a composition has been answered by pinyin it stays with pinyin until it ends.
    // Committing a spelling out of a longer one leaves a tail the wubi table may happen to
    // know, and switching back mid-composition would answer a pinyin spelling with wubi.
    bool composition_uses_pinyin_fallback_ = false;
};
