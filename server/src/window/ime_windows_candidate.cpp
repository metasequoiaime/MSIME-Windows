// 候选窗宿主的窗口过程：显示 / 隐藏 / 移动消息与去重、隐藏宽限、DPI 变化、状态切换转发、
// 配置同步定时器，以及候选上的鼠标动作。
#include "window/ime_windows_internal.h"
#include "global/globals.h"
#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "ime_windows.h"
#include "window/candidate_presenter.h"
#include "window/floating_toolbar_presenter.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include <atomic>
#include <cmath>
#include <string>
#include <fmt/xchar.h>
#include "webview2/windows_webview2.h"
#include "utils/window_utils.h"
#include "ipc/event_listener.h"
#include "utils/ime_utils.h"
#include "log/candidate_diag_log.h"
#include "voice-input/voice_input_service.h"

using namespace ime_windows_detail;

namespace
{
// WM_DPICHANGED must remasure even when the caret has not moved.
std::atomic<bool> g_candidate_force_layout{false};
// A WM_HIDE_MAIN_WINDOW is armed on TIMER_ID_CANDIDATE_HIDE_GRACE rather than
// applied straight away; a show inside the grace window cancels it.
bool g_candidate_hide_pending = false;
// Last frame actually painted, used to drop duplicate shows. Cleared on hide so
// the next session always paints.
std::wstring g_last_rendered_candidate_signature;
ULONGLONG g_last_rendered_candidate_tick = 0;
} // namespace

