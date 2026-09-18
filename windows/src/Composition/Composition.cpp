#include "Private.h"
#include "Globals.h"
#include "MetasequoiaIME.h"
#include "CompositionProcessorEngine.h"
#include <cwctype>
#include <debugapi.h>
#include <fmt/xchar.h>
#include <string>
#include "FanyDefines.h"
#include "Ipc.h"
#include "stats_collector.h"

namespace
{
// Keep SEH helpers free of C++ objects with destructors (C2712).
HRESULT SafeRangeSetText(_In_ ITfRange *range, TfEditCookie ec, DWORD flags, _In_reads_opt_(len) const WCHAR *text,
                         LONG len)
{
    if (range == nullptr)
    {
        return E_INVALIDARG;
    }

    __try
    {
        return range->SetText(ec, flags, text, len);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return E_FAIL;
    }
}

HRESULT SafeRangeGetText(_In_ ITfRange *range, TfEditCookie ec, DWORD flags, _Out_writes_(len) WCHAR *text, ULONG len,
                         _Out_ ULONG *fetched)
{
    if (range == nullptr || text == nullptr || fetched == nullptr || len == 0)
    {
        return E_INVALIDARG;
    }

    __try
    {
        return range->GetText(ec, flags, text, len, fetched);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *fetched = 0;
        return E_FAIL;
    }
}

HRESULT SafeRangeShiftStart(_In_ ITfRange *range, TfEditCookie ec, LONG count, _Out_ LONG *shifted)
{
    if (range == nullptr || shifted == nullptr)
    {
        return E_INVALIDARG;
    }

    __try
    {
        return range->ShiftStart(ec, count, shifted, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *shifted = 0;
        return E_FAIL;
    }
}

HRESULT SafeRangeShiftEnd(_In_ ITfRange *range, TfEditCookie ec, LONG count, _Out_ LONG *shifted)
{
    if (range == nullptr || shifted == nullptr)
    {
        return E_INVALIDARG;
    }

    __try
    {
        return range->ShiftEnd(ec, count, shifted, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *shifted = 0;
        return E_FAIL;
    }
}

// Places the caret after a smart-punctuation replacement. SetText leaves the
// range covering the replaced text, so walking one character from its start
// lands after the single ASCII mark, or between the two halves of a converted
// pair: both forms want the caret after one character of replacement text.
void PlaceSmartPunctuationCaret(TfEditCookie ec, _In_ ITfContext *pContext, _In_ ITfRange *range)
{
    if (range == nullptr || pContext == nullptr)
    {
        return;
    }
    if (FAILED(range->Collapse(ec, TF_ANCHOR_START)))
    {
        return;
    }
    LONG shifted = 0;
    if (FAILED(SafeRangeShiftEnd(range, ec, 1, &shifted)) || shifted != 1)
    {
        return;
    }
    if (FAILED(range->Collapse(ec, TF_ANCHOR_END)))
    {
        return;
    }
    TF_SELECTION selection = {};
    selection.range = range;
    selection.style.ase = TF_AE_NONE;
    selection.style.fInterimChar = FALSE;
    pContext->SetSelection(ec, 1, &selection);
}

bool AreCaretModifiersPhysicallyDown()
{
    // VK_LWIN/VK_RWIN matter as much as Shift here: Win+Left is the window snap
    // shortcut, so an arrow released into a held Win chord rearranges the
    // desktop instead of moving the caret.
    static const int keys[] = {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN};
    for (int key : keys)
    {
        if ((GetAsyncKeyState(key) & 0x8000) != 0)
        {
            return true;
        }
    }
    return false;
}
} // namespace

WCHAR CMetasequoiaIME::_GetPrecedingDocumentChar(TfEditCookie ec, _In_ ITfContext *pContext)
{
    if (pContext == nullptr)
    {
        return 0;
    }

    ITfRange *pAnchor = nullptr;
    bool releaseAnchor = false;

    if (_IsComposing() && _pComposition != nullptr)
    {
        if (FAILED(_pComposition->GetRange(&pAnchor)) || pAnchor == nullptr)
        {
            return 0;
        }
        releaseAnchor = true;
    }
    else
    {
        TF_SELECTION tfSelection = {};
        ULONG fetched = 0;
        const HRESULT hr = pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched);
        if (FAILED(hr) || fetched != 1 || tfSelection.range == nullptr)
        {
            return 0;
        }
        pAnchor = tfSelection.range;
        releaseAnchor = true;
    }

    ITfRange *pClone = nullptr;
    WCHAR preceding = 0;
    HRESULT hr = pAnchor->Clone(&pClone);
    if (SUCCEEDED(hr) && pClone != nullptr)
    {
        hr = pClone->Collapse(ec, TF_ANCHOR_START);
        if (SUCCEEDED(hr))
        {
            LONG shifted = 0;
            hr = SafeRangeShiftStart(pClone, ec, -1, &shifted);
            if (SUCCEEDED(hr) && shifted == -1)
            {
                // Terminals and other shallow text stores accept the shift but
                // expose no text, leaving preceding at 0.
                WCHAR buffer[2] = {};
                ULONG fetched = 0;
                hr = SafeRangeGetText(pClone, ec, 0, buffer, 1, &fetched);
                if (SUCCEEDED(hr) && fetched == 1)
                {
                    preceding = buffer[0];
                }
            }
        }
        pClone->Release();
    }

    if (releaseAnchor && pAnchor != nullptr)
    {
        pAnchor->Release();
    }
    return preceding;
}

int CMetasequoiaIME::_GetPrecedingDocumentChars(TfEditCookie ec, _In_ ITfContext *pContext,
                                                _Out_writes_(count) WCHAR *buffer, int count)
{
    if (pContext == nullptr || buffer == nullptr || count <= 0)
    {
        return 0;
    }

    ITfRange *pAnchor = nullptr;
    bool releaseAnchor = false;

    if (_IsComposing() && _pComposition != nullptr)
    {
        if (FAILED(_pComposition->GetRange(&pAnchor)) || pAnchor == nullptr)
        {
            return 0;
        }
        releaseAnchor = true;
    }
    else
    {
        TF_SELECTION tfSelection = {};
        ULONG fetched = 0;
        const HRESULT hr = pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched);
        if (FAILED(hr) || fetched != 1 || tfSelection.range == nullptr)
        {
            return 0;
        }
        pAnchor = tfSelection.range;
        releaseAnchor = true;
    }

