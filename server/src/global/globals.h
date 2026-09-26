#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <tuple>
#include <vector>
#include <windows.h>
#include "engine/core/word_item.h"
#include "window/candidate_view_model.h"

namespace GlobalIme
{
inline std::wstring AppName = L"metasequoiaime";
inline std::wstring ServerName = L"metasequoiaime";
inline std::unordered_set<WCHAR> PUNC_SET = {
    L'`', //
    L'!', //
    L'@', //
    L'#', //
    L'$', //
    L'%', //
    L'^', //
    L'&', //
    L'*', //
    L'(', //
    L')', //
    // L'-',  //
    L'_', //
    // L'=',  //
    // L'+',  //
    L'[',  //
    L']',  //
    L'\\', //
    L';',  //
    L':',  //
    L'\'', //
    L'"',  //
    L',',  //
    L'<',  //
    L'.',  //
    L'>',  //
    L'?'   //
};

struct CreatingWordState
{
    std::string preedit;
    std::string pinyin;
    std::string word;
    bool active = false;

    void clear()
    {
        preedit.clear();
        pinyin.clear();
        word.clear();
        active = false;
    }
};

struct CreatingWordSnapshot
{
    // Raw spelling the selection consumed, in the user's original form. It is the
    // raw the engine returns to when the last selected segment is retracted.
    std::string consumed_raw_input_with_cases;
    // The word accumulated before that selection. On the first selection this is
    // an inactive empty state, so retracting it leaves a plain pinyin composition.
    CreatingWordState previous_creating_word;
    // Absolute candidate position (page * page_size + index_in_page) the user
    // picked for this segment. Retraction re-highlights that item on the rebuilt
    // page, so the next key can re-pick or change it without hunting (Rime's
    // reopen "keeps selection"). -1 when unknown.
    int selected_absolute_index = -1;
    // Normalized prefix the picked page was built for (see
    // FanyImeIpc::NormalizeCandidatePagePrefix). The rebuilt page only holds the
    // same items when it decodes the same prefix; the retraction compares the two
    // before re-applying the recorded position.
    std::string selected_page_prefix;
    // Rime's selected_before_editing: a character typed after this selection
    // locks it, and Backspace then goes back to deleting characters so the new
    // input stays editable. Caret moves deliberately do not set it -- the caret
    // may stand anywhere, retraction still wins.
    bool raw_edited_after_selection = false;
};

// One-shot hand-off from restore_last_selection to the frame builder: the
// recorded candidate position plus the page it belongs to, so the caller only
// re-highlights after rebuilding a page with the same prefix.
struct RestoredSelectionHighlight
{
    int absolute_index = -1;
    std::string page_prefix;
};

struct CompositionState
{
    std::string segmented_pinyin;
    std::string raw_input_with_cases;
    size_t caret_position = 0;
    CreatingWordState creating_word;
    // One entry per selection that entered the creating-word state, newest last.
    // Retracting pops the newest entry and restores it, which is why an empty
    // history must disable retraction entirely.
    std::vector<CreatingWordSnapshot> selection_history;
    // One-shot hand-off: the absolute candidate position the newest restore wants
    // re-highlighted after the caller rebuilds the page, recorded with the page
    // prefix it belongs to. Set by restore_last_selection, consumed (and reset)
    // by the caller.
    RestoredSelectionHighlight restored_selection_highlight;

    void clear()
    {
        segmented_pinyin.clear();
        raw_input_with_cases.clear();
        caret_position = 0;
        creating_word.clear();
        selection_history.clear();
        restored_selection_highlight = {};
    }

    void clear_creating_word()
    {
        // The composition is still alive (a longer phrase is possible), so the
        // selection history stays; only the finished word resets.
        creating_word.clear();
    }

