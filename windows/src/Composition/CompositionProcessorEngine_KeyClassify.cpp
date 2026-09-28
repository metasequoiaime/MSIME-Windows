// Virtual-key classification: IsVirtualKeyNeed and IsVirtualKeyNeedForFreshComposition, with
// the keystroke, candidate and number-range tests they use.

#include "Private.h"
#include "MetasequoiaIME.h"
#include "CompositionProcessorEngine.h"
#include "TfInputProcessorProfile.h"
#include "Globals.h"
#include "FanyDefines.h"
#include "Compartment.h"
#include "LanguageBar.h"
#include "RegKey.h"
#include "define.h"
#include <msctf.h>
#include <string>
#include <fmt/xchar.h>
#include "Ipc.h"
#include "FanyUtils.h"
#include "FanyLog.h"
#include "EditSession.h"
#include "TfTextLayoutSink.h"
#include <new>

namespace
{
// True while the JIS direct-kana layout (input.japanese_schema="kana") is on.
bool IsJapaneseKanaLayoutActive()
{
    return Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed) &&
           Global::JapaneseKanaLayoutEnabled.load(std::memory_order_relaxed);
}

// Physical keys that carry a kana in the JIS 106 layout: letters, the digit
// row and the symbol positions (れ け ほ へ ゛ ゜ ろ ね る め). Shift selects the
// shifted legend (small kana / voiced kana / を / ー) on the server side, which
// maps by virtual-key rather than produced character.
bool IsJapaneseKanaPhysicalKey(UINT uCode)
{
    if (uCode >= 'A' && uCode <= 'Z')
        return true;
    if (uCode >= '0' && uCode <= '9')
        return true;
    switch (uCode)
    {
    case VK_OEM_1:      // れ / れ
    case VK_OEM_MINUS:  // ほ / ー
    case VK_OEM_3:      // へ / べ
    case VK_OEM_4:      // ゛
    case VK_OEM_5:      // ろ
    case VK_OEM_6:      // ゜
    case VK_OEM_7:      // け / げ
    case VK_OEM_COMMA:  // ね
    case VK_OEM_PERIOD: // る
    case VK_OEM_2:      // め
        return true;
    default:
        return false;
    }
}

// A JIS kana key counts as an input key only without Ctrl/Alt (Shift alone is
// the shifted kana legend).
bool IsJapaneseKanaInputKey(UINT uCode)
{
    if (!IsJapaneseKanaLayoutActive() || !IsJapaneseKanaPhysicalKey(uCode))
        return false;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return !ctrl && !alt;
}

// 日语模式禁用 -/= 翻页：'-' 是长音符（ー）的输入键。空编码时也要起头组合，
// 候选框第一项是长音符 ー、第二项是普通连字符 '-'（候选由服务端提供）。
// JIS 假名配列下该键是 ほ/ー，由服务器按物理键直映射，不走这里的长音候选。
bool IsJapaneseLongVowelKey(UINT uCode, WCHAR wch)
{
    return uCode == VK_OEM_MINUS && wch == L'-' && Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed) &&
           !Global::JapaneseKanaLayoutEnabled.load(std::memory_order_relaxed);
}

// '_' '=' '+' 在日语模式下也不翻页，但它们没有假名写法，按标点上屏处理，
// 且只在已有编码时接管——空编码时仍然是普通标点，归应用程序。
bool IsJapaneseMinusEqualPunctuationKey(UINT uCode, WCHAR wch, BOOL fComposing, CANDIDATE_MODE candidateMode,
                                        DWORD_PTR keystrokeLength)
{
    if (uCode != VK_OEM_MINUS && uCode != VK_OEM_PLUS)
    {
        return false;
    }
    if (keystrokeLength == 0 || !Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed))
    {
        return false;
    }
    // JIS layout: OEM_MINUS is the ほ/ー kana key, handled as direct input.
    if (Global::JapaneseKanaLayoutEnabled.load(std::memory_order_relaxed))
    {
        return false;
    }
    if (IsJapaneseLongVowelKey(uCode, wch))
    {
        return false;
    }
    return fComposing || candidateMode != CANDIDATE_NONE;
}