    ITfRange *pClone = nullptr;
    int readCount = 0;
    HRESULT hr = pAnchor->Clone(&pClone);
    if (SUCCEEDED(hr) && pClone != nullptr)
    {
        hr = pClone->Collapse(ec, TF_ANCHOR_START);
        if (SUCCEEDED(hr))
        {
            LONG shifted = 0;
            hr = SafeRangeShiftStart(pClone, ec, -count, &shifted);
            if (SUCCEEDED(hr) && shifted < 0)
            {
                // Terminals and other shallow text stores accept the shift but
                // expose no text, which reads back as 0 characters.
                ULONG fetched = 0;
                if (SUCCEEDED(SafeRangeGetText(pClone, ec, 0, buffer, static_cast<ULONG>(count), &fetched)))
                {
                    readCount = static_cast<int>(fetched);
                }
            }
        }
        pClone->Release();
    }

    if (releaseAnchor && pAnchor != nullptr)
    {
        pAnchor->Release();
    }
    return readCount;
}

WCHAR CMetasequoiaIME::_GetFollowingDocumentChar(TfEditCookie ec, _In_ ITfContext *pContext)
{
    if (pContext == nullptr)
    {
        return 0;
    }

    TF_SELECTION tfSelection = {};
    ULONG fetched = 0;
    if (FAILED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched)) || fetched != 1 ||
        tfSelection.range == nullptr)
    {
        return 0;
    }

    ITfRange *pClone = nullptr;
    WCHAR following = 0;
    HRESULT hr = tfSelection.range->Clone(&pClone);
    if (SUCCEEDED(hr) && pClone != nullptr)
    {
        hr = pClone->Collapse(ec, TF_ANCHOR_END);
        if (SUCCEEDED(hr))
        {
            LONG shifted = 0;
            hr = SafeRangeShiftEnd(pClone, ec, 1, &shifted);
            if (SUCCEEDED(hr) && shifted == 1)
            {
                // Terminals and other shallow text stores accept the shift but
                // expose no text, leaving following at 0.
                WCHAR buffer[2] = {};
                ULONG got = 0;
                hr = SafeRangeGetText(pClone, ec, 0, buffer, 1, &got);
                if (SUCCEEDED(hr) && got == 1)
                {
                    following = buffer[0];
                }
            }
        }
        pClone->Release();
    }

    tfSelection.range->Release();
    return following;
}

WCHAR CMetasequoiaIME::_GetPrecedingCharForSmartPunctuation(TfEditCookie ec, _In_ ITfContext *pContext)
{
    if (_smartPunctuationShadowValid)
    {
        return _smartPunctuationShadowChar;
    }
    return _GetPrecedingDocumentChar(ec, pContext);
}

bool CMetasequoiaIME::_SmartPunctuationFingerprintMatches(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR beforeChar)
{
    if (beforeChar == 0)
    {
        // Nothing was recorded at commit time, so there is no fingerprint to
        // check against.
        return true;
    }

    // Read the two characters left of the caret: the punctuation itself and,
    // before it, the character this check compares. A shallow text store
    // accepts the shift but reads back nothing, and there the pending state is
    // the only evidence available.
    WCHAR buffer[2] = {};
    const int readCount = _GetPrecedingDocumentChars(ec, pContext, buffer, 2);
    if (readCount == 0)
    {
        return true;
    }
    if (readCount == 1)
    {
        // Only the punctuation itself exists at the document start; anything
        // else means the document moved under the action.
        return false;
    }
    return buffer[0] == beforeChar;
}

WCHAR CMetasequoiaIME::_GetPairedPunctuationClosingFor(WCHAR opening)
{
    switch (opening)
    {
    case L'“':
        return L'”';
    case L'‘':
        return L'’';
    case L'【':
        return L'】';
    case L'{':
        return L'}';
    case L'《':
        return L'》';
    case L'〈':
        return L'〉';
    case L'（':
        return L'）';
    default:
        return 0;
    }
}

void CMetasequoiaIME::_PushPairedPunctuation(WCHAR opening, WCHAR closing)
{
    if (opening == 0 || closing == 0)
    {
        return;
    }

    // Deep nesting is never legitimate here; drop the outermost entry rather
    // than let a host that swallows our bookkeeping keys grow the stack.
    if (_pairedPunctuationStack.size() >= PAIRED_PUNCTUATION_MAX_DEPTH)
    {
        _pairedPunctuationStack.erase(_pairedPunctuationStack.begin());
    }

    PairedPunctuationEntry entry;
    entry.opening = opening;
    entry.closing = closing;
    entry.focusToken = _CaptureFocusSessionToken();
    _pairedPunctuationStack.push_back(entry);
}

void CMetasequoiaIME::_ClearPairedPunctuationStack()
{
    _pairedPunctuationStack.clear();
}

bool CMetasequoiaIME::_TryStepOverPairedPunctuation(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR closing)
{
    if (closing == 0 || _pairedPunctuationStack.empty())
    {
        return false;
    }

    const PairedPunctuationEntry top = _pairedPunctuationStack.back();
    if (top.closing != closing || !_IsFocusSessionCurrent(top.focusToken, pContext))
    {
        _ClearPairedPunctuationStack();
        return false;
    }

    // A mouse click or a host-side edit moves the caret without producing any
    // key event, so the stack alone cannot prove the closing half is still on
    // the right. Confirm against the document; hosts whose text store exposes
    // nothing read back 0, and there the stack is the only evidence available
    // and is trusted, exactly as the smart-punctuation shadow does.
    const WCHAR following = _GetFollowingDocumentChar(ec, pContext);
    if (following != 0 && following != closing)
    {
        _ClearPairedPunctuationStack();
        return false;
    }

    _pairedPunctuationStack.pop_back();
    // Nothing is committed on this path, so any pending smart-punctuation
    // action now describes a commit the user has stepped past.
    _ClearSmartPunctuationAction();
    _InvalidateSmartPunctuationShadow();
    _QueuePairedPunctuationCaretMove(1);
    return true;
}

