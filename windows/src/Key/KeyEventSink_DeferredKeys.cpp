// Deferred key FIFO: barrier checks, the projected composition state that classifies keys queued
// behind it, and the queue/drain/complete/fail path. A failed key is dropped, never replayed.

#include "Private.h"
#include "Globals.h"
#include "MetasequoiaIME.h"
#include "CandidateListUIPresenter.h"
#include "CompositionProcessorEngine.h"
#include "KeyHandlerEditSession.h"
#include "KeyFocusRecovery.h"
#include "KeyRepeatGuard.h"
#include "stats_collector.h"
#include "stats_passthrough.h"
#include "CaretAnchorPolicy.h"
#include "Compartment.h"
#include "MetasequoiaIMEBaseStructure.h"
#include <debugapi.h>
#include <cwctype>
#include <string>
#include "Ipc.h"
#include "FanyUtils.h"
#include "FanyDefines.h"
#include "FanyLog.h"
#include "EditSession.h"
#include "TfTextLayoutSink.h"
#include "../Utils/PerfTimer.h"
#include <chrono>
#include "../../../engine/contracts/ipc_negotiation.h"
#include "../../../engine/contracts/mid_sentence_helpcode.h"
#include "KeyEventSinkInternal.h"

using namespace key_event_sink_detail;