bool IsCommitWithHighlightedCandidatePunctuationInCandidateMode(UINT uCode, WCHAR wch, CANDIDATE_MODE candidateMode)
{
    if (candidateMode == CANDIDATE_NONE)
    {
        return false;
    }

    // Candidate paging keys must keep their navigation semantics even if the
    // corresponding character is also listed in CommitWithHighlightedCandPunc.
    switch (uCode)
    {
    case VK_PRIOR:
    case VK_NEXT:
    case VK_HOME:
    case VK_END:
    case VK_TAB:
        return false;
    case VK_OEM_MINUS:
    case VK_OEM_PLUS:
        // 日语模式下这两个键不翻页，交给下面的标点判定处理；但 '-' 走长音符输入，
        // 不是标点上屏。
        if (!Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed) || IsJapaneseLongVowelKey(uCode, wch))
        {
            return false;
        }
        break;
    default:
        break;
    }

    return wch != 0 && Global::CommitWithHighlightedCandPunc.count(wch) > 0;
}

bool IsManualPinyinSeparatorInComposition(WCHAR wch, BOOL fComposing, CANDIDATE_MODE candidateMode,
                                          DWORD_PTR keystrokeLength)
{
    if (wch != L'\'')
    {
        return false;
    }
    if (keystrokeLength == 0)
    {
        return false;
    }
    return fComposing || candidateMode != CANDIDATE_NONE;
}
} // namespace

//////////////////////////////////////////////////////////////////////
//
//    CCompositionProcessorEngine
//
//////////////////////////////////////////////////////////////////////

BOOL CCompositionProcessorEngine::IsVirtualKeyNeedForFreshComposition(UINT uCode, _In_reads_(1) WCHAR *pwch,
                                                                      _Out_opt_ _KEYSTROKE_STATE *pKeyState)
{
    if (pKeyState)
    {
        pKeyState->Category = CATEGORY_NONE;
        pKeyState->Function = FUNCTION_NONE;
    }

    // Classify against an actually empty composition. This path is used while
    // the old focus session is still being cancelled, so none of its candidate
    // mode, wildcard flags, or virtual-key buffer may affect the first key in
    // the replacement session.
    if (IsManualPinyinSeparatorInComposition(pwch ? *pwch : 0, FALSE, CANDIDATE_NONE, 0))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }
    // 日语模式下 '-' 单独按也要起头组合，弹出候选框选长音符 ー 或普通 '-'。
    if (IsJapaneseLongVowelKey(uCode, pwch ? *pwch : 0))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }
    // JIS direct-kana layout: the digit and symbol-row kana keys start a
    // composition on an empty buffer exactly like the letter keys.
    if (IsJapaneseKanaInputKey(uCode))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }
    if (IsVirtualKeyKeystrokeComposition(uCode, pKeyState, FUNCTION_INPUT))
    {
        return TRUE;
    }
    if (pwch && IsWildcard() && IsWildcardChar(*pwch) && !IsDisableWildcardAtFirst())
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }
    return FALSE;
}

