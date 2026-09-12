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

//////////////////////////////////////////////////////////////////////
//
// CMetasequoiaIME implementation.
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// _AddTextProcessorEngine
//
//----------------------------------------------------------------------------

BOOL CMetasequoiaIME::_AddTextProcessorEngine()
{
    LANGID langid = 0;
    CLSID clsid = GUID_NULL;
    GUID guidProfile = GUID_NULL;

    // Get default profile.
    CTfInputProcessorProfile profile;

    if (FAILED(profile.CreateInstance()))
    {
        return FALSE;
    }

    if (FAILED(profile.GetCurrentLanguage(&langid)))
    {
        return FALSE;
    }

    if (FAILED(profile.GetDefaultLanguageProfile(langid, GUID_TFCAT_TIP_KEYBOARD, &clsid, &guidProfile)))
    {
        return FALSE;
    }

    // Is this already added?
    // Here is Idempotent Operation for this _AddTextProcessorEngine function.
    if (_pCompositionProcessorEngine != nullptr)
    {
        LANGID langidProfile = 0;
        GUID guidLanguageProfile = GUID_NULL;

        guidLanguageProfile = _pCompositionProcessorEngine->GetLanguageProfile(&langidProfile);
        if ((langid == langidProfile) && IsEqualGUID(guidProfile, guidLanguageProfile))
        {
            return TRUE;
        }
    }

    // Create composition processor engine
    if (_pCompositionProcessorEngine == nullptr)
    {
        _pCompositionProcessorEngine = new (std::nothrow) CCompositionProcessorEngine(this);
    }
    if (!_pCompositionProcessorEngine)
    {
        return FALSE;
    }

    // setup composition processor engine
    if (FALSE == _pCompositionProcessorEngine->SetupLanguageProfile(langid, guidProfile, _GetThreadMgr(),
                                                                    _GetClientId(), _IsSecureMode(), _IsComLess()))
    {
        return FALSE;
    }

    return TRUE;
}

//////////////////////////////////////////////////////////////////////
//
// CompositionProcessorEngine implementation.
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CCompositionProcessorEngine::CCompositionProcessorEngine(_In_ CMetasequoiaIME *pTextService)
{
    _langid = 0xffff;
    _guidProfile = GUID_NULL;
    _tfClientId = TF_CLIENTID_NULL;

    _pLanguageBar_IMEMode = nullptr;
    _pLanguageBar_DoubleSingleByte = nullptr;
    _pLanguageBar_Punctuation = nullptr;

    _pCompartmentConversion = nullptr;
    _pCompartmentKeyboardOpenEventSink = nullptr;
    _pCompartmentConversionEventSink = nullptr;
    _pCompartmentDoubleSingleByteEventSink = nullptr;
    _pCompartmentPunctuationEventSink = nullptr;
    _pOwnerThreadMgr = nullptr;
    _ownerMsgWndHandle = nullptr;
    _pTextService = pTextService;
    _keyboardOpen = FALSE;
    _keyboardOpenKnown = FALSE;
    _suppressKeyboardCloseCommit = FALSE;
    _defendConfiguredImeMode = FALSE;
    _hasPendingImeModeAfterCompositionCommit = FALSE;
    _pendingImeModeAfterCompositionCommit = FALSE;

    _hasWildcardIncludedInKeystrokeBuffer = FALSE;

    _isWildcard = FALSE;
    _isDisableWildcardAtFirst = FALSE;
    _isKeystrokeSort = FALSE;

    _candidateListPhraseModifier = 0;

    _candidateWndWidth = CAND_WIDTH;

    InitKeyStrokeTable();
}

//+---------------------------------------------------------------------------
//
// dtor
//
//----------------------------------------------------------------------------

CCompositionProcessorEngine::~CCompositionProcessorEngine()
{
    if (_pLanguageBar_IMEMode)
    {
        _pLanguageBar_IMEMode->CleanUp();
        _pLanguageBar_IMEMode->Release();
        _pLanguageBar_IMEMode = nullptr;
    }
    if (_pLanguageBar_DoubleSingleByte)
    {
        _pLanguageBar_DoubleSingleByte->CleanUp();
        _pLanguageBar_DoubleSingleByte->Release();
        _pLanguageBar_DoubleSingleByte = nullptr;
    }
    if (_pLanguageBar_Punctuation)
    {
        _pLanguageBar_Punctuation->CleanUp();
        _pLanguageBar_Punctuation->Release();
        _pLanguageBar_Punctuation = nullptr;
    }

    if (_pCompartmentConversion)
    {
        delete _pCompartmentConversion;
        _pCompartmentConversion = nullptr;
    }
    if (_pCompartmentKeyboardOpenEventSink)
    {
        _pCompartmentKeyboardOpenEventSink->_Unadvise();
        delete _pCompartmentKeyboardOpenEventSink;
        _pCompartmentKeyboardOpenEventSink = nullptr;
    }
    if (_pCompartmentConversionEventSink)
    {
        _pCompartmentConversionEventSink->_Unadvise();
        delete _pCompartmentConversionEventSink;
        _pCompartmentConversionEventSink = nullptr;
    }
    if (_pCompartmentDoubleSingleByteEventSink)
    {
        _pCompartmentDoubleSingleByteEventSink->_Unadvise();
        delete _pCompartmentDoubleSingleByteEventSink;
        _pCompartmentDoubleSingleByteEventSink = nullptr;
    }
    if (_pCompartmentPunctuationEventSink)
    {
        _pCompartmentPunctuationEventSink->_Unadvise();
        delete _pCompartmentPunctuationEventSink;
        _pCompartmentPunctuationEventSink = nullptr;
    }
    if (_pOwnerThreadMgr)
    {
        _pOwnerThreadMgr->Release();
        _pOwnerThreadMgr = nullptr;
    }
    _ownerMsgWndHandle = nullptr;
}

//+---------------------------------------------------------------------------
//
// SetupLanguageProfile
//
// Setup language profile for Composition Processor Engine.
// param
//     [in] LANGID langid = Specify language ID
//     [in] GUID guidLanguageProfile - Specify GUID language profile which GUID is as same as Text Service Framework
//     language profile. [in] ITfThreadMgr - pointer ITfThreadMgr. [in] tfClientId - TfClientId value. [in] isSecureMode
//     - secure mode
// returns
//     If setup succeeded, returns true. Otherwise returns false.
// N.B. For reverse conversion, ITfThreadMgr is NULL, TfClientId is 0 and isSecureMode is ignored.
//+---------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::SetupLanguageProfile(LANGID langid, REFGUID guidLanguageProfile,
                                                       _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                                                       BOOL isSecureMode, BOOL isComLessMode)
{
    BOOL ret = TRUE;
    if ((tfClientId == 0) && (pThreadMgr == nullptr))
    {
        ret = FALSE;
        goto Exit;
    }

    _isComLessMode = isComLessMode;
    _langid = langid;
    _guidProfile = guidLanguageProfile;
    _tfClientId = tfClientId;
    if (_pOwnerThreadMgr != pThreadMgr)
    {
        if (_pOwnerThreadMgr)
        {
            _pOwnerThreadMgr->Release();
        }
        _pOwnerThreadMgr = pThreadMgr;
        _pOwnerThreadMgr->AddRef();
    }
    _ownerMsgWndHandle = Global::msgWndHandle;

    SetupPreserved(pThreadMgr, tfClientId);
    InitializeMetasequoiaIMECompartment(pThreadMgr, tfClientId);
    SetupPunctuationPair();
    SetupLanguageBar(pThreadMgr, tfClientId, isSecureMode);
    SetupKeystroke();
    SetupConfiguration();

Exit:
    return ret;
}

//+---------------------------------------------------------------------------
//
// AddVirtualKey
// Add virtual key code to Composition Processor Engine for used to parse keystroke data.
// param
//     [in] uCode - Specify virtual key code.
// returns
//     State of Text Processor Engine.
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsUnicodeModeComposition() const
{
    return _keystrokeBuffer.GetLength() > 0 && _keystrokeBuffer.Get() && _keystrokeBuffer.Get()[0] == L'U';
}

bool CCompositionProcessorEngine::IsNumberModeKeystrokes(const WCHAR *buffer, size_t length)
{
    if (buffer == nullptr || length == 0)
    {
        return false;
    }
    const bool lowercasePrefixAllowed = Global::LowercaseNumberModeEnabled.load(std::memory_order_relaxed);
    if (buffer[0] != L'V' && !(buffer[0] == L'v' && lowercasePrefixAllowed))
    {
        return false;
    }
    bool pointSeen = false;
    for (size_t index = 1; index < length; ++index)
    {
        const WCHAR ch = buffer[index];
        if (ch == L'.')
        {
            if (pointSeen)
            {
                return false;
            }
            pointSeen = true;
        }
        else if (ch < L'0' || ch > L'9')
        {
            return false;
        }
    }
    return true;
}

