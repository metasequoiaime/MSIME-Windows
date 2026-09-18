#pragma once

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <cstdint>
#include "session/input_session.h"
#include "english/english_ime.h"

inline std::condition_variable pipe_queueCv;
inline std::atomic_bool pipe_running = true;
inline std::shared_ptr<IInputSession> g_inputSession;
// Collapse bursty TSF show/move packets so the UI thread runs one FineTune
// against the latest Global::Point / CandidateString instead of restarting
// the WebView2 measure chain on every cloud refresh or layout notification.
inline std::atomic<bool> g_candidate_show_msg_pending{false};
inline std::atomic<bool> g_candidate_move_msg_pending{false};

namespace FanyNamedPipe
{
enum class CandidateUiAction
{
    Commit,
    Pin,
    Delete,
    FixPosition,
    ClearPosition,
    PageUp,
    PageDown,
};

void WorkerThread();
void EventListenerLoopThread();
void AuxPipeEventListenerLoopThread();
void TsfDiagnosticPipeEventListenerLoopThread();
void StatsPipeEventListenerLoopThread();
void ToTsfPipeEventListenerLoopThread();
void ToTsfWorkerThreadPipeEventListenerLoopThread();

// Delivers an activation that arrived before the candidate window existed. Call
// once the window is available; a no-op when nothing was deferred.
void ReplayDeferredClientActivation();

void PrepareCandidateList(uint64_t client_id, uint64_t activation_epoch);
void ClearState();
void RegisterStatusSnapshotWindow(HWND toolbar_window);
void EnqueueCloudCandidate(const std::string &candidate, const std::string &pinyin, uint64_t generation);
void EnqueueAiCandidate(const std::string &candidate, const std::string &identity, uint64_t generation);
void CancelCloudCandidateRequest();
void EnqueueEnglishCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation);
void EnqueueCandidateTranslations(std::vector<EnglishIme::TranslationResult> results, uint64_t generation,
                                  bool merge = false);
void EnqueueEmojiCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation);
void EnqueueKaomojiCandidates(std::vector<WordItem> candidates, const std::string &input, uint64_t generation);
void EnqueueCandidateUiAction(CandidateUiAction action, int one_based_index, int fixed_position = 0);
// Paging carries a step count rather than a candidate index. Steps coalesce
// into a page task already queued for the same client, so spinning the wheel
// cannot outrun the worker with a long run of engine-backed page moves.
void EnqueueCandidateUiPaging(CandidateUiAction action, int steps);
void EnqueuePipeSessionInvalidatedTask(uint64_t client_id, uint64_t invalidation_epoch);
void EnqueueReloadInputSessionTask();
void EnqueueEnsureInputSessionMatchesConfigTask();
void EnqueueApplyCandidatePageSizeTask();
void EnqueueRefreshCandidatePageTask();
void EnqueueResetInputSessionCacheTask();
void EnqueueExitEnglishInputModeTask();
} // namespace FanyNamedPipe
