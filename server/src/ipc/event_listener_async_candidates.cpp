// 异步候选结果回到任务线程后的合并：云联想、AI 联想、神经重排、英文、译文、emoji 与颜文字。
#include "ipc/event_listener_internal.h"
#include <Windows.h>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <utility>
#include "ipc.h"
#include "ipc/candidate_selection_policy.h"
#include "ipc/candidate_ui_owner.h"
#include "global/globals.h"
#include "engine/japanese/romaji_converter.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "cloud/cloud_translation.h"
#include "cloud/translation_gloss.h"
#include "ai/ai_assistant.h"
#include "english/english_ime.h"
#include "config/ime_config.h"
#include "emoji/emoji_ime.h"
#include "kaomoji/kaomoji_ime.h"

using namespace event_listener_detail;

namespace FanyNamedPipe
{
void ApplyCloudCandidate(const std::string &candidate, const std::string &pinyin, uint64_t generation,
                         const std::optional<metasequoia::OnlineQuery> &query)
{
    if (!GetConfiguredCloudCandidatesEnabled())
        return;
    // A callback can become stale after enqueueing, while earlier key tasks run.
    // 译文页占用着同一份 items，异步候选必须等它退出再合并，否则会把译文冲掉。
    if (FindCloudRequestOrigin(pinyin, generation).client_id == 0 || !g_inputSession || g_translation_candidates_active)
        return;

    if (candidate.empty())
        return;

    if (GlobalIme::composition.creating_word.active)
        return;

    const auto cloud_query_state = g_inputSession->get_cloud_query_state();
    if (cloud_query_state.query_text.empty() || cloud_query_state.query_text != pinyin)
        return;

    if (Global::candidate_ui.items.empty())
        return;

    if (!query)
        return;

    auto &items = Global::candidate_ui.items;
    // Same word already visible (dict / prior cloud): keep page and skip re-cache.
    if (std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == candidate; }))
    {
        return;
    }

    // The engine checks the original session and composition before caching the result.
    if (!g_inputSession->apply_online_candidate(*query, candidate, CandidateSource::CloudSuggestion))
        return;
    const auto &engine_candidates = g_inputSession->get_candidates();
    const auto accepted = std::find_if(engine_candidates.begin(), engine_candidates.end(), [&](const WordItem &item) {
        return item.word == candidate && item.source == CandidateSource::CloudSuggestion;
    });
    if (accepted == engine_candidates.end())
        return;

    // Replace any previous cloud suggestion with the new unique text.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const WordItem &item) { return item.source == CandidateSource::CloudSuggestion; }),
                items.end());

    size_t insert_index = items.size() >= 1 ? 1 : 0;
    items.insert(items.begin() + insert_index, *accepted);
    const SchemeType cloud_scheme = g_inputSession->current_scheme_type();
    bool preserve_single_kana_pair = false;
    if (cloud_scheme == SchemeType::JapaneseRomaji)
    {
        preserve_single_kana_pair =
            japanese::IsSingleKanaConversion(japanese::ConvertRomaji(g_inputSession->get_pinyin_sequence()));
    }
    else if (cloud_scheme == SchemeType::JapaneseKana)
    {
        // Direct-kana input already holds kana; a single mora is exactly one
        // UTF-8 code point (every kana here is a 3-byte E0-sequence).
        const std::string &raw = g_inputSession->get_pinyin_sequence();
        std::size_t code_points = 0;
        for (unsigned char c : raw)
            code_points += ((c & 0xC0) != 0x80) ? 1u : 0u;
        preserve_single_kana_pair = code_points == 1;
    }
    FanyImeIpc::NormalizeMixedCandidateOrder(items, preserve_single_kana_pair ? 2 : 1);
    Global::cloud_candidate = {true, candidate, cloud_query_state.committed_pinyin};

    Global::candidate_ui.item_total_count = static_cast<int>(items.size());
    Global::candidate_ui.page_index = 0;
    Global::candidate_ui.select_first_on_page();
    Global::candidate_ui.clear_page();
    RefreshCandidatePageUi(true);
}