bool CCompositionProcessorEngine::IsNumberPointKeystroke(const WCHAR *buffer, size_t length, UINT uCode,
                                                         const WCHAR *pwch)
{
    if (uCode != VK_OEM_PERIOD || pwch == nullptr || *pwch != L'.' || length < 2 ||
        !IsNumberModeKeystrokes(buffer, length))
    {
        return false;
    }
    for (size_t index = 1; index < length; ++index)
    {
        if (buffer[index] == L'.')
        {
            return false;
        }
    }
    return true;
}

BOOL CCompositionProcessorEngine::IsNumberModeComposition() const
{
    return IsNumberModeKeystrokes(_keystrokeBuffer.Get(), static_cast<size_t>(_keystrokeBuffer.GetLength())) ? TRUE
                                                                                                             : FALSE;
}

BOOL CCompositionProcessorEngine::IsDigitModeComposition() const
{
    return IsUnicodeModeComposition() || IsNumberModeComposition();
}

BOOL CCompositionProcessorEngine::IsNumberPointKeystroke(UINT uCode, const WCHAR *pwch) const
{
    return IsNumberPointKeystroke(_keystrokeBuffer.Get(), static_cast<size_t>(_keystrokeBuffer.GetLength()), uCode,
                                  pwch)
               ? TRUE
               : FALSE;
}

BOOL CCompositionProcessorEngine::AddVirtualKey(WCHAR wch)
{
    if (!wch)
    {
        return FALSE;
    }

    DWORD_PTR srgKeystrokeBufLen = _keystrokeBuffer.GetLength();
    _caretPosition = min(_caretPosition, srgKeystrokeBufLen);
    if (wch == L'\'' && ((_caretPosition > 0 && _keystrokeBuffer.Get()[_caretPosition - 1] == L'\'') ||
                         (_caretPosition < srgKeystrokeBufLen && _keystrokeBuffer.Get()[_caretPosition] == L'\'')))
    {
        return TRUE;
    }

    // Check if the keystroke buffer has reached the maximum length
    if (srgKeystrokeBufLen >= MAX_PINYIN_LENGTH)
    {
        return FALSE;
    }

    //
    // Insert at the logical composition caret.
    //
    PWCHAR pwch = new (std::nothrow) WCHAR[srgKeystrokeBufLen + 1];
    if (!pwch)
    {
        return FALSE;
    }

    memcpy(pwch, _keystrokeBuffer.Get(), _caretPosition * sizeof(WCHAR));
    pwch[_caretPosition] = wch;
    memcpy(pwch + _caretPosition + 1, _keystrokeBuffer.Get() + _caretPosition,
           (srgKeystrokeBufLen - _caretPosition) * sizeof(WCHAR));
    ++_caretPosition;

    if (_keystrokeBuffer.Get())
    {
        delete[] _keystrokeBuffer.Get();
    }

    _keystrokeBuffer.Set(pwch, srgKeystrokeBufLen + 1);

    std::wstring keyString(pwch, srgKeystrokeBufLen + 1);
    Global::PinyinString = keyString;

    return TRUE;
}

//+---------------------------------------------------------------------------
//
// RemoveVirtualKey
// Remove stored virtual key code.
// param
//     [in] dwIndex   - Specified index.
// returns
//     none.
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::RemoveVirtualKey(DWORD_PTR dwIndex)
{
    DWORD_PTR srgKeystrokeBufLen = _keystrokeBuffer.GetLength();
    if (dwIndex >= srgKeystrokeBufLen)
    {
        return;
    }

    if (dwIndex + 1 < srgKeystrokeBufLen)
    {
        // shift following eles left
        memmove((BYTE *)_keystrokeBuffer.Get() + (dwIndex * sizeof(WCHAR)),
                (BYTE *)_keystrokeBuffer.Get() + ((dwIndex + 1) * sizeof(WCHAR)),
                (srgKeystrokeBufLen - dwIndex - 1) * sizeof(WCHAR));
    }

    _keystrokeBuffer.Set(_keystrokeBuffer.Get(), srgKeystrokeBufLen - 1);
    if (_caretPosition > dwIndex)
    {
        --_caretPosition;
    }
    _caretPosition = min(_caretPosition, _keystrokeBuffer.GetLength());
}

BOOL CCompositionProcessorEngine::RemoveVirtualKeyBeforeCaret()
{
    if (_caretPosition == 0 || _keystrokeBuffer.GetLength() == 0)
    {
        return FALSE;
    }
    RemoveVirtualKey(_caretPosition - 1);
    return TRUE;
}

BOOL CCompositionProcessorEngine::RemoveVirtualKeyAtCaret()
{
    if (_caretPosition >= _keystrokeBuffer.GetLength())
    {
        return FALSE;
    }
    RemoveVirtualKey(_caretPosition);
    return TRUE;
}

BOOL CCompositionProcessorEngine::MoveCaret(int offset)
{
    const LONGLONG next = static_cast<LONGLONG>(_caretPosition) + offset;
    if (next < 0 || next > static_cast<LONGLONG>(_keystrokeBuffer.GetLength()))
    {
        return FALSE;
    }
    _caretPosition = static_cast<DWORD_PTR>(next);
    return TRUE;
}

void CCompositionProcessorEngine::SetRenderedPreedit(std::wstring preedit, size_t prefixLength)
{
    _renderedPreedit = std::move(preedit);
    _renderedPreeditPrefixLength = min(prefixLength, _renderedPreedit.size());
}

DWORD_PTR CCompositionProcessorEngine::GetRenderedCaretPosition() const
{
    size_t lettersBeforeCaret = 0;
    for (DWORD_PTR i = 0; i < min(_caretPosition, _keystrokeBuffer.GetLength()); ++i)
    {
        if (_keystrokeBuffer.Get()[i] != L'\'')
        {
            ++lettersBeforeCaret;
        }
    }
    size_t displayPosition = _renderedPreeditPrefixLength;
    size_t seenLetters = 0;
    while (displayPosition < _renderedPreedit.size() && seenLetters < lettersBeforeCaret)
    {
        if (_renderedPreedit[displayPosition] != L'\'')
        {
            ++seenLetters;
        }
        ++displayPosition;
    }
    if (_caretPosition > 0 && _keystrokeBuffer.Get()[_caretPosition - 1] == L'\'')
    {
        while (displayPosition < _renderedPreedit.size() && _renderedPreedit[displayPosition] == L'\'')
        {
            ++displayPosition;
        }
    }
    return displayPosition;
}

//+---------------------------------------------------------------------------
//
// PurgeVirtualKey
// Purge stored virtual key code.
// param
//     none.
// returns
//     none.
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::PurgeVirtualKey()
{
    if (_keystrokeBuffer.Get())
    {
        delete[] _keystrokeBuffer.Get();
        _keystrokeBuffer.Set(NULL, 0);
    }
    _caretPosition = 0;
    _renderedPreedit.clear();
    _renderedPreeditPrefixLength = 0;
}

WCHAR CCompositionProcessorEngine::GetVirtualKey(DWORD_PTR dwIndex)
{
    if (dwIndex < _keystrokeBuffer.GetLength())
    {
        return *(_keystrokeBuffer.Get() + dwIndex);
    }
    return 0;
}
//+---------------------------------------------------------------------------
//
// GetReadingStrings
// Retrieves string from Composition Processor Engine.
// param
//     [out] pReadingStrings - Specified returns pointer of CUnicodeString.
// returns
//     none
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::GetReadingStrings(_Inout_ CMetasequoiaImeArray<CStringRange> *pReadingStrings,
                                                    _Out_ BOOL *pIsWildcardIncluded)
{
    CStringRange oneKeystroke;

    _hasWildcardIncludedInKeystrokeBuffer = FALSE;

    if (pReadingStrings->Count() == 0 && _keystrokeBuffer.GetLength())
    {
        CStringRange *pNewString = nullptr;

        pNewString = pReadingStrings->Append();
        if (pNewString)
        {
            *pNewString = _keystrokeBuffer;
        }

        for (DWORD index = 0; index < _keystrokeBuffer.GetLength(); index++)
        {
            oneKeystroke.Set(_keystrokeBuffer.Get() + index, 1);

            if (IsWildcard() && IsWildcardChar(*oneKeystroke.Get()))
            {
                _hasWildcardIncludedInKeystrokeBuffer = TRUE;
            }
        }
    }

    *pIsWildcardIncluded = _hasWildcardIncludedInKeystrokeBuffer;
}

