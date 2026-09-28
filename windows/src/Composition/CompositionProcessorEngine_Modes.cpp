// Input modes and punctuation: the IME-mode / punctuation / full-width compartments and their
// callbacks, caret-state switch events, and the punctuation and smart-punctuation tables.

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
// Resolves the caret anchor in a read-only session so the badge event carries
// the position the user was looking at when the shortcut took effect.
class CCaretStateSwitchEditSession : public CEditSessionBase
{
  public:
    CCaretStateSwitchEditSession(CMetasequoiaIME *textService, ITfContext *context, UINT eventType, bool enabled,
                                 uint64_t focusToken, UINT trigger, bool capsLockEnabled, bool imeOpen)
        : CEditSessionBase(textService, context), eventType_(eventType), enabled_(enabled), focusToken_(focusToken),
          trigger_(trigger), capsLockEnabled_(capsLockEnabled), imeOpen_(imeOpen)
    {
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        if (!_pTextService->_IsFocusSessionCurrent(focusToken_, _pContext))
            return S_OK;
        POINT anchor{};
        if (!ResolveCollapsedSelectionAnchor(_pContext, ec, &anchor))
            return S_OK;
        SendCaretStateSwitchEventToUIProcessViaNamedPipe(eventType_, enabled_, anchor, trigger_, capsLockEnabled_,
                                                         imeOpen_);
        return S_OK;
    }

  private:
    UINT eventType_;
    bool enabled_;
    uint64_t focusToken_;
    UINT trigger_;
    bool capsLockEnabled_;
    bool imeOpen_;
};

// Ctrl+Space is the system's Chinese IME toggle, not one of our preserved
// keys: Windows writes OPENCLOSE directly and we only see the compartment
// edge. The chord still being held is what tells it apart from Server,
// host or language-bar writes.
bool IsSystemCtrlSpaceToggleHeld()
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 &&
           (GetAsyncKeyState(VK_MENU) & 0x8000) == 0;
}
} // namespace

//+---------------------------------------------------------------------------
//
// IsPunctuation
//
//----------------------------------------------------------------------------