void CMetasequoiaIME::_NoteKeyForPairedPunctuation(UINT code)
{
    if (_pairedPunctuationStack.empty() && _pendingPairedCaretDelta == 0)
    {
        return;
    }

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
    case VK_CAPITAL:
        // Modifier presses edit nothing, so the tracked pairs still hold.
        return;
    case VK_BACK:
    case VK_DELETE:
    case VK_INSERT:
    case VK_RETURN:
    case VK_TAB:
    case VK_ESCAPE:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
        // Anything that moves the caret or deletes text breaks the invariant
        // that the closing half sits immediately to the right of the caret. A
        // still-deferred caret move was computed against that invariant too, so
        // it must not land on whatever the user has just done instead.
        _ClearPairedPunctuationStack();
        _CancelPairedPunctuationCaretMove();
        return;
    default:
        break;
    }

    // Ordinary text insertion happens at the caret and keeps the closing half
    // on its right, so the stack survives it. A focus change does not.
    if (!_pairedPunctuationStack.empty() && !_IsFocusSessionCurrent(_pairedPunctuationStack.back().focusToken))
    {
        _ClearPairedPunctuationStack();
    }
}

void CMetasequoiaIME::_CancelPairedPunctuationCaretMove()
{
    if (_pairedCaretRetryTimerActive && _msgWndHandle != nullptr)
    {
        KillTimer(_msgWndHandle, TIMER_PAIRED_PUNCTUATION_CARET);
    }
    _pairedCaretRetryTimerActive = false;
    _pendingPairedCaretDelta = 0;
    _pendingPairedCaretFocusToken = 0;
    _pendingPairedCaretDeadline = 0;
}

void CMetasequoiaIME::_QueuePairedPunctuationCaretMove(int delta)
{
    if (delta == 0 || _msgWndHandle == nullptr)
    {
        return;
    }

    const uint64_t focusToken = _CaptureFocusSessionToken();
    if (focusToken == 0)
    {
        return;
    }

    // A still-pending move belongs to the same burst only while the focus
    // session matches; otherwise it is stale and its steps must not be added.
    if (_pendingPairedCaretDelta != 0 && _pendingPairedCaretFocusToken == focusToken)
    {
        delta += _pendingPairedCaretDelta;
    }

    delta = max(-PAIRED_PUNCTUATION_CARET_MAX_STEPS, min(PAIRED_PUNCTUATION_CARET_MAX_STEPS, delta));
    if (delta == 0)
    {
        _CancelPairedPunctuationCaretMove();
        return;
    }

    _pendingPairedCaretDelta = delta;
    _pendingPairedCaretFocusToken = focusToken;
    _pendingPairedCaretDeadline = GetTickCount64() + PAIRED_PUNCTUATION_CARET_TIMEOUT_MS;

    if (!PostMessage(_msgWndHandle, WM_PairedPunctuationCaretMove, static_cast<WPARAM>(focusToken & 0xFFFFFFFFULL),
                     static_cast<LPARAM>((focusToken >> 32) & 0xFFFFFFFFULL)))
    {
        _CancelPairedPunctuationCaretMove();
    }
}

void CMetasequoiaIME::_RunPairedPunctuationCaretMove()
{
    if (_pendingPairedCaretDelta == 0)
    {
        _CancelPairedPunctuationCaretMove();
        return;
    }

    if (!_IsFocusSessionCurrent(_pendingPairedCaretFocusToken) || GetTickCount64() > _pendingPairedCaretDeadline)
    {
        // The document this move was computed against is gone, or the chord was
        // held long enough that the caret is no longer where we left it.
        _ClearPairedPunctuationStack();
        _CancelPairedPunctuationCaretMove();
        return;
    }

    if (AreCaretModifiersPhysicallyDown())
    {
        if (!_pairedCaretRetryTimerActive && _msgWndHandle != nullptr)
        {
            _pairedCaretRetryTimerActive = SetTimer(_msgWndHandle, TIMER_PAIRED_PUNCTUATION_CARET,
                                                    PAIRED_PUNCTUATION_CARET_RETRY_MS, nullptr) != 0;
            if (!_pairedCaretRetryTimerActive)
            {
                _ClearPairedPunctuationStack();
                _CancelPairedPunctuationCaretMove();
            }
        }
        return;
    }

    const int delta = _pendingPairedCaretDelta;
    const WORD vk = delta < 0 ? VK_LEFT : VK_RIGHT;
    const int steps = delta < 0 ? -delta : delta;

    INPUT inputs[PAIRED_PUNCTUATION_CARET_MAX_STEPS * 2] = {};
    for (int i = 0; i < steps; ++i)
    {
        inputs[i * 2].type = INPUT_KEYBOARD;
        inputs[i * 2].ki.wVk = vk;
        inputs[i * 2].ki.dwExtraInfo = PAIRED_PUNCTUATION_SENDINPUT_EXTRA_INFO;
        inputs[i * 2 + 1] = inputs[i * 2];
        inputs[i * 2 + 1].ki.dwFlags = KEYEVENTF_KEYUP;
    }

    _CancelPairedPunctuationCaretMove();
    if (SendInput(static_cast<UINT>(steps * 2), inputs, sizeof(INPUT)) != static_cast<UINT>(steps * 2))
    {
        // The caret is no longer provably between the halves.
        _ClearPairedPunctuationStack();
    }
    _InvalidateSmartPunctuationShadow();
}

void CMetasequoiaIME::_ClearSmartPunctuationAction()
{
    _smartPunctuationAction = {};
}

bool CMetasequoiaIME::_CanInterceptSmartPunctuationConvert()
{
    const SmartPunctuationAction &state = _smartPunctuationAction;
    if (state.kind != SmartPunctuationAction::Kind::ChineseCommitted ||
        !Global::SmartPunctuationEnabled.load(std::memory_order_relaxed))
    {
        return false;
    }
    if (!Global::SmartPunctuationSpaceConvertEnabled.load(std::memory_order_relaxed))
    {
        return false;
    }
    // No time window: the state only survives until the next key, focus change
    // or foreground change, which is already narrower than an input-stream
    // burst, and a clock would cut off a slow but deliberate space.
    return _IsFocusSessionCurrent(state.focusToken) && GetForegroundWindow() == state.foregroundWindow;
}

bool CMetasequoiaIME::_CanInterceptSmartPunctuationRevert(WCHAR wch)
{
    const SmartPunctuationAction &state = _smartPunctuationAction;
    if (state.kind != SmartPunctuationAction::Kind::AsciiConverted || wch == 0 || wch != state.triggerKey ||
        !Global::SmartPunctuationEnabled.load(std::memory_order_relaxed) ||
        !Global::SmartPunctuationRepeatToChineseEnabled.load(std::memory_order_relaxed))
    {
        return false;
    }
    if (state.tick == 0 || GetTickCount64() - state.tick > SMART_PUNCTUATION_REPEAT_INTERVAL_MS)
    {
        return false;
    }
    return _IsFocusSessionCurrent(state.focusToken) && GetForegroundWindow() == state.foregroundWindow;
}