//+---------------------------------------------------------------------------
//
// GetCandidateList
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::GetCandidateList(_Inout_ CMetasequoiaImeArray<CCandidateListItem> *pCandidateList,
                                                   BOOL isIncrementalWordSearch, BOOL isWildcardSearch)
{
    isIncrementalWordSearch;
    isWildcardSearch;

    //
    // Candidate generation now lives in the IPC server. TSF keeps a minimal
    // local mirror so selection/page bookkeeping still works.
    //
    const std::wstring keystrokeStr(_keystrokeBuffer.Get(), _keystrokeBuffer.GetLength());
    CCandidateListItem *pLI = nullptr;
    pLI = pCandidateList->Append();
    if (pLI)
    {
        pLI->_ItemString.Set(keystrokeStr.c_str(), keystrokeStr.size());
        pLI->_FindKeyCode.Set(keystrokeStr.c_str(), keystrokeStr.size());
    }
    for (UINT index = 0; index < pCandidateList->Count();)
    {
        CCandidateListItem *pItem = pCandidateList->GetAt(index);
        CStringRange startItemString;
        CStringRange endItemString;

        startItemString.Set(pItem->_ItemString.Get(), 1);
        endItemString.Set(pItem->_ItemString.Get() + pItem->_ItemString.GetLength() - 1, 1);

        index++;
    }
    return;
}

//+---------------------------------------------------------------------------
//
// IsPunctuation
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsPunctuation(WCHAR wch)
{
    for (int i = 0; i < ARRAYSIZE(Global::PunctuationTable); i++)
    {
        if (Global::PunctuationTable[i]._Code == wch)
        {
            return TRUE;
        }
    }

    for (UINT j = 0; j < _PunctuationPair.Count(); j++)
    {
        CPunctuationPair *pPuncPair = _PunctuationPair.GetAt(j);

        if (pPuncPair->_punctuation._Code == wch)
        {
            return TRUE;
        }
    }

    for (UINT k = 0; k < _PunctuationNestPair.Count(); k++)
    {
        CPunctuationNestPair *pPuncNestPair = _PunctuationNestPair.GetAt(k);

        if (pPuncNestPair->_punctuation_begin._Code == wch)
        {
            return TRUE;
        }
        if (pPuncNestPair->_punctuation_end._Code == wch)
        {
            return TRUE;
        }
    }
    return FALSE;
}

namespace
{
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
    case VK_OEM_MINUS:
    case VK_OEM_PLUS:
    case VK_HOME:
    case VK_END:
    case VK_TAB:
        return false;
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

//+---------------------------------------------------------------------------
//
// GetPunctuationPair
//
//----------------------------------------------------------------------------

const WCHAR *CCompositionProcessorEngine::GetPunctuation(WCHAR wch)
{
    for (int i = 0; i < ARRAYSIZE(Global::PunctuationTable); i++)
    {
        if (Global::PunctuationTable[i]._Code == wch)
        {
            return Global::PunctuationTable[i]._Punctuation;
        }
    }

    for (UINT j = 0; j < _PunctuationPair.Count(); j++)
    {
        CPunctuationPair *pPuncPair = _PunctuationPair.GetAt(j);

        if (pPuncPair->_punctuation._Code == wch)
        {
            if (!pPuncPair->_isPairToggle)
            {
                pPuncPair->_isPairToggle = TRUE;
                return pPuncPair->_punctuation._Punctuation;
            }
            else
            {
                pPuncPair->_isPairToggle = FALSE;
                return pPuncPair->_pairPunctuation;
            }
        }
    }

    for (UINT k = 0; k < _PunctuationNestPair.Count(); k++)
    {
        CPunctuationNestPair *pPuncNestPair = _PunctuationNestPair.GetAt(k);

        if (pPuncNestPair->_punctuation_begin._Code == wch)
        {
            if (pPuncNestPair->_nestCount++ == 0)
            {
                return pPuncNestPair->_punctuation_begin._Punctuation;
            }
            else
            {
                return pPuncNestPair->_pairPunctuation_begin;
            }
        }
        if (pPuncNestPair->_punctuation_end._Code == wch)
        {
            // An unmatched closing mark must leave the depth at zero. Decrementing past it would make
            // the next opening mark produce the inner 〈 instead of 《, and the count would never
            // recover without an equal number of extra openings.
            if (pPuncNestPair->_nestCount == 0)
            {
                return pPuncNestPair->_punctuation_end._Punctuation;
            }
            if (--pPuncNestPair->_nestCount == 0)
            {
                return pPuncNestPair->_punctuation_end._Punctuation;
            }
            return pPuncNestPair->_pairPunctuation_end;
        }
    }
    return 0;
}

BOOL CCompositionProcessorEngine::IsSmartAsciiPunctuationKey(WCHAR wch)
{
    // Matches rime-ice punctuator/digit_separators: ",.:"
    return wch == L',' || wch == L'.' || wch == L':';
}

std::wstring CCompositionProcessorEngine::ResolvePunctuation(WCHAR wch, WCHAR precedingChar)
{
    if (Global::SmartPunctuationEnabled.load(std::memory_order_relaxed) && IsSmartAsciiPunctuationKey(wch) &&
        ((precedingChar >= L'0' && precedingChar <= L'9') || (precedingChar >= L'A' && precedingChar <= L'Z') ||
         (precedingChar >= L'a' && precedingChar <= L'z')))
    {
        return std::wstring(1, wch);
    }

    const WCHAR *punctuation = GetPunctuation(wch);
    return punctuation ? std::wstring(punctuation) : std::wstring();
}

//+---------------------------------------------------------------------------
//
// IsDoubleSingleByte
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsDoubleSingleByte(WCHAR wch)
{
    if (L' ' <= wch && wch <= L'~')
    {
        return TRUE;
    }
    return FALSE;
}

//+---------------------------------------------------------------------------
//
// SetupKeystroke
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::SetupKeystroke()
{
    SetKeystrokeTable(&_KeystrokeComposition);
    return;
}

//+---------------------------------------------------------------------------
//
// SetKeystrokeTable
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::SetKeystrokeTable(_Inout_ CMetasequoiaImeArray<_KEYSTROKE> *pKeystroke)
{
    for (int i = 0; i < 26; i++)
    {
        _KEYSTROKE *pKS = nullptr;

        pKS = pKeystroke->Append();
        if (!pKS)
        {
            break;
        }
        *pKS = _keystrokeTable[i];
    }
}

//+---------------------------------------------------------------------------
//
// SetupPreserved
// Setup hotkeys
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::SetupPreserved(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId)
{
    UNREFERENCED_PARAMETER(pThreadMgr);
    UNREFERENCED_PARAMETER(tfClientId);

    TF_PRESERVEDKEY preservedKeyImeMode;
    preservedKeyImeMode.uVKey = VK_SHIFT;
    preservedKeyImeMode.uModifiers = _TF_MOD_ON_KEYUP_SHIFT_ONLY;
    SetPreservedKey(                                  //
        Global::MetasequoiaIMEGuidImeModePreserveKey, //
        preservedKeyImeMode,                          //
        Global::ImeModeDescription,                   //
        &_PreservedKey_IMEMode                        //
    );

    TF_PRESERVEDKEY preservedKeyImeMode02;
    preservedKeyImeMode02.uVKey = VK_SPACE;
    preservedKeyImeMode02.uModifiers = TF_MOD_CONTROL | TF_MOD_ALT;
    SetPreservedKey(                                    //
        Global::MetasequoiaIMEGuidImeModePreserveKey02, //
        preservedKeyImeMode02,                          //
        Global::ImeModeDescription02,                   //
        &_PreservedKey_IMEMode02                        //
    );

    TF_PRESERVEDKEY preservedKeyImeMode03;
    preservedKeyImeMode03.uVKey = VK_CONTROL;
    preservedKeyImeMode03.uModifiers = _TF_MOD_ON_KEYUP_CONTROL_ONLY;
    SetPreservedKey(                                    //
        Global::MetasequoiaIMEGuidImeModePreserveKey03, //
        preservedKeyImeMode03,                          //
        Global::ImeModeDescription03,                   //
        &_PreservedKey_IMEMode03                        //
    );

    TF_PRESERVEDKEY preservedKeyEnglishInputMode;
    preservedKeyEnglishInputMode.uVKey = 'E';
    preservedKeyEnglishInputMode.uModifiers = TF_MOD_CONTROL | TF_MOD_SHIFT;
    SetPreservedKey(                                           //
        Global::MetasequoiaIMEGuidEnglishInputModePreserveKey, //
        preservedKeyEnglishInputMode,                          //
        Global::EnglishInputModeDescription,                   //
        &_PreservedKey_EnglishInputMode                        //
    );

    TF_PRESERVEDKEY preservedKeyDoubleSingleByte;
    preservedKeyDoubleSingleByte.uVKey = VK_SPACE;
    preservedKeyDoubleSingleByte.uModifiers = TF_MOD_SHIFT | TF_MOD_CONTROL;
    SetPreservedKey(                                           //
        Global::MetasequoiaIMEGuidDoubleSingleBytePreserveKey, //
        preservedKeyDoubleSingleByte,                          //
        Global::DoubleSingleByteDescription,                   //
        &_PreservedKey_DoubleSingleByte                        //
    );

    TF_PRESERVEDKEY preservedKeyPunctuation;
    preservedKeyPunctuation.uVKey = VK_OEM_PERIOD;
    preservedKeyPunctuation.uModifiers = TF_MOD_CONTROL;
    SetPreservedKey(                                      //
        Global::MetasequoiaIMEGuidPunctuationPreserveKey, //
        preservedKeyPunctuation,                          //
        Global::PunctuationDescription,                   //
        &_PreservedKey_Punctuation                        //
    );

    /* Keep GUID/action tables for deferred hotkey dispatch. All input-mode
     * shortcuts (including Ctrl+Shift+E) are detected from ITfKeyEventSink. */
    return;
}

//+---------------------------------------------------------------------------
//
// SetKeystrokeTable
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::SetPreservedKey(const CLSID clsid, TF_PRESERVEDKEY &tfPreservedKey,
                                                  _In_z_ LPCWSTR pwszDescription, _Out_ XPreservedKey *pXPreservedKey)
{
    pXPreservedKey->Guid = clsid;

    TF_PRESERVEDKEY *ptfPsvKey1 = pXPreservedKey->TSFPreservedKeyTable.Append();
    if (!ptfPsvKey1)
    {
        return;
    }
    *ptfPsvKey1 = tfPreservedKey;

    size_t srgKeystrokeBufLen = 0;
    if (StringCchLength(pwszDescription, STRSAFE_MAX_CCH, &srgKeystrokeBufLen) != S_OK)
    {
        return;
    }
    pXPreservedKey->Description = new (std::nothrow) WCHAR[srgKeystrokeBufLen + 1];
    if (!pXPreservedKey->Description)
    {
        return;
    }

    StringCchCopy((LPWSTR)pXPreservedKey->Description, srgKeystrokeBufLen, pwszDescription);

    return;
}
//+---------------------------------------------------------------------------
//
// InitPreservedKey
//
// Register a hotkey.
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::InitPreservedKey(_In_ XPreservedKey *pXPreservedKey, _In_ ITfThreadMgr *pThreadMgr,
                                                   TfClientId tfClientId)
{
    ITfKeystrokeMgr *pKeystrokeMgr = nullptr;
    BOOL registered = TRUE;

    if (IsEqualGUID(pXPreservedKey->Guid, GUID_NULL))
    {
        return FALSE;
    }

    if (pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void **)&pKeystrokeMgr) != S_OK)
    {
        return FALSE;
    }

    for (UINT i = 0; i < pXPreservedKey->TSFPreservedKeyTable.Count(); i++)
    {
        TF_PRESERVEDKEY preservedKey = *pXPreservedKey->TSFPreservedKeyTable.GetAt(i);
        preservedKey.uModifiers &= 0xffff;

        size_t lenOfDesc = 0;
        if (StringCchLength(pXPreservedKey->Description, STRSAFE_MAX_CCH, &lenOfDesc) != S_OK)
        {
            return FALSE;
        }
        if (FAILED(pKeystrokeMgr->PreserveKey(tfClientId, pXPreservedKey->Guid, &preservedKey,
                                              pXPreservedKey->Description, static_cast<ULONG>(lenOfDesc))))
        {
            registered = FALSE;
        }
    }

    pKeystrokeMgr->Release();

    return registered;
}

