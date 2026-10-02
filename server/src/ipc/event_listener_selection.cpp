// 选词上屏：解析页内候选、等待候选窗渲染同步，以及 ProcessSelectionKey 的造词与调频学习。
#include "ipc/event_listener_internal.h"
#include <Windows.h>
#include <string>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include "ipc.h"
#include "ipc/candidate_render_sync.h"
#include "ipc/candidate_selection_policy.h"
#include "ipc/input_key_policy.h"
#include "utils/common_utils.h"
#include "global/globals.h"
#include "config/ime_config.h"
#include "engine/local_modes/quick_phrase_query.h"
#include "log/candidate_diag_log.h"

using namespace event_listener_detail;

namespace
{
// NeedToCreateWord 带光标变体。可选第 4 字段（offset into remaining_raw）必须以
// CompositionRestore 协商为前提：旧 DLL 的解析器把第 2 个 '\t' 之后的尾部整个当
// display_preedit，未协商时追加会污染 inline preedit（AC8），此时帧与旧 Server 的
// plain builder 字节一致。协商侧前缀选词结算后光标归后缀首（0），必须显式携带，
// 否则 DLL 按省略语义把光标镜到末尾。串尾造词流 caret 恒在末尾，不带字段。
std::wstring BuildCreateWordPipePayloadWithCaret(bool client_supports_restore,
                                                 const std::string &remaining_raw_input_with_cases,
                                                 const std::string &current_word)
{
    std::wstring payload = BuildCreateWordPipePayload(remaining_raw_input_with_cases, current_word);
    if (FanyImeIpc::ShouldCreateWordFrameCarryCaret(client_supports_restore, GlobalIme::composition.caret_position,
                                                    remaining_raw_input_with_cases.size()))
    {
        payload += L'\t' + std::to_wstring(GlobalIme::composition.caret_position);
    }
    return payload;
}

// 快捷短语组的调频：选中组里的一条让组前移，越过整组选了排在后面的普通候选让组后退。
// 编码要在 reset_state() 之前取；列表里有快捷短语就说明当前输入就是它们的编码。
void LearnQuickPhraseOrder(const WordItem &selected, size_t absolute_index, uint64_t client_id,
                           uint64_t activation_epoch)
{
    const auto &items = Global::candidate_ui.items;
    if (!GetConfiguredQuickPhraseFrequencyEnabled() || absolute_index >= items.size())
        return;
    const auto group = std::find_if(items.begin(), items.end(),
                                    [](const WordItem &item) { return item.source == CandidateSource::QuickPhrase; });
    if (group == items.end())
        return;
    const size_t group_index = static_cast<size_t>(group - items.begin());
    // Shift+K 模式的编码带大写 K 前缀，那里的快捷短语按前缀列出，不是混排组，不学槽位。
    const std::string code = g_inputSession->get_pinyin_sequence_with_cases();
    if (code.empty() || !std::all_of(code.begin(), code.end(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z'; }))
        return;
    if (selected.source == CandidateSource::QuickPhrase)
    {
        FanyNamedPipe::EnqueueLearnQuickPhraseOrderTask(code, selected.word, absolute_index == group_index, 0,
                                                        client_id, activation_epoch);
        return;
    }
    if (absolute_index < group_index || !metasequoia::local_modes::counts_toward_quick_phrase_slot(selected))
        return;
    const auto ordinary_rank = std::count_if(
        items.begin(), items.begin() + static_cast<std::ptrdiff_t>(absolute_index),
        [](const WordItem &item) { return metasequoia::local_modes::counts_toward_quick_phrase_slot(item); });
    FanyNamedPipe::EnqueueLearnQuickPhraseOrderTask(code, "", false, static_cast<int>(ordinary_rank), client_id,
                                                    activation_epoch);
}

// 混输英文槽位的调频，和快捷短语组同一套：选中占槽位的英文让它前移，越过它选了后面的候选让它后退。
// 槽位要在 reset_state() 之前取输入。专用英文模式和 Y 模式整页都是英文，不走槽位。
void LearnEnglishSlot(const WordItem &selected, size_t absolute_index, uint64_t client_id, uint64_t activation_epoch)
{
    const auto &items = Global::candidate_ui.items;
    if (!g_inputSession || g_english_input_mode || IsYModeInput(g_inputSession->get_pinyin_sequence_with_cases()) ||
        (g_inputSession->current_scheme_type() != SchemeType::Quanpin &&
         g_inputSession->current_scheme_type() != SchemeType::Shuangpin) ||
        absolute_index >= items.size())
        return;
    const auto english_index = FanyImeIpc::SlottedEnglishIndex(items);
    if (!english_index)
        return;
    const std::string code = FanyNamedPipe::EnglishInputKey();
    if (absolute_index == *english_index)
    {
        FanyNamedPipe::EnqueueLearnEnglishSlotTask(code, static_cast<int>(*english_index), std::nullopt, client_id,
                                                   activation_epoch);
        return;
    }
    if (absolute_index > *english_index && selected.source != CandidateSource::EnglishDictionary)
    {
        FanyNamedPipe::EnqueueLearnEnglishSlotTask(code, static_cast<int>(*english_index),
                                                   static_cast<int>(absolute_index), client_id, activation_epoch);
    }
}
} // namespace

namespace FanyNamedPipe
{
bool ResolveCandidateItem(int one_based_index, WordItem &item)
{
    if (!g_inputSession || one_based_index <= 0)
    {
        return false;
    }

    const auto &ui = Global::candidate_ui;
    const size_t indexInPage = static_cast<size_t>(one_based_index - 1);
    if (indexInPage >= ui.page_words.size() || ui.page_index < 0 || ui.page_size <= 0)
    {
        return false;
    }

    const size_t pageStart = static_cast<size_t>(ui.page_index) * static_cast<size_t>(ui.page_size);
    if (pageStart > ui.items.size() || indexInPage >= ui.items.size() - pageStart)
    {
        return false;
    }

    item = ui.items[pageStart + indexInPage];
    return true;
}

// A digit/space selection settles against the live page_words, while the user is looking at the
// asynchronously painted snapshot. Pin-frequency reorders the page after every commit, so a
// keystroke that lands between publish and paint would commit a candidate the user never saw.
// Block until the UI echoes back the generation it painted (Global::PublishRenderedCandidatePageGeneration
// wakes us immediately), with a hard bound so a wedged UI thread cannot hang input: on timeout the
// selection continues with the current page and the miss is recorded in the diagnostic log. Runs on
// the IPC worker thread only.
void WaitForCandidateRenderSync(UINT keycode)
{
    const bool uiless = IsUiLessMode();
    const auto renderLagsPublished = [uiless]() {
        return FanyImeIpc::ShouldWaitForCandidateRender(
            Global::rendered_candidate_page_generation.load(std::memory_order_acquire),
            Global::candidate_page_generation.load(std::memory_order_acquire), uiless,
            Global::candidate_window_rendered_visible.load(std::memory_order_acquire));
    };
    if (!renderLagsPublished())
    {
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    bool synced = false;
    {
        std::unique_lock lock(Global::candidate_render_mutex);
        synced = Global::candidate_render_cv.wait_for(
            lock, std::chrono::milliseconds(FanyImeIpc::kCandidateSelectionRenderWaitMaxMs),
            [&renderLagsPublished]() { return !renderLagsPublished(); });
    }
    const auto waitedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (!synced)
    {
        CAND_DIAG_LOGF(L"candidate-select-render-timeout keycode={} waited_ms={} current={} rendered={}", keycode,
                       waitedMs, Global::candidate_page_generation.load(std::memory_order_acquire),
                       Global::rendered_candidate_page_generation.load(std::memory_order_acquire));
        return;
    }
    CAND_DIAG_LOGF(L"candidate-select-render-synced keycode={} waited_ms={}", keycode, waitedMs);
}

void ProcessSelectionKey(UINT keycode, uint64_t client_id, uint64_t activation_epoch, int forced_index_in_page)
{
    /* 先清理一下状态 */
    Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;

    static bool isNeedUpdateWeight = false;
    isNeedUpdateWeight = false;

    if (g_translation_candidates_active)
    {
        // 译文页：选中的就是要上屏的完整文本，既不调频也不造词，更不能把译文当候选词学进库。
        if (forced_index_in_page < 0 && (keycode == VK_SPACE || (keycode >= '1' && keycode <= '9')))
        {
            WaitForCandidateRenderSync(keycode);
        }
        EnsureCandidatePageReady();
        auto &ui = Global::candidate_ui;
        const int index = forced_index_in_page >= 0
                              ? forced_index_in_page
                              : (keycode == VK_SPACE ? ui.selected_index_in_page : static_cast<int>(keycode - '1'));
        if (index < 0 || static_cast<size_t>(index) >= ui.page_words.size())
        {
            // 这一页没有这个序号：什么都不上屏，译文页原样留着。走 SELECT_BY_NUMBER 的
            // TSF 路径只认 Normal / OutofRange 两种回复，这里必须是后者。
            Global::MsgTypeToTsf = Global::DataFromServerMsgType::OutofRange;
            return;
        }
        // 同理，上屏必须回 Normal：_HandleCandidateFinalize 只在 Normal 时写入文本，
        // 而且它写的就是 candidate_string 本身，不会再补标点。
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::Normal;
        ui.selected_text = ui.page_words[index];
        return;
    }

    // Keyboard selection must match the painted page. Mouse clicks carry an explicit index
    // (forced_index_in_page >= 0) and keep the old behavior: waiting cannot restore the intent of a
    // click that targeted a candidate of a page that is no longer on screen.
    if (forced_index_in_page < 0 && (keycode == VK_SPACE || (keycode >= '1' && keycode <= '9')))
    {
        WaitForCandidateRenderSync(keycode);
    }

    EnsureCandidatePageReady();

    const bool is_space = keycode == VK_SPACE;
    const bool is_digit_selection = keycode >= '1' && keycode <= '9';
    const bool is_direct_selection = forced_index_in_page >= 0;
    const int index = is_direct_selection
                          ? forced_index_in_page
                          : (is_space ? Global::candidate_ui.selected_index_in_page : static_cast<int>(keycode - '1'));
    WordItem curWordItem;
    const int page_size =
        Global::candidate_ui.page_size > 0 ? Global::candidate_ui.page_size : GetConfiguredCandidatePageSize();
    const bool within_page_size = !is_digit_selection || index < page_size;
    const bool is_valid_selection = within_page_size && (is_direct_selection || is_space || is_digit_selection) &&
                                    index >= 0 && static_cast<size_t>(index) < Global::candidate_ui.page_words.size() &&
                                    ResolveCandidateItem(index + 1, curWordItem);

    if (is_valid_selection)
    {
        // Capture ranking keys before reset_state()/composition advance clears the
        // input sequence. CandidateDatabaseKey() consults get_pinyin_sequence(),
        // and for single-code lists (e.g. "n") it must keep item.pinyin ("na"/"nv")
        // rather than falling back to the one-letter context key.
        const auto ranking_keys = RankingKeysForCandidate(curWordItem);
        const std::string ranking_context_key = ranking_keys.first;
        const std::string ranking_entry_key = ranking_keys.second;
        // First-page first slot is already the default commit; space/mouse/digit
        // should only learn when the user picked something else.
        const bool is_first_page_first = Global::candidate_ui.page_index == 0 && index == 0;
        // 句中辅助码筛过的候选页不能当调频的参照：选中的词在这一页里往往已经排第一，而用户下次
        // 不敲辅助码时看到的是没筛过的那份排序。改用去掉约束后的候选给它挪位置，所以选的是第一个
        // 也照样调（空格、数字键、鼠标点选都走这里）。要在推进组合之前取，推进后输入串就短了。
        std::vector<WordItem> mid_sentence_ranking_candidates;
        if ((curWordItem.source == CandidateSource::Database || curWordItem.source == CandidateSource::UserDatabase) &&
            g_inputSession->has_mid_sentence_helpcode())
        {
            mid_sentence_ranking_candidates = g_inputSession->candidates_without_mid_sentence_helpcode();
        }
        const bool mid_sentence_ranking = !mid_sentence_ranking_candidates.empty();
        isNeedUpdateWeight = mid_sentence_ranking || !is_first_page_first;
        const size_t absolute_index =
            static_cast<size_t>(Global::candidate_ui.page_index) * static_cast<size_t>(Global::candidate_ui.page_size) +
            static_cast<size_t>(index);
        LearnQuickPhraseOrder(curWordItem, absolute_index, client_id, activation_epoch);
        LearnEnglishSlot(curWordItem, absolute_index, client_id, activation_epoch);
        Global::candidate_ui.selected_text = Global::candidate_ui.page_words[index];
        std::string curWord = curWordItem.word;
        std::string curWordPinyin = curWordItem.pinyin;
        if (curWordItem.source == CandidateSource::EnglishDictionary ||
            curWordItem.source == CandidateSource::QuickPhrase || curWordItem.source == CandidateSource::Emoji ||
            curWordItem.source == CandidateSource::Kaomoji || curWordItem.source == CandidateSource::DateTime ||
            curWordItem.source == CandidateSource::Generated)
        {
            Global::candidate_ui.selected_text =
                string_to_wstring(CandidateTextForOutput(GlobalIme::composition.creating_word.word + curWord));
            // 整句候选走的是这条提前返回的捷径，到不了下面 creating_word 的收尾逻辑，
            // 因此造好的词必须在这里落库，否则前缀 + 整句只上屏、学不到。没有前缀时
            // 整句自己就是要落库的那条词：它在词库里没有行，下面的调频改不到它。
            // 拼音两段都是 canonical quanpin（creating_word.pinyin 由
            // append_canonical_pinyin 累积，lattice 候选的 canonical_pinyin 是整句 key），
            // 直接按 '\'' 拼接即可；音节数与汉字数是否匹配由
            // create_word_from_canonical_pinyin 自行校验，不匹配时安全地拒绝入库。
            if (FanyImeIpc::ShouldStoreEarlyReturnPhrase(
                    curWordItem.source, GlobalIme::composition.creating_word.active,
                    GlobalIme::composition.creating_word.pinyin, curWordItem.canonical_pinyin))
            {
                const std::string &prefix_pinyin = GlobalIme::composition.creating_word.pinyin;
                // 前缀为空时不能带上那个分隔符，'na'yi'tiao 这种前导撇号会让整条读音作废。
                const std::string stored_pinyin = prefix_pinyin.empty()
                                                      ? curWordItem.canonical_pinyin
                                                      : prefix_pinyin + "'" + curWordItem.canonical_pinyin;
                // 这里异步处理，不然有可能会阻塞住 TSF 端读取 pipe 导致超时
                EnqueueStoreUserPhraseTask(stored_pinyin, GlobalIme::composition.creating_word.word + curWord,
                                           /*pinyin_is_canonical=*/true);
            }
            // 同理，这条捷径也到不了下面的 AI 上下文累积。造词前缀在它自己被选中的那次
            // ProcessSelectionKey 里已经追加过了，这里只补本次提交的这一段。
            AppendAiContext(curWord);
            if (curWordItem.source == CandidateSource::EnglishDictionary && isNeedUpdateWeight)
            {
                EnqueueAdjustCandidateRankingTask(/*english=*/true, /*wubi=*/false, EnglishRankingContextKey(),
                                                  curWordItem.pinyin, curWordItem.word, client_id, activation_epoch);
            }
            UpdateCloudInput("");
            UpdateEnglishInput("");
            UpdateMixedInput({});
            g_inputSession->reset_state();
            if (g_r_mode_original_session)
            {
                g_inputSession = g_r_mode_original_session;
                g_r_mode_original_session.reset();
            }
            GlobalIme::composition.clear();
            ClearSpecialModeTriggers();
            return;
        }
        std::string cloudCommittedPinyin;
        bool cloudCommittedPinyinIsCanonical = false;
        std::string aiCommittedPinyin;
        bool aiCommittedPinyinIsCanonical = false;
        if (curWordItem.source == CandidateSource::CloudSuggestion)
        {
            if (!curWordItem.canonical_pinyin.empty())
            {
                cloudCommittedPinyin = curWordItem.canonical_pinyin;
                cloudCommittedPinyinIsCanonical = true;
            }
            else if (g_inputSession->is_all_complete_pure_pinyin())
            {
                // 与 AI 联想同一个坑：committed_pinyin 走的是 normalized_input，丢掉了音节
                // 边界，create_word 会用贪心的 "correction" 切分重新断句，qi'e'huan 落成
                // qie'huan，音节数与字数对不上、do_validate 静默失败而不入库。整串是完整
                // 拼音时改用带撇号的 normalized_segmentation 作 canonical 键，按用户实际断句
                // 落库。简拼 / 带 helpcode 等非完整拼音的云候选仍走原来的 committed_pinyin
                // 路径（见下面的 else），避免回归。
                cloudCommittedPinyin = g_inputSession->get_pinyin_segmentation();
                cloudCommittedPinyinIsCanonical = true;
            }
            else
            {
                cloudCommittedPinyin = g_inputSession->get_cloud_query_state().committed_pinyin;
            }
        }
        if (curWordItem.source == CandidateSource::AiSuggestion)
        {
            if (g_inputSession->is_all_complete_pure_pinyin())
            {
                if (!curWordItem.canonical_pinyin.empty())
                {
                    aiCommittedPinyin = curWordItem.canonical_pinyin;
                    aiCommittedPinyinIsCanonical = true;
                }
                else
                {
                    // AI 候选自己不带 canonical_pinyin。committed_pinyin 走的是
                    // normalized_input，那串已经去掉了音节边界（见 quanpin_scheme 里
                    // 只往 normalized_input 里塞非撇号字符），create_word 会用贪心的
                    // "correction" 切分重新断句：qi'e'huan 落成 qie'huan，音节数与字数
                    // 对不上，do_validate 直接判失败、静默不入库，于是用户再打同样的音
                    // 时这条 AI 联想仍然要靠现场猜。这里改用带撇号的 normalized_segmentation
                    // （即屏幕上的分段，is_all_complete_pure_pinyin 已保证它整串是完整音节）
                    // 作 canonical 键，create_word_from_canonical_pinyin 按撇号 split、不再
                    // 重新贪心断句，用户实际选的那条读音就能正确落库。
                    aiCommittedPinyin = g_inputSession->get_pinyin_segmentation();
                    aiCommittedPinyinIsCanonical = true;
                }
            }
            isNeedUpdateWeight = false;
        }
        // 这次选择之前是否已经在造词。下面的造词收尾会清掉这个标志，之后就问不出来了。
        const bool was_creating_word = GlobalIme::composition.creating_word.active;
        // R5 前缀选词：候选来自光标前缀时，advance 消耗的正是前缀本身，剩余 raw 就是
        // 后缀。必须在 advance 之前判定（advance 会缩短 raw）；结算把会话光标复位为
        // 「后缀整串转换」（nullopt），组合态光标归后缀首，之后由编辑键按新光标重新
        // 接管前缀语义。该状态只可能由门控内的编辑键产生（未协商/UILess 的光标从不
        // 进会话），因此无需重复门控。云/英文/表情等特殊候选在上方提前返回，不会进入
        // 这里。
        // 用户眼前这一页的身份也取在 advance 之前：撤销重建的页面只有前缀一致时才
        // 装着同一批候选，记录的位置才有意义。
        const std::string selection_page_prefix = FanyImeIpc::NormalizeCandidatePagePrefix(
            g_inputSession->get_pinyin_sequence_with_cases(), g_inputSession->prefix_end());
        const bool caret_prefix_selection =
            g_inputSession->prefix_end() < g_inputSession->get_pinyin_sequence_with_cases().size();
        auto selection_transition = g_inputSession->advance_composition_after_selection(
            curWordPinyin, curWord, curWordItem.canonical_pinyin, curWordItem.scheme);
        if (caret_prefix_selection)
        {
            g_inputSession->set_caret(std::nullopt);
            g_inputSession->recompute_candidates();
            GlobalIme::composition.caret_position = 0;
        }
        // A cloud suggestion is an already-composed result returned for the
        // current query.  It must commit as one candidate even when the
        // returned query spelling is shorter than the raw input (for example
        // with an abbreviation or an active help-code suffix).  Treating it
        // like an ordinary partial candidate enters word-creation mode, while
        // the cloud branch below still persists the selected word.
        const bool isNeedCreateWord =
            FanyImeIpc::ShouldEnterCreatingWord(curWordItem.source, selection_transition.continues_composition);
        if (isNeedCreateWord)
        { /* 候选只消耗了输入的一部分，继续使用剩余输入造词。完整拼音和简拼均可进入。 */
            // Snapshot the state the user is leaving before this selection
            // overwrites it. The engine's current raw cannot serve as the
            // snapshot: it still contains the remaining suffix, which the user
            // may delete before asking to retract this segment. The picked
            // candidate position travels with it, together with the page prefix
            // it was recorded on, so a retraction can put that item back under
            // the highlight.
            GlobalIme::composition.push_selection_snapshot(selection_transition.consumed_raw_input_with_cases,
                                                           Global::candidate_ui.page_index * page_size + index,
                                                           selection_page_prefix);
            /* 打开造词开关 */
            GlobalIme::composition.creating_word.active = true;
            Global::MsgTypeToTsf = Global::DataFromServerMsgType::NeedToCreateWord;
            GlobalIme::composition.segmented_pinyin = selection_transition.current_segmentation_with_cases;

            PrepareCandidateList(client_id, activation_epoch);
        }

        // 详细处理一下造词的逻辑
        if (GlobalIme::composition.creating_word.active)
        {
            /* 造词的时候，不可以更新词频 */
            isNeedUpdateWeight = false;

            const auto creating_word_progress = g_inputSession->update_creating_word_progress(
                GlobalIme::composition.creating_word.pinyin, GlobalIme::composition.creating_word.word, curWord,
                selection_transition);
            GlobalIme::composition.creating_word.pinyin = creating_word_progress.pinyin;
            GlobalIme::composition.creating_word.word = creating_word_progress.word;
            GlobalIme::composition.creating_word.preedit = creating_word_progress.preedit;
            /* 更新一下中间态的造词时 tsf 端所需的数据 */
            Global::candidate_ui.selected_text = BuildCreateWordPipePayloadWithCaret(
                ClientNegotiatedCompositionRestore(client_id), g_inputSession->get_pinyin_sequence_with_cases(),
                GlobalIme::composition.creating_word.word);
            if (creating_word_progress.completed)
            { /* 最终的造词 */
#ifdef FANY_DEBUG
                (void)0;
#endif

                /* 更新一下被选中的候选项 */
                Global::candidate_ui.selected_text =
                    string_to_wstring(CandidateTextForOutput(GlobalIme::composition.creating_word.word));

                if (creating_word_progress.can_store)
                {
                    // 这里异步处理，不然有可能会阻塞住 TSF 端读取 pipe 导致超时
                    EnqueueStoreUserPhraseTask(GlobalIme::composition.creating_word.pinyin,
                                               GlobalIme::composition.creating_word.word,
                                               /*pinyin_is_canonical=*/true);
                }

                /* 清理 */
                GlobalIme::composition.clear_creating_word();
            }
        }

        // Google 解码器那条整句（Fallback）不在上面提前返回的名单里，走的是这条普通路径，
        // 但它和词格整句一样是猜出来的：词库里没有它那一行，下面 isNeedUpdateWeight 要改的
        // 行根本不存在。所以它独立上屏时也要落库。接在造词前缀后面的那种由上面的造词收尾
        // 负责（creating_word_progress.can_store），这里不重复存。
        if (!isNeedCreateWord && !was_creating_word &&
            FanyImeIpc::ShouldStoreStandaloneSentence(curWordItem.source, curWordItem.canonical_pinyin))
        {
            EnqueueStoreUserPhraseTask(curWordItem.canonical_pinyin, curWord, /*pinyin_is_canonical=*/true);
        }

        // 看看云联想出来的词是否需要被插入到数据库
        if (curWordItem.source == CandidateSource::CloudSuggestion && !cloudCommittedPinyin.empty())
        {
            EnqueueStoreUserPhraseTask(cloudCommittedPinyin, curWord, cloudCommittedPinyinIsCanonical);
            // 清理云联想变量状态
            Global::cloud_candidate.added = false;
            Global::cloud_candidate.word.clear();
            Global::cloud_candidate.pinyin.clear();
        }
        if (curWordItem.source == CandidateSource::AiSuggestion && !aiCommittedPinyin.empty())
        {
            EnqueueStoreUserPhraseTask(aiCommittedPinyin, curWord, aiCommittedPinyinIsCanonical);
            Global::ai_candidate = {};
        }

        AppendAiContext(curWord);

        if (!isNeedCreateWord)
        {
            g_inputSession->reset_state();
            if (g_r_mode_original_session)
            {
                g_inputSession = g_r_mode_original_session;
                g_r_mode_original_session.reset();
            }
            GlobalIme::composition.caret_position = 0;
            GlobalIme::composition.raw_input_with_cases.clear();
            // The composition is over; stale snapshots must not survive into the
            // next one where they could restore an unrelated spelling.
            GlobalIme::composition.selection_history.clear();
            ClearSpecialModeTriggers();
        }
        else
        {
            // 组合继续时同步剩余 raw。HandleImeKey 在选词之前就写过它，不同步的话下一次
            // 编辑键里「raw 变了且 caret==0 → 光标移到串尾」的判定会误触发：前缀选词后
            // caret 已按造词帧的 caret 字段归 0，DLL 光标停在后缀首，Server 却跳到串尾，
            // 之后的插入与移动两侧分叉。
            GlobalIme::composition.raw_input_with_cases = g_inputSession->get_pinyin_sequence_with_cases();
            /* TODO: 这里到 main 线程的时候，可能下面的那个清理状态的操作已经执行了，因此，这里可能会导致 string
             * 越界的问题 */
            RequestShowCandidateWindow();
        }

        if (isNeedUpdateWeight)
        {
            // Written on the dictionary writer thread, off the keystroke path. Once the write
            // changes the order, the worker drops its candidate cache and the next lookup sees
            // the new order; a lookup that lands before the write finishes still shows the old
            // one.
            EnqueueAdjustCandidateRankingTask(/*english=*/false, /*wubi=*/curWordItem.scheme == SchemeType::Wubi,
                                              ranking_context_key, ranking_entry_key, curWord, client_id,
                                              activation_epoch,
                                              mid_sentence_ranking ? &mid_sentence_ranking_candidates : nullptr);
        }
    }
    else
    {
        Global::candidate_ui.selected_text = L"OutofRange";
        Global::MsgTypeToTsf = Global::DataFromServerMsgType::OutofRange;
        // With the raw spelling gone, the only composition text left is the
        // accumulated word, and TSF's empty-buffer finalize commits exactly that
        // text. So an out-of-range selection here really ends the composition:
        // drop the creating-word state instead of leaving one that the next key
        // would resurrect as a duplicate preedit prefix.
        if (GlobalIme::composition.creating_word.active && g_inputSession->get_pinyin_sequence_with_cases().empty())
        {
            ClearState();
        }
    }
}
} // namespace FanyNamedPipe