void CMetasequoiaIME::_WriteDirectSmartPunctuationState(WCHAR triggerKey, WCHAR chinese, WCHAR ascii, WCHAR beforeChar)
{
    _smartPunctuationAction = {};
    _smartPunctuationAction.kind = SmartPunctuationAction::Kind::AsciiConverted;
    _smartPunctuationAction.triggerKey = triggerKey;
    _smartPunctuationAction.chineseLeft = chinese;
    _smartPunctuationAction.asciiLeft = ascii;
    _smartPunctuationAction.beforeChar = beforeChar;
    _smartPunctuationAction.tick = GetTickCount64();
    _smartPunctuationAction.focusToken = _CaptureFocusSessionToken();
    _smartPunctuationAction.foregroundWindow = GetForegroundWindow();
}

void CMetasequoiaIME::_NoteCommittedChinesePunctuation(const std::wstring &committedText, bool autoClosedPair,
                                                       WCHAR beforeChar)
{
    if (committedText.empty() || !Global::SmartPunctuationEnabled.load(std::memory_order_relaxed))
    {
        // The feature being off at commit time must not arm a conversion that
        // a later re-enable would consume.
        return;
    }
    if (autoClosedPair)
    {
        // An auto-completed pair is out of scope on purpose. The two halves sit
        // on either side of the caret, so rewriting just the left one would
        // leave the right one orphaned (〔《>〕). The space inserts normally.
        return;
    }
    const WCHAR tail = committedText.back();
    if (!CCompositionProcessorEngine::IsSmartPunctuationChinese(tail))
    {
        // ASCII commits (direct output, numpad '.') keep whatever
        // _ResolveSmartPunctuation recorded so the same key can revert them.
        return;
    }

    _smartPunctuationAction = {};
    _smartPunctuationAction.kind = SmartPunctuationAction::Kind::ChineseCommitted;
    _smartPunctuationAction.chineseLeft = tail;
    _smartPunctuationAction.triggerKey = CCompositionProcessorEngine::GetSmartPunctuationAscii(tail);
    _smartPunctuationAction.beforeChar = beforeChar;
    _smartPunctuationAction.tick = GetTickCount64();
    _smartPunctuationAction.focusToken = _CaptureFocusSessionToken();
    _smartPunctuationAction.foregroundWindow = GetForegroundWindow();
}

void CMetasequoiaIME::_InvalidateSmartPunctuationShadow()
{
    _smartPunctuationShadowChar = 0;
    _smartPunctuationShadowValid = false;
}

void CMetasequoiaIME::_UpdateSmartPunctuationShadow(UINT code, WCHAR wch, bool isEaten)
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
    case VK_CAPITAL:
        // Modifier presses edit nothing, so the shadow still describes the caret.
        return;
    case VK_BACK:
    case VK_DELETE:
    case VK_INSERT:
    case VK_RETURN:
    case VK_TAB:
    case VK_ESCAPE:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
        _InvalidateSmartPunctuationShadow();
        return;
    default:
        break;
    }

    if (CCompositionProcessorEngine::IsSmartAsciiPunctuationKey(wch))
    {
        // _ResolveSmartPunctuation needs the current shadow to decide the form,
        // and records whatever it commits once that decision is made.
        return;
    }

    if (isEaten || wch == 0 || std::iswprint(static_cast<wint_t>(wch)) == 0)
    {
        // Eaten keys feed the composition and reach the document as committed
        // text, which even a proxy store exposes, so let the document answer.
        _InvalidateSmartPunctuationShadow();
        return;
    }

    _smartPunctuationShadowChar = wch;
    _smartPunctuationShadowValid = true;
}

void CMetasequoiaIME::_NoteKeyForSmartPunctuation(UINT code, WCHAR wch, bool isEaten, KEYSTROKE_FUNCTION function)
{
    _UpdateSmartPunctuationShadow(code, wch, isEaten);
    // Self-generated caret moves never reach here: the sinks bail out on the
    // extra-info marker before noting the key, so stepping over a pair does not
    // clear the very stack it is walking.
    _NoteKeyForPairedPunctuation(code);

    // The two intercept keys must survive until the edit session reads them:
    // the space that converts the last Chinese punctuation, and the punctuation
    // key that reverts a conversion. Only the classification result protects
    // the state; the claim predicates are broader than the modes that allow a
    // claim, so a passthrough key (English punctuation, full-width, composing,
    // candidate) must still clear a pending action rather than leave it armed
    // for the next space.
    if (function == FUNCTION_SMART_PUNCTUATION_CONVERT || function == FUNCTION_SMART_PUNCTUATION_REVERT)
    {
        return;
    }
    _ClearSmartPunctuationAction();
}

std::wstring CMetasequoiaIME::_ResolveSmartPunctuation(WCHAR wch, WCHAR precedingChar)
{
    if (_pCompositionProcessorEngine == nullptr)
    {
        return {};
    }

    // A fresh punctuation resolution replaces whatever conversion is pending:
    // the previous commit is no longer the most recent input.
    _ClearSmartPunctuationAction();

    const bool smartEnabled = Global::SmartPunctuationEnabled.load(std::memory_order_relaxed);
    std::wstring resolved = _pCompositionProcessorEngine->ResolvePunctuation(wch, precedingChar);
    if (!CCompositionProcessorEngine::IsSmartAsciiPunctuationKey(wch) || !smartEnabled)
    {
        if (!resolved.empty())
        {
            _smartPunctuationShadowChar = resolved.back();
            _smartPunctuationShadowValid = true;
        }
        return resolved;
    }

    // ResolvePunctuation returns the ASCII key only when the sub-switch for
    // the preceding character class is on; record it so the same key can
    // revert to Chinese within the window.
    const bool committedAscii = resolved.size() == 1 && resolved[0] == wch &&
                                (Global::SmartPunctuationDirectDigitEnabled.load(std::memory_order_relaxed) ||
                                 Global::SmartPunctuationDirectLetterEnabled.load(std::memory_order_relaxed));
    if (committedAscii)
    {
        const WCHAR *chinese = _pCompositionProcessorEngine->GetPunctuation(wch);
        const WCHAR chineseChar = (chinese != nullptr && chinese[0] != L'\0' && chinese[1] == L'\0') ? chinese[0] : 0;
        if (chineseChar != 0)
        {
            _WriteDirectSmartPunctuationState(wch, chineseChar, wch, precedingChar);
        }
    }
    if (!resolved.empty())
    {
        _smartPunctuationShadowChar = resolved.back();
        _smartPunctuationShadowValid = true;
    }
    return resolved;
}