// 候选列表当初是按词格的静态顺序发出去的，因为打分那会儿还没算完。现在算完了：把引擎的候选缓存
// 丢掉重查一次，这一次 quanpin::make_neural_reranker 能在结果表里查到顺序，整句就落到它该在的位
// 置上。重查本身不碰模型，走的还是词格那条快路。
//
// 后台线程算的可能已经是上一次输入的了（用户没停手），所以这里不认「哪一批」，只看重查出来的词
// 序有没有真的变：没变就一个字节都不动 UI。这既挡掉了过期结果，也挡掉了模型弃权的情况。
void ApplyRescoredOrder()
{
    if (!g_inputSession || g_translation_candidates_active)
        return;
    // 造词界面和译文页各自占着 items，重排不该去动它们。
    if (GlobalIme::composition.creating_word.active)
        return;
    if (Global::candidate_ui.items.empty())
        return;
    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (!owner || !IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
        return;

    const std::vector<WordItem> before = g_inputSession->get_candidates();
    // 云/AI 候选只活在 series cache 里，reset_cache 会把它们一起清掉，而它们的请求早已回来、不会
    // 再发一次——不补回来，重排一落地它们就从列表里消失了。重查不推进在线请求的 generation，
    // 所以用当前的 online_query 原样回填即可；回填时若同一句已被整句候选占了，引擎自己会拒绝。
    std::vector<WordItem> online_items;
    std::copy_if(before.begin(), before.end(), std::back_inserter(online_items), [](const WordItem &item) {
        return item.source == CandidateSource::CloudSuggestion || item.source == CandidateSource::AiSuggestion;
    });
    g_inputSession->reset_cache();
    g_inputSession->recompute_candidates();
    if (!online_items.empty())
    {
        if (const auto query = g_inputSession->online_query())
        {
            for (const WordItem &item : online_items)
                g_inputSession->apply_online_candidate(*query, item.word, item.source);
        }
    }
    const std::vector<WordItem> &after = g_inputSession->get_candidates();
    if (after.size() == before.size() &&
        std::equal(before.begin(), before.end(), after.begin(),
                   [](const WordItem &a, const WordItem &b) { return a.word == b.word; }))
        return;

    PrepareCandidateList(owner.client_id, owner.activation_epoch);
    RequestShowCandidateWindow();
}

void ApplyAiCandidate(const std::string &candidate, const std::string &identity, uint64_t generation,
                      const std::optional<metasequoia::OnlineQuery> &engine_query)
{
    if (!engine_query || FindAiRequestOrigin(identity, generation).client_id == 0)
        return;
    const bool enabled = GetConfiguredAiAssistant().enabled;
    const bool has_session = static_cast<bool>(g_inputSession);
    const bool non_pinyin = has_session && g_inputSession->current_scheme_type() != SchemeType::Quanpin &&
                            g_inputSession->current_scheme_type() != SchemeType::Shuangpin;
    const bool complete = has_session && g_inputSession->is_all_complete_pure_pinyin();
    const bool helpcode_active = has_session && g_inputSession->has_active_helpcode();
    const std::string current_identity = has_session ? g_inputSession->get_pinyin_segmentation() : std::string{};
    if (!enabled || candidate.empty() || !has_session || non_pinyin || !complete || helpcode_active ||
        GlobalIme::composition.creating_word.active || current_identity != identity || g_translation_candidates_active)
    {
        (void)0;
        return;
    }
    auto &items = Global::candidate_ui.items;
    const auto query = g_inputSession->get_cloud_query_state();
    // Align with cloud: if the word is already in the list, do not erase / reinsert /
    // reset page_index / re-cache. This stops cache-hit reapply from breaking paging.
    if (std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == candidate; }))
    {
        return;
    }

    if (!g_inputSession->apply_online_candidate(*engine_query, candidate, CandidateSource::AiSuggestion))
        return;
    const auto &engine_candidates = g_inputSession->get_candidates();
    const auto accepted = std::find_if(engine_candidates.begin(), engine_candidates.end(), [&](const WordItem &item) {
        return item.word == candidate && item.source == CandidateSource::AiSuggestion;
    });
    if (accepted == engine_candidates.end())
        return;

    // Only replace prior AI rows when inserting a genuinely new suggestion text.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const WordItem &item) { return item.source == CandidateSource::AiSuggestion; }),
                items.end());
    const size_t insert_index = std::min<size_t>(2, items.size());
    items.insert(items.begin() + insert_index, *accepted);
    FanyImeIpc::NormalizeMixedCandidateOrder(items);
    (void)0;
    Global::ai_candidate = {true, candidate, query.committed_pinyin};
    Global::candidate_ui.item_total_count = static_cast<int>(items.size());
    Global::candidate_ui.page_index = 0;
    Global::candidate_ui.select_first_on_page();
    Global::candidate_ui.clear_page();
    RefreshCandidatePageUi(true);
}

void ApplyEnglishCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)
{
    const std::string session_input =
        g_inputSession != nullptr ? g_inputSession->get_pinyin_sequence_with_cases() : std::string{};
    const bool y_mode = IsYModeInput(session_input);
    const bool dedicated_mode = g_english_input_mode || y_mode;
    const std::string expected_input = y_mode ? session_input.substr(1) : session_input;
    if ((!dedicated_mode && !GetConfiguredEnglishCandidatesEnabled()) ||
        !EnglishIme::IsCurrent(input, generation, dedicated_mode) || g_inputSession == nullptr ||
        (!dedicated_mode && g_inputSession->current_scheme_type() != SchemeType::Quanpin &&
         g_inputSession->current_scheme_type() != SchemeType::Shuangpin) ||
        expected_input != input || GlobalIme::composition.creating_word.active || g_translation_candidates_active)
    {
        return;
    }

    auto &items = Global::candidate_ui.items;
    if (dedicated_mode)
    {
        std::string context_input = input;
        std::transform(context_input.begin(), context_input.end(), context_input.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        user_dictionary::apply_fixed_positions(user_dictionary::default_user_db_path(), "english:" + context_input,
                                               candidates, false, {}, false);
        if (y_mode)
        {
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                            [&](const WordItem &item) {
                                                if (item.word.size() != input.size())
                                                    return false;
                                                for (size_t i = 0; i < input.size(); ++i)
                                                {
                                                    if (std::tolower(static_cast<unsigned char>(item.word[i])) !=
                                                        std::tolower(static_cast<unsigned char>(input[i])))
                                                        return false;
                                                }
                                                return true;
                                            }),
                             candidates.end());
            candidates.insert(candidates.begin(), WordItem("", input, 0, CandidateSource::Generated));
        }
        else if (candidates.empty() && !input.empty())
        {
            // A raw fallback is selectable, but it is not an english.db row
            // and therefore must not participate in dictionary mutations.
            candidates.emplace_back("", input, 0, CandidateSource::Generated);
        }
        items = std::move(candidates);
        Global::candidate_ui.item_total_count = static_cast<int>(items.size());
        Global::candidate_ui.page_index = 0;
        Global::candidate_ui.select_first_on_page();
        Global::candidate_ui.clear_page();
        g_dedicated_english_answer_pending = false;
        RefreshCandidatePageUi(true);
        return;
    }
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const WordItem &item) { return item.source == CandidateSource::EnglishDictionary; }),
                items.end());

    std::vector<WordItem> unique_candidates;
    for (auto &candidate : candidates)
    {
        const bool duplicate =
            std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == candidate.word; });
        if (!duplicate)
        {
            unique_candidates.push_back(std::move(candidate));
        }
    }

    if (!unique_candidates.empty())
    {
        user_dictionary::apply_fixed_positions(user_dictionary::default_user_db_path(), EnglishRankingContextKey(),
                                               unique_candidates, false);
        const size_t insert_index = std::min<size_t>(1, items.size());
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(unique_candidates.front()));
        user_dictionary::apply_fixed_positions(user_dictionary::default_user_db_path(), CurrentRankingContextKey(),
                                               items, false, {}, g_inputSession->has_active_helpcode());
        for (size_t index = 1; index < unique_candidates.size(); ++index)
        {
            items.push_back(std::move(unique_candidates[index]));
        }
        FanyImeIpc::NormalizeMixedCandidateOrder(items);
    }

    Global::candidate_ui.item_total_count = static_cast<int>(items.size());
    Global::candidate_ui.page_index = 0;
    Global::candidate_ui.select_first_on_page();
    Global::candidate_ui.clear_page();
    RefreshCandidatePageUi(true);
}

