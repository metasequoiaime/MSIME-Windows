// 候选页的构建与发布：按输入模式查候选（PrepareCandidateList）、组页并发布给候选窗，
// 以及调频用的排序键和翻页时的候选扩展。
#include "ipc/event_listener_internal.h"
#include <Windows.h>
#include <string>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <utility>
#include "ipc.h"
#include "ipc/candidate_ui_owner.h"
#include "ipc/input_key_policy.h"
#include "utils/common_utils.h"
#include "fmt/xchar.h"
#include <utf8.h>
#include "global/globals.h"
#include "engine/common/helpcode_utils.h"
#include "engine/japanese/japanese_glossary.h"
#include "engine/core/scheme_type.h"
#include "engine/quanpin/quanpin_query.h"
#include "engine/user_dictionary/user_dictionary_journal.h"
#include "cloud/cloud_translation.h"
#include "english/english_ime.h"
#include "config/ime_config.h"
#include "engine/local_modes/quick_phrase_query.h"
#include "engine/local_modes/unicode_query.h"
#include "engine/local_modes/date_time_query.h"
#include "engine/local_modes/emoji_query.h"
#include "engine/local_modes/kaomoji_query.h"
#include "engine/local_modes/jianpin_query.h"
#include "engine/shuangpin/shuangpin_profile.h"
#include "log/candidate_diag_log.h"

using namespace event_listener_detail;

namespace
{
// The engine's local mode queries take a resolved ShuangpinProfile and default it to Xiaohe. The
// modules this file used to call resolved the *configured* scheme instead, so every call site here
// has to pass this explicitly: letting the default through would silently decode J mode, emoji and
// kaomoji as Xiaohe for anyone on Ziranma, Shoudao or Microsoft shuangpin.
const ShuangpinProfile &ConfiguredShuangpinProfile()
{
    return GetShuangpinProfile(GetConfiguredShuangpinSchema());
}

std::string BuildCurrentCandidatePage();
void PrepareCandidateTranslationRequest();

std::string g_candidate_translation_scope;

bool BuildTranslationQuery(const WordItem &item, EnglishIme::TranslationQuery &query)
{
    // Dictionary keys remain simplified even when the visible candidate is
    // converted to traditional Chinese at render/commit time.
    const std::string visible = item.word;
    if (visible.empty())
        return false;
    if (item.source == CandidateSource::Emoji || item.source == CandidateSource::Kaomoji)
        return false;
    if (item.source == CandidateSource::EnglishDictionary && !item.pinyin.empty())
    {
        query = {item.pinyin, EnglishIme::TranslationDirection::EnglishToChinese};
        return true;
    }

    bool has_ascii_letter = false;
    bool english = true;
    std::string normalized;
    normalized.reserve(visible.size());
    for (const unsigned char ch : visible)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            normalized.push_back(static_cast<char>(ch + ('a' - 'A')));
            has_ascii_letter = true;
        }
        else if (ch >= 'a' && ch <= 'z')
        {
            normalized.push_back(static_cast<char>(ch));
            has_ascii_letter = true;
        }
        else if (ch == ' ' || ch == '-' || ch == '\'')
        {
            normalized.push_back(static_cast<char>(ch));
        }
        else
        {
            english = false;
            break;
        }
    }
    if (english && has_ascii_letter)
    {
        query = {std::move(normalized), EnglishIme::TranslationDirection::EnglishToChinese};
        return true;
    }
    if (HelpcodeUtils::count_han_chars(visible) > 0)
    {
        query = {visible, EnglishIme::TranslationDirection::ChineseToEnglish};
        return true;
    }
    return false;
}

bool IsQuickPhraseInput(const std::string &raw)
{
    return g_quick_phrase_triggered && raw.size() > 1 && raw.front() == 'K' &&
           std::all_of(raw.begin() + 1, raw.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; });
}