//+---------------------------------------------------------------------------
//
// CheckShiftKeyOnly
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::CheckShiftKeyOnly(_In_ CMetasequoiaImeArray<TF_PRESERVEDKEY> *pTSFPreservedKeyTable)
{
    for (UINT i = 0; i < pTSFPreservedKeyTable->Count(); i++)
    {
        TF_PRESERVEDKEY *ptfPsvKey = pTSFPreservedKeyTable->GetAt(i);

        if (((ptfPsvKey->uModifiers & (_TF_MOD_ON_KEYUP_SHIFT_ONLY & 0xffff0000)) && !Global::IsShiftKeyDownOnly) ||
            ((ptfPsvKey->uModifiers & (_TF_MOD_ON_KEYUP_CONTROL_ONLY & 0xffff0000)) && !Global::IsControlKeyDownOnly) ||
            ((ptfPsvKey->uModifiers & (_TF_MOD_ON_KEYUP_ALT_ONLY & 0xffff0000)) && !Global::IsAltKeyDownOnly))
        {
            return FALSE;
        }
    }

    return TRUE;
}

//+---------------------------------------------------------------------------
//
// OnPreservedKey
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsPreservedKeyEligible(REFGUID rguid)
{
    const FanyUtils::SwitchLanguageHotkeys hotkeys = FanyUtils::ReadConfiguredSwitchLanguageHotkeys();
    if (IsEqualGUID(rguid, _PreservedKey_IMEMode.Guid))
    {
        return hotkeys.shift && CheckShiftKeyOnly(&_PreservedKey_IMEMode.TSFPreservedKeyTable);
    }
    if (IsEqualGUID(rguid, _PreservedKey_IMEMode02.Guid))
    {
        return hotkeys.ctrl_alt_space && CheckShiftKeyOnly(&_PreservedKey_IMEMode02.TSFPreservedKeyTable);
    }
    if (IsEqualGUID(rguid, _PreservedKey_IMEMode03.Guid))
    {
        return hotkeys.ctrl && CheckShiftKeyOnly(&_PreservedKey_IMEMode03.TSFPreservedKeyTable);
    }
    if (IsEqualGUID(rguid, _PreservedKey_EnglishInputMode.Guid))
    {
        return CheckShiftKeyOnly(&_PreservedKey_EnglishInputMode.TSFPreservedKeyTable);
    }
    if (IsEqualGUID(rguid, _PreservedKey_DoubleSingleByte.Guid))
    {
        return CheckShiftKeyOnly(&_PreservedKey_DoubleSingleByte.TSFPreservedKeyTable);
    }
    if (IsEqualGUID(rguid, _PreservedKey_Punctuation.Guid))
    {
        return CheckShiftKeyOnly(&_PreservedKey_Punctuation.TSFPreservedKeyTable);
    }
    return FALSE;
}

CCompositionProcessorEngine::PreservedKeyAction CCompositionProcessorEngine::GetPreservedKeyAction(REFGUID rguid) const
{
    if (IsEqualGUID(rguid, _PreservedKey_IMEMode.Guid) || IsEqualGUID(rguid, _PreservedKey_IMEMode02.Guid) ||
        IsEqualGUID(rguid, _PreservedKey_IMEMode03.Guid))
    {
        return PreservedKeyAction::ToggleImeMode;
    }
    if (IsEqualGUID(rguid, _PreservedKey_DoubleSingleByte.Guid))
    {
        return PreservedKeyAction::ToggleDoubleSingleByteMode;
    }
    if (IsEqualGUID(rguid, _PreservedKey_Punctuation.Guid))
    {
        return PreservedKeyAction::TogglePunctuationMode;
    }
    return PreservedKeyAction::None;
}