BOOL CCompositionProcessorEngine::IsPunctuation(WCHAR wch)
{
    const bool japanese = Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed) &&
                          Global::JapanesePunctuationEnabled.load(std::memory_order_relaxed);
    const auto &table = japanese ? Global::JapanesePunctuationTable : Global::PunctuationTable;
    const int table_size = japanese ? static_cast<int>(ARRAYSIZE(Global::JapanesePunctuationTable))
                                    : static_cast<int>(ARRAYSIZE(Global::PunctuationTable));
    for (int i = 0; i < table_size; i++)
    {
        if (table[i]._Code == wch)
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

//+---------------------------------------------------------------------------
//
// GetPunctuationPair
//
//----------------------------------------------------------------------------

const WCHAR *CCompositionProcessorEngine::GetPunctuation(WCHAR wch)
{
    const bool japanese = Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed) &&
                          Global::JapanesePunctuationEnabled.load(std::memory_order_relaxed);
    const auto &table = japanese ? Global::JapanesePunctuationTable : Global::PunctuationTable;
    const int table_size = japanese ? static_cast<int>(ARRAYSIZE(Global::JapanesePunctuationTable))
                                    : static_cast<int>(ARRAYSIZE(Global::PunctuationTable));
    for (int i = 0; i < table_size; i++)
    {
        if (table[i]._Code == wch)
        {
            return table[i]._Punctuation;
        }
    }

    for (UINT j = 0; j < _PunctuationPair.Count(); j++)
    {
        CPunctuationPair *pPuncPair = _PunctuationPair.GetAt(j);

        if (pPuncPair->_punctuation._Code == wch)
        {
            // The opening/closing glyphs depend on the active punctuation mode,
            // so resolve them from the current flag rather than the values baked
            // into the pair at construction time (mode may switch afterwards).
            const bool isDoubleQuote = (wch == L'"');
            const WCHAR *opening = japanese ? (isDoubleQuote ? L"「" : L"『") : (isDoubleQuote ? L"“" : L"‘");
            const WCHAR *closing = japanese ? (isDoubleQuote ? L"」" : L"』") : (isDoubleQuote ? L"”" : L"’");
            if (!pPuncPair->_isPairToggle)
            {
                pPuncPair->_isPairToggle = TRUE;
                return opening;
            }
            else
            {
                pPuncPair->_isPairToggle = FALSE;
                return closing;
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

void CCompositionProcessorEngine::BalanceNestPairAfterAutoClose(WCHAR openingCode)
{
    for (UINT k = 0; k < _PunctuationNestPair.Count(); k++)
    {
        CPunctuationNestPair *pPuncNestPair = _PunctuationNestPair.GetAt(k);

        if (pPuncNestPair->_punctuation_begin._Code == openingCode)
        {
            if (pPuncNestPair->_nestCount > 0)
            {
                --pPuncNestPair->_nestCount;
            }
            return;
        }
    }
}

namespace
{
// Chinese -> ASCII mapping for the reversible smart punctuation conversion.
// Every symbol converts only when it reaches the document on its own. The
// paired marks are listed so a lone " or 【 / 《 / （ -- what auto-complete off
// produces -- converts like any other mark; the auto-completed pair form is
// deliberately left out of the feature and never arms a conversion (see
// _NoteCommittedChinesePunctuation), because rewriting one half of 〘|〙
// would orphan the other.
struct SmartPunctuationMapping
{
    WCHAR chinese;
    WCHAR ascii;
};

constexpr SmartPunctuationMapping kSmartPunctuationMap[] = {
    {L'。', L'.'}, {L'，', L','}, {L'！', L'!'}, {L'？', L'?'}, {L'；', L';'}, {L'：', L':'},
    {L'、', L'/'}, {L'“', L'"'},  {L'”', L'"'},  {L'‘', L'\''}, {L'’', L'\''}, {L'【', L'['},
    {L'】', L']'}, {L'《', L'<'}, {L'》', L'>'}, {L'（', L'('}, {L'）', L')'},
};
} // namespace

BOOL CCompositionProcessorEngine::IsSmartAsciiPunctuationKey(WCHAR wch)
{
    // Matches rime-ice punctuator/digit_separators: ",.:"
    return wch == L',' || wch == L'.' || wch == L':';
}

bool CCompositionProcessorEngine::IsSmartPunctuationChinese(WCHAR ch)
{
    for (const SmartPunctuationMapping &mapping : kSmartPunctuationMap)
    {
        if (mapping.chinese == ch)
        {
            return true;
        }
    }
    return false;
}

WCHAR CCompositionProcessorEngine::GetSmartPunctuationAscii(WCHAR chinese)
{
    for (const SmartPunctuationMapping &mapping : kSmartPunctuationMap)
    {
        if (mapping.chinese == chinese)
        {
            return mapping.ascii;
        }
    }
    return 0;
}

std::wstring CCompositionProcessorEngine::ResolvePunctuation(WCHAR wch, WCHAR precedingChar)
{
    // Direct ASCII output is opt-in (smart_punctuation_direct_digit and
    // smart_punctuation_direct_letter, both default off): the default path
    // leaves ',' '.' ':' to the reversible space conversion.
    // feature; Japanese input always resolves through the punctuation table so
    // 、。 are produced instead of raw ASCII.
    const bool japanesePunct = Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed) &&
                               Global::JapanesePunctuationEnabled.load(std::memory_order_relaxed);
    if (!japanesePunct && Global::SmartPunctuationEnabled.load(std::memory_order_relaxed) &&
        IsSmartAsciiPunctuationKey(wch))
    {
        const bool afterDigit = precedingChar >= L'0' && precedingChar <= L'9';
        const bool afterLetter =
            (precedingChar >= L'A' && precedingChar <= L'Z') || (precedingChar >= L'a' && precedingChar <= L'z');
        if ((afterDigit && Global::SmartPunctuationDirectDigitEnabled.load(std::memory_order_relaxed)) ||
            (afterLetter && Global::SmartPunctuationDirectLetterEnabled.load(std::memory_order_relaxed)))
        {
            return std::wstring(1, wch);
        }
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
        if (_pTextService != nullptr)
        {
            _pTextService->_ClearPairedPunctuationStack();
        }
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
        if (_pTextService != nullptr)
        {
            // The closing halves already on screen can no longer be stepped
            // over by the key that produced them once the mode decides a
            // different character for it.
            _pTextService->_ClearPairedPunctuationStack();
        }
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
// SetupPunctuationPair
//
//----------------------------------------------------------------------------

void CCompositionProcessorEngine::SetupPunctuationPair()
{
    const bool japanese = Global::JapaneseInputModeEnabled.load(std::memory_order_relaxed) &&
                          Global::JapanesePunctuationEnabled.load(std::memory_order_relaxed);

    _PunctuationPair.Clear();
    _PunctuationNestPair.Clear();

    // Punctuation pair
    const int pair_count = 2;
    CPunctuationPair punc_quotation_mark(L'"', japanese ? L"「" : L"“", japanese ? L"」" : L"”");
    CPunctuationPair punc_apostrophe(L'\'', japanese ? L"『" : L"‘", japanese ? L"』" : L"’");

    CPunctuationPair puncPairs[pair_count] = {
        punc_quotation_mark,
        punc_apostrophe,
    };

    for (int i = 0; i < pair_count; ++i)
    {
        CPunctuationPair *pPuncPair = _PunctuationPair.Append();
        *pPuncPair = puncPairs[i];
    }

    // Punctuation nest pair (book title brackets). Japanese uses the same 《〉/〈〉
    // nesting as Chinese; only the simple bracket [ ] differs, which lives in the table.
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
    Global::JapaneseKanaLayoutEnabled.store(FanyUtils::ReadConfiguredJapaneseKanaLayout() != FALSE,
                                            std::memory_order_relaxed);
    Global::JapanesePunctuationEnabled.store(FanyUtils::ReadConfiguredJapanesePunctuation() != FALSE,
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

void CCompositionProcessorEngine::SendCaretStateSwitchEvent(UINT eventType, bool enabled, UINT trigger,
                                                            bool capsLockEnabled)
{
    // Only explicit user actions call this: shortcuts (including the system
    // Ctrl+Space toggle), Caps Lock, and moving focus into another text
    // field. Compartment writes from the Server, the
    // host's conversion mode or activation never do, so none of those can
    // surface a badge the user did not ask for.
    if (!_pOwnerThreadMgr || !_pTextService || !Global::g_connected || !SupportsCaretStateIndicator())
        return;
    const uint64_t focusToken = _pTextService->_CaptureFocusSessionToken();
    if (focusToken == 0)
        return;
    ITfDocumentMgr *document = nullptr;
    if (FAILED(_pOwnerThreadMgr->GetFocus(&document)) || !document)
        return;
    ITfContext *context = nullptr;
    const HRESULT topResult = document->GetTop(&context);
    document->Release();
    if (FAILED(topResult) || !context)
        return;
    if (trigger == FanyImeCaretStateTrigger::UserToggle)
        capsLockEnabled = Global::CapsLockEnabled.load(std::memory_order_relaxed);
    // Captured now so the punctuation badge's mode slot reflects this moment,
    // not whatever the toolbar snapshot says when the event is rendered.
    const bool imeOpen = GetIMEMode(_pOwnerThreadMgr, _tfClientId) != FALSE;
    auto *session = new (std::nothrow) CCaretStateSwitchEditSession(_pTextService, context, eventType, enabled,
                                                                    focusToken, trigger, capsLockEnabled, imeOpen);
    if (session)
    {
        HRESULT sessionResult = E_FAIL;
        context->RequestEditSession(_tfClientId, session, TF_ES_ASYNCDONTCARE | TF_ES_READ, &sessionResult);
        session->Release();
    }
    context->Release();
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

        if (ownerWindow && IsWindow(ownerWindow))
        {
            PostMessage(ownerWindow, WM_UpdateIMEStatus, (WPARAM)(isOpen ? 1 : 0), 0);
        }

        fakeThis->KeyboardOpenCompartmentUpdated(pThreadMgr);
        if (externallyClosed)
        {
            fakeThis->CommitCompositionOnExternalKeyboardClose();
        }
        if (keyboardStateChanged && !fakeThis->_suppressKeyboardCloseCommit && IsSystemCtrlSpaceToggleHeld())
        {
            fakeThis->SendCaretStateSwitchEvent(FanyImePipeEventType::IMESwitch, isOpen != FALSE);
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