bool IsUnicodeInput(const std::string &raw)
{
    if (!IsUnicodeCompositionActive(raw) || raw.size() <= 1)
        return false;
    size_t index = 1;
    if (raw[index] == '+')
        ++index;
    return index < raw.size();
}

bool IsDateTimeInput(const std::string &raw)
{
    if (!IsDateTimeCompositionActive(raw) || raw.size() <= 1)
        return false;
    return metasequoia::local_modes::is_date_time_keyword(raw.substr(1));
}

bool IsEmojiInput(const std::string &raw)
{
    return IsEmojiCompositionActive(raw) && raw.size() > 1;
}

bool IsKaomojiInput(const std::string &raw)
{
    return IsKaomojiCompositionActive(raw) && raw.size() > 1;
}

bool IsJianpinInput(const std::string &raw)
{
    return IsJianpinCompositionActive(raw) && raw.size() > 1 &&
           std::all_of(raw.begin() + 1, raw.end(),
                       [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); });
}
} // namespace

namespace event_listener_detail
{
void EnsureCandidatePageReady()
{
    if (!Global::candidate_ui.page_words.empty())
    {
        return;
    }
    if (Global::candidate_ui.items.empty())
    {
        return;
    }
    BuildCurrentCandidatePage();
}

std::wstring BuildUiLessCandidatePageW()
{
    EnsureCandidatePageReady();
    auto &ui = Global::candidate_ui;
    if (ui.page_words.empty() && !ui.items.empty())
    {
        BuildCurrentCandidatePage();
    }
    std::wstring page;
    for (size_t i = 0; i < ui.page_words.size(); ++i)
    {
        if (i != 0)
        {
            page += L',';
        }
        page += ui.page_words[i];
    }
    return page;
}
} // namespace event_listener_detail