namespace
{
// Room for a fast burst typed while a slow edit session or a focus session
// activation holds the FIFO. A key that does not fit is not eaten.
constexpr size_t MAX_DEFERRED_KEY_DOWN_COUNT = static_cast<size_t>(MAX_PINYIN_LENGTH) * 2 + 32;

struct DeferredShadowState
{
    bool imeOpen = false;
    bool punctuationOpen = false;
    bool doubleSingleByteOpen = false;
    size_t inputLength = 0;
    std::wstring rawInput;
    size_t caret = 0;
    bool candidateActive = false;
    bool unicodeMode = false;
    // False once a queued key's effect on the future composition is unknown.
    // The projection is then discarded and the next key re-reads reality.
    bool projectionValid = true;
};

bool IsBareModifierKey(UINT code)
{
    switch (code)
    {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

void ApplyDeferredKeyState(DeferredShadowState &shadow, const _KEYSTROKE_STATE &keyState, WCHAR wch = 0)
{
    const auto clearComposition = [&shadow]() {
        shadow.inputLength = 0;
        shadow.rawInput.clear();
        shadow.caret = 0;
        shadow.candidateActive = false;
        shadow.unicodeMode = false;
    };

    switch (keyState.Function)
    {
    case FUNCTION_INPUT:
        if (shadow.inputLength == 0)
        {
            shadow.unicodeMode = (wch == L'U');
        }
        shadow.caret = min(shadow.caret, shadow.rawInput.size());
        if (shadow.rawInput.size() < MAX_PINYIN_LENGTH && wch != L'\0')
        {
            const bool duplicateSeparator =
                wch == L'\'' && ((shadow.caret > 0 && shadow.rawInput[shadow.caret - 1] == L'\'') ||
                                 (shadow.caret < shadow.rawInput.size() && shadow.rawInput[shadow.caret] == L'\''));
            if (!duplicateSeparator)
            {
                shadow.rawInput.insert(shadow.caret, 1, wch);
                ++shadow.caret;
            }
        }
        shadow.inputLength = shadow.rawInput.size();
        shadow.candidateActive = false;
        break;
    case FUNCTION_FINALIZE_TEXTSTORE_AND_INPUT:
    case FUNCTION_FINALIZE_CANDIDATELIST_AND_INPUT:
        shadow.rawInput.assign(wch == L'\0' ? 0 : 1, wch);
        shadow.caret = shadow.rawInput.size();
        shadow.inputLength = shadow.rawInput.size();
        shadow.candidateActive = false;
        shadow.unicodeMode = (wch == L'U');
        break;
    case FUNCTION_BACKSPACE:
        shadow.caret = min(shadow.caret, shadow.rawInput.size());
        if (shadow.caret > 0)
        {
            shadow.rawInput.erase(shadow.caret - 1, 1);
            --shadow.caret;
        }
        shadow.inputLength = shadow.rawInput.size();
        if (shadow.inputLength == 0)
        {
            shadow.candidateActive = false;
            shadow.unicodeMode = false;
        }
        break;
    case FUNCTION_BACKSPACE_SEGMENT:
    case FUNCTION_MOVE_LEFT_SEGMENT:
    case FUNCTION_MOVE_RIGHT_SEGMENT:
        // How much one unit covers is Server-owned, so the future raw and caret
        // cannot be predicted here. Invalidate the projection instead of
        // advancing it by one character; the next key re-reads the real
        // composition.
        shadow.projectionValid = false;
        break;
    case FUNCTION_CONVERT_WILDCARD:
        shadow.candidateActive = shadow.inputLength > 0;
        break;
    case FUNCTION_CONVERT:
        // This TIP routes Space+Convert to WM_AsyncFinalizeCandidate, which
        // commits and ends the composition rather than merely opening a list.
        clearComposition();
        break;
    case FUNCTION_CANCEL:
        clearComposition();
        break;
    case FUNCTION_MOVE_LEFT:
        if (shadow.caret > 0)
        {
            --shadow.caret;
        }
        break;
    case FUNCTION_DELETE:
        shadow.caret = min(shadow.caret, shadow.rawInput.size());
        if (shadow.caret < shadow.rawInput.size())
        {
            shadow.rawInput.erase(shadow.caret, 1);
        }
        shadow.inputLength = shadow.rawInput.size();
        if (shadow.inputLength == 0)
        {
            shadow.candidateActive = false;
            shadow.unicodeMode = false;
        }
        break;
    case FUNCTION_MOVE_RIGHT:
        if (shadow.caret < shadow.rawInput.size())
        {
            ++shadow.caret;
        }
        break;
    case FUNCTION_FINALIZE_TEXTSTORE:
    case FUNCTION_FINALIZE_CANDIDATELIST:
    case FUNCTION_FINALIZE_CANDIDATELISTForVKReturn:
    case FUNCTION_SELECT_BY_NUMBER:
    case FUNCTION_TOGGLE_IME_MODE:
    case FUNCTION_PUNCTUATION:
    case FUNCTION_DOUBLE_SINGLE_BYTE:
        clearComposition();
        break;
    default:
        break;
    }
}

const wchar_t *DeferredKeyFailureKindName(DeferredKeyFailureKind kind)
{
    switch (kind)
    {
    case DeferredKeyFailureKind::Stale:
        return L"stale";
    case DeferredKeyFailureKind::Offline:
        return L"offline";
    case DeferredKeyFailureKind::Resync:
        return L"resync";
    case DeferredKeyFailureKind::Transport:
        return L"transport";
    default:
        return L"unknown";
    }
}

const wchar_t *DeferredKeyFailureReasonName(DeferredKeyFailureReason reason)
{
    switch (reason)
    {
    case DeferredKeyFailureReason::Superseded:
        return L"superseded";
    case DeferredKeyFailureReason::HostEditRejected:
        return L"host-edit-rejected";
    case DeferredKeyFailureReason::EditSessionRequestFailed:
        return L"edit-session-request-failed";
    case DeferredKeyFailureReason::AsyncPostFailed:
        return L"async-post-failed";
    case DeferredKeyFailureReason::TransportBroken:
        return L"transport-broken";
    case DeferredKeyFailureReason::DeliveryAmbiguous:
        return L"delivery-ambiguous";
    default:
        return L"unknown";
    }
}

} // namespace

bool CMetasequoiaIME::_HasDeferredKeyBarrier() const
{
    if (_localSessionResetPending.load(std::memory_order_acquire) || _focusResetPending || _activationRequired ||
        _deferredKeyProjectionValid || !_deferredKeyDowns.empty() || _hasDeferredKeyInFlight)
    {
        return true;
    }
    if (Global::g_connected)
    {
        const uint64_t expectedToken = _expectedWorkerFocusToken.load(std::memory_order_acquire);
        return expectedToken == 0 || _acknowledgedWorkerFocusToken.load(std::memory_order_acquire) != expectedToken ||
               !_workerCommitReady.load(std::memory_order_acquire);
    }
    return false;
}

bool CMetasequoiaIME::_DeferredKeyQueueHasCapacity() const
{
    size_t deferredCount = _deferredKeyDowns.size();
    if (_hasDeferredKeyInFlight)
    {
        ++deferredCount;
    }
    return deferredCount < MAX_DEFERRED_KEY_DOWN_COUNT;
}

void CMetasequoiaIME::_EnsureDeferredKeyProjection()
{
    if (_deferredKeyProjectionValid)
    {
        return;
    }
    _deferredKeyProjectionValid = true;
    _deferredProjectedImeOpen = _pCompositionProcessorEngine && _pThreadMgr &&
                                _pCompositionProcessorEngine->GetIMEMode(_pThreadMgr, _tfClientId) != FALSE;
    _deferredProjectedPunctuationOpen =
        _pCompositionProcessorEngine && _pThreadMgr &&
        _pCompositionProcessorEngine->GetPunctuationMode(_pThreadMgr, _tfClientId) != FALSE;
    _deferredProjectedDoubleSingleByteOpen =
        _pCompositionProcessorEngine && _pThreadMgr &&
        _pCompositionProcessorEngine->GetDoubleSingleByteMode(_pThreadMgr, _tfClientId) != FALSE;
    // In the healthy path every IME-owned key enters the FIFO as well, so its
    // first projection starts from the composition that is already visible.
    // After a failure, _ArmEmptyDeferredProjection starts it from the empty
    // composition the pending reset leaves behind instead.
    _deferredProjectedInputLength = _pCompositionProcessorEngine
                                        ? min(static_cast<size_t>(MAX_PINYIN_LENGTH),
                                              static_cast<size_t>(_pCompositionProcessorEngine->GetVirtualKeyLength()))
                                        : 0;
    _deferredProjectedRawInput.clear();
    _deferredProjectedCaret = 0;
    if (_pCompositionProcessorEngine)
    {
        const CStringRange &buffer = _pCompositionProcessorEngine->GetKeystrokeBuffer();
        if (buffer.Get() && buffer.GetLength() > 0)
        {
            _deferredProjectedRawInput.assign(buffer.Get(), buffer.GetLength());
        }
        _deferredProjectedCaret = min(static_cast<size_t>(_pCompositionProcessorEngine->GetCaretPosition()),
                                      _deferredProjectedRawInput.size());
    }
    // Incremental candidates are still the ordinary composing path: another
    // letter extends the same raw input.  Only an explicit/original candidate
    // list makes the next input a finalize-and-start-new boundary.
    _deferredProjectedCandidateActive = _candidateMode == CANDIDATE_ORIGINAL;
    _deferredProjectedUnicodeMode =
        _pCompositionProcessorEngine && _pCompositionProcessorEngine->IsUnicodeModeComposition() != FALSE;
}

void CMetasequoiaIME::_ApplyDeferredKeyProjection(const _KEYSTROKE_STATE &keyState, WCHAR wch)
{
    _EnsureDeferredKeyProjection();
    DeferredShadowState shadow;
    shadow.imeOpen = _deferredProjectedImeOpen;
    shadow.punctuationOpen = _deferredProjectedPunctuationOpen;
    shadow.doubleSingleByteOpen = _deferredProjectedDoubleSingleByteOpen;
    shadow.inputLength = _deferredProjectedInputLength;
    shadow.rawInput = _deferredProjectedRawInput;
    shadow.caret = _deferredProjectedCaret;
    shadow.candidateActive = _deferredProjectedCandidateActive;
    shadow.unicodeMode = _deferredProjectedUnicodeMode;
    ApplyDeferredKeyState(shadow, keyState, wch);
    if (keyState.Function == FUNCTION_CANCEL && !GlobalIme::word_for_creating_word.empty())
    {
        // Esc inside a creating word may keep the selected word alive
        // (input.escape_keeps_selected_word is Server-owned), so the
        // composition is not necessarily gone.
        shadow.projectionValid = false;
    }
    if (!shadow.projectionValid)
    {
        // The queued key's effect on the composition is unknown (a segment
        // edit -- Ctrl+Backspace deletion or Ctrl+Left/Right caret move -- has
        // a Server-owned unit length): drop the projection so the next key
        // classifies against the real state instead of against a guess that
        // could edit a different amount than TSF believes.
        _deferredKeyProjectionValid = false;
        _deferredProjectedInputLength = 0;
        _deferredProjectedRawInput.clear();
        _deferredProjectedCaret = 0;
        _deferredProjectedCandidateActive = false;
        _deferredProjectedUnicodeMode = false;
        return;
    }
    _deferredProjectedInputLength = shadow.inputLength;
    _deferredProjectedRawInput = std::move(shadow.rawInput);
    _deferredProjectedCaret = shadow.caret;
    _deferredProjectedCandidateActive = shadow.candidateActive;
    _deferredProjectedUnicodeMode = shadow.unicodeMode;

    if (keyState.Function == FUNCTION_BACKSPACE && shadow.inputLength == 0)
    {
        // This queued Backspace ends the composition when it replays, even
        // though the real composition still contains it. Arm the repeat guard
        // from the projection too, so the repeats already waiting behind it
        // are classified as IME-owned from the projected state instead of
        // falling back to the host while the real state lags behind.
        _backspaceHoldArmed = true;
    }
}

void CMetasequoiaIME::_ApplyDeferredPreservedKeyProjection(REFGUID preservedKey)
{
    _EnsureDeferredKeyProjection();
    if (_pCompositionProcessorEngine == nullptr)
    {
        return;
    }
    switch (_pCompositionProcessorEngine->GetPreservedKeyAction(preservedKey))
    {
    case CCompositionProcessorEngine::PreservedKeyAction::ToggleImeMode:
        FanyUtils::RefreshPunctuationLockFromConfig();
        _deferredProjectedImeOpen = !_deferredProjectedImeOpen;
        _deferredProjectedPunctuationOpen = Global::ResolvePunctuationOpen(_deferredProjectedImeOpen) != FALSE;
        _deferredProjectedInputLength = 0;
        _deferredProjectedRawInput.clear();
        _deferredProjectedCaret = 0;
        _deferredProjectedCandidateActive = false;
        _deferredProjectedUnicodeMode = false;
        break;
    case CCompositionProcessorEngine::PreservedKeyAction::ToggleDoubleSingleByteMode:
        _deferredProjectedDoubleSingleByteOpen = !_deferredProjectedDoubleSingleByteOpen;
        break;
    case CCompositionProcessorEngine::PreservedKeyAction::TogglePunctuationMode:
        FanyUtils::RefreshPunctuationLockFromConfig();
        _deferredProjectedPunctuationOpen = Global::ResolvePunctuationOpen(!_deferredProjectedPunctuationOpen) != FALSE;
        break;
    default:
        break;
    }
}

bool CMetasequoiaIME::_ClassifyDeferredKeyDown(_In_ ITfContext *pContext, WPARAM wParam, LPARAM lParam,
                                               _In_opt_ const WCHAR *translatedWch, _In_opt_ const UINT *modifiersDown,
                                               _Out_ WCHAR *classifiedWch, _Out_ UINT *classifiedCode,
                                               _Out_ _KEYSTROKE_STATE *keyState)
{
    if (pContext == nullptr || classifiedWch == nullptr || classifiedCode == nullptr || keyState == nullptr ||
        _pCompositionProcessorEngine == nullptr || _pThreadMgr == nullptr)
    {
        return false;
    }

    *classifiedWch = translatedWch ? *translatedWch : ConvertVKey(static_cast<UINT>(wParam));
    *classifiedCode = VKeyFromVKPacketAndWchar(static_cast<UINT>(wParam), *classifiedWch);
    keyState->Category = CATEGORY_NONE;
    keyState->Function = FUNCTION_NONE;

    const UINT capturedModifiers = modifiersDown ? *modifiersDown : CaptureIpcModifiers();
    const bool projectedImeOpen = _deferredKeyProjectionValid
                                      ? _deferredProjectedImeOpen
                                      : _pCompositionProcessorEngine->GetIMEMode(_pThreadMgr, _tfClientId) != FALSE;
    if (projectedImeOpen && !_serverUnavailableFallbackActive && !_IsKeyboardDisabled() &&
        IsCharacterSetInputModeToggle(*classifiedCode, capturedModifiers))
    {
        keyState->Category = CATEGORY_COMPOSING;
        keyState->Function = FUNCTION_TOGGLE_CHARACTER_SET;
        return true;
    }
    if (projectedImeOpen && IsEnglishInputModeToggle(*classifiedCode, capturedModifiers))
    {
        keyState->Category = CATEGORY_COMPOSING;
        keyState->Function = FUNCTION_CANCEL;
        return true;
    }

    // 同上：译文上屏也要在这条 Ctrl/Alt 拦截之前分类，否则这颗键会被交还给应用。
    const bool projectedCandidateActive =
        _deferredKeyProjectionValid ? _deferredProjectedCandidateActive : (_candidateMode == CANDIDATE_ORIGINAL);
    if (projectedImeOpen && !_IsKeyboardDisabled() && projectedCandidateActive &&
        IsTranslationCommitShortcut(*classifiedCode, capturedModifiers))
    {
        keyState->Category = CATEGORY_CANDIDATE;
        keyState->Function = FUNCTION_SERVER_CANDIDATE_KEY;
        return true;
    }

    // Other Ctrl/Alt/Windows combinations belong to the application. In particular,
    // never turn a recovery FIFO into a shortcut sink. The segment edit chords
    // (Ctrl+Backspace, Ctrl+Left, Ctrl+Right) are the exception; their unit
    // length is Server-owned, and the projection may already be invalid, so the
    // liveness test falls back to the projected input when one exists and to
    // the real state otherwise.
    {
        const bool projectedCompositionActive =
            _deferredKeyProjectionValid
                ? (_deferredProjectedInputLength > 0 || _deferredProjectedCandidateActive)
                : (_pCompositionProcessorEngine->GetVirtualKeyLength() > 0 || _candidateMode != CANDIDATE_NONE);
        const KEYSTROKE_FUNCTION segmentEdit = SegmentEditFunction(*classifiedCode, capturedModifiers);
        if (segmentEdit != FUNCTION_NONE && !_IsKeyboardDisabled() &&
            (projectedCompositionActive || !GlobalIme::word_for_creating_word.empty()))
        {
            keyState->Category = CATEGORY_COMPOSING;
            keyState->Function = segmentEdit;
            return true;
        }
    }
    if ((capturedModifiers & 0b00000110) != 0 || (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0 || IsBareModifierKey(*classifiedCode) || _IsKeyboardDisabled())
    {
        return false;
    }

    DeferredShadowState shadow;
    if (_deferredKeyProjectionValid)
    {
        shadow.imeOpen = _deferredProjectedImeOpen;
        shadow.punctuationOpen = _deferredProjectedPunctuationOpen;
        shadow.doubleSingleByteOpen = _deferredProjectedDoubleSingleByteOpen;
        shadow.inputLength = _deferredProjectedInputLength;
        shadow.rawInput = _deferredProjectedRawInput;
        shadow.caret = _deferredProjectedCaret;
        shadow.candidateActive = _deferredProjectedCandidateActive;
        shadow.unicodeMode = _deferredProjectedUnicodeMode;
    }
    else
    {
        shadow.imeOpen = _pCompositionProcessorEngine->GetIMEMode(_pThreadMgr, _tfClientId) != FALSE;
        shadow.punctuationOpen = _pCompositionProcessorEngine->GetPunctuationMode(_pThreadMgr, _tfClientId) != FALSE;
        shadow.doubleSingleByteOpen =
            _pCompositionProcessorEngine->GetDoubleSingleByteMode(_pThreadMgr, _tfClientId) != FALSE;
        shadow.inputLength = min(static_cast<size_t>(MAX_PINYIN_LENGTH),
                                 static_cast<size_t>(_pCompositionProcessorEngine->GetVirtualKeyLength()));
        const CStringRange &buffer = _pCompositionProcessorEngine->GetKeystrokeBuffer();
        if (buffer.Get() && buffer.GetLength() > 0)
        {
            shadow.rawInput.assign(buffer.Get(), buffer.GetLength());
        }
        shadow.caret =
            min(static_cast<size_t>(_pCompositionProcessorEngine->GetCaretPosition()), shadow.rawInput.size());
        shadow.candidateActive = _candidateMode == CANDIDATE_ORIGINAL;
        shadow.unicodeMode = _pCompositionProcessorEngine->IsUnicodeModeComposition() != FALSE;
    }

    const auto setKeyState = [keyState](KEYSTROKE_CATEGORY category, KEYSTROKE_FUNCTION function) {
        keyState->Category = category;
        keyState->Function = function;
        return true;
    };

    // Backspace hold guard (#347): inside a hold that began in composition, a
    // repeat that arrives after the projected composition is gone must still be
    // claimed. Handing it back here would make it application text and delete
    // document content; instead it is queued as an ordinary FUNCTION_BACKSPACE
    // and swallowed by _DispatchKeyDown when it replays.
    //
    // This is ShouldSuppressBackspaceRepeat against the projected state, widened
    // by one case: an empty projected buffer is claimed even while
    // word_for_creating_word is still set. That is the retraction window of #35,
    // where the queued Backspace may restore the spelling and leave the
    // composition alive -- the regular classifier cannot claim VK_BACK once the
    // projected length is 0, so the replay side has to re-test the real state
    // instead of letting the host delete document text here.
    if (static_cast<UINT>(wParam) == VK_BACK && _backspaceHoldArmed && IsAutoRepeat(lParam) &&
        shadow.inputLength == 0 && !shadow.candidateActive)
    {
        return setKeyState(CATEGORY_COMPOSING, FUNCTION_BACKSPACE);
    }

    _KEYSTROKE_STATE inputState = {};
    WCHAR inputWch = *classifiedWch;
    bool isInputKey = false;
    if (shadow.imeOpen)
    {
        isInputKey = _pCompositionProcessorEngine->IsVirtualKeyNeedForFreshComposition(*classifiedCode, &inputWch,
                                                                                       &inputState) != FALSE;
        if (!isInputKey && shadow.inputLength > 0 &&
            ((*classifiedWch == L'\'') || (_pCompositionProcessorEngine->IsWildcard() &&
                                           _pCompositionProcessorEngine->IsWildcardChar(*classifiedWch))))
        {
            isInputKey = true;
        }
        if (!isInputKey && Global::MicrosoftShuangpinEnabled.load(std::memory_order_relaxed) &&
            *classifiedCode == VK_OEM_1 && *classifiedWch == L';' && !shadow.rawInput.empty())
        {
            const size_t caret = min(shadow.caret, shadow.rawInput.size());
            const size_t separator = caret == 0 ? std::wstring::npos : shadow.rawInput.rfind(L'\'', caret - 1);
            const size_t chunkStart = separator == std::wstring::npos ? 0 : separator + 1;
            isInputKey = (caret - chunkStart) % 2 == 1;
        }
        // 与 CompositionProcessorEngine_KeyClassify.cpp 的句中辅助码判断一致，按影子状态算。
        if (!isInputKey && Global::MidSentenceHelpcodeEnabled.load(std::memory_order_relaxed) &&
            *classifiedCode == VK_OEM_3 && *classifiedWch == L'`' && !shadow.rawInput.empty())
        {
            isInputKey = FanyImeMidSentenceHelpcode::AcceptsMarkerAt(shadow.rawInput.data(), shadow.rawInput.size(),
                                                                     min(shadow.caret, shadow.rawInput.size()));
        }
        if (shadow.inputLength == 0 && (GetKeyState(VK_CAPITAL) & 0x0001) != 0 && *classifiedWch >= L'A' &&
            *classifiedWch <= L'Z' && *classifiedCode >= L'A' && *classifiedCode <= L'Z')
        {
            // Match the normal fresh-composition path: CapsLock uppercase at
            // the beginning belongs to the application.
            isInputKey = false;
        }
    }

    if (shadow.candidateActive && isInputKey)
    {
        return setKeyState(CATEGORY_CANDIDATE, FUNCTION_FINALIZE_CANDIDATELIST_AND_INPUT);
    }
    if (!shadow.candidateActive && isInputKey)
    {
        return setKeyState(CATEGORY_COMPOSING, FUNCTION_INPUT);
    }

    if (shadow.imeOpen && shadow.inputLength > 0)
    {
        const bool candidateKey = shadow.candidateActive;
        switch (*classifiedCode)
        {
        case VK_BACK:
            return candidateKey ? setKeyState(CATEGORY_CANDIDATE, FUNCTION_CANCEL)
                                : setKeyState(CATEGORY_COMPOSING, FUNCTION_BACKSPACE);
        case VK_DELETE:
            return candidateKey ? setKeyState(CATEGORY_CANDIDATE, FUNCTION_CANCEL)
                                : setKeyState(CATEGORY_COMPOSING, FUNCTION_DELETE);
        case VK_SPACE:
            return setKeyState(CATEGORY_CANDIDATE, FUNCTION_CONVERT);
        case VK_RETURN:
            return candidateKey ? setKeyState(CATEGORY_CANDIDATE, FUNCTION_FINALIZE_CANDIDATELIST)
                                : setKeyState(CATEGORY_CANDIDATE, FUNCTION_FINALIZE_CANDIDATELISTForVKReturn);
        case VK_ESCAPE:
            return setKeyState(CATEGORY_CANDIDATE, FUNCTION_CANCEL);
        case VK_LEFT:
            return setKeyState(CATEGORY_COMPOSING, FUNCTION_MOVE_LEFT);
        case VK_RIGHT:
            return setKeyState(CATEGORY_COMPOSING, FUNCTION_MOVE_RIGHT);
        case VK_HOME:
            return setKeyState(CATEGORY_CANDIDATE, FUNCTION_MOVE_PAGE_TOP);
        case VK_END:
            return setKeyState(CATEGORY_CANDIDATE, FUNCTION_MOVE_PAGE_BOTTOM);
        case VK_OEM_MINUS:
        case VK_OEM_PLUS:
            if (*classifiedCode == VK_OEM_PLUS && shadow.unicodeMode && shadow.inputLength == 1 &&
                *classifiedWch == L'+')
            {
                return setKeyState(CATEGORY_COMPOSING, FUNCTION_INPUT);
            }
            if (Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed))
            {
                // 日语模式禁用 -/= 翻页：'-' 输入长音符（ー），其余字符退回标点上屏。
                return *classifiedCode == VK_OEM_MINUS && *classifiedWch == L'-'
                           ? setKeyState(CATEGORY_COMPOSING, FUNCTION_INPUT)
                           : setKeyState(CATEGORY_COMPOSING, FUNCTION_PUNCTUATION);
            }
            return setKeyState(CATEGORY_CANDIDATE, FUNCTION_SERVER_CANDIDATE_KEY);
        case VK_OEM_COMMA:
        case VK_OEM_PERIOD:
        case VK_OEM_4:
        case VK_OEM_6:
        case VK_TAB:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_UP:
        case VK_DOWN:
            return setKeyState(CATEGORY_CANDIDATE, FUNCTION_SERVER_CANDIDATE_KEY);
        default:
            break;
        }

        if (*classifiedCode >= L'1' && *classifiedCode <= L'9')
        {
            // U-mode: bare digits compose hex; Shift+1..9 selects candidates.
            if (shadow.unicodeMode)
            {
                const bool shift_only = (capturedModifiers & 0b00000111u) == 0b00000001u;
                if (shift_only)
                {
                    return setKeyState(CATEGORY_CANDIDATE, FUNCTION_SELECT_BY_NUMBER);
                }
                return setKeyState(CATEGORY_COMPOSING, FUNCTION_INPUT);
            }
            return setKeyState(CATEGORY_CANDIDATE, FUNCTION_SELECT_BY_NUMBER);
        }
        if (*classifiedCode == L'0' && shadow.unicodeMode)
        {
            return setKeyState(CATEGORY_COMPOSING, FUNCTION_INPUT);
        }
        if (Global::CommitWithHighlightedCandPunc.count(*classifiedWch) > 0 ||
            (shadow.punctuationOpen && _pCompositionProcessorEngine->IsPunctuation(*classifiedWch)))
        {
            return setKeyState(CATEGORY_COMPOSING, FUNCTION_PUNCTUATION);
        }
        return false;
    }

    if (shadow.punctuationOpen && _pCompositionProcessorEngine->IsPunctuation(*classifiedWch))
    {
        return setKeyState(CATEGORY_COMPOSING, FUNCTION_PUNCTUATION);
    }
    if (shadow.doubleSingleByteOpen && _pCompositionProcessorEngine->IsDoubleSingleByte(*classifiedWch))
    {
        return setKeyState(CATEGORY_COMPOSING, FUNCTION_DOUBLE_SINGLE_BYTE);
    }
    if (!shadow.imeOpen && *classifiedWch != L'\0' && std::iswprint(static_cast<wint_t>(*classifiedWch)) != 0)
    {
        // A queued Shift may make this future key English. It still belongs
        // behind the FIFO prefix; CATEGORY_NONE/FUNCTION_NONE marks a direct
        // application-text replay instead of misclassifying it as Chinese.
        return true;
    }
    return false;
}

bool CMetasequoiaIME::_QueueDeferredKeyDown(_In_ ITfContext *pContext, WPARAM wParam, LPARAM lParam,
                                            WCHAR translatedWch, UINT modifiersDown, const _KEYSTROKE_STATE &keyState,
                                            bool scheduleDrain)
{
    // Repeats are owned but do not enqueue another global configuration toggle.
    if (keyState.Function == FUNCTION_TOGGLE_CHARACTER_SET && (lParam & 0x40000000) != 0)
        return true;
    if (pContext == nullptr || !_DeferredKeyQueueHasCapacity())
    {
        return false;
    }

    pContext->AddRef();
    DeferredKeyDown key;
    key.kind = keyState.Category == CATEGORY_NONE && keyState.Function == FUNCTION_NONE
                   ? DeferredKeyDown::Kind::ApplicationText
                   : DeferredKeyDown::Kind::KeyDown;
    key.context = pContext;
    key.wParam = wParam;
    key.lParam = lParam;
    key.translatedWch = translatedWch;
    key.modifiersDown = modifiersDown;
    key.keyState = keyState;
    key.focusGeneration = _deferredKeyFocusGeneration;
    key.queuedAtMs = GetTickCount64();
    _deferredKeyDowns.push_back(key);
    if (key.kind == DeferredKeyDown::Kind::KeyDown)
    {
        _ApplyDeferredKeyProjection(keyState, translatedWch);
    }
    else
    {
        _EnsureDeferredKeyProjection();
    }
    if (scheduleDrain)
    {
        _ScheduleDeferredKeyDownDrain();
    }
    return true;
}

bool CMetasequoiaIME::_QueueDeferredPreservedKey(_In_ ITfContext *pContext, REFGUID preservedKey)
{
    if (pContext == nullptr || !_DeferredKeyQueueHasCapacity())
    {
        return false;
    }

    pContext->AddRef();
    DeferredKeyDown key;
    key.kind = DeferredKeyDown::Kind::PreservedKey;
    key.context = pContext;
    key.preservedKey = preservedKey;
    key.focusGeneration = _deferredKeyFocusGeneration;
    key.queuedAtMs = GetTickCount64();
    _deferredKeyDowns.push_back(key);
    _ApplyDeferredPreservedKeyProjection(preservedKey);
    _ScheduleDeferredKeyDownDrain();
    return true;
}

void CMetasequoiaIME::_ClearDeferredKeyDowns()
{
    const size_t queuedCount = _deferredKeyDowns.size();
    const bool hadInFlight = _hasDeferredKeyInFlight;
    const uint64_t inFlightToken = _deferredKeyReplayToken;
    if (queuedCount != 0 || hadInFlight)
    {
        DebugTsfIssue47(L"deferred-queue-cleared", FANY_IME_NO_REQUEST_ID,
                        hadInFlight ? static_cast<UINT>(_deferredKeyInFlight.wParam) : 0,
                        hadInFlight ? _deferredKeyInFlight.translatedWch : L'\0',
                        hadInFlight ? _deferredKeyInFlight.keyState.Category : 0,
                        hadInFlight ? _deferredKeyInFlight.keyState.Function : 0, -1, _IsComposing(),
                        _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_FALSE,
                        inFlightToken);
    }
    // A posted drain belongs to the current message window/generation.  It
    // may never run if Deactivate destroys that window, so never carry this
    // latch into a later Activate on the same TIP instance.
    _deferredKeyDrainPosted = false;
    if (++_deferredKeyFocusGeneration == 0)
    {
        ++_deferredKeyFocusGeneration;
    }
    while (!_deferredKeyDowns.empty())
    {
        ITfContext *context = _deferredKeyDowns.front().context;
        _deferredKeyDowns.pop_front();
        if (context)
        {
            context->Release();
        }
    }
    if (_hasDeferredKeyInFlight)
    {
        ITfContext *context = _deferredKeyInFlight.context;
        _hasDeferredKeyInFlight = false;
        _deferredKeyReplayToken = 0;
        _deferredKeyInFlight = {};
        if (context)
        {
            context->Release();
        }
    }
    _deferredKeyProjectionValid = false;
    _deferredProjectedImeOpen = false;
    _deferredProjectedPunctuationOpen = false;
    _deferredProjectedDoubleSingleByteOpen = false;
    _deferredProjectedInputLength = 0;
    _deferredProjectedRawInput.clear();
    _deferredProjectedCaret = 0;
    _deferredProjectedCandidateActive = false;
    _deferredProjectedUnicodeMode = false;
    _shiftHotkeyArmed = false;
    _ctrlHotkeyArmed = false;
    _backspaceHoldArmed = false;
}

void CMetasequoiaIME::_ArmEmptyDeferredProjection()
{
    // Keys typed while the reset is pending are drained after it has cancelled
    // the composition, so classify them against that empty composition rather
    // than against the one still on screen.
    _deferredKeyProjectionValid = false;
    _EnsureDeferredKeyProjection();
    _deferredProjectedInputLength = 0;
    _deferredProjectedRawInput.clear();
    _deferredProjectedCaret = 0;
    _deferredProjectedCandidateActive = false;
    _deferredProjectedUnicodeMode = false;
}

void CMetasequoiaIME::_ReleaseIdleDeferredProjection()
{
    // Once the reset gate has reopened and nothing is queued, the real
    // composition is authoritative again and the healthy path (including the
    // synchronous first-key drain) must not stay behind a stale barrier.
    if (!_deferredKeyProjectionValid || !_deferredKeyDowns.empty() || _hasDeferredKeyInFlight ||
        _localSessionResetPending.load(std::memory_order_acquire))
    {
        return;
    }
    _deferredKeyProjectionValid = false;
    _deferredProjectedInputLength = 0;
    _deferredProjectedRawInput.clear();
    _deferredProjectedCaret = 0;
    _deferredProjectedCandidateActive = false;
    _deferredProjectedUnicodeMode = false;
}

void CMetasequoiaIME::_ResetSessionAfterFailure(DeferredKeyFailureKind kind)
{
    if (kind == DeferredKeyFailureKind::Stale)
    {
        return;
    }
    DebugTsfIssue47(L"session-failure-reset", FANY_IME_NO_REQUEST_ID, 0, L'\0', 0, static_cast<UINT>(kind), -1,
                    _IsComposing(),
                    _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_FALSE,
                    _deferredKeyReplayToken);
    // Every queued key was classified against the composition that is being
    // discarded: swallow them with it, never hand them back or resend them.
    // The Backspace hold guard survives, so the rest of a hold that began in
    // the composition cannot delete document text once it is gone (#347).
    const bool backspaceHoldArmed = _backspaceHoldArmed;
    _ClearDeferredKeyDowns();
    _backspaceHoldArmed = backspaceHoldArmed;

    if (kind == DeferredKeyFailureKind::Transport)
    {
        MarkNamedpipeSessionDirtyForOwner(this);
    }
    else if (kind == DeferredKeyFailureKind::Resync)
    {
        (void)_RequestLocalResync();
    }
    // Offline keeps the local composition: it is the only authority there.
    if (kind != DeferredKeyFailureKind::Offline && _localSessionResetPending.load(std::memory_order_acquire))
    {
        _ArmEmptyDeferredProjection();
    }
    _TryLeaveServerUnavailableFallback();
}

void CMetasequoiaIME::_CompleteDeferredKeyReplay(uint64_t replayToken)
{
    if (replayToken == 0 || !_hasDeferredKeyInFlight || _deferredKeyReplayToken != replayToken)
    {
        return;
    }

    DebugTsfIssue47(L"deferred-replay-complete", FANY_IME_NO_REQUEST_ID, static_cast<UINT>(_deferredKeyInFlight.wParam),
                    _deferredKeyInFlight.translatedWch, _deferredKeyInFlight.keyState.Category,
                    _deferredKeyInFlight.keyState.Function, 1, _IsComposing(),
                    _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_OK,
                    replayToken);

    ITfContext *context = _deferredKeyInFlight.context;
    if (_deferredKeyDowns.empty())
    {
        // The exact final edit session has completed, so the real composition
        // has caught up with the future projection.
        _deferredKeyProjectionValid = false;
        _deferredProjectedInputLength = 0;
        _deferredProjectedRawInput.clear();
        _deferredProjectedCaret = 0;
        _deferredProjectedCandidateActive = false;
        _deferredProjectedUnicodeMode = false;
    }
    _deferredKeyInFlight = {};
    _hasDeferredKeyInFlight = false;
    _deferredKeyReplayToken = 0;
    if (context)
    {
        context->Release();
    }
    _TryLeaveServerUnavailableFallback();
    _ScheduleDeferredKeyDownDrain();
}

bool CMetasequoiaIME::_IsDeferredKeyReplayCurrent(uint64_t replayToken, uint64_t focusGeneration,
                                                  _In_opt_ ITfContext *expectedContext) const
{
    return replayToken != 0 && _hasDeferredKeyInFlight && _deferredKeyReplayToken == replayToken &&
           focusGeneration == _deferredKeyFocusGeneration && _deferredKeyInFlight.focusGeneration == focusGeneration &&
           (expectedContext == nullptr || _deferredKeyInFlight.context == expectedContext);
}

void CMetasequoiaIME::_FailDeferredKey(uint64_t replayToken, DeferredKeyFailureReason reason)
{
    if (replayToken == 0 || !_hasDeferredKeyInFlight || _deferredKeyReplayToken != replayToken)
    {
        return;
    }
    DeferredKeyFailureKind kind = ResolveDeferredKeyFailure(reason, _serverUnavailableFallbackActive);
    if (_deferredKeyInFlight.focusGeneration != _deferredKeyFocusGeneration)
    {
        // A focus change already discarded everything this key belonged to.
        kind = DeferredKeyFailureKind::Stale;
    }
    const std::wstring stage = std::wstring(L"deferred-key-dropped:") + DeferredKeyFailureKindName(kind) + L":" +
                               DeferredKeyFailureReasonName(reason);
    DebugTsfIssue47(stage.c_str(), FANY_IME_NO_REQUEST_ID, static_cast<UINT>(_deferredKeyInFlight.wParam),
                    _deferredKeyInFlight.translatedWch, _deferredKeyInFlight.keyState.Category,
                    _deferredKeyInFlight.keyState.Function, 1, _IsComposing(),
                    _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_FALSE,
                    replayToken);
    if (kind == DeferredKeyFailureKind::Stale)
    {
        // Retire the token without applying the key. Whatever superseded it
        // owns the cleanup; the keys behind it keep their order.
        _CompleteDeferredKeyReplay(replayToken);
        return;
    }
    // The key is dropped with the queue: an eaten key is never handed back to
    // the host and never sent again. A delivered commit whose reply was lost
    // may already have run on the Server; resending it would pick again from a
    // page the first run may have reordered, so the user chooses again.
    _ResetSessionAfterFailure(kind);
}

void CMetasequoiaIME::_ScheduleDeferredKeyDownDrain()
{
    if (!_deferredKeyDowns.empty() && !_hasDeferredKeyInFlight && !_deferredKeyDrainPosted && _msgWndHandle &&
        IsWindow(_msgWndHandle))
    {
        _deferredKeyDrainPosted = true;
        if (!PostMessage(_msgWndHandle, WM_DrainDeferredKeyDown, 0, 0))
        {
            // The owner window is thread-affine. A synchronous fallback keeps
            // a transient queue-post failure from stranding an eaten key.
            SendMessage(_msgWndHandle, WM_DrainDeferredKeyDown, 0, 0);
        }
    }
}

bool CMetasequoiaIME::_IsServerUnavailableFallbackActive() const
{
    return _serverUnavailableFallbackActive;
}

void CMetasequoiaIME::_TryLeaveServerUnavailableFallback()
{
    const uint64_t expectedToken = _expectedWorkerFocusToken.load(std::memory_order_acquire);
    if (_serverUnavailableFallbackActive && !_IsComposing() && expectedToken != 0 &&
        _workerCommitReady.load(std::memory_order_acquire) &&
        _acknowledgedWorkerFocusToken.load(std::memory_order_acquire) == expectedToken)
    {
        _serverUnavailableFallbackActive = false;
    }
}

void CMetasequoiaIME::_DrainOneDeferredKeyDown()
{
    _deferredKeyDrainPosted = false;
    if (_hasDeferredKeyInFlight || _deferredKeyDowns.empty() || !Global::g_connected)
    {
        return;
    }
    if (_localSessionResetPending.load(std::memory_order_acquire))
    {
        PostOwnerMessageWithSyncFallback(_msgWndHandle, WM_IpcReconnect);
        return;
    }
    if (_serverUnavailableFallbackActive)
    {
        // Every key typed into the offline lane is one more request for the
        // Server. Re-probing the transport here is not allowed while the local
        // composition owns the keystroke stream, so count the key itself.
        _NoteKeyEventIpcFailure();
    }
    else if (!EnsureNamedpipeFocusSessionActivated())
    {
        // Do not strand an eaten key when the Server cannot establish a
        // focus session. The queued FIFO becomes an isolated local/raw-input
        // lane until its composition has been finalized.
        _serverUnavailableFallbackActive = true;
        _NoteKeyEventIpcFailure();
    }

    _deferredKeyInFlight = _deferredKeyDowns.front();
    _deferredKeyDowns.pop_front();
    _hasDeferredKeyInFlight = true;
    do
    {
        _deferredKeyReplayToken = ++_nextDeferredKeyReplayToken;
    } while (_deferredKeyReplayToken == 0);

    DeferredKeyDown &key = _deferredKeyInFlight;
    if (key.queuedAtMs != 0)
    {
        DebugTsfKeyLatency(L"deferred-key-queue", 0, static_cast<double>(GetTickCount64() - key.queuedAtMs), S_OK);
    }
    const uint64_t replayToken = _deferredKeyReplayToken;
    const uint64_t focusToken = _CaptureFocusSessionToken();
    if (key.focusGeneration != _deferredKeyFocusGeneration)
    {
        // All queued keys belong to the focused top context that captured
        // them. Never dispatch into another editor after another focus change.
        _ClearDeferredKeyDowns();
        return;
    }
    if (!_serverUnavailableFallbackActive && !_IsFocusSessionCurrent(focusToken, key.context))
    {
        // The session is ready, so the key's context is no longer the focused
        // top context: never dispatch it into another editor.
        _FailDeferredKey(replayToken, DeferredKeyFailureReason::Superseded);
        return;
    }

    BOOL eaten = FALSE;
    if (key.kind == DeferredKeyDown::Kind::ApplicationText)
    {
        (void)_RequestDeferredApplicationTextEditSession(key.context, key.translatedWch, focusToken,
                                                         key.focusGeneration, replayToken);
        return;
    }
    if (key.kind == DeferredKeyDown::Kind::PreservedKey)
    {
        const auto preservedAction = _pCompositionProcessorEngine
                                         ? _pCompositionProcessorEngine->GetPreservedKeyAction(key.preservedKey)
                                         : CCompositionProcessorEngine::PreservedKeyAction::None;
        const bool awaitsEditSession =
            preservedAction == CCompositionProcessorEngine::PreservedKeyAction::ToggleImeMode;
        // OnPreservedKey changes compartments synchronously; a failure of the
        // commit phase that follows drops the key and never toggles twice.
        _DispatchPreservedKey(key.context, key.preservedKey, &eaten, key.focusGeneration, true, replayToken);
        if (!awaitsEditSession && _deferredKeyReplayToken == replayToken)
        {
            _CompleteDeferredKeyReplay(replayToken);
        }
        return;
    }

    if (_serverUnavailableFallbackActive)
    {
        _KEYSTROKE_STATE offlineState = key.keyState;
        if (offlineState.Function == FUNCTION_TOGGLE_CHARACTER_SET)
        {
            _CompleteDeferredKeyReplay(replayToken);
            return;
        }
        offlineState.Category = CATEGORY_COMPOSING;
        switch (offlineState.Function)
        {
        case FUNCTION_CONVERT:
        case FUNCTION_FINALIZE_CANDIDATELIST:
        case FUNCTION_FINALIZE_CANDIDATELISTForVKReturn:
        case FUNCTION_SELECT_BY_NUMBER:
        case FUNCTION_SERVER_CANDIDATE_KEY:
            // There is no candidate authority offline. Commit exactly the
            // raw text already held by the TSF composition.
            offlineState.Function = FUNCTION_FINALIZE_TEXTSTORE;
            break;
        case FUNCTION_MOVE_PAGE_UP:
        case FUNCTION_MOVE_PAGE_DOWN:
        case FUNCTION_MOVE_PAGE_TOP:
        case FUNCTION_MOVE_PAGE_BOTTOM:
            _CompleteDeferredKeyReplay(replayToken);
            return;
        default:
            break;
        }
        _InvokeKeyHandler(key.context, static_cast<UINT>(key.wParam), key.translatedWch, static_cast<DWORD>(key.lParam),
                          offlineState, FANY_IME_NO_REQUEST_ID, {}, 0, 0, 0, replayToken);
        return;
    }

    const KeyDownDispatchResult result =
        _DispatchKeyDown(key.context, key.wParam, key.lParam, &eaten, &key.translatedWch, &key.modifiersDown,
                         &key.keyState, false, key.focusGeneration, replayToken);
    if (result == KeyDownDispatchResult::Superseded)
    {
        _FailDeferredKey(replayToken, DeferredKeyFailureReason::Superseded);
        return;
    }
    if (result == KeyDownDispatchResult::TransportFailed)
    {
        // DefinitelyNotSent and DeliveryAmbiguous alike: the key is dropped,
        // never written again.
        _FailDeferredKey(replayToken, DeferredKeyFailureReason::TransportBroken);
        return;
    }

    if (result == KeyDownDispatchResult::Complete && _deferredKeyReplayToken == replayToken)
    {
        _CompleteDeferredKeyReplay(replayToken);
    }
}
