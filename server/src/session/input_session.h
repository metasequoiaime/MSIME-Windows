#pragma once

#include "engine/core/input_session.h"
#include "engine/shuangpin/shuangpin_dictionary.h"
#include <Windows.h>
#include <string>
#include <vector>
#include <optional>

class IInputSession
{
  public:
    using WordItem = DictionaryUlPb::WordItem;

    using SelectionTransition = metasequoia::InputSession::SelectionTransition;
    using CloudQueryState = metasequoia::InputSession::CloudQueryState;
    using CreatingWordProgress = metasequoia::InputSession::CreatingWordProgress;

    virtual ~IInputSession() = default;

    virtual void handle_key(UINT vk, UINT modifiers_down, WCHAR wch) = 0;
    virtual void recompute_candidates() = 0;
    virtual SchemeType current_scheme_type() const = 0;
    virtual void switch_scheme(SchemeType scheme_type) = 0;
    // Japanese composition only: flip the kana form of the leading candidate
    // (F9 -> katakana, F10 -> hiragana). Returns true when handled.
    virtual bool cycle_japanese_kana_form(bool to_katakana)
    {
        return false;
    }

    virtual void reset_state() = 0;
    virtual void reset_cache() = 0;

    virtual const std::vector<WordItem> &get_candidates() const = 0;
    virtual bool expand_initial_candidates() = 0;
    virtual std::string get_helpcode_annotation(const std::string &word, bool uppercase_all) const = 0;
    virtual std::optional<WordItem> find_candidate(const std::string &, const std::string &)
    {
        return std::nullopt;
    }

    virtual const std::string &get_pinyin_sequence() const = 0;
    virtual const std::string &get_pinyin_sequence_with_cases() const = 0;
    virtual const std::string &get_pure_pinyin_sequence() const = 0;
    virtual const std::string &get_pinyin_segmentation() const = 0;
    virtual std::string get_pinyin_segmentation_with_cases() const = 0;
    // Raw offsets where one input unit starts, for segment deletion. Empty when
    // the scheme or mode has no unit model.
    virtual std::vector<std::size_t> segment_raw_boundaries() const = 0;
    virtual std::string get_quanpin() const = 0;
    virtual bool is_all_complete_pure_pinyin() const = 0;
    // Engine fact for wubi auto-commit: the composition is a complete four-letter wubi code the
    // table answered with exactly one candidate. The Server decides whether the setting makes that
    // commit immediately; the engine only reports it.
    virtual bool wubi_unique_four_code() const = 0;
    // Engine fact for wubi top-word commit: a complete four-letter code the table answered, no
    // uniqueness required. The Server commits the first candidate when the user types past it.
    virtual bool wubi_four_code_is_complete() const = 0;
    virtual bool has_active_helpcode() const = 0;

    // 本会话最近上屏的文本，给神经整句重排当前文。空实现：除引擎会话外没人需要前文。
    virtual void set_rescoring_context(std::string)
    {
    }

    virtual void set_pinyin_sequence(const std::string &pinyin_sequence) = 0;
    virtual void set_pinyin_sequence_with_cases(const std::string &pinyin_sequence) = 0;

    // R2 光标前缀重算：把组合态光标喂给会话，nullopt = 串尾（整串解码，默认）。
    // 语义与 engine/core/input_session.h 的同名方法一致；只更新状态，候选在下一次
    // recompute_candidates() 时按新边界重解。
    virtual void set_caret(std::optional<std::size_t> caret) = 0;
    // 当前解码消费的 raw 长度：光标 floor 到最后一个完整单元边界；未设光标或方案无
    // 单元模型时等于 raw 长度（整串解码）。
    virtual std::size_t prefix_end() const = 0;
    // raw[prefix_end, size)：本次解码未消费的原始后缀，前缀覆盖整串时为空。
    virtual std::string pending_suffix() const = 0;

    virtual int store_user_phrase(std::string pinyin, std::string word) = 0;
    virtual int store_user_phrase_from_canonical_pinyin(std::string pinyin, std::string word) = 0;
    virtual int pin_candidate(std::string pinyin, std::string word) = 0;
    virtual int remove_candidate(std::string pinyin, std::string word) = 0;
    virtual int cache_dynamic_candidate(const std::string &pinyin, const std::string &word, CandidateSource source) = 0;
    virtual SelectionTransition advance_composition_after_selection(const std::string &selected_pinyin,
                                                                    const std::string &selected_word,
                                                                    const std::string &selected_canonical_pinyin) = 0;
    virtual CloudQueryState get_cloud_query_state() const = 0;
    virtual std::optional<metasequoia::OnlineQuery> online_query() const = 0;
    virtual bool apply_online_candidate(const metasequoia::OnlineQuery &query, std::string candidate,
                                        CandidateSource source) = 0;
    virtual CreatingWordProgress update_creating_word_progress(
        const std::string &current_pinyin, const std::string &current_word, const std::string &selected_word,
        const SelectionTransition &selection_transition) const = 0;
};
