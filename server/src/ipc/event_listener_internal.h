#pragma once

// event_listener*.cpp 之间共享的内部声明：拆分前同在 event_listener.cpp 里、现在跨文件使用的状态、类型与函数。
// 只给 server/src/ipc/event_listener*.cpp 包含，其他地方不要引用。

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "ipc.h"
#include "ipc/async_request_origin.h"
#include "ipc/candidate_selection_policy.h"
#include "ipc/candidate_ui_owner.h"
#include "ipc/event_listener.h"
#include "window/caret_state_indicator_policy.h"

// IPC logging is compiled out. The macros still have to *mention* their arguments, otherwise every
// parameter of the log helpers below is unreferenced (C4100). sizeof keeps the arguments in an
// unevaluated context, so nothing is computed and no side effect runs — only the name is used.
template <typename... Args> int FanyIpcDiscardLogArgs(const Args &...);
#define FANY_IPC_LOG_RAW(message) ((void)sizeof(FanyIpcDiscardLogArgs(message)))
#define FANY_IPC_LOGW(message) ((void)sizeof(FanyIpcDiscardLogArgs(message)))
#define FANY_IPC_LOGF(...) ((void)sizeof(FanyIpcDiscardLogArgs(__VA_ARGS__)))

namespace event_listener_detail
{
using AsyncRequestOrigin = FanyImeIpc::AsyncRequestOrigin;

extern bool g_quick_phrase_triggered;
extern bool g_unicode_mode_triggered;
extern bool g_date_time_mode_triggered;
extern bool g_emoji_mode_triggered;
extern bool g_kaomoji_mode_triggered;
extern bool g_jianpin_mode_triggered;
extern bool g_y_mode_triggered;
extern bool g_r_mode_triggered;
extern std::shared_ptr<IInputSession> g_r_mode_original_session;
extern bool g_english_input_mode;
// Glosses are a cache keyed by TranslationIdentity, not the current page's
// results: the next keystroke's page mostly repeats the same words, and
// rebuilding it without their glosses first shrank every row, then grew it back
// once the lookup answered. Bounded like the cloud cache, and dropped whenever
// the provider set changes (the target language is already in the identity).
constexpr size_t kMaxCandidateTranslationGlosses = 2048;
extern std::unordered_map<std::string, std::string> g_candidate_translation_glosses;
extern std::string g_candidate_translation_signature;
extern bool g_translation_candidates_active;
extern std::vector<WordItem> g_translation_saved_items;
extern int g_translation_saved_page_index;
extern int g_translation_saved_selected_index;
extern bool g_dedicated_english_answer_pending;
extern int g_authoritative_cn_mode;

std::string TranslationIdentity(const EnglishIme::TranslationQuery &query);
bool IsUiLessMode();
void ApplyUiLessFromPacket(const FanyImeNamedpipeData &pipe_data);
void RequestShowCandidateWindow();
void HideCandidateWindowAndDropItems();
bool IsQuickPhraseCompositionActive(const std::string &raw);
bool IsUnicodeCompositionActive(const std::string &raw);
bool IsDateTimeCompositionActive(const std::string &raw);
bool IsEmojiCompositionActive(const std::string &raw);
bool IsKaomojiCompositionActive(const std::string &raw);
bool IsJianpinCompositionActive(const std::string &raw);
bool IsYModeCompositionActive(const std::string &raw);
bool IsYModeInput(const std::string &raw);
void ClearSpecialModeTriggers();
bool IsSpecialModeCompositionActive(const std::string &raw);

void PublishCandidateUiOwner(uint64_t client_id, uint64_t activation_epoch);
FanyImeIpc::CandidateUiOwner SnapshotCandidateUiOwner();
bool CandidateUiOwnerIsCurrent(const FanyImeIpc::CandidateUiOwner &owner);
void PublishStatusSnapshotValue(int packed_state);
void SetEnglishInputMode(bool enabled);
void RememberClientStatusSnapshot(uint64_t client_id, int packed_state);
void ForgetClientStatusSnapshot(uint64_t client_id);
int RecallClientStatusSnapshot(uint64_t client_id);
void PostCaretStateBadge(FanyImeUi::CaretStateBadge badge, int x, int y);

void UpdateCloudInput(const std::string &input, uint64_t client_id = 0, uint64_t activation_epoch = 0);
void UpdateEnglishInput(const std::string &input, uint64_t client_id = 0, uint64_t activation_epoch = 0,
                        bool dedicated_mode = false);
// 混输（英文、emoji、颜文字、日期时间）的查询请求；request.input 为空表示取消。
void UpdateMixedInput(MixedCandidates::Request request, uint64_t client_id = 0, uint64_t activation_epoch = 0);
void UpdateAiInput(const std::string &identity, uint64_t client_id = 0, uint64_t activation_epoch = 0);
AsyncRequestOrigin FindCloudRequestOrigin(const std::string &input, uint64_t generation);
AsyncRequestOrigin FindEnglishRequestOrigin(const std::string &input, uint64_t generation);
AsyncRequestOrigin FindMixedRequestOrigin(const std::string &input, uint64_t generation);
AsyncRequestOrigin FindAiRequestOrigin(const std::string &input, uint64_t generation);
std::string CandidateTextForOutput(const std::string &text);
void AppendAiContext(const std::string &committed_word);
std::wstring BuildCreateWordPipePayload(const std::string &remaining_raw_input_with_cases,
                                        const std::string &current_word);

// event_listener_candidates.cpp
void EnsureCandidatePageReady();
std::wstring BuildUiLessCandidatePageW();
void RefreshCandidatePageUi(bool show_window);

// event_listener_pipes.cpp
void WakeNamedPipeListenersForShutdown();
} // namespace event_listener_detail