    // Record the state the user is leaving when a selection continues the word.
    // An empty raw spelling means the candidate came from a source that consumed
    // no typed input, so there is nothing this selection could ever retract.
    // selected_absolute_index is where the picked candidate sat in the
    // pre-selection list (page * page_size + index_in_page) and
    // selected_page_prefix is the normalized prefix that list was built for, so a
    // restore can re-highlight that item once it rebuilt the same page.
    void push_selection_snapshot(const std::string &consumed_raw_input_with_cases, int selected_absolute_index = -1,
                                 const std::string &selected_page_prefix = std::string())
    {
        if (consumed_raw_input_with_cases.empty())
        {
            return;
        }
        selection_history.push_back(
            {consumed_raw_input_with_cases, creating_word, selected_absolute_index, selected_page_prefix, false});
    }

    // Typing a character after a selection locks the newest snapshot: Backspace
    // must then keep deleting the freshly typed input instead of retracting the
    // selection out from under it (Rime's selected_before_editing guard). Only
    // the newest snapshot is tagged, matching Rime: a restore pops it and the
    // snapshot below becomes the newest untagged decision again.
    void note_raw_inserted()
    {
        if (!selection_history.empty())
        {
            selection_history.back().raw_edited_after_selection = true;
        }
    }

    bool last_selection_raw_edited() const
    {
        return !selection_history.empty() && selection_history.back().raw_edited_after_selection;
    }

    RestoredSelectionHighlight take_restored_selection_highlight()
    {
        RestoredSelectionHighlight highlight = std::move(restored_selection_highlight);
        restored_selection_highlight = {};
        return highlight;
    }

    // Restore the newest snapshot: the raw spelling that selection consumed, the
    // word accumulated before it, and the previously picked candidate position.
    // Returns false when there is nothing to retract.
    //
    // One placement rule regardless of the caret: the spelling goes back in front
    // of the untouched suffix (raw only ever holds what follows the newest
    // selection), and the caret shifts by the restored length, keeping its
    // distance to the suffix. That is Rime's "input grows, the caret does not
    // move" translated to this shrinking-raw model: caret 0 (standing behind the
    // word) lands at the end of the restored word, caret at the end of the raw
    // stays at the end, so the next Backspace deletes through the suffix first.
    bool restore_last_selection()
    {
        if (selection_history.empty())
        {
            return false;
        }
        CreatingWordSnapshot snapshot = std::move(selection_history.back());
        selection_history.pop_back();
        const std::size_t restored_length = snapshot.consumed_raw_input_with_cases.size();
        raw_input_with_cases.insert(0, snapshot.consumed_raw_input_with_cases);
        caret_position += restored_length;
        creating_word = std::move(snapshot.previous_creating_word);
        restored_selection_highlight = {snapshot.selected_absolute_index, std::move(snapshot.selected_page_prefix)};
        return true;
    }

    // Drop the newest snapshot without restoring its spelling: the accumulated
    // word returns to its pre-selection state while the raw stays empty. This is
    // the Ctrl+Backspace semantics for a selected segment -- the user asked to
    // delete it, not to edit its pinyin again -- so candidates are not rebuilt
    // from the consumed input. Returns false when there is nothing to drop.
    bool drop_last_selection()
    {
        if (selection_history.empty())
        {
            return false;
        }
        CreatingWordSnapshot snapshot = std::move(selection_history.back());
        selection_history.pop_back();
        creating_word = std::move(snapshot.previous_creating_word);
        return true;
    }
};

inline CompositionState composition;
} // namespace GlobalIme

namespace CandidateUi
{
inline std::string NumHanSeparator = " "; // Number and Hanzi separator
} // namespace CandidateUi