namespace
{
std::string BuildCurrentCandidatePage()
{
    auto &ui = Global::candidate_ui;
    ui.clear_page();
    const SchemeType current_scheme = g_inputSession->current_scheme_type();
    const bool uppercase_all_helpcodes = current_scheme == SchemeType::Quanpin;
    // 副候选框里装的是译文，不是这次输入的候选：助记码、云/AI 角标和「右侧译文」都不适用，
    // 而且 g_candidate_translation_glosses 还留着原候选的译文，照常查会把译文再标注一遍。
    const bool translation_page = g_translation_candidates_active;
    const bool show_helpcodes =
        !translation_page && ((current_scheme == SchemeType::Shuangpin && GetConfiguredShuangpinHelpcodeEnabled() &&
                               GetConfiguredShowShuangpinHelpcodeInCandidateWindow()) ||
                              (current_scheme == SchemeType::Quanpin && GetConfiguredQuanpinHelpcodeEnabled() &&
                               GetConfiguredShowQuanpinHelpcodeInCandidateWindow()));

    // 组页时读一次徽标配置，循环内不再逐条查
    const bool show_fixed_badge = GetConfiguredCandidateFixedBadge();
    const std::string fixed_badge_style = GetConfiguredCandidateFixedBadgeStyle();
    const bool show_sentence_source_badge = GetConfiguredAssocSentenceSourceBadge();

    const int start = ui.current_page_start();
    const int loop = ui.current_page_count();

    int maxCount = 0;
    std::string candidate_string;
    for (int i = 0; i < loop; i++)
    {
        const auto &item = ui.items[start + i];
        const std::string word = CandidateTextForOutput(item.word);

        CandidateViewItem view;
        view.text = word;
        if (!item.corrected_from.empty())
        {
            // Correction-sourced candidates carry a light visible marker (PRD R5/AC7).
            // Only the display text is touched: commits, word frequency updates and
            // pinned-position lookups read item.word / page_words and must never see
            // the marker suffix.
            view.text += "*";
        }
        if (item.source == CandidateSource::Generated && show_helpcodes)
        {
            // Generated whole-sentence candidates carry the raw spelling in
            // item.pinyin.  When helpcodes are enabled, do not expose that
            // full preedit; sentence annotations must use the normal
            // first/last-character helpcode rule instead.  If the helpcode
            // cannot be computed, leave the annotation empty rather than
            // falling back to the raw preedit.
            view.annotation = g_inputSession->get_helpcode_annotation(item.word, uppercase_all_helpcodes);
        }
        if (show_helpcodes && item.source != CandidateSource::EnglishDictionary &&
            item.source != CandidateSource::QuickPhrase && item.source != CandidateSource::Emoji &&
            item.source != CandidateSource::Kaomoji && item.source != CandidateSource::Generated)
            view.annotation = g_inputSession->get_helpcode_annotation(item.word, uppercase_all_helpcodes);
        if (item.source == CandidateSource::CloudSuggestion)
            view.badge = " ☁️";
        else if (item.source == CandidateSource::AiSuggestion)
            view.badge = " 🤖";
        // 整句来源标签与设置页名称保持一致，方便同时比较四个来源。两家选中同一句时只剩一行，
        // 标签归先保留下来的来源；开启去重补位后，其余来源会改为显示自己的下一条不同结果。
        // Generated/Fallback 也被原样上屏、英文、日期等合成候选借用，只有引擎标了整句联想的才挂标签。
        // 设置页可整体关闭这组标签，候选本身和排序不受影响。
        else if (show_sentence_source_badge)
        {
            if (item.source == CandidateSource::Generated && item.sentence_association)
                view.badge = " 〔Trigram〕";
            else if (item.source == CandidateSource::Fallback && item.sentence_association)
                view.badge = " 〔Unigram〕";
            else if (item.source == CandidateSource::NeuralDesktop)
                view.badge = " 〔神经D〕";
            else if (item.source == CandidateSource::NeuralKeyboard)
                view.badge = " 〔神经K〕";
        }
        view.fixed_position = item.fixed_position > 0;
        ApplyFixedPositionBadge(view, show_fixed_badge, fixed_badge_style);
        EnglishIme::TranslationQuery translation_query;
        if (!translation_page && BuildTranslationQuery(item, translation_query))
        {
            const auto gloss = g_candidate_translation_glosses.find(TranslationIdentity(translation_query));
            if (gloss != g_candidate_translation_glosses.end())
                view.translation = gloss->second;
        }
        // Japanese candidates carry a local preview gloss (English original for
        // katakana loanwords such as コーヒー -> coffee, full forms for slang
        // abbreviations such as キタコレ -> 来たこれ). This is a local table, so
        // it needs no translation request and renders through the exact same
        // candidate-window UI as Chinese translation previews.
        if (IsJapaneseScheme(current_scheme) && view.translation.empty())
        {
            if (std::string japanese_gloss = japanese::LookUpCandidateGloss(word); !japanese_gloss.empty())
                view.translation = std::move(japanese_gloss);
        }
        const std::string visible = view.text + view.annotation + view.badge;
        const int display_length = static_cast<int>(utf8::distance(visible.begin(), visible.end()));
        candidate_string += CandidateViewHtml(view);
        ui.page_glosses.push_back(string_to_wstring(view.translation));
        ui.page_views.push_back(std::move(view));
        maxCount = (std::max)(maxCount, display_length);
        ui.page_words.push_back(string_to_wstring(word));
        if (i < loop - 1)
        {
            candidate_string += ",";
        }
    }

    if (maxCount > 2)
    {
        ui.cur_page_max_word_len = maxCount;
    }
    ui.cur_page_item_cnt = loop;
    if (!ui.page_words.empty())
    {
        ui.selected_index_in_page = std::clamp(ui.selected_index_in_page, 0, loop - 1);
        ui.selected_text = ui.page_words[ui.selected_index_in_page];
    }
    return candidate_string;
}

void PrepareCandidateTranslationRequest()
{
    const bool japanese = g_inputSession && IsJapaneseScheme(g_inputSession->current_scheme_type());
    const bool enabled = GetConfiguredCandidateTranslationsEnabled() && !IsUiLessMode() && !japanese;
    auto &ui = Global::candidate_ui;
    if (g_translation_candidates_active)
    {
        // 译文页不再查译文。已经取出的 glosses 也不能清，退出子模式后原候选还要用。
        return;
    }
    if (!enabled)
    {
        g_candidate_translation_glosses.clear();
    }
    if (!enabled || ui.items.empty())
    {
        // An empty page (English mode between keystrokes, or the end of a
        // composition) only cancels the pending lookups; the cache stays for
        // the next page.
        if (!g_candidate_translation_signature.empty())
        {
            g_candidate_translation_signature.clear();
            EnglishIme::ClearTranslations();
            CloudTranslation::Clear();
        }
        return;
    }
    const std::string scope = fmt::format("{}|{}|{}", GetConfiguredTencentTmt().enabled,
                                          GetConfiguredCustomTranslation().enabled, GetConfiguredNiuTrans().enabled);
    if (scope != g_candidate_translation_scope)
    {
        g_candidate_translation_scope = scope;
        g_candidate_translation_glosses.clear();
    }

    std::vector<EnglishIme::TranslationQuery> queries;
    std::string signature;
    const int start = ui.current_page_start();
    const int count = ui.current_page_count();
    for (int i = 0; i < count; ++i)
    {
        EnglishIme::TranslationQuery query;
        if (!BuildTranslationQuery(ui.items[start + i], query))
            continue;
        const std::string identity = TranslationIdentity(query);
        signature += std::to_string(identity.size()) + ":" + identity;
        if (std::none_of(queries.begin(), queries.end(), [&](const auto &existing) {
                return existing.key == query.key && existing.direction == query.direction;
            }))
            queries.push_back(std::move(query));
    }

    if (signature == g_candidate_translation_signature)
        return;
    g_candidate_translation_signature = std::move(signature);
    CloudTranslation::Clear();
    EnglishIme::RequestTranslations(std::move(queries), GetConfiguredTencentTmt().target_language == "en");
}

// Copy the page the worker just finished into an immutable snapshot for the UI thread. This is the single publish
// point: set_items() and clear_page() are only intermediate steps of a rebuild, so publishing there would hand the UI a
// half-built page.
void PublishBuiltCandidatePage(const std::wstring &candidate_string)
{
    const auto &ui = Global::candidate_ui;
    auto snapshot = std::make_shared<Global::CandidatePageSnapshot>();
    snapshot->page_views = ui.page_views;
    snapshot->page_words = ui.page_words;
    snapshot->candidate_string = candidate_string;
    snapshot->selected_index_in_page = ui.selected_index_in_page;
    snapshot->page_count = ui.current_page_count();
    snapshot->page_item_count = ui.cur_page_item_cnt;
    // Stamp before publishing so the UI thread can echo back exactly which page it painted.
    snapshot->generation = ++Global::candidate_page_generation;
    Global::PublishCandidatePageSnapshot(std::move(snapshot));
}
} // namespace