//+---------------------------------------------------------------------------
//
// CCompositionProcessorEngine::IsVirtualKeyNeed
//
// Test virtual key code need to the Composition Processor Engine.
// param
//     [in] uCode - Specify virtual key code.
//     [in/out] pwch       - char code
//     [in] fComposing     - Specified composing.
//     [in] fCandidateMode - Specified candidate mode.
//     [out] pKeyState     - Returns function regarding virtual key.
// returns
//     If engine need this virtual key code, returns true. Otherwise returns false.
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsVirtualKeyNeed( //
    UINT uCode,                                     //
    _In_reads_(1) WCHAR *pwch,                      //
    BOOL fComposing,                                //
    CANDIDATE_MODE candidateMode,                   //
    BOOL hasCandidateWithWildcard,                  //
    _Out_opt_ _KEYSTROKE_STATE *pKeyState           //
)
{
    if (pKeyState)
    {
        pKeyState->Category = CATEGORY_NONE;
        pKeyState->Function = FUNCTION_NONE;
    }

    if (candidateMode == CANDIDATE_ORIGINAL)
    {
        fComposing = FALSE;
    }

    if (IsManualPinyinSeparatorInComposition(pwch ? *pwch : 0, fComposing, candidateMode, _keystrokeBuffer.GetLength()))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }

    if (IsJapaneseLongVowelKey(uCode, pwch ? *pwch : 0))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }

    // U-mode: bare digits compose hex; Shift+1..9 selects candidates.
    if (IsUnicodeModeComposition() && uCode >= L'0' && uCode <= L'9')
    {
        const bool shift_down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl_down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool shift_only = shift_down && !ctrl_down && !alt_down;
        if (shift_only && uCode >= L'1' && uCode <= L'9')
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_SELECT_BY_NUMBER;
            }
            return TRUE;
        }
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }
    if (IsUnicodeModeComposition() && _keystrokeBuffer.GetLength() == 1 && uCode == VK_OEM_PLUS && pwch &&
        *pwch == L'+')
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }

    if (IsJapaneseMinusEqualPunctuationKey(uCode, pwch ? *pwch : 0, fComposing, candidateMode,
                                           _keystrokeBuffer.GetLength()))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_PUNCTUATION;
        }
        return TRUE;
    }

    // JIS direct-kana layout: while entering reading text (empty buffer,
    // composing, or the incremental candidate list), every physical kana key —
    // including the digit row, comma/period and the voiced/half-voiced-mark
    // keys — is plain kana input and must not become candidate navigation or
    // punctuation. Candidate navigation still works via Space / PgUp / PgDn,
    // and the full candidate window (CANDIDATE_ORIGINAL) keeps digit selection.
    if ((fComposing || candidateMode == CANDIDATE_INCREMENTAL || candidateMode == CANDIDATE_NONE) &&
        IsJapaneseKanaInputKey(uCode))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }

    // The Server owns the configurable comma/period behavior. Always route
    // these keys through it while candidates are active; its response decides
    // whether the key navigates or commits the highlighted candidate with punctuation.
    const bool isCommaPeriodPagingKey = uCode == VK_OEM_COMMA || uCode == VK_OEM_PERIOD;
    const bool isBracketPagingKey = uCode == VK_OEM_4 || uCode == VK_OEM_6;
    const bool isMinusEqualPagingKey = (uCode == VK_OEM_MINUS || uCode == VK_OEM_PLUS) &&
                                       !Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed);
    if (candidateMode != CANDIDATE_NONE &&
        (isMinusEqualPagingKey || isCommaPeriodPagingKey || isBracketPagingKey || uCode == VK_TAB ||
         uCode == VK_PRIOR || uCode == VK_NEXT || uCode == VK_UP || uCode == VK_DOWN))
    {
        if (IsUnicodeModeComposition() && _keystrokeBuffer.GetLength() == 1 && uCode == VK_OEM_PLUS && pwch &&
            *pwch == L'+')
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = FUNCTION_INPUT;
            }
            return TRUE;
        }
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_CANDIDATE;
            pKeyState->Function = FUNCTION_SERVER_CANDIDATE_KEY;
        }
        return TRUE;
    }

    if (candidateMode != CANDIDATE_NONE && (uCode == VK_LEFT || uCode == VK_RIGHT))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = uCode == VK_LEFT ? FUNCTION_MOVE_LEFT : FUNCTION_MOVE_RIGHT;
        }
        return TRUE;
    }

    bool isMicrosoftShuangpinIngKey = false;
    if (Global::MicrosoftShuangpinEnabled.load(std::memory_order_relaxed) && uCode == VK_OEM_1 && pwch &&
        *pwch == L';' && _keystrokeBuffer.GetLength() > 0 && _keystrokeBuffer.Get())
    {
        const DWORD_PTR caret = min(_caretPosition, _keystrokeBuffer.GetLength());
        DWORD_PTR chunkLength = 0;
        for (DWORD_PTR index = caret; index > 0 && _keystrokeBuffer.Get()[index - 1] != L'\''; --index)
        {
            ++chunkLength;
        }
        isMicrosoftShuangpinIngKey = chunkLength % 2 == 1;
    }
    if (isMicrosoftShuangpinIngKey)
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_INPUT;
        }
        return TRUE;
    }

    if (IsCommitWithHighlightedCandidatePunctuationInCandidateMode(uCode, pwch ? *pwch : 0, candidateMode))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_COMPOSING;
            pKeyState->Function = FUNCTION_PUNCTUATION;
        }
        return TRUE;
    }

    if (fComposing || candidateMode == CANDIDATE_INCREMENTAL || candidateMode == CANDIDATE_NONE)
    {
        if (IsVirtualKeyKeystrokeComposition(uCode, pKeyState, FUNCTION_NONE)) // 26 basic English chars
        {
            return TRUE;
        }
        else if ((IsWildcard() && IsWildcardChar(*pwch) && !IsDisableWildcardAtFirst()) ||
                 (IsWildcard() && IsWildcardChar(*pwch) && IsDisableWildcardAtFirst() && _keystrokeBuffer.GetLength()))
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = FUNCTION_INPUT;
            }
            return TRUE;
        }
        else if (_hasWildcardIncludedInKeystrokeBuffer && uCode == VK_SPACE)
        {
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = FUNCTION_CONVERT_WILDCARD;
            }
            return TRUE;
        }
        if (Global::PureShiftKeyUp)
        {
            return TRUE;
        }
    }

    if (candidateMode == CANDIDATE_ORIGINAL)
    {
        BOOL isRetCode = TRUE;
        if (IsVirtualKeyKeystrokeCandidate(uCode, pKeyState, candidateMode, &isRetCode, &_KeystrokeCandidate))
        {
            return isRetCode;
        }

        if (hasCandidateWithWildcard)
        {
            if (IsVirtualKeyKeystrokeCandidate(uCode, pKeyState, candidateMode, &isRetCode,
                                               &_KeystrokeCandidateWildcard))
            {
                return isRetCode;
            }
        }

        // Candidate list could not handle key. We can try to restart the composition.
        if (IsVirtualKeyKeystrokeComposition(uCode, pKeyState, FUNCTION_INPUT))
        {
            if (candidateMode == CANDIDATE_ORIGINAL)
            {
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_FINALIZE_CANDIDATELIST_AND_INPUT;
                }
                return TRUE;
            }
        }
    }

    // CANDIDATE_INCREMENTAL should process Keystroke.Candidate virtual keys.
    else if (candidateMode == CANDIDATE_INCREMENTAL)
    {
        BOOL isRetCode = TRUE;
        if (IsVirtualKeyKeystrokeCandidate(uCode, pKeyState, candidateMode, &isRetCode, &_KeystrokeCandidate))
        {
            return isRetCode;
        }
    }

    if (!fComposing && candidateMode != CANDIDATE_ORIGINAL)
    {
        if (IsVirtualKeyKeystrokeComposition(uCode, pKeyState, FUNCTION_INPUT))
        {
            return TRUE;
        }
    }

    // System pre-defined keystroke
    if (fComposing)
    {
        if ((candidateMode != CANDIDATE_INCREMENTAL))
        {
            switch (uCode)
            {
            case VK_LEFT:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_LEFT;
                }
                return TRUE;
            case VK_RIGHT:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_RIGHT;
                }
                return TRUE;
            case VK_RETURN:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_FINALIZE_CANDIDATELIST;
                }
                return TRUE;
            case VK_ESCAPE:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_CANCEL;
                }
                return TRUE;
            case VK_BACK:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_BACKSPACE;
                }
                return TRUE;
            case VK_DELETE:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_DELETE;
                }
                return TRUE;

            case VK_UP:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_UP;
                }
                return TRUE;
            case VK_DOWN:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_DOWN;
                }
                return TRUE;
            case VK_PRIOR:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                }
                return TRUE;
            case VK_OEM_MINUS:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                }
                return TRUE;
            case VK_NEXT:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                }
                return TRUE;
            case VK_OEM_PLUS:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                }
                return TRUE;
            case VK_TAB:
                if (pKeyState)
                {
                    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
                    {
                        pKeyState->Category = CATEGORY_COMPOSING;
                        pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                    }
                    else
                    {
                        pKeyState->Category = CATEGORY_COMPOSING;
                        pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                    }
                }
                return TRUE;

            case VK_HOME:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_TOP;
                }
                return TRUE;
            case VK_END:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_BOTTOM;
                }
                return TRUE;

            case VK_SPACE:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_CONVERT;
                }
                return TRUE;
            }
        }
        else if (candidateMode == CANDIDATE_INCREMENTAL)
        {
            switch (uCode)
            {
                // VK_LEFT, VK_RIGHT - set *pIsEaten = FALSE for application could move caret left or right.
                // and for CUAS, invoke _HandleCompositionCancel() edit session due to ignore CUAS default key handler
                // for send out terminate composition
            case VK_LEFT:
            case VK_RIGHT: {
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_INVOKE_COMPOSITION_EDIT_SESSION;
                    pKeyState->Function = FUNCTION_CANCEL;
                }
            }
                return FALSE;

            case VK_RETURN:
                // Do something when user press return key
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_FINALIZE_CANDIDATELISTForVKReturn;
                }
                return TRUE;
            case VK_ESCAPE:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_CANCEL;
                }
                return TRUE;

                // VK_BACK - remove one char from reading string.
            case VK_BACK:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_BACKSPACE;
                }
                return TRUE;
            case VK_DELETE:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_DELETE;
                }
                return TRUE;

            case VK_UP:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_UP;
                }
                return TRUE;
            case VK_DOWN:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_DOWN;
                }
                return TRUE;
            case VK_PRIOR:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                }
                return TRUE;
            case VK_OEM_MINUS:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                }
                return TRUE;
            case VK_NEXT:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                }
                return TRUE;
            case VK_OEM_PLUS:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                }
                return TRUE;
            case VK_TAB:
                if (pKeyState)
                {
                    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
                    {
                        pKeyState->Category = CATEGORY_COMPOSING;
                        pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                    }
                    else
                    {
                        pKeyState->Category = CATEGORY_COMPOSING;
                        pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                    }
                }
                return TRUE;
            case VK_HOME:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_TOP;
                }
                return TRUE;
            case VK_END:
                if (pKeyState)
                {
                    pKeyState->Category = CATEGORY_CANDIDATE;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_BOTTOM;
                }
                return TRUE;

            case VK_SPACE: {
                if (candidateMode == CANDIDATE_INCREMENTAL)
                {
                    if (pKeyState)
                    {
                        pKeyState->Category = CATEGORY_CANDIDATE;
                        pKeyState->Function = FUNCTION_CONVERT;
                    }
                    return TRUE;
                }
                else
                {
                    if (pKeyState)
                    {
                        pKeyState->Category = CATEGORY_COMPOSING;
                        pKeyState->Function = FUNCTION_CONVERT;
                    }
                    return TRUE;
                }
            }
            }
        }
    }

    if (candidateMode == CANDIDATE_ORIGINAL)
    {
        switch (uCode)
        {
        case VK_UP:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_UP;
            }
            return TRUE;
        case VK_DOWN:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_DOWN;
            }
            return TRUE;
        case VK_PRIOR:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
            }
            return TRUE;
        case VK_OEM_MINUS:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
            }
            return TRUE;
        case VK_NEXT:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
            }
            return TRUE;
        case VK_OEM_PLUS:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
            }
            return TRUE;
        case VK_TAB:
            if (pKeyState)
            {
                if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_UP;
                }
                else
                {
                    pKeyState->Category = CATEGORY_COMPOSING;
                    pKeyState->Function = FUNCTION_MOVE_PAGE_DOWN;
                }
            }
            return TRUE;
        case VK_HOME:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_TOP;
            }
            return TRUE;
        case VK_END:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_MOVE_PAGE_BOTTOM;
            }
            return TRUE;
        case VK_RETURN:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_FINALIZE_CANDIDATELIST;
            }
            return TRUE;
        case VK_SPACE:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_CONVERT;
            }
            return TRUE;
        case VK_BACK:
        case VK_DELETE:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_CANCEL;
            }
            return TRUE;

        case VK_ESCAPE:
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;
                pKeyState->Function = FUNCTION_CANCEL;
            }
            return TRUE;
        }
    }

    //
    // Check whether the keystroke is number(for selecting candidate) and is in the range
    //
    if (IsKeystrokeRange(uCode, pKeyState, candidateMode))
    {
        return TRUE;
    }
    else if (pKeyState && pKeyState->Category != CATEGORY_NONE)
    {
        return FALSE;
    }

    if (*pwch && !IsVirtualKeyKeystrokeComposition(uCode, pKeyState, FUNCTION_NONE))
    {
        if (pKeyState)
        {
            pKeyState->Category = CATEGORY_INVOKE_COMPOSITION_EDIT_SESSION;
            pKeyState->Function = FUNCTION_FINALIZE_TEXTSTORE;
        }
        return FALSE;
    }

    return FALSE;
}