namespace Global
{
inline LONG INVALID_Y = -100000;
inline int MarginTop = 0;
// Horizontal offset of the opaque card inside the stable (often 720 DIP-wide) host.
inline int MarginLeft = 0;

// Candidate pages are published by the IPC worker thread but painted by the UI thread. The worker
// stamps every published page (candidate_page_generation), the UI echoes back the generation it
// actually painted (rendered_candidate_page_generation), and a digit/space selection waits for that
// echo before settling against page_words. Without it, pin-frequency reorders the page between
// publish and paint and the selection commits a candidate the user never saw. 0 means "nothing
// published/rendered yet".
inline std::atomic<std::uint64_t> candidate_page_generation{0};
inline std::atomic<std::uint64_t> rendered_candidate_page_generation{0};
// Mirrors ::is_global_wnd_cand_shown so the worker thread can cheaply tell whether an on-screen
// candidate list exists. Kept in lockstep at every write site of the plain flag.
inline std::atomic<bool> candidate_window_rendered_visible{false};
// Wakes a selection waiting for the render echo the moment it lands. Polling with sleep_for(1ms)
// sleeps a whole timer tick (~15.6ms by default, worse on battery), which turned a short paint lag
// into a 30-47ms stall inside the TSF reply budget. The mutex only orders the store against the
// waiter's predicate check so a notification cannot slip in between.
inline std::mutex candidate_render_mutex;
inline std::condition_variable candidate_render_cv;

inline void PublishRenderedCandidatePageGeneration(std::uint64_t generation)
{
    {
        std::lock_guard lock(candidate_render_mutex);
        rendered_candidate_page_generation.store(generation, std::memory_order_release);
    }
    candidate_render_cv.notify_all();
}

inline void SetCandidateWindowRenderedVisible(bool visible)
{
    {
        std::lock_guard lock(candidate_render_mutex);
        candidate_window_rendered_visible.store(visible, std::memory_order_relaxed);
    }
    candidate_render_cv.notify_all();
}

using CandidateWordItem = WordItem;

struct CandidateUiState
{
    std::vector<CandidateWordItem> items;
    std::vector<CandidateViewItem> page_views;
    std::vector<std::wstring> page_words;
    std::vector<std::wstring> page_glosses;
    std::wstring selected_text = L"";
    int page_size = 8;
    int page_index = 0;
    int selected_index_in_page = 0;
    int item_total_count = 0;
    int cur_page_max_word_len = 2;
    int cur_page_item_cnt = 8;
    bool is_num_out_of_range = false;

    void set_items(std::vector<CandidateWordItem> new_items)
    {
        items = std::move(new_items);
        item_total_count = static_cast<int>(items.size());
        page_index = 0;
        selected_index_in_page = 0;
        clear_page();
    }

    void clear_page()
    {
        page_views.clear();
        page_words.clear();
        page_glosses.clear();
        selected_text.clear();
        cur_page_item_cnt = 0;
        cur_page_max_word_len = 2;
    }

    void select_first_on_page()
    {
        selected_index_in_page = 0;
    }

    bool move_selection(int offset)
    {
        if (page_size <= 0)
        {
            page_index = 0;
            selected_index_in_page = 0;
            return false;
        }
        const int count = current_page_count();
        if (count <= 0)
        {
            selected_index_in_page = 0;
            return false;
        }
        selected_index_in_page = std::clamp(selected_index_in_page, 0, count - 1);
        const int next = current_page_start() + selected_index_in_page + offset;
        if (next < 0 || next >= item_total_count)
        {
            return false;
        }
        page_index = next / page_size;
        selected_index_in_page = next % page_size;
        return true;
    }

    int current_page_start() const
    {
        return page_index * page_size;
    }

    int current_page_count() const
    {
        const int remaining = item_total_count - current_page_start();
        if (remaining <= 0)
        {
            return 0;
        }
        return remaining < page_size ? remaining : page_size;
    }

    bool has_prev_page() const
    {
        return page_index > 0;
    }

    bool has_next_page() const
    {
        return page_index < (item_total_count - 1) / page_size;
    }

    bool is_current_page_full() const
    {
        return page_size > 0 && current_page_count() == page_size;
    }

    bool is_next_page_partial_last_page() const
    {
        if (page_size <= 0 || item_total_count <= 0 || item_total_count % page_size == 0)
        {
            return false;
        }
        const int last_page = (item_total_count - 1) / page_size;
        return page_index + 1 == last_page;
    }

    bool is_selection_at_current_page_end() const
    {
        const int count = current_page_count();
        return count > 0 && selected_index_in_page + 1 >= count;
    }