namespace event_listener_detail
{
void RefreshCandidatePageUi(bool show_window)
{
    PrepareCandidateTranslationRequest();
    const std::string candidate_string = BuildCurrentCandidatePage();
    // Host-drawn UI wants plain words (Microsoft IME style), not helpcodes.
    const std::wstring published = IsUiLessMode() ? BuildUiLessCandidatePageW() : string_to_wstring(candidate_string);
    ::WriteDataToSharedMemory(published, true);
    PublishBuiltCandidatePage(published);
    CAND_DIAG_LOGF(L"candidate UI refreshed show={} uiless={} items={} page_words={} selected={} page={} "
                   L"serialized_units={}",
                   show_window, IsUiLessMode(), Global::candidate_ui.items.size(),
                   Global::candidate_ui.page_words.size(), Global::candidate_ui.selected_index_in_page,
                   Global::candidate_ui.page_index, candidate_string.size());
    if (show_window)
    {
        RequestShowCandidateWindow();
    }
}
} // namespace event_listener_detail

namespace FanyNamedPipe
{
std::string CurrentRankingContextKey()
{
    if (g_inputSession)
    {
        const std::string raw = g_inputSession->get_pinyin_sequence_with_cases();
        if (IsJianpinCompositionActive(raw) && raw.size() > 1)
            return metasequoia::local_modes::jianpin_ranking_context(
                raw.substr(1), g_inputSession->current_scheme_type(), ConfiguredShuangpinProfile());
    }
    std::string converted = g_inputSession->get_quanpin();
    if (converted.empty())
        converted = g_inputSession->get_pinyin_segmentation();
    if (g_inputSession->get_pinyin_sequence().size() == 1)
        return converted;
    std::string plain = converted;
    plain.erase(std::remove(plain.begin(), plain.end(), '\''), plain.end());
    const auto cuts = quanpin::cut_pinyin_by_mode(plain, "correction");
    return cuts.empty() ? converted : quanpin::join_segments(cuts.front());
}

std::string EnglishRankingContextKey()
{
    std::string key = g_inputSession->get_pinyin_sequence_with_cases();
    if (IsYModeInput(key))
        key = key.substr(1);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return "english:" + key;
}

// Pulls the next batch of candidates out of the session without disturbing
// where the user currently is: set_items resets the page and the selection, so
// both are restored afterwards. Returns false when nothing more was loaded.
bool ExpandCandidatesKeepingPagePosition()
{
    auto &ui = Global::candidate_ui;
    // 译文页是一份固定的列表，session 里再多的候选也不属于它。
    if (g_translation_candidates_active ||
        IsSpecialModeCompositionActive(GlobalIme::composition.raw_input_with_cases) || !g_inputSession ||
        !g_inputSession->expand_initial_candidates())
    {
        return false;
    }
    const int current_page = ui.page_index;
    const int current_selection = ui.selected_index_in_page;
    auto expanded = g_inputSession->get_candidates();
    user_dictionary::apply_fixed_positions(
        user_dictionary::default_user_db_path(), CurrentRankingContextKey(), expanded, true,
        [](const std::string &key, const std::string &value) { return g_inputSession->find_candidate(key, value); },
        g_inputSession->has_active_helpcode());
    ui.set_items(std::move(expanded));
    ui.page_index = current_page;
    ui.selected_index_in_page = current_selection;
    return true;
}

// Moves one page in `offset`'s direction, expanding the candidate list first
// when the move would run off the end. Anything other than Unchanged needs a
// UI refresh.
PageMoveResult MoveCandidatePage(int offset)
{
    auto &ui = Global::candidate_ui;
    if (offset > 0 && ui.is_next_page_partial_last_page())
    {
        // Populate the last partial page before entering it, so the first
        // display of that page is already full.
        ExpandCandidatesKeepingPagePosition();
    }
    else if (offset > 0 && !ui.has_next_page())
    {
        const bool current_page_was_full = ui.is_current_page_full();
        if (ExpandCandidatesKeepingPagePosition() && !current_page_was_full)
        {
            // Newly loaded items first fill the unused slots on the current last
            // page. Refresh that page instead of skipping those items by
            // advancing immediately.
            return PageMoveResult::CurrentPageRefilled;
        }
    }
    if (offset < 0 ? ui.has_prev_page() : ui.has_next_page())
    {
        ui.page_index += offset;
        return PageMoveResult::Moved;
    }
    return PageMoveResult::Unchanged;
}

std::string CandidateDatabaseKey(const WordItem &item, const std::string &context_key)
{
    if (!item.canonical_pinyin.empty())
        return item.canonical_pinyin;
    if (g_inputSession->get_pinyin_sequence().size() == 1)
        return item.pinyin;
    auto segments = quanpin::split_segments(context_key);
    const size_t han_count = HelpcodeUtils::count_han_chars(item.word);
    if (segments.empty() || han_count == 0)
        return item.pinyin;
    if (segments.size() > han_count)
        segments.resize(han_count);
    return quanpin::join_segments(segments);
}

bool IsWubiRankingScheme()
{
    return g_inputSession && g_inputSession->current_scheme_type() == SchemeType::Wubi;
}

std::pair<std::string, std::string> RankingKeysForCandidate(const WordItem &item)
{
    if (IsWubiRankingScheme())
    {
        const std::string key =
            item.pinyin.empty() && g_inputSession ? g_inputSession->get_pinyin_sequence() : item.pinyin;
        return {key, item.pinyin};
    }
    const std::string context_key = CurrentRankingContextKey();
    return {context_key, CandidateDatabaseKey(item, context_key)};
}

// Stopwatch for the per-keystroke candidate build. Everything this function does
// runs on the shared task thread, so a stall anywhere in it delays the hide/show
// messages queued behind it — which is what reaches the screen as flicker. The
// splits exist to tell those segments apart: the engine candidate lookup and the
// user-dictionary fixed-position pass both touch a database, and only a
// measurement says which one is paying for it.
class CandidateBuildTimer
{
  public:
    // Milliseconds since the previous split, then restarts the segment.
    double Split()
    {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
        return ms;
    }
    double TotalMs() const
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    }