//+---------------------------------------------------------------------------
//
// CCompositionProcessorEngine::IsVirtualKeyKeystrokeComposition
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsVirtualKeyKeystrokeComposition( //
    UINT uCode,                                                     //
    _Out_opt_ _KEYSTROKE_STATE *pKeyState,                          //
    KEYSTROKE_FUNCTION function                                     //
)
{
    if (pKeyState == nullptr)
    {
        return FALSE;
    }

    pKeyState->Category = CATEGORY_NONE;
    pKeyState->Function = FUNCTION_NONE;

    // 26 basic English characters
    for (UINT i = 0; i < _KeystrokeComposition.Count(); i++)
    {
        _KEYSTROKE *pKeystroke = nullptr;

        pKeystroke = _KeystrokeComposition.GetAt(i);

        if ((pKeystroke->VirtualKey == uCode) &&
            (Global::ModifiersValue == 36 || Global::ModifiersValue == 260 || Global::ModifiersValue == 292 ||
             Global::CheckModifiers(Global::ModifiersValue, pKeystroke->Modifiers)))
        {
            if (function == FUNCTION_NONE)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = pKeystroke->Function;
                return TRUE;
            }
            else if (function == pKeystroke->Function)
            {
                pKeyState->Category = CATEGORY_COMPOSING;
                pKeyState->Function = pKeystroke->Function;
                return TRUE;
            }
        }
    }

    return FALSE;
}