namespace FanyNamedPipe
{
enum class TaskType
{
    ShowCandidate,
    HideCandidate,
    HideCaretState,
    MoveCandidate,
    ImeKeyEvent,
    LangbarRightClick,
    IMESwitch,
    PuncSwitch,
    DoubleSingleByteSwitch,
    ApplyCloudCandidate,
    ApplyAiCandidate,
    ApplyEnglishCandidates,
    ApplyCandidateTranslations,
    ApplyMixedCandidates,
    StoreUserPhrase,
    ClientActivated,
    ClientDeactivated,
    ClientSuspended,
    StatusSnapshot,
    UiCommitCandidate,
    UiPinCandidate,
    UiDeleteCandidate,
    UiFixCandidatePosition,
    UiClearCandidatePosition,
    UiPageUp,
    UiPageDown,
    ReloadInputSession,
    EnsureInputSessionMatchesConfig,
    ApplyCandidatePageSize,
    RefreshCandidatePage,
    ResetInputSessionCache,
    ExitEnglishInputMode,
    // 神经整句重排在后台线程里算完了，候选顺序需要就地更新一次。
    ApplyRescoredOrder,
};

enum class PageMoveResult
{
    Unchanged,
    // An expansion filled out the current page; the page index did not move.
    CurrentPageRefilled,
    Moved,
};

// event_listener_candidates.cpp
std::string CurrentRankingContextKey();
std::string EnglishRankingContextKey();
// 混输英文槽位：按小写输入记（Y 模式去掉前缀），是英文在候选列表里的下标，上限是首页末位。
std::string EnglishInputKey();
int EnglishSlotMaximum();
FanyImeIpc::EnglishPlacement CurrentEnglishPlacement(const std::vector<WordItem> &items);
bool ExpandCandidatesKeepingPagePosition();
PageMoveResult MoveCandidatePage(int offset);
std::pair<std::string, std::string> RankingKeysForCandidate(const WordItem &item);

// event_listener_worker.cpp
void NoteTopCommitPushed(uint64_t client_id, uint64_t activation_epoch);
void EnqueueTask(TaskType type, const FanyImeNamedpipeData &pipeData, uint64_t activation_epoch);
// wubi 指明目标候选来自五笔码表；混输组合里拼音候选写拼音词典、五笔候选写码表，不能按会话方案判断。
// ranking_candidates 为空时按当前候选页排位；句中辅助码组合传去掉约束后的候选。
void EnqueueAdjustCandidateRankingTask(bool english, bool wubi, const std::string &context_key,
                                       const std::string &entry_key, const std::string &word, uint64_t client_id,
                                       uint64_t activation_epoch,
                                       const std::vector<WordItem> *ranking_candidates = nullptr);
void EnqueueLearnEnteredEnglishWordTask(const std::string &word);
// 快捷短语组的调频。word 非空：选中了组里的这一条，first_in_group 表示它本来就排在组首；
// word 为空：越过整组选了普通候选，ordinary_rank 是它在普通候选里的名次。
void EnqueueLearnQuickPhraseOrderTask(const std::string &code, const std::string &word, bool first_in_group,
                                      int ordinary_rank, uint64_t client_id, uint64_t activation_epoch);
// 混输英文槽位的调频。selected_index 为空：选中了占槽位的英文（english_index），按名次前移；
// 否则越过了英文选了 selected_index 处的候选，英文后退一位。
void EnqueueLearnEnglishSlotTask(const std::string &code, int english_index, std::optional<int> selected_index,
                                 uint64_t client_id, uint64_t activation_epoch);

// event_listener.cpp
bool SendUiLessCompositionToClient(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id);

// 拆分前 event_listener.cpp 里的前置声明，定义分布在各个 event_listener*.cpp。
void PrepareCandidateList(uint64_t client_id, uint64_t activation_epoch);
void HandleImeKey(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id);
void ClearState();
void ProcessSelectionKey(UINT keycode, uint64_t client_id, uint64_t activation_epoch, int forced_index_in_page = -1);
void WaitForCandidateRenderSync(UINT keycode);
void ApplyCloudCandidate(const std::string &candidate, const std::string &pinyin, uint64_t generation,
                         const std::optional<metasequoia::OnlineQuery> &query);
void ApplyRescoredOrder();
void ApplyAiCandidate(const std::string &candidate, const std::string &identity, uint64_t generation,
                      const std::optional<metasequoia::OnlineQuery> &query);
void ApplyEnglishCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation);
void ApplyCandidateTranslations(std::vector<EnglishIme::TranslationResult> results, uint64_t generation, bool merge);
void ApplyMixedCandidates(MixedCandidates::Result result, const std::string &input, uint64_t generation);
void EnqueueStoreUserPhraseTask(const std::string &pinyin, const std::string &word, bool pinyin_is_canonical = false);
bool ResolveCandidateItem(int one_based_index, WordItem &item);
bool SendCurrentDataToClient(uint64_t client_id, uint64_t activation_epoch, uint64_t request_id);
} // namespace FanyNamedPipe