HRESULT CMetasequoiaIME::_ExecuteSmartPunctuationAction(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR wch,
                                                        KEYSTROKE_FUNCTION function)
{
    const SmartPunctuationAction state = _smartPunctuationAction;
    // The action consumes the pending state either way: success keeps the
    // converted form armed for the revert, failure falls back to committing
    // the key normally. An early return must not leave the key armed.
    _ClearSmartPunctuationAction();

    const bool smartEnabled = Global::SmartPunctuationEnabled.load(std::memory_order_relaxed);
    if (function == FUNCTION_SMART_PUNCTUATION_CONVERT)
    {
        CStringRange spaceString;
        spaceString.Set(L" ", 1);
        const auto insertPlainSpace = [&]() -> HRESULT { return _AddCharAndFinalize(ec, pContext, &spaceString); };

        if (!smartEnabled || state.kind != SmartPunctuationAction::Kind::ChineseCommitted || state.chineseLeft == 0)
        {
            return insertPlainSpace();
        }

        WCHAR left = _GetPrecedingDocumentChar(ec, pContext);
        if (left == 0)
        {
            // Terminals and proxy stores expose no document text; the
            // commit-time state is the only evidence of what is on screen.
            left = state.chineseLeft;
        }
        if (left != state.chineseLeft)
        {
            // The caret moved (mouse click) or something else edited the
            // document. Rewriting a punctuation that was not just committed
            // would turn historical text into ASCII.
            return insertPlainSpace();
        }
        if (!_SmartPunctuationFingerprintMatches(ec, pContext, state.beforeChar))
        {
            // Same punctuation, different neighbourhood: the caret sits on a
            // historical copy rather than the spot just committed.
            return insertPlainSpace();
        }

        if (!CCompositionProcessorEngine::IsSmartPunctuationChinese(left) ||
            !Global::SmartPunctuationSpaceConvertEnabled.load(std::memory_order_relaxed))
        {
            return insertPlainSpace();
        }
        const WCHAR asciiOpen = CCompositionProcessorEngine::GetSmartPunctuationAscii(left);
        if (asciiOpen == 0)
        {
            return insertPlainSpace();
        }

        TF_SELECTION tfSelection = {};
        ULONG fetched = 0;
        HRESULT hr = pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched);
        if (FAILED(hr) || fetched != 1 || tfSelection.range == nullptr)
        {
            return insertPlainSpace();
        }

        // Cover the Chinese punctuation just committed: one character left of
        // the caret.
        LONG shiftedStart = 0;
        hr = tfSelection.range->Collapse(ec, TF_ANCHOR_START);
        if (SUCCEEDED(hr))
        {
            hr = SafeRangeShiftStart(tfSelection.range, ec, -1, &shiftedStart);
            if (SUCCEEDED(hr) && shiftedStart != -1)
            {
                hr = E_FAIL;
            }
        }
        const WCHAR replacement[1] = {asciiOpen};
        bool converted = false;
        if (SUCCEEDED(hr))
        {
            converted = SUCCEEDED(SafeRangeSetText(tfSelection.range, ec, 0, replacement, 1));
        }
        if (!converted)
        {
            tfSelection.range->Release();
            return insertPlainSpace();
        }

        PlaceSmartPunctuationCaret(ec, pContext, tfSelection.range);
        tfSelection.range->Release();

        _smartPunctuationAction = {};
        _smartPunctuationAction.kind = SmartPunctuationAction::Kind::AsciiConverted;
        _smartPunctuationAction.triggerKey = state.triggerKey;
        _smartPunctuationAction.chineseLeft = state.chineseLeft;
        _smartPunctuationAction.asciiLeft = asciiOpen;
        // The revert happens at the same spot, so it checks the same
        // fingerprint the commit recorded.
        _smartPunctuationAction.beforeChar = state.beforeChar;
        _smartPunctuationAction.tick = GetTickCount64();
        _smartPunctuationAction.focusToken = _CaptureFocusSessionToken();
        _smartPunctuationAction.foregroundWindow = GetForegroundWindow();
        _smartPunctuationShadowChar = asciiOpen;
        _smartPunctuationShadowValid = true;
        return S_OK;
    }

    // Revert: the punctuation key that produced the ASCII form is pressed
    // again inside the window. Replace it back with the Chinese form.
    const auto insertChinesePunctuation = [&]() -> HRESULT {
        std::wstring chinese;
        const WCHAR *punctuation =
            _pCompositionProcessorEngine != nullptr ? _pCompositionProcessorEngine->GetPunctuation(wch) : nullptr;
        if (punctuation != nullptr && punctuation[0] != L'\0')
        {
            chinese.assign(punctuation);
        }
        else if (state.chineseLeft != 0)
        {
            chinese.assign(1, state.chineseLeft);
        }
        if (chinese.empty())
        {
            return E_FAIL;
        }
        CStringRange chineseString;
        chineseString.Set(chinese.c_str(), chinese.length());
        const HRESULT insertHr = _AddCharAndFinalize(ec, pContext, &chineseString);
        if (SUCCEEDED(insertHr))
        {
            // The intercepted key never updated the shadow, so point it at the
            // character that now actually reached the document.
            _smartPunctuationShadowChar = chinese.back();
            _smartPunctuationShadowValid = true;
        }
        return insertHr;
    };

    if (!smartEnabled || !Global::SmartPunctuationRepeatToChineseEnabled.load(std::memory_order_relaxed) ||
        state.kind != SmartPunctuationAction::Kind::AsciiConverted || state.chineseLeft == 0)
    {
        return insertChinesePunctuation();
    }

    // The document read confirms what the state remembers: while the host
    // exposes text, what sits around the caret must be the converted form.
    // A read of 0 means the host exposes nothing and the state is trusted.
    const WCHAR left = _GetPrecedingDocumentChar(ec, pContext);
    if (left != 0 && left != state.asciiLeft)
    {
        return insertChinesePunctuation();
    }
    if (!_SmartPunctuationFingerprintMatches(ec, pContext, state.beforeChar))
    {
        return insertChinesePunctuation();
    }
    TF_SELECTION tfSelection = {};
    ULONG fetched = 0;
    HRESULT hr = pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched);
    if (FAILED(hr) || fetched != 1 || tfSelection.range == nullptr)
    {
        return insertChinesePunctuation();
    }

    LONG shiftedStart = 0;
    hr = tfSelection.range->Collapse(ec, TF_ANCHOR_START);
    if (SUCCEEDED(hr))
    {
        hr = SafeRangeShiftStart(tfSelection.range, ec, -1, &shiftedStart);
        if (SUCCEEDED(hr) && shiftedStart != -1)
        {
            hr = E_FAIL;
        }
    }

    const WCHAR replacement[1] = {state.chineseLeft};
    bool reverted = false;
    if (SUCCEEDED(hr))
    {
        reverted = SUCCEEDED(SafeRangeSetText(tfSelection.range, ec, 0, replacement, 1));
    }
    if (!reverted)
    {
        tfSelection.range->Release();
        return insertChinesePunctuation();
    }

    PlaceSmartPunctuationCaret(ec, pContext, tfSelection.range);
    tfSelection.range->Release();
    _smartPunctuationShadowChar = state.chineseLeft;
    _smartPunctuationShadowValid = true;
    return S_OK;
}