    bool is_selection_at_last_candidate() const
    {
        if (page_size <= 0 || item_total_count <= 0 || current_page_count() <= 0)
        {
            return false;
        }
        const int selected = std::clamp(selected_index_in_page, 0, current_page_count() - 1);
        return current_page_start() + selected == item_total_count - 1;
    }
};
inline CandidateUiState candidate_ui;

// candidate_ui belongs to the IPC worker thread, which rebuilds and destroys its vectors while the UI thread paints;
// the handoff is an asynchronous PostMessage, so nothing serialises the two. The worker therefore copies each finished
// page into one of these immutable snapshots and publishes it atomically, and every UI-thread reader (candidate
// presenter, its mouse callbacks, the WebView2 payload) loads a snapshot once and reads only from that copy.
struct CandidatePageSnapshot
{
    std::vector<CandidateViewItem> page_views;
    std::vector<std::wstring> page_words;
    std::wstring candidate_string;
    int selected_index_in_page = 0;
    // page_count mirrors current_page_count() and page_item_count mirrors cur_page_item_cnt. Both are carried because
    // the height estimate prefers the derived count and only falls back to the rendered one.
    int page_count = 0;
    int page_item_count = 0;
    // Generation stamped by the worker at publish time. The UI thread echoes this exact value back
    // after painting, which is what lets a selection distinguish "what the user sees" from "what
    // the worker has just rebuilt".
    std::uint64_t generation = 0;
};

using CandidatePageSnapshotPtr = std::shared_ptr<const CandidatePageSnapshot>;

inline std::mutex &CandidatePageSnapshotMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline CandidatePageSnapshotPtr &CandidatePageSnapshotStorage()
{
    static CandidatePageSnapshotPtr snapshot = std::make_shared<const CandidatePageSnapshot>();
    return snapshot;
}

inline CandidatePageSnapshotPtr LoadCandidatePageSnapshot()
{
    std::lock_guard lock(CandidatePageSnapshotMutex());
    return CandidatePageSnapshotStorage();
}

inline void PublishCandidatePageSnapshot(CandidatePageSnapshotPtr snapshot)
{
    std::lock_guard lock(CandidatePageSnapshotMutex());
    CandidatePageSnapshotStorage() = std::move(snapshot);
}

inline void ClearCandidatePageSnapshot()
{
    PublishCandidatePageSnapshot(std::make_shared<const CandidatePageSnapshot>());
}

//
// 云候选
//
struct CloudCandidate
{
    bool added = false;
    std::string word;
    std::string pinyin;
};
inline CloudCandidate cloud_candidate;
struct AiCandidate
{
    bool added = false;
    std::string word;
    std::string pinyin;
};
inline AiCandidate ai_candidate;
} // namespace Global

namespace GlobalSettings
{
//
// 支持的 TSF 预编辑格式，这里是为了和 TSF 端进行同步
//  - raw: 原始按键序列
//  - pinyin: 分词后的拼音序列
//  - empty: 行内不显示预编辑
//  - cand: 当前高亮的候选词序列（预留）
//
namespace TsfPreeditStyle
{
constexpr std::string_view Raw = "raw";
constexpr std::string_view Pinyin = "pinyin";
constexpr std::string_view Empty = "empty";
constexpr std::string_view Cand = "cand";
} // namespace TsfPreeditStyle

inline bool isKnownTsfPreeditStyle(std::string_view style)
{
    return style == TsfPreeditStyle::Raw || style == TsfPreeditStyle::Pinyin || style == TsfPreeditStyle::Empty;
}

inline std::string normalizeTsfPreeditStyle(std::string_view style)
{
    if (style == TsfPreeditStyle::Pinyin || style == TsfPreeditStyle::Empty)
    {
        return std::string(style);
    }
    return std::string(TsfPreeditStyle::Raw);
}

inline std::string &tsfPreeditStyleStorage()
{
    static std::string style = std::string(TsfPreeditStyle::Raw); // 默认的原始按键序列
    return style;
}

inline const std::string &getTsfPreeditStyle()
{
    return tsfPreeditStyleStorage();
}

inline void setTsfPreeditStyle(std::string_view newStyle)
{
    tsfPreeditStyleStorage() = normalizeTsfPreeditStyle(newStyle);
}
} // namespace GlobalSettings