void ApplyCandidateTranslations(std::vector<EnglishIme::TranslationResult> results, uint64_t generation, bool merge)
{
    if (!EnglishIme::IsTranslationCurrent(generation) || !GetConfiguredCandidateTranslationsEnabled() ||
        IsUiLessMode() || g_candidate_translation_signature.empty() || g_translation_candidates_active ||
        (g_inputSession && IsJapaneseScheme(g_inputSession->current_scheme_type())))
        return;

    std::vector<EnglishIme::TranslationQuery> misses;
    // The page reads its glosses from g_candidate_translation_glosses when it is
    // built, so an unchanged map means the page on screen is already current.
    // Most lookups on a new keystroke hit words the previous page already
    // glossed; repainting for them cost a full candidate frame per keystroke.
    bool glosses_changed = false;
    for (auto &result : results)
    {
        std::string gloss = std::move(result.gloss);
        if (gloss.empty() && !merge)
            gloss = CloudTranslation::LookupCache(result.key, result.direction);
        const std::string identity = TranslationIdentity({result.key, result.direction});
        if (!gloss.empty())
        {
            auto existing = g_candidate_translation_glosses.find(identity);
            if (existing != g_candidate_translation_glosses.end() && existing->second == gloss)
                continue;
            if (g_candidate_translation_glosses.size() >= kMaxCandidateTranslationGlosses &&
                existing == g_candidate_translation_glosses.end())
                g_candidate_translation_glosses.clear();
            g_candidate_translation_glosses[identity] = std::move(gloss);
            glosses_changed = true;
        }
        else if (!merge)
        {
            // The authoritative lookup found nothing: drop what an earlier
            // configuration may have cached for this word.
            if (g_candidate_translation_glosses.erase(identity) > 0)
                glosses_changed = true;
            const bool cloud_translatable = result.direction == EnglishIme::TranslationDirection::EnglishToChinese
                                                ? CloudTranslation::IsCloudTranslatableEnglish(result.key)
                                                : CloudTranslation::IsCloudTranslatableChinese(result.key);
            if (cloud_translatable)
                misses.push_back({result.key, result.direction});
        }
    }
    const FanyImeIpc::CandidateUiOwner owner = SnapshotCandidateUiOwner();
    if (glosses_changed && owner && IsPipeActivationCurrent(owner.client_id, owner.activation_epoch))
        RefreshCandidatePageUi(true);
    if (!merge)
        CloudTranslation::RequestMisses(std::move(misses), generation);
}

void ApplyEmojiCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)
{
    if (!GetConfiguredEmojiMixedInputEnabled() || !EmojiIme::IsCurrent(input, generation) ||
        g_inputSession == nullptr || g_translation_candidates_active ||
        (g_inputSession->current_scheme_type() != SchemeType::Quanpin &&
         g_inputSession->current_scheme_type() != SchemeType::Shuangpin) ||
        g_inputSession->get_pinyin_sequence_with_cases() != input || GlobalIme::composition.creating_word.active)
    {
        return;
    }

    auto &items = Global::candidate_ui.items;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const WordItem &item) { return item.source == CandidateSource::Emoji; }),
                items.end());

    std::vector<WordItem> unique_candidates;
    for (auto &candidate : candidates)
    {
        const bool duplicate =
            std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == candidate.word; });
        if (!duplicate)
        {
            unique_candidates.push_back(std::move(candidate));
        }
    }

    if (!unique_candidates.empty())
    {
        const size_t insert_index = std::min<size_t>(2, items.size());
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(unique_candidates.front()));
        for (size_t index = 1; index < unique_candidates.size(); ++index)
        {
            items.push_back(std::move(unique_candidates[index]));
        }
        FanyImeIpc::NormalizeMixedCandidateOrder(items);
    }

    Global::candidate_ui.item_total_count = static_cast<int>(items.size());
    Global::candidate_ui.page_index = 0;
    Global::candidate_ui.select_first_on_page();
    Global::candidate_ui.clear_page();
    RefreshCandidatePageUi(true);
}

void ApplyKaomojiCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation)
{
    if (!GetConfiguredKaomojiMixedInputEnabled() || !KaomojiIme::IsCurrent(input, generation) ||
        g_inputSession == nullptr || g_translation_candidates_active ||
        (g_inputSession->current_scheme_type() != SchemeType::Quanpin &&
         g_inputSession->current_scheme_type() != SchemeType::Shuangpin) ||
        g_inputSession->get_pinyin_sequence_with_cases() != input || GlobalIme::composition.creating_word.active)
    {
        return;
    }

    auto &items = Global::candidate_ui.items;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const WordItem &item) { return item.source == CandidateSource::Kaomoji; }),
                items.end());

    std::vector<WordItem> unique_candidates;
    for (auto &candidate : candidates)
    {
        const bool duplicate =
            std::any_of(items.begin(), items.end(), [&](const WordItem &item) { return item.word == candidate.word; });
        if (!duplicate)
        {
            unique_candidates.push_back(std::move(candidate));
        }
    }

    if (!unique_candidates.empty())
    {
        const size_t insert_index = std::min<size_t>(3, items.size());
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(unique_candidates.front()));
        for (size_t index = 1; index < unique_candidates.size(); ++index)
        {
            items.push_back(std::move(unique_candidates[index]));
        }
        FanyImeIpc::NormalizeMixedCandidateOrder(items);
    }

    Global::candidate_ui.item_total_count = static_cast<int>(items.size());
    Global::candidate_ui.page_index = 0;
    Global::candidate_ui.select_first_on_page();
    Global::candidate_ui.clear_page();
    RefreshCandidatePageUi(true);
}
} // namespace FanyNamedPipe