  private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_ = start_;
};

void PrepareCandidateList(uint64_t client_id, uint64_t activation_epoch)
{
    CandidateBuildTimer segment;
    // Zero means the branch taken this keystroke has no such segment, not that
    // the segment was instant.
    double queryMs = 0;
    double fixedPosMs = 0;

    auto &ui = Global::candidate_ui;
    std::string pinyin = wstring_to_string(Global::PinyinString);
    const std::string current_input = g_inputSession->get_pinyin_sequence_with_cases();
    std::vector<WordItem> items;
    if (g_english_input_mode)
    {
        // Do not expose transient Chinese/raw fallback candidates while the
        // dedicated English query is in flight.
    }
    else if (IsUnicodeInput(current_input))
    {
        items = metasequoia::local_modes::query_unicode(current_input.substr(1));
    }
    else if (IsQuickPhraseInput(current_input))
    {
        items = metasequoia::local_modes::query_quick_phrases(current_input.substr(1)).candidates;
    }
    else if (IsDateTimeInput(current_input))
    {
        items = metasequoia::local_modes::query_date_time(current_input.substr(1));
    }
    else if (IsEmojiInput(current_input))
    {
        items = metasequoia::local_modes::query_emoji(current_input.substr(1), g_inputSession->current_scheme_type(),
                                                      10, ConfiguredShuangpinProfile())
                    .candidates;
    }
    else if (IsKaomojiInput(current_input))
    {
        items = metasequoia::local_modes::query_kaomoji(current_input.substr(1), g_inputSession->current_scheme_type(),
                                                        10, ConfiguredShuangpinProfile())
                    .candidates;
    }
    else if (IsJianpinInput(current_input))
    {
        const int limit = current_input.size() == 2 ? 24 : 100;
        items = metasequoia::local_modes::query_jianpin(current_input.substr(1), g_inputSession->current_scheme_type(),
                                                        limit, ConfiguredShuangpinProfile())
                    .candidates;
        const std::string typed = g_inputSession->get_pinyin_sequence();
        for (auto &item : items)
            item.pinyin = typed;
        queryMs = segment.Split();
        user_dictionary::apply_fixed_positions(user_dictionary::default_user_db_path(), CurrentRankingContextKey(),
                                               items, false);
        fixedPosMs = segment.Split();
    }
    else if (IsYModeInput(current_input))
    {
        // Show the typed English immediately; dictionary completions arrive asynchronously.
        items.emplace_back("", current_input.substr(1), 0, CandidateSource::Generated);
    }
    else if (IsSpecialModeCompositionActive(current_input))
    {
        // A K/U/T/E/M/J/Y special-mode prefix that is not yet a complete input (e.g.
        // "K", "U", "U+", "Tw", "Txin", "E", "M", "J", "Y"): do not translate it into
        // normal pinyin candidates. Leave items empty so only the raw typed text
        // shows as the fallback.
    }
    else
    {
        items = g_inputSession->get_candidates();
        queryMs = segment.Split();
        user_dictionary::apply_fixed_positions(
            user_dictionary::default_user_db_path(), CurrentRankingContextKey(), items,
            g_inputSession->get_pinyin_sequence().size() == 1,
            [](const std::string &key, const std::string &value) { return g_inputSession->find_candidate(key, value); },
            g_inputSession->has_active_helpcode());
        fixedPosMs = segment.Split();
        if (g_inputSession->get_pinyin_sequence().size() == 1 && items.size() > 24)
            items.resize(24);
    }

    // R4：光标前缀为空时不造「整串假候选」——前缀为空就该没有候选（候选窗由调用方
    // 收起）。caret 未设置时 prefix_end 等于串长，此分支永不触发，现状零差异。
    if (items.empty() && !g_english_input_mode &&
        !FanyImeIpc::IsCaretPrefixEmpty(g_inputSession->prefix_end(),
                                        g_inputSession->get_pinyin_sequence_with_cases().size()))
    {
        items.emplace_back(pinyin, pinyin, 1, CandidateSource::Fallback);
    }

    // Whatever the branch above did that the two splits did not already claim.
    const double branchMs = segment.Split();
    const size_t itemCount = items.size();

    ui.set_items(std::move(items));
    RefreshCandidatePageUi(false);
    PublishCandidateUiOwner(client_id, activation_epoch);
    const double uiMs = segment.Split();

    const SchemeType scheme = g_inputSession->current_scheme_type();
    g_dedicated_english_answer_pending = false;
    if (g_english_input_mode)
    {
        UpdateEnglishInput(current_input, client_id, activation_epoch, true);
        g_dedicated_english_answer_pending = !current_input.empty() && EnglishIme::IsRunning();
    }
    else if (IsYModeInput(current_input))
    {
        UpdateEnglishInput(current_input.substr(1), client_id, activation_epoch, true);
        g_dedicated_english_answer_pending = EnglishIme::IsRunning();
    }
    else if (!IsSpecialModeCompositionActive(current_input) && GetConfiguredEnglishCandidatesEnabled() &&
             (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin) &&
             !GlobalIme::composition.creating_word.active)
    {
        UpdateEnglishInput(current_input, client_id, activation_epoch);
    }
    else
    {
        UpdateEnglishInput("");
    }
    const double englishMs = segment.Split();

    if (!g_english_input_mode && !IsSpecialModeCompositionActive(current_input) &&
        GetConfiguredEmojiMixedInputEnabled() && (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin) &&
        !GlobalIme::composition.creating_word.active)
    {
        UpdateEmojiInput(current_input, client_id, activation_epoch);
    }
    else
    {
        UpdateEmojiInput("");
    }
    const double emojiMs = segment.Split();

    if (!g_english_input_mode && !IsSpecialModeCompositionActive(current_input) &&
        GetConfiguredKaomojiMixedInputEnabled() && (scheme == SchemeType::Quanpin || scheme == SchemeType::Shuangpin) &&
        !GlobalIme::composition.creating_word.active)
    {
        UpdateKaomojiInput(current_input, client_id, activation_epoch);
    }
    else
    {
        UpdateKaomojiInput("");
    }
    const double kaomojiMs = segment.Split();

    CAND_DIAG_LOGF(L"candidate build total_ms={:.1f} query_ms={:.1f} fixed_pos_ms={:.1f} branch_ms={:.1f} "
                   L"ui_ms={:.1f} english_ms={:.1f} emoji_ms={:.1f} kaomoji_ms={:.1f} items={}",
                   segment.TotalMs(), queryMs, fixedPosMs, branchMs, uiMs, englishMs, emojiMs, kaomojiMs, itemCount);
}
} // namespace FanyNamedPipe