void CCompositionProcessorEngine::OnPreservedKey( //
    ITfContext * /*pContext*/,                    //
    REFGUID rguid,                                //
    _Out_ BOOL *pIsEaten,                         //
    _In_ ITfThreadMgr *pThreadMgr,                //
    TfClientId tfClientId,                        //
    BOOL *pNeedToggleIMEMode,                     //
    BOOL isPrevalidated,                          //
    BOOL notifyServer                             //
)
{
    if (IsEqualGUID(rguid, _PreservedKey_IMEMode.Guid) || IsEqualGUID(rguid, _PreservedKey_IMEMode02.Guid) ||
        IsEqualGUID(rguid, _PreservedKey_IMEMode03.Guid))
    {
        if (!isPrevalidated)
        {
            CMetasequoiaImeArray<TF_PRESERVEDKEY> *table = &_PreservedKey_IMEMode.TSFPreservedKeyTable;
            if (IsEqualGUID(rguid, _PreservedKey_IMEMode02.Guid))
            {
                table = &_PreservedKey_IMEMode02.TSFPreservedKeyTable;
            }
            else if (IsEqualGUID(rguid, _PreservedKey_IMEMode03.Guid))
            {
                table = &_PreservedKey_IMEMode03.TSFPreservedKeyTable;
            }
            if (!CheckShiftKeyOnly(table) || !IsPreservedKeyEligible(rguid))
            {
                *pIsEaten = FALSE;
                return;
            }
        }
        BOOL isOpen = FALSE;
        CCompartment CompartmentKeyboardOpen(pThreadMgr, tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
        CompartmentKeyboardOpen._GetCompartmentBOOL(isOpen);
        isOpen = isOpen ? FALSE : TRUE;
        ReleaseConfiguredImeModeDefense();
        FanyUtils::RefreshPunctuationLockFromConfig();

        // Closing CN→EN while composing: keep KEYBOARD_OPENCLOSE open until
        // after EndComposition. CUAS/Win32 EDIT double-commits if we close
        // first and finalize later (Ctrl+Space / Chrome are fine).
        const BOOL deferCloseUntilCompositionCommit = !isOpen && _pTextService && _pTextService->_IsComposing();
        if (deferCloseUntilCompositionCommit)
        {
            _pendingImeModeAfterCompositionCommit = isOpen;
            _hasPendingImeModeAfterCompositionCommit = TRUE;
        }
        else
        {
            _hasPendingImeModeAfterCompositionCommit = FALSE;
            SetKeyboardOpenCompartment(pThreadMgr, tfClientId, isOpen);
            SyncPunctuationWithImeMode(pThreadMgr, tfClientId, isOpen);
        }

        *pIsEaten = TRUE;
        *pNeedToggleIMEMode = TRUE;

        Global::Keycode = VK_SHIFT;
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
            Global::ModifiersDown |= 0b00000001;
        else
            Global::ModifiersDown &= ~0b00000001;
        if (notifyServer)
        {
            WriteDataToSharedMemory(Global::Keycode, L'\0', Global::ModifiersDown, nullptr, 0, L"", 0b000111);
            SendKeyEventToUIProcess();
            ClearNamedpipeDataIfExists();
        }
    }
    else if (IsEqualGUID(rguid, _PreservedKey_DoubleSingleByte.Guid))
    {
        if (!isPrevalidated && !CheckShiftKeyOnly(&_PreservedKey_DoubleSingleByte.TSFPreservedKeyTable))
        {
            *pIsEaten = FALSE;
            return;
        }
        BOOL isDouble = FALSE;
        CCompartment CompartmentDoubleSingleByte(pThreadMgr, tfClientId,
                                                 Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
        CompartmentDoubleSingleByte._GetCompartmentBOOL(isDouble);
        CompartmentDoubleSingleByte._SetCompartmentBOOL(isDouble ? FALSE : TRUE);
        *pIsEaten = TRUE;
    }
    else if (IsEqualGUID(rguid, _PreservedKey_Punctuation.Guid))
    {
        if (!isPrevalidated && !CheckShiftKeyOnly(&_PreservedKey_Punctuation.TSFPreservedKeyTable))
        {
            *pIsEaten = FALSE;
            return;
        }
        // Ctrl + .: toggle Chinese/English punctuation
        BOOL isPunctuation = FALSE;
        CCompartment CompartmentPunctuation(pThreadMgr, tfClientId, Global::MetasequoiaIMEGuidCompartmentPunctuation);
        CompartmentPunctuation._GetCompartmentBOOL(isPunctuation);
        SetPunctuationMode(pThreadMgr, tfClientId, isPunctuation ? FALSE : TRUE);
        *pIsEaten = TRUE;
    }
    else
    {
        *pIsEaten = FALSE;
    }
    *pIsEaten = TRUE;
}

//+---------------------------------------------------------------------------
//
// ToggleIMEMode
//
//----------------------------------------------------------------------------
void CCompositionProcessorEngine::ToggleIMEMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId)
{
    ReleaseConfiguredImeModeDefense();
    BOOL isOpen = FALSE;
    CCompartment CompartmentKeyboardOpen(pThreadMgr, tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    CompartmentKeyboardOpen._GetCompartmentBOOL(isOpen);
    SetKeyboardOpenCompartment(pThreadMgr, tfClientId, isOpen ? FALSE : TRUE);

    SyncPunctuationWithImeMode(pThreadMgr, tfClientId, isOpen ? FALSE : TRUE);
}

//+---------------------------------------------------------------------------
//
// SetIMEMode
//
//----------------------------------------------------------------------------
void CCompositionProcessorEngine::SetIMEMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL bOpen)
{
    BOOL isOpen = FALSE;
    CCompartment CompartmentKeyboardOpen(pThreadMgr, tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    CompartmentKeyboardOpen._GetCompartmentBOOL(isOpen);

    if (isOpen != bOpen)
    {
        ReleaseConfiguredImeModeDefense();
        SetKeyboardOpenCompartment(pThreadMgr, tfClientId, bOpen);
    }
}

HRESULT CCompositionProcessorEngine::SetKeyboardOpenCompartment(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                                                                BOOL isOpen)
{
    CCompartment compartment(pThreadMgr, tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    const BOOL previousSuppression = _suppressKeyboardCloseCommit;
    _suppressKeyboardCloseCommit = TRUE;
    const HRESULT result = compartment._SetCompartmentBOOL(isOpen);
    _suppressKeyboardCloseCommit = previousSuppression;
    return result;
}

void CCompositionProcessorEngine::ReleaseConfiguredImeModeDefense()
{
    if (!_defendConfiguredImeMode)
    {
        return;
    }
    _defendConfiguredImeMode = FALSE;
}

void CCompositionProcessorEngine::SyncPunctuationWithImeMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                                                             BOOL isOpen)
{
    SetPunctuationMode(pThreadMgr, tfClientId, isOpen);
}

void CCompositionProcessorEngine::ApplyPendingImeModeAfterCompositionCommit(_In_ ITfThreadMgr *pThreadMgr,
                                                                            TfClientId tfClientId)
{
    if (!_hasPendingImeModeAfterCompositionCommit)
    {
        return;
    }
    _hasPendingImeModeAfterCompositionCommit = FALSE;
    const BOOL isOpen = _pendingImeModeAfterCompositionCommit;
    SetKeyboardOpenCompartment(pThreadMgr, tfClientId, isOpen);
    SyncPunctuationWithImeMode(pThreadMgr, tfClientId, isOpen);
}

/**
 * @brief 获取当前 IME 的状态
 *
 * @param pThreadMgr
 * @param tfClientId
 * @return BOOL True: 中文输入法打开， False: 中文输入法关闭
 */
BOOL CCompositionProcessorEngine::GetIMEMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId)
{
    BOOL isOpen = FALSE;
    CCompartment CompartmentKeyboardOpen(pThreadMgr, tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    CompartmentKeyboardOpen._GetCompartmentBOOL(isOpen);
    return isOpen;
}

//+---------------------------------------------------------------------------
//
// SetPunctuationMode
//
//----------------------------------------------------------------------------
void CCompositionProcessorEngine::SetPunctuationMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL bOpen)
{
    FanyUtils::RefreshPunctuationLockFromConfig();
    bOpen = Global::ResolvePunctuationOpen(bOpen);

    BOOL isOpen = FALSE;
    CCompartment CompartmentPunctuation(pThreadMgr, tfClientId, Global::MetasequoiaIMEGuidCompartmentPunctuation);
    CompartmentPunctuation._GetCompartmentBOOL(isOpen);

    if (isOpen != bOpen)
    {
        CompartmentPunctuation._SetCompartmentBOOL(bOpen);
    }
}

/**
 * @brief 获取当前标点符号模式
 *
 * @param pThreadMgr
 * @param tfClientId
 * @return BOOL True: 中文标点符号打开， False: 中文标点符号关闭
 */
BOOL CCompositionProcessorEngine::GetPunctuationMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId)
{
    BOOL isOpen = FALSE;
    CCompartment CompartmentPunctuation(pThreadMgr, tfClientId, Global::MetasequoiaIMEGuidCompartmentPunctuation);
    CompartmentPunctuation._GetCompartmentBOOL(isOpen);
    return isOpen;
}

/**
 * @brief 设置全角/半角模式
 *
 * @param pThreadMgr
 * @param tfClientId
 * @param bOpen True: 全角， False: 半角
 */