LRESULT CALLBACK WndProcCandWindow(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_POWERBROADCAST || message == WM_DISPLAYCHANGE || message == WM_DWMCOMPOSITIONCHANGED ||
        message == WM_SETTINGCHANGE)
    {
        CAND_DIAG_LOGF(L"system window event message={:#x} wparam={:#x} lparam={:#x} {}", message,
                       static_cast<unsigned long long>(wParam), static_cast<unsigned long long>(lParam),
                       DescribeCandidateHostState());
    }

    if (IsSystemLightDarkToggle(message, lParam))
    {
        // A visible candidate window must flip immediately; a hidden one
        // re-resolves its theme on the next ShowFromGlobalState via the
        // ApplySkin fingerprint.
        if (::is_global_wnd_cand_shown && CandidatePresenter::Instance().IsBound())
        {
            CandidatePresenter::Instance().ShowFromGlobalState(GetCandidateLayoutCaret());
        }
    }

    if (message == WM_IMEACTIVATE)
    {
        g_is_ime_active = true;
        ApplyConfiguredFloatingToolbarVisibility(L"wm-imeactivate");
        return 0;
    }

    if (message == WM_IMEDEACTIVATE)
    {
        g_is_ime_active = false;
        ApplyConfiguredFloatingToolbarVisibility(L"wm-imedeactivate");
        return 0;
    }

    if (message == WM_SHOW_MAIN_WINDOW)
    {
        g_candidate_show_msg_pending.store(false);
        // A show arriving while a hide is still inside its grace window means the
        // hide belonged to an earlier keystroke of the same burst. Drop it, so the
        // window stays up and the user never sees the blink.
        if (g_candidate_hide_pending)
        {
            KillTimer(hwnd, TIMER_ID_CANDIDATE_HIDE_GRACE);
            g_candidate_hide_pending = false;
            CAND_DIAG_LOGF(L"hide cancelled by show within grace");
        }
        // This handler always renders the newest published state, never a payload
        // captured when the message was posted, so an older queued show can only
        // repaint what the newer one is about to repaint. Under load the worker
        // drains a backlog and posts several at once; painting every one of them
        // is what turns a queue stall into visible strobing.
        MSG supersedingShow{};
        if (PeekMessage(&supersedingShow, hwnd, WM_SHOW_MAIN_WINDOW, WM_SHOW_MAIN_WINDOW, PM_NOREMOVE))
        {
            CAND_DIAG_LOGF(L"candidate-frame path=superseded");
            return 0;
        }
        const uint64_t contentGeneration = ++g_candidate_content_generation;
        const ULONGLONG updateStartedTick = GetTickCount64();
        ::ReadDataFromSharedMemory(0b1000000);
        // The worker does not wait for this posted message before rebuilding its candidate state, so render the page it
        // published rather than the live one.
        const Global::CandidatePageSnapshotPtr candidatePage = Global::LoadCandidatePageSnapshot();
        LogSmallWindowReadyGate(L"show-candidate");
        CAND_DIAG_LOGF(L"candidate-frame show content_gen={} tick={} preedit_units={} payload_units={} "
                       L"caret=({},{}) follow_cursor={} anchor_valid={} {}",
                       contentGeneration, updateStartedTick,
                       GetConfiguredCandidateWindowPreeditStyle() == "empty" ? 0 : GetPreeditWithCaretMarker().size(),
                       candidatePage->candidate_string.size(), Global::Point[0], Global::Point[1],
                       GetConfiguredCandidateWindowFollowCursor(), g_candidate_session_anchor_valid,
                       DescribeCandidateHostState());
        if (!EnsureSmallWindowsTopmost(L"show-candidate"))
        {
            (void)0;
        }
        const POINT layoutCaret = GetCandidateLayoutCaret();
        const std::wstring preedit =
            GetConfiguredCandidateWindowPreeditStyle() == "empty" ? std::wstring{} : GetPreeditWithCaretMarker();
        // Suppress a repaint that would reproduce the frame already on screen.
        // g_candidate_force_layout marks the cases (DPI / display change) where
        // the same content must still be re-laid out, so never dedup through it.
        const std::wstring frameSignature =
            fmt::format(L"{}|{}|{},{}|{}", preedit, candidatePage->candidate_string, layoutCaret.x, layoutCaret.y,
                        candidatePage->selected_index_in_page);
        if (::is_global_wnd_cand_shown && !g_candidate_force_layout.load() &&
            frameSignature == g_last_rendered_candidate_signature &&
            updateStartedTick - g_last_rendered_candidate_tick < kCandidateShowDedupWindowMs)
        {
            // A deduped show paints nothing: the DOM still holds exactly the frame
            // the in-flight measurement was started for. The unconditional bump at
            // the top of this handler would nevertheless make that measurement look
            // stale, and both guards below drop it — so every content-only update
            // lost its SetWindowRgn refresh and the region stayed frozen at the last
            // full FineTune's width while the DOM kept changing. That is the
            // right-edge truncation: a wider later page painted past a stale region.
            // Hand the generation back (only if nobody bumped it since) so the
            // pending clip still lands. path=superseded above returns before the
            // bump for the same reason.
            uint64_t expectedGeneration = contentGeneration;
            const bool generationRestored =
                g_candidate_content_generation.compare_exchange_strong(expectedGeneration, contentGeneration - 1);
            // The frame already on screen is identical to the page just loaded, so the render echo is
            // honest even though nothing was repainted. Without it a digit/space selection would wait
            // out the full timeout on content that is already visible.
            Global::PublishRenderedCandidatePageGeneration(candidatePage->generation);
            CAND_DIAG_LOGF(L"candidate-frame path=dedup content_gen={} generation_restored={} page_gen={}",
                           contentGeneration, generationRestored, candidatePage->generation);
            return 0;
        }
        g_last_rendered_candidate_signature = frameSignature;
        g_last_rendered_candidate_tick = updateStartedTick;
        if (CandidatePresenter::Instance().IsBound())
        {
            CandidatePresenter::Instance().ShowFromGlobalState(layoutCaret);
            g_last_placed_caret_x = layoutCaret.x;
            g_last_placed_caret_y = layoutCaret.y;
            CAND_DIAG_LOGF(L"candidate-frame path=d2d content_gen={} elapsed_ms={}", contentGeneration,
                           GetTickCount64() - updateStartedTick);
            return 0;
        }

        // Do not SetWindowPos / ExecuteScript the candidate host while its
        // WebView2 controller is still being created. That is what makes
        // CreateCoreWebView2Controller fail for this HWND while menu/FTB succeed.
        ::is_global_wnd_cand_shown = true;
        Global::SetCandidateWindowRenderedVisible(true);
        if (!IsCandidateWebviewReady())
        {
            DeferCandidateShowUntilWebviewReady();
            // Nothing was painted, so this frame must not count as rendered:
            // the replay posted once the webview is ready would otherwise be
            // deduped against itself and the window would never appear.
            g_last_rendered_candidate_signature.clear();
            CAND_DIAG_LOGF(L"candidate-frame path=deferred-webview content_gen={} {}", contentGeneration,
                           DescribeCandidateHostState());
            return 0;
        }
        RaiseCandidateHostForShow(L"show-candidate");

        std::wstring str = preedit + L"," + candidatePage->candidate_string;
        const bool sameCaret = g_last_placed_caret_x == layoutCaret.x && g_last_placed_caret_y == layoutCaret.y;
        const bool alreadyVisible = IsCandidateHostPaintedVisible(hwnd);
        const bool layoutInflight = g_candidate_layout_inflight.load();
        CAND_DIAG_LOGF(L"candidate-layout-decision content_gen={} last_caret=({},{}) caret=({},{}) "
                       L"delta=({},{}) same={} visible={} inflight={}",
                       contentGeneration, g_last_placed_caret_x, g_last_placed_caret_y, layoutCaret.x, layoutCaret.y,
                       static_cast<long long>(layoutCaret.x) - g_last_placed_caret_x,
                       static_cast<long long>(layoutCaret.y) - g_last_placed_caret_y, sameCaret, alreadyVisible,
                       layoutInflight);
        if (!alreadyVisible)
        {
            PlaceCandidateHostNearCaret(hwnd);
        }
        if (alreadyVisible)
        {
            CAND_DIAG_LOGF(L"candidate-frame path=content-only content_gen={} inflight={}", contentGeneration,
                           layoutInflight);
            InflateCandWnd(str, [hwnd, contentGeneration, updateStartedTick,
                                 pageGeneration = candidatePage->generation]() {
                const ULONGLONG callbackTick = GetTickCount64();
                CAND_DIAG_LOGF(L"candidate-frame dom-callback content_gen={} current_gen={} elapsed_ms={} page_gen={}",
                               contentGeneration, g_candidate_content_generation.load(),
                               callbackTick - updateStartedTick, pageGeneration);
                if (!::is_global_wnd_cand_shown || contentGeneration != g_candidate_content_generation.load())
                {
                    return;
                }
                // The DOM holds this page now; echo the captured generation (not a live read: the
                // page may already have been superseded) so selections can settle against it.
                Global::PublishRenderedCandidatePageGeneration(pageGeneration);
                RefreshCandidateClipAfterPaint(hwnd, contentGeneration, updateStartedTick);
            });
            if (!sameCaret)
            {
                if (CandidateCaretNeedsStableFollow(hwnd, layoutCaret))
                {
                    KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
                    SetTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE, kCandidateMoveSettleMs, nullptr);
                }
                else
                {
                    CAND_DIAG_LOGF(L"candidate-position same-line-follow-suppressed anchor=({},{}) reported=({},{})",
                                   g_last_placed_caret_x, g_last_placed_caret_y, layoutCaret.x, layoutCaret.y);
                }
            }
            return 0;
        }
        if (layoutInflight)
        {
            CAND_DIAG_LOGF(L"show folded into in-flight fine-tune caret=({},{})", Global::Point[0], Global::Point[1]);
            CAND_DIAG_LOGF(L"candidate-frame path=folded-inflight content_gen={}", contentGeneration);
            // The in-flight FineTune echoes only the page it captured, which may predate this one. Without
            // an echo here a digit/space selection right after a fast burst waits out the full render-sync
            // timeout, felt as a stall on commit.
            InflateCandWnd(str, [contentGeneration, pageGeneration = candidatePage->generation]() {
                if (!::is_global_wnd_cand_shown || contentGeneration != g_candidate_content_generation.load())
                {
                    return;
                }
                Global::PublishRenderedCandidatePageGeneration(pageGeneration);
            });
            KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
            SetTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE, kCandidateMoveSettleMs, nullptr);
            return 0;
        }
        CAND_DIAG_LOGF(L"candidate-frame path=full-layout content_gen={}", contentGeneration);
        InflateMeasureDivCandWnd(str, [hwnd]() {
            if (!::is_global_wnd_cand_shown)
            {
                return;
            }
            FineTuneWindow(hwnd);
        });
        return 0;
    }

    if (message == WM_HIDE_MAIN_WINDOW || (message == WM_TIMER && wParam == TIMER_ID_CANDIDATE_HIDE_GRACE))
    {
        // wParam != 0 marks a hide the worker delivered late, i.e. one that may
        // already be superseded by keystrokes queued behind it. Only those get the
        // grace; a hide arriving on time is a real commit or focus loss and is
        // applied straight away so typing keeps its snap.
        if (message == WM_HIDE_MAIN_WINDOW && wParam != 0)
        {
            // Repeat hides while one is already armed need no extra work — that is
            // where the redundant back-to-back hides get absorbed.
            if (!g_candidate_hide_pending)
            {
                g_candidate_hide_pending = true;
                SetTimer(hwnd, TIMER_ID_CANDIDATE_HIDE_GRACE, kCandidateHideGraceMs, nullptr);
            }
            CAND_DIAG_LOGF(L"hide message deferred grace_ms={} {}", kCandidateHideGraceMs,
                           DescribeCandidateHostState());
            return 0;
        }
        KillTimer(hwnd, TIMER_ID_CANDIDATE_HIDE_GRACE);
        g_candidate_hide_pending = false;
        CAND_DIAG_LOGF(L"hide message begin {}", DescribeCandidateHostState());
        ::is_global_wnd_cand_shown = false;
        Global::SetCandidateWindowRenderedVisible(false);
        g_last_rendered_candidate_signature.clear();
        KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
        g_candidate_session_anchor_valid = false;
        ++g_candidate_content_generation;
        g_candidate_layout_inflight.store(false);
        ++g_candidate_finetune_generation;
        Global::MarginTop = 0;
        Global::MarginLeft = 0;
        g_has_last_candidate_clip = false;
        ResetCandidatePlacementMemory();
        {
            SuppressCandidateDpiChange suppressDpi;
            if (CandidatePresenter::Instance().IsBound())
            {
                CandidatePresenter::Instance().Hide();
            }
            else if (!webviewControllerCandWnd)
            {
                // Moving this HWND while CreateCoreWebView2Controller is in
                // flight makes the candidate controller fail (menu/FTB still work).
                SetHostWindowCloaked(hwnd, true);
            }
            else
            {
                ClearCandidateWindowRegion(hwnd);
                // Park off-screen but keep the stable quarter-screen host size.
                // Shrinking back to the default card size made the next show race
                // put_Bounds / margins against a tiny HWND and park the card at
                // the monitor's left edge.
                const auto parkHost = ComputeQuarterScreenHostPixels(GetMainMonitorCoordinates());
                SetWindowPos(hwnd, HWND_TOP, 0, Global::INVALID_Y, parkHost.first, parkHost.second, SWP_SHOWWINDOW);
                SetHostWindowCloaked(hwnd, true);
                UpdateHtmlContentWithJavaScript(webviewCandWnd, L"");
            }
        }
        CAND_DIAG_LOGF(L"hide message end {}", DescribeCandidateHostState());
        return 0;
    }

    if (message == WM_MOVE_CANDIDATE_WINDOW)
    {
        g_candidate_move_msg_pending.store(false);
        CAND_DIAG_LOGF(L"move message caret=({},{}) {}", Global::Point[0], Global::Point[1],
                       DescribeCandidateHostState());
        if (!::is_global_wnd_cand_shown)
        {
            return 0;
        }
        const bool forceLayout = g_candidate_force_layout.exchange(false);
        const POINT layoutCaret = GetCandidateLayoutCaret();
        const bool sameCaret = g_last_placed_caret_x == layoutCaret.x && g_last_placed_caret_y == layoutCaret.y;
        // Position locking applies only after the first usable anchor has been
        // placed. A deferred show still needs its first valid MoveCandidate.
        const bool awaitingInitialPlacement = g_last_placed_caret_y == Global::INVALID_Y;
        if (!forceLayout && !GetConfiguredCandidateWindowFollowCursor() && !awaitingInitialPlacement)
        {
            CAND_DIAG_LOGF(L"candidate-position move-ignored locked_anchor=({},{}) reported=({},{})", layoutCaret.x,
                           layoutCaret.y, Global::Point[0], Global::Point[1]);
            return 0;
        }
        if (!forceLayout && !sameCaret && !CandidateCaretNeedsStableFollow(hwnd, layoutCaret))
        {
            CAND_DIAG_LOGF(L"candidate-position same-line-follow-suppressed anchor=({},{}) reported=({},{})",
                           g_last_placed_caret_x, g_last_placed_caret_y, layoutCaret.x, layoutCaret.y);
            return 0;
        }
        if (!forceLayout && sameCaret && (g_candidate_layout_inflight.load() || IsCandidateHostPaintedVisible(hwnd)))
        {
            CAND_DIAG_LOGF(L"move skipped same caret inflight={} visible={}", g_candidate_layout_inflight.load(),
                           IsCandidateHostPaintedVisible(hwnd));
            return 0;
        }
        if (forceLayout)
        {
            KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
            FineTuneWindow(hwnd);
            return 0;
        }
        // Some hosts animate the composition rectangle and report a new caret
        // every few milliseconds. Lay out only the latest settled anchor.
        KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
        SetTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE, kCandidateMoveSettleMs, nullptr);
        return 0;
    }

    if (message == WM_DPICHANGED)
    {
        // Never apply the suggested rect or FineTune while we are inside our own
        // SetWindowPos: that path is what delivers this message synchronously on
        // focus-switch hide, and re-entering stalls the server UI thread.
        if (g_candidate_dpi_change_suppress_count > 0)
        {
            return 0;
        }

        // Visible candidate: FineTune already re-syncs HWND DPI after its own
        // SetWindowPos. For external DPI changes, defer via the move message so
        // we never nest ExecuteScript/measure on this stack.
        if (::is_global_wnd_cand_shown && Global::Point[1] != Global::INVALID_Y)
        {
            g_candidate_force_layout.store(true);
            PostMessage(hwnd, WM_MOVE_CANDIDATE_WINDOW, 0, 0);
        }
        // Hidden: keep the off-screen park / warmup size. DefWindowProc must not
        // run — returning 0 without SetWindowPos is intentional.
        return 0;
    }

    if (message == WM_IMESWITCH)
    {
        const int cn = wParam != 0 ? 1 : 0;
        const std::string &punctuation_lock = GetConfiguredPunctuationLock();
        const int punc = punctuation_lock == "chinese" ? 1 : punctuation_lock == "english" ? 0 : cn;
        UpdateFtbCnEnAndPuncState(::webviewFtbWnd, cn, punc);
        return 0;
    }

    if (message == WM_PUNCSWITCH)
    {
        if (wParam == 0) // 此时是中文标点状态
        {
            /* 更新 floating toolbar 的标点全角和半角状态为全角 */
            UpdateFtbPuncState(::webviewFtbWnd, 0);
        }
        else // 此时是英文标点状态
        {
            /* 更新 floating toolbar 的标点全角和半角状态为半角 */
            UpdateFtbPuncState(::webviewFtbWnd, 1);
        }
        return 0;
    }

    if (message == WM_DOUBLESINGLEBYTESWITCH)
    {
        if (wParam == 0) // 此时是半角状态
        {
            /* 更新 floating toolbar 的全角和半角状态为半角 */
            UpdateFtbDoubleSingleByteState(::webviewFtbWnd, 0);
        }
        else // 此时是全角状态
        {
            /* 更新 floating toolbar 的全角和半角状态为全角 */
            UpdateFtbDoubleSingleByteState(::webviewFtbWnd, 1);
        }
        return 0;
    }

    if (message == WM_REFRESH_CHARACTER_SET)
    {
        UpdateFtbCharacterSetState(::webviewFtbWnd);
        FloatingToolbarPresenter::Instance().ApplyTheme();
        FanyNamedPipe::EnqueueRefreshCandidatePageTask();
        PostSettingsConfig();
        return 0;
    }

    if (message == WM_APPLY_IME_CONFIG)
    {
        if (wParam != 0)
        {
            ForceReloadConfiguredCandidateSkin();
            if (::is_global_wnd_cand_shown)
            {
                if (CandidatePresenter::Instance().IsBound())
                {
                    CandidatePresenter::Instance().ShowFromGlobalState(GetCandidateLayoutCaret());
                }
                else
                {
                    FineTuneWindow(hwnd);
                }
            }
            return 0;
        }
        InvalidateImeConfigWriteTime();
        PostMessage(hwnd, WM_TIMER, TIMER_ID_CONFIG_SYNC, 0);
        return 0;
    }

    if (message == WM_APPLY_IME_INPUT_SCHEME)
    {
        InvalidateImeConfigWriteTime();
        ReloadImeConfigIfChanged();
        ApplyConfiguredInputScheme();
        return 0;
    }

    if (CandidatePresenter::Instance().HandleMessage(message, wParam, lParam))
    {
        return message == WM_ERASEBKGND ? 1 : 0;
    }

    switch (message)
    {
    case WM_TIMER: {
        if (wParam == TIMER_ID_CANDIDATE_MOVE_SETTLE)
        {
            KillTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE);
            if (!::is_global_wnd_cand_shown)
            {
                break;
            }
            if (g_candidate_layout_inflight.load())
            {
                SetTimer(hwnd, TIMER_ID_CANDIDATE_MOVE_SETTLE, kCandidateMoveSettleMs, nullptr);
                break;
            }
            const POINT layoutCaret = GetCandidateLayoutCaret();
            const bool sameCaret = g_last_placed_caret_x == layoutCaret.x && g_last_placed_caret_y == layoutCaret.y;
            if (!sameCaret && CandidateCaretNeedsStableFollow(hwnd, layoutCaret))
            {
                CAND_DIAG_LOGF(L"candidate-layout settled-move last=({},{}) caret=({},{})", g_last_placed_caret_x,
                               g_last_placed_caret_y, layoutCaret.x, layoutCaret.y);
                FineTuneWindow(hwnd);
            }
            break;
        }
        if (wParam == TIMER_ID_CONFIG_SYNC)
        {
            const SchemeType previous_input_scheme = GetConfiguredActiveInputScheme();
            const std::string previous_shuangpin_schema = GetConfiguredShuangpinSchema();
            const std::string previous_character_set = GetConfiguredCharacterSet();
            const std::string previous_layout = GetConfiguredCandidateWindowLayout();
            const std::string previous_candidate_skin = GetConfiguredCandidateSkin();
            const bool previous_floating_toolbar = GetConfiguredFloatingToolbarEnabled();
            const bool previous_caret_state_indicator = GetConfiguredCaretStateIndicatorEnabled();
            const FloatingToolbarItemsConfig previous_floating_toolbar_items = GetConfiguredFloatingToolbarItems();
            const double previous_floating_toolbar_scale = GetConfiguredFloatingToolbarScale();
            const int previous_floating_toolbar_font_size = GetConfiguredFloatingToolbarFontSize();
            const bool previous_floating_toolbar_auto_hide = GetConfiguredFloatingToolbarAutoHide();
            const int previous_floating_toolbar_auto_hide_delay = GetConfiguredFloatingToolbarAutoHideDelay();
            const bool previous_cloud_candidates = GetConfiguredCloudCandidatesEnabled();
            const bool previous_comma_period = GetConfiguredPagingCommaPeriodEnabled();
            const bool previous_smart_punctuation = GetConfiguredSmartPunctuationEnabled();
            const bool previous_smart_punctuation_space_convert = GetConfiguredSmartPunctuationSpaceConvertEnabled();
            const bool previous_smart_punctuation_direct_digit = GetConfiguredSmartPunctuationDirectDigitEnabled();
            const bool previous_smart_punctuation_direct_letter = GetConfiguredSmartPunctuationDirectLetterEnabled();
            const bool previous_smart_punctuation_repeat_to_chinese =
                GetConfiguredSmartPunctuationRepeatToChineseEnabled();
            const bool previous_paired_punctuation = GetConfiguredPairedPunctuationEnabled();
            const std::string previous_punctuation_lock = GetConfiguredPunctuationLock();
            const bool previous_tsf_diagnostic_log = GetConfiguredTsfDiagnosticLogEnabled();
            const bool previous_statistics_enabled = GetConfiguredStatisticsEnabled();
            const std::string previous_tsf_preedit_style = GetConfiguredTsfPreeditStyle();
            const std::string previous_theme_mode = GetConfiguredThemeMode();
            const std::string previous_theme_cand = GetConfiguredThemeCand();
            const std::string previous_theme_ftb = GetConfiguredThemeFtb();
            const std::string previous_theme_menu = GetConfiguredThemeMenu();
            const std::string previous_font = GetConfiguredCandidateFont();
            const std::string previous_english_font = GetConfiguredCandidateEnglishFont();
            const std::string previous_default_font = GetConfiguredCandidateDefaultFont();
            const int previous_font_size = GetConfiguredCandidateFontSize();
            const int previous_preedit_font_size = GetConfiguredCandidateWindowPreeditFontSize();
            const std::string previous_cand_text_color = GetConfiguredCandidateTextColor();
            const VoiceInputConfig previous_voice_input = GetConfiguredVoiceInput();
            const bool previous_japanese_punctuation = GetConfiguredJapanesePunctuation();
            const bool previous_japanese_katakana_fkey = GetConfiguredJapaneseKatakanaFkey();
            if (ReloadImeConfigIfChanged())
            {
                FanyNamedPipe::EnqueueApplyCandidatePageSizeTask();
                if (previous_input_scheme != GetConfiguredActiveInputScheme())
                    ApplyConfiguredInputScheme();
                else if (previous_shuangpin_schema != GetConfiguredShuangpinSchema())
                    ApplyConfiguredShuangpinSchema();
                // The engine session snapshots the Japanese punctuation mode at
                // construction; a toggle while Japanese stays active needs a
                // session rebuild to take effect (the mode change above already
                // rebuilds via ApplyConfiguredInputScheme).
                else if (previous_japanese_punctuation != GetConfiguredJapanesePunctuation() ||
                         previous_japanese_katakana_fkey != GetConfiguredJapaneseKatakanaFkey())
                    FanyNamedPipe::EnqueueReloadInputSessionTask();
                if (previous_character_set != GetConfiguredCharacterSet())
                {
                    UpdateFtbCharacterSetState(::webviewFtbWnd);
                    FanyNamedPipe::EnqueueRefreshCandidatePageTask();
                }
                if (previous_layout != GetConfiguredCandidateWindowLayout())
                    ApplyConfiguredCandidateWindowLayout();
                if (previous_candidate_skin != GetConfiguredCandidateSkin() ||
                    previous_theme_mode != GetConfiguredThemeMode() ||
                    previous_theme_cand != GetConfiguredThemeCand() || previous_theme_ftb != GetConfiguredThemeFtb() ||
                    previous_theme_menu != GetConfiguredThemeMenu())
                {
                    ApplyConfiguredUiThemes();
                }
                else if (previous_font != GetConfiguredCandidateFont() ||
                         previous_english_font != GetConfiguredCandidateEnglishFont() ||
                         previous_default_font != GetConfiguredCandidateDefaultFont() ||
                         previous_font_size != GetConfiguredCandidateFontSize() ||
                         previous_preedit_font_size != GetConfiguredCandidateWindowPreeditFontSize() ||
                         previous_cand_text_color != GetConfiguredCandidateTextColor())
                {
                    ApplyConfiguredCandidateAppearance();
                }
                if (::is_global_wnd_cand_shown &&
                    (previous_layout != GetConfiguredCandidateWindowLayout() ||
                     previous_candidate_skin != GetConfiguredCandidateSkin() ||
                     previous_theme_mode != GetConfiguredThemeMode() ||
                     previous_theme_cand != GetConfiguredThemeCand() || previous_font != GetConfiguredCandidateFont() ||
                     previous_font_size != GetConfiguredCandidateFontSize() ||
                     previous_preedit_font_size != GetConfiguredCandidateWindowPreeditFontSize() ||
                     previous_cand_text_color != GetConfiguredCandidateTextColor()))
                {
                    if (CandidatePresenter::Instance().IsBound())
                    {
                        CandidatePresenter::Instance().ShowFromGlobalState(GetCandidateLayoutCaret());
                    }
                    else
                    {
                        FineTuneWindow(hwnd);
                    }
                }
                if (previous_floating_toolbar != GetConfiguredFloatingToolbarEnabled())
                {
                    RestartFloatingToolbarAutoHide(L"config-sync");
                    SyncMenuFloatingToolbarToggle();
                }
                else if (previous_floating_toolbar_auto_hide != GetConfiguredFloatingToolbarAutoHide() ||
                         previous_floating_toolbar_auto_hide_delay != GetConfiguredFloatingToolbarAutoHideDelay())
                {
                    RestartFloatingToolbarAutoHide(L"config-sync-auto-hide");
                }
                if (previous_caret_state_indicator != GetConfiguredCaretStateIndicatorEnabled() &&
                    !GetConfiguredCaretStateIndicatorEnabled() && ::global_hwnd_caret_state)
                {
                    PostMessage(::global_hwnd_caret_state, WM_HIDE_CARET_STATE, 0, 0);
                }
                if (!FloatingToolbarItemsEqual(previous_floating_toolbar_items, GetConfiguredFloatingToolbarItems()))
                {
                    ApplyConfiguredFloatingToolbarItems();
                }
                else if (std::fabs(previous_floating_toolbar_scale - GetConfiguredFloatingToolbarScale()) > 0.001 ||
                         previous_floating_toolbar_font_size != GetConfiguredFloatingToolbarFontSize())
                {
                    ApplyConfiguredFloatingToolbarSize();
                }
                if (previous_cloud_candidates && !GetConfiguredCloudCandidatesEnabled())
                    FanyNamedPipe::CancelCloudCandidateRequest();
                if (previous_comma_period != GetConfiguredPagingCommaPeriodEnabled() ||
                    previous_tsf_preedit_style != GetConfiguredTsfPreeditStyle())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::PagingCommaPeriodChanged,
                        FormatPagingCommaPeriodWorkerPayload());
                }
                if (previous_smart_punctuation != GetConfiguredSmartPunctuationEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationChanged,
                        GetConfiguredSmartPunctuationEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_space_convert != GetConfiguredSmartPunctuationSpaceConvertEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationSpaceConvertChanged,
                        GetConfiguredSmartPunctuationSpaceConvertEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_direct_digit != GetConfiguredSmartPunctuationDirectDigitEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectDigitChanged,
                        GetConfiguredSmartPunctuationDirectDigitEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_direct_letter != GetConfiguredSmartPunctuationDirectLetterEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationDirectLetterChanged,
                        GetConfiguredSmartPunctuationDirectLetterEnabled() ? L"1" : L"0");
                }
                if (previous_smart_punctuation_repeat_to_chinese !=
                    GetConfiguredSmartPunctuationRepeatToChineseEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::SmartPunctuationRepeatToChineseChanged,
                        GetConfiguredSmartPunctuationRepeatToChineseEnabled() ? L"1" : L"0");
                }
                if (previous_paired_punctuation != GetConfiguredPairedPunctuationEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::PairedPunctuationChanged,
                        GetConfiguredPairedPunctuationEnabled() ? L"1" : L"0");
                }
                if (previous_punctuation_lock != GetConfiguredPunctuationLock())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::PunctuationLockChanged,
                        FormatPunctuationLockWorkerPayload());
                    const std::string &punctuation_lock = GetConfiguredPunctuationLock();
                    if (punctuation_lock == "chinese")
                    {
                        UpdateFtbPuncState(::webviewFtbWnd, 1);
                    }
                    else if (punctuation_lock == "english")
                    {
                        UpdateFtbPuncState(::webviewFtbWnd, 0);
                    }
                }
                if (previous_tsf_diagnostic_log != GetConfiguredTsfDiagnosticLogEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::TsfDiagnosticLogChanged,
                        GetConfiguredTsfDiagnosticLogEnabled() ? L"1" : L"0");
                }
                if (previous_statistics_enabled != GetConfiguredStatisticsEnabled())
                {
                    BroadcastToTsfWorkerThreadViaNamedpipe(
                        Global::DataFromServerMsgTypeToTsfWorkerThread::StatisticsEnabledChanged,
                        GetConfiguredStatisticsEnabled() ? L"1" : L"0");
                }
                const VoiceInputConfig &voice_input = GetConfiguredVoiceInput();
                if (previous_voice_input.enabled != voice_input.enabled ||
                    previous_voice_input.hotkey_ralt != voice_input.hotkey_ralt ||
                    previous_voice_input.hotkey_ctrl_f9 != voice_input.hotkey_ctrl_f9 ||
                    previous_voice_input.hotkey_ctrl_win != voice_input.hotkey_ctrl_win ||
                    previous_voice_input.hotkey_rctrl_ralt != voice_input.hotkey_rctrl_ralt)
                {
                    VoiceInput::RefreshKeyboardHook();
                }
            }
            FanyNamedPipe::EnqueueEnsureInputSessionMatchesConfigTask();
            ApplyConfiguredCandidateSkinIfChanged();
        }
        break;
    }

    case WM_MOUSEACTIVATE:
        // Stop the window from being activated by mouse click
        return MA_NOACTIVATE;

    case WM_ACTIVATE: {
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        break;
    }

    case WM_SIZE: {
        if (::webviewControllerCandWnd && wParam != SIZE_MINIMIZED)
        {
            SyncCandidateWebViewBoundsToHost(hwnd);
        }
        break;
    }

    /* Clear dictionary buffer cache */
    case WM_CLS_DICT_CACHE: {
        FanyNamedPipe::EnqueueResetInputSessionCacheTask();
#ifdef FANY_DEBUG
        (void)0;
#endif
        break;
    }

    case WM_COMMIT_CANDIDATE: {
        int one_based = static_cast<int>(wParam);
#ifdef FANY_DEBUG
        (void)0;
#endif
        FanyNamedPipe::EnqueueCandidateUiAction(FanyNamedPipe::CandidateUiAction::Commit, one_based);

        break;
    }

    case WM_PIN_TO_TOP_CANDIDATE: {
        int one_based = static_cast<int>(wParam);
#ifdef FANY_DEBUG
        (void)0;
#endif
        FanyNamedPipe::EnqueueCandidateUiAction(FanyNamedPipe::CandidateUiAction::Pin, one_based);

        break;
    }

    case WM_DELETE_CANDIDATE: {
        int one_based = static_cast<int>(wParam);
#ifdef FANY_DEBUG
        (void)0;
#endif
        FanyNamedPipe::EnqueueCandidateUiAction(FanyNamedPipe::CandidateUiAction::Delete, one_based);

        break;
    }
    case WM_FIX_CANDIDATE_POSITION:
        FanyNamedPipe::EnqueueCandidateUiAction(FanyNamedPipe::CandidateUiAction::FixPosition, static_cast<int>(wParam),
                                                static_cast<int>(lParam));
        break;
    case WM_CLEAR_CANDIDATE_POSITION:
        FanyNamedPipe::EnqueueCandidateUiAction(FanyNamedPipe::CandidateUiAction::ClearPosition,
                                                static_cast<int>(wParam));
        break;

    case WM_PAGE_CANDIDATE:
        // Both candidate hosts funnel wheel paging through here — the D2D
        // presenter and the WebView2 page's `candidateWheel` message — so this is
        // the one place the setting has to be honoured.
        if (!GetConfiguredPagingMouseWheelEnabled())
        {
            break;
        }
        FanyNamedPipe::EnqueueCandidateUiPaging(wParam == CANDIDATE_PAGE_NEXT
                                                    ? FanyNamedPipe::CandidateUiAction::PageDown
                                                    : FanyNamedPipe::CandidateUiAction::PageUp,
                                                static_cast<int>(lParam));
        break;

    case WM_CLEAR_IME_ENGINE_CACHE: {
#ifdef FANY_DEBUG
        (void)0;
#endif
        /* 清除候选词缓存 */
        FanyNamedPipe::EnqueueResetInputSessionCacheTask();
        break;
    }

    case WM_DESTROY: {
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }

    return 0;
}