HRESULT CMetasequoiaIME::_ExecuteSmartPunctuationFallback(TfEditCookie ec, _In_ ITfContext *pContext, WCHAR wch,
                                                          KEYSTROKE_FUNCTION function)
{
    std::wstring fallback;
    if (function == FUNCTION_SMART_PUNCTUATION_CONVERT)
    {
        fallback = L" ";
    }
    else
    {
        const WCHAR *punctuation =
            _pCompositionProcessorEngine != nullptr ? _pCompositionProcessorEngine->GetPunctuation(wch) : nullptr;
        if (punctuation != nullptr && punctuation[0] != L'\0')
        {
            fallback.assign(punctuation);
        }
    }
    if (fallback.empty())
    {
        return E_FAIL;
    }

    CStringRange fallbackString;
    fallbackString.Set(fallback.c_str(), fallback.length());
    const HRESULT hr = _AddCharAndFinalize(ec, pContext, &fallbackString);
    if (SUCCEEDED(hr))
    {
        _smartPunctuationShadowChar = fallback.back();
        _smartPunctuationShadowValid = true;
    }
    // The key was claimed and can no longer be handed back, so the pending
    // action is consumed either way.
    _ClearSmartPunctuationAction();
    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfCompositionSink::OnCompositionTerminated
//
// Callback for ITfCompositionSink.  The system calls this method whenever
// someone other than this service ends a composition.
//----------------------------------------------------------------------------

STDAPI CMetasequoiaIME::OnCompositionTerminated(TfEditCookie ecWrite, _In_ ITfComposition *pComposition)
{
    if (pComposition == nullptr || !_IsCompositionCurrent(pComposition))
    {
        DebugTsfIssue47(L"host-terminated-stale-composition", FANY_IME_NO_REQUEST_ID, 0, L'\0', 0, 0, -1,
                        _IsComposing(),
                        _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_FALSE,
                        _CaptureCompositionEpoch());
        // A delayed termination callback for an older composition must never
        // delete the current candidate presenter or release newer ownership.
        return S_OK;
    }

    DebugTsfIssue47(L"host-terminated-current-composition", FANY_IME_NO_REQUEST_ID, 0, L'\0', 0, 0, -1, TRUE,
                    _pCompositionProcessorEngine ? _pCompositionProcessorEngine->GetVirtualKeyLength() : 0, S_OK,
                    _CaptureCompositionEpoch());

    // Count what the host is about to keep in the document. This path never
    // wipes the text (see the comment below), so whatever the range holds is
    // what the user actually got.
    _CaptureCompositionStats(ecWrite, pComposition);

    // The callback already carries a write cookie and the host has already
    // ended this exact composition. Detach ownership before making COM calls,
    // so a re-entrant/stale callback cannot observe it as current or tear down
    // a composition created later.
    ITfComposition *terminatedComposition = pComposition;
    terminatedComposition->AddRef();
    _pComposition->Release();
    _pComposition = nullptr;
    _voiceCompositionActive = false;

    ITfContext *ownerContext = _pContext;
    if (ownerContext)
    {
        ownerContext->AddRef();
        _pContext->Release();
        _pContext = nullptr;
    }

    uint64_t nextEpoch = _compositionEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (nextEpoch == 0)
    {
        _compositionEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    // Detach and end the old candidate/session before the COM cleanup calls
    // below can re-enter and create a presenter for a newer composition.
    _DeleteCandidateList(FALSE, ownerContext);
    if (Global::g_connected)
    {
        // EndCandidateUiSession is intentionally idempotent, but a presenter
        // can exist before its UI session becomes active. Always send one
        // exact routed clear so the Server cannot retain that composition.
        SendHideCandidateWndEventToUIProcess();
    }

    // Do NOT SetText(empty) here. Cancel paths already wipe via
    // _HandleCancel → _RemoveDummyCompositionForComposing. Wiping again after
    // a normal commit/EndComposition can delete the just-committed text and
    // destabilize fragile hosts (notably QQ).
    if (ownerContext)
    {
        _ClearCompositionDisplayAttributes(ecWrite, ownerContext, terminatedComposition);
    }
    terminatedComposition->Release();

    if (ownerContext)
    {
        ownerContext->Release();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _IsComposing
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_IsComposing()
{
    return _pComposition != nullptr;
}

//+---------------------------------------------------------------------------
//
// _SetComposition
//
//----------------------------------------------------------------------------

void CMetasequoiaIME::_SetComposition(_In_ ITfComposition *pComposition)
{
    _pComposition = pComposition;
    // A new composition may be captured exactly once by the statistics side
    // channel. This is the only creation entry (StartComposition.cpp).
    _compositionStatsCaptured.store(false, std::memory_order_release);
    uint64_t nextEpoch = _compositionEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (nextEpoch == 0)
    {
        _compositionEpoch.fetch_add(1, std::memory_order_acq_rel);
    }
}

//+---------------------------------------------------------------------------
//
// _CaptureCompositionStats
//
// Read-only statistics side channel shared by _TerminateComposition and
// OnCompositionTerminated. Classifies the composition text and queues the
// five counters; the text itself never leaves this process. Every failure is
// silent: statistics must not change input behavior, cleanup order or any
// HRESULT.
//---------------------------------------------------------------------------

void CMetasequoiaIME::_CaptureCompositionStats(TfEditCookie ec, _In_ ITfComposition *pComposition)
{
    if (_compositionStatsCaptured.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    if (pComposition == nullptr || !Global::StatisticsEnabled.load(std::memory_order_relaxed))
    {
        return;
    }

    ITfRange *pRange = nullptr;
    if (FAILED(pComposition->GetRange(&pRange)) || pRange == nullptr)
    {
        return;
    }

    // Compositions are preedit-sized. A longer range is truncated instead of
    // read in chunks: the capture must stay a cheap bypass of the commit path.
    constexpr ULONG kMaxCapturedUnits = 1024;
    WCHAR buffer[kMaxCapturedUnits];
    ULONG fetched = 0;
    const HRESULT readResult = SafeRangeGetText(pRange, ec, 0, buffer, kMaxCapturedUnits, &fetched);
    pRange->Release();
    if (FAILED(readResult) || fetched == 0)
    {
        return;
    }

    MsimeStats::QueueStatisticsCommit(MsimeStats::ClassifyText(buffer, fetched));
}

//+---------------------------------------------------------------------------
//
// _AddComposingAndChar
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_AddComposingAndChar(TfEditCookie ec, _In_ ITfContext *pContext,
                                              _In_ CStringRange *pstrAddString)
{
    HRESULT hr = S_OK;

    if (_pComposition != nullptr)
    {
        ITfRange *pRangeComposition = nullptr;
        hr = _pComposition->GetRange(&pRangeComposition);
        if (SUCCEEDED(hr) && pRangeComposition != nullptr)
        {
            hr = SafeRangeSetText(pRangeComposition, ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
            if (SUCCEEDED(hr))
            {
                _SetCompositionDisplayAttributesForRange(ec, pContext, pRangeComposition, _gaDisplayAttributeInput);

                TF_SELECTION sel;
                pRangeComposition->Collapse(ec, TF_ANCHOR_END);
                sel.range = pRangeComposition;
                sel.style.ase = TF_AE_NONE;
                sel.style.fInterimChar = FALSE;
                pContext->SetSelection(ec, 1, &sel);

                pRangeComposition->Release();
                return hr;
            }
            pRangeComposition->Release();
        }
    }

    ULONG fetched = 0;
    TF_SELECTION tfSelection;

    if (pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched) != S_OK || fetched == 0)
        return S_FALSE;

    //
    // make range start to selection
    //
    ITfRange *pAheadSelection = nullptr;
    hr = pContext->GetStart(ec, &pAheadSelection);
    if (SUCCEEDED(hr))
    {
        hr = pAheadSelection->ShiftEndToRange(ec, tfSelection.range, TF_ANCHOR_START);
        if (SUCCEEDED(hr))
        {
            ITfRange *pRange = nullptr;
            BOOL exist_composing = _FindComposingRange(ec, pContext, pAheadSelection, &pRange);

            std::wstring strAddString(pstrAddString->Get(), pstrAddString->GetLength());

            _SetInputString(ec, pContext, pRange, pstrAddString, exist_composing);

            if (pRange)
            {
                pRange->Release();
            }
        }
    }

    tfSelection.range->Release();

    if (pAheadSelection)
    {
        pAheadSelection->Release();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _AddCharAndFinalize
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_AddCharAndFinalize(TfEditCookie ec, _In_ ITfContext *pContext,
                                             _In_ CStringRange *pstrAddString)
{
    HRESULT hr = E_FAIL;

    if (_pComposition != nullptr)
    {
        hr = _SetCompositionTextAndSelection(ec, pContext, pstrAddString);
        if (SUCCEEDED(hr))
        {
            return hr;
        }
    }

    ULONG fetched = 0;
    TF_SELECTION tfSelection;

    if ((hr = pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched)) != S_OK || fetched != 1)
        return hr;

    // We use SetText here instead of InsertTextAtSelection because we've already started a composition
    // We don't want to the app to adjust the insertion point inside our composition
    hr = SafeRangeSetText(tfSelection.range, ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
    if (hr == S_OK)
    {
        // Statistics: text written here lands straight in the document, so it
        // never passes the two composition-end capture points (punctuation with
        // no active composition, full/half-width conversion, server-delivered
        // inserts, smart-punctuation fallbacks). Counting is limited to this
        // direct branch because the preedit branch above returns first: text set
        // into an active composition is still counted exactly once when the
        // composition ends. Read-only side channel: silent on failure, and no
        // HRESULT, selection or cleanup order changes.
        MsimeStats::QueueStatisticsCommit(
            MsimeStats::ClassifyText(pstrAddString->Get(), static_cast<size_t>(pstrAddString->GetLength())));

        // Update the selection, we'll make it an insertion point just past
        // the inserted text.
        tfSelection.range->Collapse(ec, TF_ANCHOR_END);
        pContext->SetSelection(ec, 1, &tfSelection);
    }

    tfSelection.range->Release();

    return hr;
}

HRESULT CMetasequoiaIME::_InsertTextToComposition(TfEditCookie ec, _In_ ITfContext *pContext,
                                                  _In_ CStringRange *pstrAddString)
{
    if (_pComposition == nullptr)
    {
        return E_FAIL;
    }

    ITfRange *pRangeComposition = nullptr;
    HRESULT hr = _pComposition->GetRange(&pRangeComposition);
    if (FAILED(hr) || pRangeComposition == nullptr)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    hr = SafeRangeSetText(pRangeComposition, ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
    if (SUCCEEDED(hr))
    {
        TF_SELECTION tfSelection;
        pRangeComposition->Collapse(ec, TF_ANCHOR_END);
        tfSelection.range = pRangeComposition;
        tfSelection.style.ase = TF_AE_NONE;
        tfSelection.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &tfSelection);
    }

    pRangeComposition->Release();
    return hr;
}

//+---------------------------------------------------------------------------
//
// _SetCompositionTextAndSelection
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_SetCompositionTextAndSelection(TfEditCookie ec, _In_ ITfContext *pContext,
                                                         _In_ CStringRange *pstrAddString)
{
    if (_pComposition == nullptr)
    {
        return E_FAIL;
    }

    ITfRange *pRangeComposition = nullptr;
    HRESULT hr = _pComposition->GetRange(&pRangeComposition);
    if (FAILED(hr) || pRangeComposition == nullptr)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    hr = SafeRangeSetText(pRangeComposition, ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
    if (SUCCEEDED(hr))
    {
        TF_SELECTION tfSelection;
        pRangeComposition->Collapse(ec, TF_ANCHOR_END);
        tfSelection.range = pRangeComposition;
        tfSelection.style.ase = TF_AE_NONE;
        tfSelection.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &tfSelection);
    }

    pRangeComposition->Release();
    return hr;
}

//+---------------------------------------------------------------------------
//
// _FindComposingRange
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_FindComposingRange(TfEditCookie ec, _In_ ITfContext *pContext, _In_ ITfRange *pSelection,
                                          _Outptr_result_maybenull_ ITfRange **ppRange)
{
    if (ppRange == nullptr)
    {
        return FALSE;
    }

    *ppRange = nullptr;

    // find GUID_PROP_COMPOSING
    ITfProperty *pPropComp = nullptr;
    IEnumTfRanges *enumComp = nullptr;

    HRESULT hr = pContext->GetProperty(GUID_PROP_COMPOSING, &pPropComp);
    if (FAILED(hr) || pPropComp == nullptr)
    {
        return FALSE;
    }

    hr = pPropComp->EnumRanges(ec, &enumComp, pSelection);
    if (FAILED(hr) || enumComp == nullptr)
    {
        pPropComp->Release();
        return FALSE;
    }

    BOOL isCompExist = FALSE;
    VARIANT var;
    ULONG fetched = 0;

    while (enumComp->Next(1, ppRange, &fetched) == S_OK && fetched == 1)
    {
        hr = pPropComp->GetValue(ec, *ppRange, &var);
        if (hr == S_OK)
        {
            if (var.vt == VT_I4 && var.lVal != 0)
            {
                isCompExist = TRUE;
                break;
            }
        }
        (*ppRange)->Release();
        *ppRange = nullptr;
    }

    pPropComp->Release();
    enumComp->Release();

    return isCompExist;
}

//+---------------------------------------------------------------------------
//
// _SetInputString
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_SetInputString(TfEditCookie ec, _In_ ITfContext *pContext, _Out_opt_ ITfRange *pRange,
                                         _In_ CStringRange *pstrAddString, BOOL exist_composing)
{
    ITfRange *pRangeInsert = nullptr;
    if (!exist_composing)
    {
        _InsertAtSelection(ec, pContext, pstrAddString, &pRangeInsert);
        if (pRangeInsert == nullptr)
        {
            return S_OK;
        }
        else
        {
            // pRange = pRangeInsert;

            /* To make TsfPad work, we need to get range manually */
            _pComposition->GetRange(&pRange);
        }
    }
    if (pRange != nullptr)
    {
        SafeRangeSetText(pRange, ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
    }

    _SetCompositionLanguage(ec, pContext);

    _SetCompositionDisplayAttributes(ec, pContext, _gaDisplayAttributeInput);

    // update the selection, we'll make it an insertion point just past
    // the inserted text.
    ITfRange *pSelection = nullptr;
    TF_SELECTION sel;

    if ((pRange != nullptr) && (pRange->Clone(&pSelection) == S_OK))
    {
        pSelection->Collapse(ec, TF_ANCHOR_END);

        sel.range = pSelection;
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &sel);
        pSelection->Release();
    }

    if (pRangeInsert)
    {
        pRangeInsert->Release();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _InsertAtSelection
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_InsertAtSelection(TfEditCookie ec, _In_ ITfContext *pContext,
                                            _In_ CStringRange *pstrAddString, _Outptr_ ITfRange **ppCompRange)
{
    ITfRange *rangeInsert = nullptr;
    ITfInsertAtSelection *pias = nullptr;
    HRESULT hr = S_OK;

    if (ppCompRange == nullptr)
    {
        hr = E_INVALIDARG;
        goto Exit;
    }

    *ppCompRange = nullptr;

    hr = pContext->QueryInterface(IID_ITfInsertAtSelection, (void **)&pias);
    if (FAILED(hr))
    {
        goto Exit;
    }

    hr = pias->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, pstrAddString->Get(), (LONG)pstrAddString->GetLength(),
                                     &rangeInsert);

    if (FAILED(hr) || rangeInsert == nullptr)
    {
        rangeInsert = nullptr;
        pias->Release();
        goto Exit;
    }

    *ppCompRange = rangeInsert;
    pias->Release();
    hr = S_OK;

Exit:
    return hr;
}

//+---------------------------------------------------------------------------
//
// _RemoveDummyCompositionForComposing
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::_RemoveDummyCompositionForComposing(TfEditCookie ec, _In_ ITfComposition *pComposition)
{
    HRESULT hr = S_OK;

    ITfRange *pRange = nullptr;

    if (pComposition)
    {
        hr = pComposition->GetRange(&pRange);
        if (SUCCEEDED(hr))
        {
            hr = SafeRangeSetText(pRange, ec, 0, nullptr, 0);
            pRange->Release();
        }
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// _SetCompositionLanguage
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_SetCompositionLanguage(TfEditCookie ec, _In_ ITfContext *pContext)
{
    HRESULT hr = S_OK;
    BOOL ret = TRUE;

    CCompositionProcessorEngine *pCompositionProcessorEngine = nullptr;
    pCompositionProcessorEngine = _pCompositionProcessorEngine;

    LANGID langidProfile = 0;
    pCompositionProcessorEngine->GetLanguageProfile(&langidProfile);

    ITfRange *pRangeComposition = nullptr;
    ITfProperty *pLanguageProperty = nullptr;

    // we need a range and the context it lives in
    hr = _pComposition->GetRange(&pRangeComposition);
    if (FAILED(hr))
    {
        ret = FALSE;
        goto Exit;
    }

    // get our the language property
    hr = pContext->GetProperty(GUID_PROP_LANGID, &pLanguageProperty);
    if (FAILED(hr))
    {
        ret = FALSE;
        goto Exit;
    }

    VARIANT var;
    var.vt = VT_I4; // we're going to set DWORD
    var.lVal = langidProfile;

    hr = pLanguageProperty->SetValue(ec, pRangeComposition, &var);
    if (FAILED(hr))
    {
        ret = FALSE;
        goto Exit;
    }

    pLanguageProperty->Release();
    pRangeComposition->Release();

Exit:
    return ret;
}