void CCompositionProcessorEngine::SetDoubleSingleByteMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                                                          BOOL bOpen)
{
    BOOL isOpen = FALSE;
    CCompartment CompartmentDoubleSingleByte(pThreadMgr, tfClientId,
                                             Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    CompartmentDoubleSingleByte._GetCompartmentBOOL(isOpen);

    if (isOpen != bOpen)
    {
        CompartmentDoubleSingleByte._SetCompartmentBOOL(bOpen);
    }
}

//+---------------------------------------------------------------------------
//
// GetDoubleSingleByteMode
//
//----------------------------------------------------------------------------
BOOL CCompositionProcessorEngine::GetDoubleSingleByteMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId)
{
    BOOL isOpen = FALSE;
    CCompartment CompartmentDoubleSingleByte(pThreadMgr, tfClientId,
                                             Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    CompartmentDoubleSingleByte._GetCompartmentBOOL(isOpen);
    return isOpen;
}

//+---------------------------------------------------------------------------
//
// SetupConfiguration
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::SetupConfiguration()
{
    _isWildcard = TRUE;
    _isDisableWildcardAtFirst = TRUE;
    _isKeystrokeSort = TRUE;
    _candidateWndWidth = CAND_WIDTH;

    SetInitialCandidateListRange();

    SetDefaultCandidateTextFont();

    return;
}

//+---------------------------------------------------------------------------
//
// SetupLanguageBar
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::SetupLanguageBar(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                                                   BOOL isSecureMode)
{
    DWORD dwEnable = 1;
    CreateLanguageBarButton(dwEnable, GUID_LBI_INPUTMODE, Global::LangbarImeModeDescription, Global::ImeModeDescription,
                            Global::ImeModeOnIcoIndex, Global::ImeModeOffIcoIndex, &_pLanguageBar_IMEMode,
                            isSecureMode);
    CreateLanguageBarButton(dwEnable, Global::MetasequoiaIMEGuidLangBarDoubleSingleByte,
                            Global::LangbarDoubleSingleByteDescription, Global::DoubleSingleByteDescription,
                            Global::DoubleSingleByteOnIcoIndex, Global::DoubleSingleByteOffIcoIndex,
                            &_pLanguageBar_DoubleSingleByte, isSecureMode);
    CreateLanguageBarButton(dwEnable, Global::MetasequoiaIMEGuidLangBarPunctuation,
                            Global::LangbarPunctuationDescription, Global::PunctuationDescription,
                            Global::PunctuationOnIcoIndex, Global::PunctuationOffIcoIndex, &_pLanguageBar_Punctuation,
                            isSecureMode);

    InitLanguageBar(_pLanguageBar_IMEMode, pThreadMgr, tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    InitLanguageBar(_pLanguageBar_DoubleSingleByte, pThreadMgr, tfClientId,
                    Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    InitLanguageBar(_pLanguageBar_Punctuation, pThreadMgr, tfClientId,
                    Global::MetasequoiaIMEGuidCompartmentPunctuation);

    _pCompartmentConversion =
        new (std::nothrow) CCompartment(pThreadMgr, tfClientId, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
    _pCompartmentKeyboardOpenEventSink = new (std::nothrow) CCompartmentEventSink(CompartmentCallback, this);
    _pCompartmentConversionEventSink = new (std::nothrow) CCompartmentEventSink(CompartmentCallback, this);
    _pCompartmentDoubleSingleByteEventSink = new (std::nothrow) CCompartmentEventSink(CompartmentCallback, this);
    _pCompartmentPunctuationEventSink = new (std::nothrow) CCompartmentEventSink(CompartmentCallback, this);

    if (_pCompartmentKeyboardOpenEventSink)
    {
        _pCompartmentKeyboardOpenEventSink->_Advise(pThreadMgr, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    }
    if (_pCompartmentConversionEventSink)
    {
        _pCompartmentConversionEventSink->_Advise(pThreadMgr, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
    }
    if (_pCompartmentDoubleSingleByteEventSink)
    {
        _pCompartmentDoubleSingleByteEventSink->_Advise(pThreadMgr,
                                                        Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    }
    if (_pCompartmentPunctuationEventSink)
    {
        _pCompartmentPunctuationEventSink->_Advise(pThreadMgr, Global::MetasequoiaIMEGuidCompartmentPunctuation);
    }

    return;
}

//+---------------------------------------------------------------------------
//
// CreateLanguageBarButton
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::CreateLanguageBarButton(
    DWORD dwEnable, GUID guidLangBar, _In_z_ LPCWSTR pwszDescriptionValue, _In_z_ LPCWSTR pwszTooltipValue,
    DWORD dwOnIconIndex, DWORD dwOffIconIndex, _Outptr_result_maybenull_ CLangBarItemButton **ppLangBarItemButton,
    BOOL isSecureMode)
{
    dwEnable;

    if (ppLangBarItemButton)
    {
        *ppLangBarItemButton = new (std::nothrow) CLangBarItemButton(
            guidLangBar, pwszDescriptionValue, pwszTooltipValue, dwOnIconIndex, dwOffIconIndex, isSecureMode);
    }

    return;
}

//+---------------------------------------------------------------------------
//
// InitLanguageBar
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::InitLanguageBar(_In_ CLangBarItemButton *pLangBarItemButton,
                                                  _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                                                  REFGUID guidCompartment)
{
    if (pLangBarItemButton)
    {
        if (pLangBarItemButton->_AddItem(pThreadMgr) == S_OK)
        {
            if (pLangBarItemButton->_RegisterCompartment(pThreadMgr, tfClientId, guidCompartment))
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

//+---------------------------------------------------------------------------
//
// SetupPunctuationPair
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::SetupPunctuationPair()
{
    // Punctuation pair
    const int pair_count = 2;
    // Left quotation mark and right quotation mark “”
    CPunctuationPair punc_quotation_mark(L'"', L"“", L"”");
    // Left single quotation mark and right single quotation mark ‘’
    CPunctuationPair punc_apostrophe(L'\'', L"‘", L"’");

    CPunctuationPair puncPairs[pair_count] = {
        punc_quotation_mark,
        punc_apostrophe,
    };

    for (int i = 0; i < pair_count; ++i)
    {
        CPunctuationPair *pPuncPair = _PunctuationPair.Append();
        *pPuncPair = puncPairs[i];
    }

    // Punctuation nest pair
    CPunctuationNestPair punc_angle_bracket(L'<', L"《", L"〈", L'>', L"》", L"〉");

    CPunctuationNestPair *pPuncNestPair = _PunctuationNestPair.Append();
    *pPuncNestPair = punc_angle_bracket;
}

void CCompositionProcessorEngine::InitializeMetasequoiaIMECompartment(_In_ ITfThreadMgr *pThreadMgr,
                                                                      TfClientId tfClientId)
{
    // Default CN/EN on IME activate / switch-in (input.default_ime_mode).
    const BOOL openChinese = FanyUtils::ReadConfiguredDefaultImeModeChinese();
    Global::JapaneseInputModeEnabled.store(FanyUtils::ReadConfiguredJapaneseInputMode() != FALSE,
                                           std::memory_order_relaxed);
    // Use the suppressing writer so the OPENCLOSE sink does not treat this as
    // a user choice and drop the defense we are about to arm.
    SetKeyboardOpenCompartment(pThreadMgr, tfClientId, openChinese);
    _keyboardOpen = openChinese;
    _keyboardOpenKnown = TRUE;
    _defendConfiguredImeMode = TRUE;

    CCompartment CompartmentDoubleSingleByte(pThreadMgr, tfClientId,
                                             Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    CompartmentDoubleSingleByte._SetCompartmentBOOL(FALSE);

    SetPunctuationMode(pThreadMgr, tfClientId, openChinese);

    PrivateCompartmentsUpdated(pThreadMgr);
}
//+---------------------------------------------------------------------------
//
// CompartmentCallback
//
//----------------------------------------------------------------------------

// static
HRESULT CCompositionProcessorEngine::CompartmentCallback(_In_ void *pv, REFGUID guidCompartment)
{
    CCompositionProcessorEngine *fakeThis = (CCompositionProcessorEngine *)pv;
    if (nullptr == fakeThis)
    {
        return E_INVALIDARG;
    }

    ITfThreadMgr *pThreadMgr = fakeThis->_pOwnerThreadMgr;
    if (!pThreadMgr)
    {
        return E_UNEXPECTED;
    }
    pThreadMgr->AddRef();
    const HWND ownerWindow = fakeThis->_ownerMsgWndHandle;

    if (IsEqualGUID(guidCompartment, Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte))
    {
        BOOL isDoubleSingleByte = FALSE;
        CCompartment CompartmentDoubleSingleByte(pThreadMgr, fakeThis->_tfClientId,
                                                 Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
        CompartmentDoubleSingleByte._GetCompartmentBOOL(isDoubleSingleByte);
        // 0: halfwidth, 1: fullwidth
        // SendDoubleSingleByteSwitchEventToUIProcessViaNamedPipe(isDoubleSingleByte ? 1 : 0);
        if (ownerWindow && IsWindow(ownerWindow))
        {
            PostMessage(ownerWindow, WM_UpdateDoubleSingleByte, (WPARAM)(isDoubleSingleByte ? 1 : 0), 0);
        }
        fakeThis->PrivateCompartmentsUpdated(pThreadMgr);
    }
    else if (IsEqualGUID(guidCompartment, Global::MetasequoiaIMEGuidCompartmentPunctuation))
    {
        BOOL isPunctuation = FALSE;
        CCompartment CompartmentPunctuation(pThreadMgr, fakeThis->_tfClientId,
                                            Global::MetasequoiaIMEGuidCompartmentPunctuation);
        CompartmentPunctuation._GetCompartmentBOOL(isPunctuation);
        // SendPuncSwitchEventToUIProcessViaNamedPipe(isPunctuation ? 1 : 0);
        if (ownerWindow && IsWindow(ownerWindow))
        {
            PostMessage(ownerWindow, WM_UpdatePuncMode, (WPARAM)(isPunctuation ? 1 : 0), 0);
        }
        fakeThis->PrivateCompartmentsUpdated(pThreadMgr);
    }
    else if (IsEqualGUID(guidCompartment, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION) ||
             IsEqualGUID(guidCompartment, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_SENTENCE))
    {
        fakeThis->ConversionModeCompartmentUpdated(pThreadMgr);
    }
    else if (IsEqualGUID(guidCompartment, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE))
    {
        // 如果标点状态和当前输入法状态不一致，那么，需要更新标点状态
        BOOL isOpen = FALSE;
        CCompartment CompartmentKeyboardOpen(pThreadMgr, fakeThis->_tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
        if (FAILED(CompartmentKeyboardOpen._GetCompartmentBOOL(isOpen)))
        {
            pThreadMgr->Release();
            return S_OK;
        }
        const BOOL keyboardWasOpen = fakeThis->_keyboardOpen;
        const BOOL keyboardStateWasKnown = fakeThis->_keyboardOpenKnown;
        fakeThis->_keyboardOpen = isOpen;
        fakeThis->_keyboardOpenKnown = TRUE;
        const BOOL keyboardStateChanged = keyboardStateWasKnown && keyboardWasOpen != isOpen;
        const BOOL externallyClosed =
            keyboardStateChanged && keyboardWasOpen && !isOpen && !fakeThis->_suppressKeyboardCloseCommit;
        // Language-bar clicks write OPENCLOSE without going through
        // SetIMEMode/ToggleIMEMode. Treat any non-suppressed edge as the user
        // (or a true external writer) accepting a new mode.
        if (keyboardStateChanged && !fakeThis->_suppressKeyboardCloseCommit)
        {
            fakeThis->ReleaseConfiguredImeModeDefense();
        }
        FanyUtils::RefreshPunctuationLockFromConfig();
        BOOL isPunctuation = FALSE;
        CCompartment CompartmentPunctuation(pThreadMgr, fakeThis->_tfClientId,
                                            Global::MetasequoiaIMEGuidCompartmentPunctuation);
        CompartmentPunctuation._GetCompartmentBOOL(isPunctuation);
        const BOOL desiredPunctuation = Global::ResolvePunctuationOpen(isOpen);
        if (desiredPunctuation != isPunctuation)
        {
            CompartmentPunctuation._SetCompartmentBOOL(desiredPunctuation);
        }

        // SendIMESwitchEventToUIProcessViaNamedPipe(isOpen ? 1 : 0);
        if (ownerWindow && IsWindow(ownerWindow))
        {
            PostMessage(ownerWindow, WM_UpdateIMEStatus, (WPARAM)(isOpen ? 1 : 0), 0);
        }

        fakeThis->KeyboardOpenCompartmentUpdated(pThreadMgr);
        if (externallyClosed)
        {
            fakeThis->CommitCompositionOnExternalKeyboardClose();
        }
    }

    pThreadMgr->Release();
    pThreadMgr = nullptr;

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// UpdatePrivateCompartments
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::ConversionModeCompartmentUpdated(_In_ ITfThreadMgr *pThreadMgr)
{
    if (!_pCompartmentConversion)
    {
        return;
    }

    DWORD conversionMode = 0;
    if (FAILED(_pCompartmentConversion->_GetCompartmentDWORD(conversionMode)))
    {
        return;
    }

    if (_defendConfiguredImeMode)
    {
        // Chromium rewrites the whole conversion DWORD on Chrome_WidgetWin_1
        // (NATIVE, SYMBOL, often FULLSHAPE together). Mirroring any of those
        // bits into our private compartments would wipe the defaults applied
        // at Activate. Push private state back instead, and skip the pull.
        PrivateCompartmentsUpdated(pThreadMgr);
        KeyboardOpenCompartmentUpdated(pThreadMgr);
        return;
    }

    BOOL isDouble = FALSE;
    CCompartment CompartmentDoubleSingleByte(pThreadMgr, _tfClientId,
                                             Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    if (SUCCEEDED(CompartmentDoubleSingleByte._GetCompartmentBOOL(isDouble)))
    {
        if (!isDouble && (conversionMode & TF_CONVERSIONMODE_FULLSHAPE))
        {
            CompartmentDoubleSingleByte._SetCompartmentBOOL(TRUE);
        }
        else if (isDouble && !(conversionMode & TF_CONVERSIONMODE_FULLSHAPE))
        {
            CompartmentDoubleSingleByte._SetCompartmentBOOL(FALSE);
        }
    }
    SetPunctuationMode(pThreadMgr, _tfClientId, (conversionMode & TF_CONVERSIONMODE_SYMBOL) ? TRUE : FALSE);

    BOOL fOpen = FALSE;
    CCompartment CompartmentKeyboardOpen(pThreadMgr, _tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    if (SUCCEEDED(CompartmentKeyboardOpen._GetCompartmentBOOL(fOpen)))
    {
        if (fOpen && !(conversionMode & TF_CONVERSIONMODE_NATIVE))
        {
            CompartmentKeyboardOpen._SetCompartmentBOOL(FALSE);
        }
        else if (!fOpen && (conversionMode & TF_CONVERSIONMODE_NATIVE))
        {
            CompartmentKeyboardOpen._SetCompartmentBOOL(TRUE);
        }
    }
}

//+---------------------------------------------------------------------------
//
// PrivateCompartmentsUpdated()
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::PrivateCompartmentsUpdated(_In_ ITfThreadMgr *pThreadMgr)
{
    if (!_pCompartmentConversion)
    {
        return;
    }

    DWORD conversionMode = 0;
    DWORD conversionModePrev = 0;
    if (FAILED(_pCompartmentConversion->_GetCompartmentDWORD(conversionMode)))
    {
        return;
    }

    conversionModePrev = conversionMode;

    BOOL isDouble = FALSE;
    CCompartment CompartmentDoubleSingleByte(pThreadMgr, _tfClientId,
                                             Global::MetasequoiaIMEGuidCompartmentDoubleSingleByte);
    if (SUCCEEDED(CompartmentDoubleSingleByte._GetCompartmentBOOL(isDouble)))
    {
        if (!isDouble && (conversionMode & TF_CONVERSIONMODE_FULLSHAPE))
        {
            conversionMode &= ~TF_CONVERSIONMODE_FULLSHAPE;
        }
        else if (isDouble && !(conversionMode & TF_CONVERSIONMODE_FULLSHAPE))
        {
            conversionMode |= TF_CONVERSIONMODE_FULLSHAPE;
        }
    }

    BOOL isPunctuation = FALSE;
    CCompartment CompartmentPunctuation(pThreadMgr, _tfClientId, Global::MetasequoiaIMEGuidCompartmentPunctuation);
    if (SUCCEEDED(CompartmentPunctuation._GetCompartmentBOOL(isPunctuation)))
    {
        if (!isPunctuation && (conversionMode & TF_CONVERSIONMODE_SYMBOL))
        {
            conversionMode &= ~TF_CONVERSIONMODE_SYMBOL;
        }
        else if (isPunctuation && !(conversionMode & TF_CONVERSIONMODE_SYMBOL))
        {
            conversionMode |= TF_CONVERSIONMODE_SYMBOL;
        }
    }

    if (conversionMode != conversionModePrev)
    {
        _pCompartmentConversion->_SetCompartmentDWORD(conversionMode);
    }
}

//+---------------------------------------------------------------------------
//
// KeyboardOpenCompartmentUpdated
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::KeyboardOpenCompartmentUpdated(_In_ ITfThreadMgr *pThreadMgr)
{
    if (!_pCompartmentConversion)
    {
        return;
    }

    DWORD conversionMode = 0;
    DWORD conversionModePrev = 0;
    if (FAILED(_pCompartmentConversion->_GetCompartmentDWORD(conversionMode)))
    {
        return;
    }

    conversionModePrev = conversionMode;

    BOOL isOpen = FALSE;
    CCompartment CompartmentKeyboardOpen(pThreadMgr, _tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    if (SUCCEEDED(CompartmentKeyboardOpen._GetCompartmentBOOL(isOpen)))
    {
        if (isOpen && !(conversionMode & TF_CONVERSIONMODE_NATIVE))
        {
            conversionMode |= TF_CONVERSIONMODE_NATIVE;
        }
        else if (!isOpen && (conversionMode & TF_CONVERSIONMODE_NATIVE))
        {
            conversionMode &= ~TF_CONVERSIONMODE_NATIVE;
        }
    }

    if (conversionMode != conversionModePrev)
    {
        _pCompartmentConversion->_SetCompartmentDWORD(conversionMode);
    }
}

void CCompositionProcessorEngine::CommitCompositionOnExternalKeyboardClose()
{
    CMetasequoiaIME *textService = _pTextService;
    if (textService == nullptr || !textService->_IsComposing() || textService->_pContext == nullptr)
    {
        return;
    }

    ITfContext *context = textService->_pContext;
    context->AddRef();
    const uint64_t compositionEpoch = textService->_CaptureCompositionEpoch();
    const uint64_t focusToken = textService->_CaptureFocusSessionToken();

    _KEYSTROKE_STATE keyState = {};
    keyState.Category = CATEGORY_COMPOSING;
    keyState.Function = FUNCTION_TOGGLE_IME_MODE;
    textService->_InvokeKeyHandler(context, 0, L'\0', 0, keyState, FANY_IME_NO_REQUEST_ID, {}, 0, compositionEpoch,
                                   focusToken);
    context->Release();
}

//////////////////////////////////////////////////////////////////////
//
// XPreservedKey implementation.
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// UninitPreservedKey
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::XPreservedKey::UninitPreservedKey(_In_ ITfThreadMgr *pThreadMgr)
{
    ITfKeystrokeMgr *pKeystrokeMgr = nullptr;

    if (IsEqualGUID(Guid, GUID_NULL))
    {
        return FALSE;
    }

    if (FAILED(pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void **)&pKeystrokeMgr)))
    {
        return FALSE;
    }

    for (UINT i = 0; i < TSFPreservedKeyTable.Count(); i++)
    {
        TF_PRESERVEDKEY pPreservedKey = *TSFPreservedKeyTable.GetAt(i);
        pPreservedKey.uModifiers &= 0xffff;

        pKeystrokeMgr->UnpreserveKey(Guid, &pPreservedKey);
    }

    pKeystrokeMgr->Release();

    return TRUE;
}

CCompositionProcessorEngine::XPreservedKey::XPreservedKey()
{
    Guid = GUID_NULL;
    Description = nullptr;
}

CCompositionProcessorEngine::XPreservedKey::~XPreservedKey()
{
    ITfThreadMgr *pThreadMgr = nullptr;

    HRESULT hr =
        CoCreateInstance(CLSID_TF_ThreadMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr, (void **)&pThreadMgr);
    if (SUCCEEDED(hr))
    {
        UninitPreservedKey(pThreadMgr);
        pThreadMgr->Release();
        pThreadMgr = nullptr;
    }

    if (Description)
    {
        delete[] Description;
    }
}
//+---------------------------------------------------------------------------
//
// CMetasequoiaIME::CreateInstance
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::CreateInstance(REFCLSID rclsid, REFIID riid, _Outptr_result_maybenull_ LPVOID *ppv,
                                        _Out_opt_ HINSTANCE *phInst, BOOL isComLessMode)
{
    HRESULT hr = S_OK;
    if (phInst == nullptr)
    {
        return E_INVALIDARG;
    }

    *phInst = nullptr;

    if (!isComLessMode)
    {
        hr = ::CoCreateInstance(rclsid, NULL, CLSCTX_INPROC_SERVER, riid, ppv);
    }
    else
    {
        hr = CMetasequoiaIME::ComLessCreateInstance(rclsid, riid, ppv, phInst);
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// CMetasequoiaIME::ComLessCreateInstance
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::ComLessCreateInstance(REFGUID rclsid, REFIID riid, _Outptr_result_maybenull_ void **ppv,
                                               _Out_opt_ HINSTANCE *phInst)
{
    HRESULT hr = S_OK;
    HINSTANCE metasequoiaIMEDllHandle = nullptr;
    WCHAR wchPath[MAX_PATH] = {'\0'};
    WCHAR szExpandedPath[MAX_PATH] = {'\0'};
    DWORD dwCnt = 0;
    *ppv = nullptr;

    hr = phInst ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
    {
        *phInst = nullptr;
        hr = CMetasequoiaIME::GetComModuleName(rclsid, wchPath, ARRAYSIZE(wchPath));
        if (SUCCEEDED(hr))
        {
            dwCnt = ExpandEnvironmentStringsW(wchPath, szExpandedPath, ARRAYSIZE(szExpandedPath));
            hr = (0 < dwCnt && dwCnt <= ARRAYSIZE(szExpandedPath)) ? S_OK : E_FAIL;
            if (SUCCEEDED(hr))
            {
                metasequoiaIMEDllHandle = LoadLibraryEx(szExpandedPath, NULL, 0);
                hr = metasequoiaIMEDllHandle ? S_OK : E_FAIL;
                if (SUCCEEDED(hr))
                {
                    *phInst = metasequoiaIMEDllHandle;
                    FARPROC pfn = GetProcAddress(metasequoiaIMEDllHandle, "DllGetClassObject");
                    hr = pfn ? S_OK : E_FAIL;
                    if (SUCCEEDED(hr))
                    {
                        IClassFactory *pClassFactory = nullptr;
                        hr = ((HRESULT(STDAPICALLTYPE *)(REFCLSID rclsid, REFIID riid, LPVOID * ppv))(pfn))(
                            rclsid, IID_IClassFactory, (void **)&pClassFactory);
                        if (SUCCEEDED(hr) && pClassFactory)
                        {
                            hr = pClassFactory->CreateInstance(NULL, riid, ppv);
                            pClassFactory->Release();
                        }
                    }
                }
            }
        }
    }

    if (!SUCCEEDED(hr) && phInst && *phInst)
    {
        FreeLibrary(*phInst);
        *phInst = 0;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// CMetasequoiaIME::GetComModuleName
//
//----------------------------------------------------------------------------

HRESULT CMetasequoiaIME::GetComModuleName(REFGUID rclsid, _Out_writes_(cchPath) WCHAR *wchPath, DWORD cchPath)
{
    HRESULT hr = S_OK;

    CRegKey key;
    WCHAR wchClsid[CLSID_STRLEN + 1];
    hr = CLSIDToString(rclsid, wchClsid) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
    {
        WCHAR wchKey[MAX_PATH];
        hr = StringCchPrintfW(wchKey, ARRAYSIZE(wchKey), L"CLSID\\%s\\InProcServer32", wchClsid);
        if (SUCCEEDED(hr))
        {
            hr = (key.Open(HKEY_CLASSES_ROOT, wchKey, KEY_READ) == ERROR_SUCCESS) ? S_OK : E_FAIL;
            if (SUCCEEDED(hr))
            {
                WCHAR wszModel[MAX_PATH];
                ULONG cch = ARRAYSIZE(wszModel);
                hr = (key.QueryStringValue(L"ThreadingModel", wszModel, &cch) == ERROR_SUCCESS) ? S_OK : E_FAIL;
                if (SUCCEEDED(hr))
                {
                    if (CompareStringOrdinal(wszModel, -1, L"Apartment", -1, TRUE) == CSTR_EQUAL)
                    {
                        hr = (key.QueryStringValue(NULL, wchPath, &cchPath) == ERROR_SUCCESS) ? S_OK : E_FAIL;
                    }
                    else
                    {
                        hr = E_FAIL;
                    }
                }
            }
        }
    }

    return hr;
}

void CCompositionProcessorEngine::InitKeyStrokeTable()
{
    for (int i = 0; i < 26; i++)
    {
        _keystrokeTable[i].VirtualKey = 'A' + i;
        _keystrokeTable[i].Modifiers = 0;
        _keystrokeTable[i].Function = FUNCTION_INPUT;
    }
}

void CCompositionProcessorEngine::ShowAllLanguageBarIcons()
{
    SetLanguageBarStatus(TF_LBI_STATUS_HIDDEN, FALSE);
}

void CCompositionProcessorEngine::HideAllLanguageBarIcons()
{
    SetLanguageBarStatus(TF_LBI_STATUS_HIDDEN, TRUE);
}

void CCompositionProcessorEngine::SetInitialCandidateListRange()
{
    for (DWORD i = 1; i <= CANDWND_ITEM_CNT_PER_PAGE; i++)
    {
        DWORD *pNewIndexRange = nullptr;

        pNewIndexRange = _candidateListIndexRange.Append();
        if (pNewIndexRange != nullptr)
        {
            *pNewIndexRange = i;
        }
    }
}

void CCompositionProcessorEngine::SetDefaultCandidateTextFont()
{
    // Candidate Text Font
    if (Global::defaultlFontHandle == nullptr)
    {
        WCHAR fontName[50] = {'\0'};
        LoadString(Global::dllInstanceHandle, IDS_DEFAULT_FONT, fontName, 50);
        Global::defaultlFontHandle = CreateFont(-MulDiv(10, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72), 0, 0, 0,
                                                FW_MEDIUM, 0, 0, 0, 0, 0, 0, 0, 0, fontName);
        if (!Global::defaultlFontHandle)
        {
            LOGFONT lf;
            SystemParametersInfo(SPI_GETICONTITLELOGFONT, sizeof(LOGFONT), &lf, 0);
            // Fall back to the default GUI font on failure.
            Global::defaultlFontHandle = CreateFont(-MulDiv(10, GetDeviceCaps(GetDC(NULL), LOGPIXELSY), 72), 0, 0, 0,
                                                    FW_MEDIUM, 0, 0, 0, 0, 0, 0, 0, 0, lf.lfFaceName);
        }
    }
}

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

    // U-mode / V-mode: bare digits compose; Shift+1..9 selects candidates.
    if (IsDigitModeComposition() && uCode >= L'0' && uCode <= L'9')
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
    // V-mode: the period is the decimal point, not a paging or punctuation key.
    if (IsNumberPointKeystroke(uCode, pwch))
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
    if (candidateMode != CANDIDATE_NONE &&
        (uCode == VK_OEM_MINUS || uCode == VK_OEM_PLUS || isCommaPeriodPagingKey || isBracketPagingKey ||
         uCode == VK_TAB || uCode == VK_PRIOR || uCode == VK_NEXT || uCode == VK_UP || uCode == VK_DOWN))
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

    // U-mode and V-mode own 0-9 as composition input.
    if (IsDigitModeComposition() && uCode >= L'0' && uCode <= L'9')
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