//+---------------------------------------------------------------------------
//
// CCompositionProcessorEngine::IsVirtualKeyKeystrokeCandidate
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsVirtualKeyKeystrokeCandidate(
    UINT uCode, _In_ _KEYSTROKE_STATE *pKeyState, CANDIDATE_MODE /*candidateMode*/, _Out_ BOOL *pfRetCode,
    _In_ CMetasequoiaImeArray<_KEYSTROKE> *pKeystrokeMetric)
{
    if (pfRetCode == nullptr)
    {
        return FALSE;
    }
    *pfRetCode = FALSE;

    for (UINT i = 0; i < pKeystrokeMetric->Count(); i++)
    {
        _KEYSTROKE *pKeystroke = nullptr;

        pKeystroke = pKeystrokeMetric->GetAt(i);

        if ((pKeystroke->VirtualKey == uCode) && Global::CheckModifiers(Global::ModifiersValue, pKeystroke->Modifiers))
        {
            *pfRetCode = TRUE;
            if (pKeyState)
            {
                pKeyState->Category = CATEGORY_CANDIDATE;

                pKeyState->Function = pKeystroke->Function;
            }
            return TRUE;
        }
    }

    return FALSE;
}

//+---------------------------------------------------------------------------
//
// CCompositionProcessorEngine::IsKeyKeystrokeRange
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsKeystrokeRange(UINT uCode, _Out_ _KEYSTROKE_STATE *pKeyState,
                                                   CANDIDATE_MODE candidateMode)
{
    if (pKeyState == nullptr)
    {
        return FALSE;
    }

    pKeyState->Category = CATEGORY_NONE;
    pKeyState->Function = FUNCTION_NONE;

    // U-mode owns 0-9 as hex composition input.
    if (IsUnicodeModeComposition() && uCode >= L'0' && uCode <= L'9')
    {
        return FALSE;
    }

    if (_candidateListIndexRange.IsRange(uCode))
    {
        if (candidateMode != CANDIDATE_NONE)
        {
            pKeyState->Category = CATEGORY_CANDIDATE;
            pKeyState->Function = FUNCTION_SELECT_BY_NUMBER;
            return TRUE;
        }
        else if (GetVirtualKeyLength() > 0)
        {
            pKeyState->Category = CATEGORY_CANDIDATE;
            pKeyState->Function = FUNCTION_SELECT_BY_NUMBER;
            return TRUE;
        }
    }
    return FALSE;
}
