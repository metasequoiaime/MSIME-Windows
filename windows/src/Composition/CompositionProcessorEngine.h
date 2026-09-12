#pragma once

#include "MetasequoiaIME.h"
#include "sal.h"
#include "KeyHandlerEditSession.h"
#include "MetasequoiaIMEBaseStructure.h"
#include "Compartment.h"
#include "define.h"

class CCompositionProcessorEngine
{
    friend class CMetasequoiaIME;

  public:
    enum class PreservedKeyAction
    {
        None,
        ToggleImeMode,
        ToggleDoubleSingleByteMode,
        TogglePunctuationMode
    };

    explicit CCompositionProcessorEngine(_In_ CMetasequoiaIME *pTextService);
    ~CCompositionProcessorEngine(void);

    BOOL SetupLanguageProfile(LANGID langid, REFGUID guidLanguageProfile, _In_ ITfThreadMgr *pThreadMgr,
                              TfClientId tfClientId, BOOL isSecureMode, BOOL isComLessMode);

    // Get language profile.
    GUID GetLanguageProfile(LANGID *plangid)
    {
        *plangid = _langid;
        return _guidProfile;
    }
    // Get locale
    LCID GetLocale()
    {
        return MAKELCID(_langid, SORT_DEFAULT);
    }

    BOOL IsVirtualKeyNeed(UINT uCode, _In_reads_(1) WCHAR *pwch, BOOL fComposing, CANDIDATE_MODE candidateMode,
                          BOOL hasCandidateWithWildcard, _Out_opt_ _KEYSTROKE_STATE *pKeyState);
    BOOL IsVirtualKeyNeedForFreshComposition(UINT uCode, _In_reads_(1) WCHAR *pwch,
                                             _Out_opt_ _KEYSTROKE_STATE *pKeyState);

    BOOL AddVirtualKey(WCHAR wch);
    void RemoveVirtualKey(DWORD_PTR dwIndex);
    BOOL RemoveVirtualKeyBeforeCaret();
    BOOL RemoveVirtualKeyAtCaret();
    void PurgeVirtualKey();
    BOOL MoveCaret(int offset);
    DWORD_PTR GetCaretPosition() const
    {
        return _caretPosition;
    }
    void SetRenderedPreedit(std::wstring preedit, size_t prefixLength);
    DWORD_PTR GetRenderedCaretPosition() const;

    DWORD_PTR GetVirtualKeyLength()
    {
        return _keystrokeBuffer.GetLength();
    }
    CStringRange &GetKeystrokeBuffer()
    {
        return _keystrokeBuffer;
    };
    WCHAR GetVirtualKey(DWORD_PTR dwIndex);
    // Shift+U unicode input: composition buffer starts with 'U'.
    BOOL IsUnicodeModeComposition() const;
    // V-mode number input: composition buffer is a V (or a lowercase v when the
    // Server enabled that for quanpin) followed only by digits and at most one point.
    BOOL IsNumberModeComposition() const;
    // U-mode and V-mode both own bare digits as composition input.
    BOOL IsDigitModeComposition() const;
    // A decimal point extends a V-mode number that already has a digit and no point.
    BOOL IsNumberPointKeystroke(UINT uCode, const WCHAR *pwch) const;
    // Shared with the deferred-key shadow in KeyEventSink, which mirrors the buffer.
    static bool IsNumberModeKeystrokes(const WCHAR *buffer, size_t length);
    static bool IsNumberPointKeystroke(const WCHAR *buffer, size_t length, UINT uCode, const WCHAR *pwch);

    void GetReadingStrings(                                          //
        _Inout_ CMetasequoiaImeArray<CStringRange> *pReadingStrings, //
        _Out_ BOOL *pIsWildcardIncluded                              //
    );
    void GetCandidateList(                                                //
        _Inout_ CMetasequoiaImeArray<CCandidateListItem> *pCandidateList, //
        BOOL isIncrementalWordSearch, BOOL isWildcardSearch               //
    );

    // Preserved key handler
    BOOL IsPreservedKeyEligible(REFGUID rguid);
    PreservedKeyAction GetPreservedKeyAction(REFGUID rguid) const;
    void OnPreservedKey(ITfContext *pContext, REFGUID rguid, _Out_ BOOL *pIsEaten, _In_ ITfThreadMgr *pThreadMgr,
                        TfClientId tfClientId, BOOL *pNeedToggleIMEMode, BOOL isPrevalidated = FALSE,
                        BOOL notifyServer = TRUE);

    // Toggle IME Mode
    void ToggleIMEMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    void SetIMEMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL bOpen);
    BOOL CCompositionProcessorEngine::GetIMEMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    // Apply CN/EN compartment change deferred until after composition commit.
    // Closing KEYBOARD_OPENCLOSE before EndComposition makes CUAS/Win32 EDIT
    // finalize the same preedit twice (Chrome/TSF-only hosts are unaffected).
    void ApplyPendingImeModeAfterCompositionCommit(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    void SetPunctuationMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL bOpen);
    BOOL GetPunctuationMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    void SetDoubleSingleByteMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL bOpen);
    BOOL GetDoubleSingleByteMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);

    // Punctuation
    BOOL IsPunctuation(WCHAR wch);
    const WCHAR *GetPunctuation(WCHAR wch);
    // Smart punctuation (rime digit_separators-style): after ASCII letters/digits,
    // keep ',' '.' ':' as ASCII instead of mapping to Chinese punctuation.
    static BOOL IsSmartAsciiPunctuationKey(WCHAR wch);
    std::wstring ResolvePunctuation(WCHAR wch, WCHAR precedingChar);

    BOOL IsDoubleSingleByte(WCHAR wch);
    BOOL IsWildcard()
    {
        return _isWildcard;
    }
    BOOL IsDisableWildcardAtFirst()
    {
        return _isDisableWildcardAtFirst;
    }
    BOOL IsWildcardChar(WCHAR wch)
    {
        return ((IsWildcardOneChar(wch) || IsWildcardAllChar(wch)) ? TRUE : FALSE);
    }
    BOOL IsWildcardOneChar(WCHAR wch)
    {
        return (wch == L'?' ? TRUE : FALSE);
    }
    BOOL IsWildcardAllChar(WCHAR wch)
    {
        return (wch == L'*' ? TRUE : FALSE);
    }
    BOOL IsKeystrokeSort()
    {
        return _isKeystrokeSort;
    }

    // Language bar control
    void SetLanguageBarStatus(DWORD status, BOOL isSet);

    void ConversionModeCompartmentUpdated(_In_ ITfThreadMgr *pThreadMgr);

    void ShowAllLanguageBarIcons();
    void HideAllLanguageBarIcons();
    void RefreshLanguageBarIcons();

    inline CCandidateRange *GetCandidateListIndexRange()
    {
        return &_candidateListIndexRange;
    }
    inline UINT GetCandidateListPhraseModifier()
    {
        return _candidateListPhraseModifier;
    }
    inline UINT GetCandidateWindowWidth()
    {
        return _candidateWndWidth;
    }

  private:
    void InitKeyStrokeTable();
    BOOL InitLanguageBar(_In_ CLangBarItemButton *pLanguageBar, _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId,
                         REFGUID guidCompartment);

    struct _KEYSTROKE;
    BOOL IsVirtualKeyKeystrokeComposition(UINT uCode, _Out_opt_ _KEYSTROKE_STATE *pKeyState,
                                          KEYSTROKE_FUNCTION function);
    BOOL IsVirtualKeyKeystrokeCandidate(UINT uCode, _In_ _KEYSTROKE_STATE *pKeyState, CANDIDATE_MODE candidateMode,
                                        _Out_ BOOL *pfRetCode, _In_ CMetasequoiaImeArray<_KEYSTROKE> *pKeystrokeMetric);
    BOOL IsKeystrokeRange(UINT uCode, _Out_ _KEYSTROKE_STATE *pKeyState, CANDIDATE_MODE candidateMode);

    void SetupKeystroke();
    void SetupPreserved(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    void SetupConfiguration();
    void SetupLanguageBar(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL isSecureMode);
    void SetKeystrokeTable(_Inout_ CMetasequoiaImeArray<_KEYSTROKE> *pKeystroke);
    void SetupPunctuationPair();
    void CreateLanguageBarButton(DWORD dwEnable, GUID guidLangBar, _In_z_ LPCWSTR pwszDescriptionValue,
                                 _In_z_ LPCWSTR pwszTooltipValue, DWORD dwOnIconIndex, DWORD dwOffIconIndex,
                                 _Outptr_result_maybenull_ CLangBarItemButton **ppLangBarItemButton, BOOL isSecureMode);
    void SetInitialCandidateListRange();
    void SetDefaultCandidateTextFont();
    void InitializeMetasequoiaIMECompartment(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);

    class XPreservedKey;
    void SetPreservedKey(const CLSID clsid, TF_PRESERVEDKEY &tfPreservedKey, _In_z_ LPCWSTR pwszDescription,
                         _Out_ XPreservedKey *pXPreservedKey);
    BOOL InitPreservedKey(_In_ XPreservedKey *pXPreservedKey, _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    BOOL CheckShiftKeyOnly(_In_ CMetasequoiaImeArray<TF_PRESERVEDKEY> *pTSFPreservedKeyTable);

    static HRESULT CompartmentCallback(_In_ void *pv, REFGUID guidCompartment);
    void PrivateCompartmentsUpdated(_In_ ITfThreadMgr *pThreadMgr);
    void KeyboardOpenCompartmentUpdated(_In_ ITfThreadMgr *pThreadMgr);
    HRESULT SetKeyboardOpenCompartment(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL isOpen);
    void SyncPunctuationWithImeMode(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL isOpen);
    void CommitCompositionOnExternalKeyboardClose();
    void ReleaseConfiguredImeModeDefense();

  private:
    struct _KEYSTROKE
    {
        UINT VirtualKey;
        UINT Modifiers;
        KEYSTROKE_FUNCTION Function;

        _KEYSTROKE()
        {
            VirtualKey = 0;
            Modifiers = 0;
            Function = FUNCTION_NONE;
        }
    };
    _KEYSTROKE _keystrokeTable[26];

    CStringRange _keystrokeBuffer;
    DWORD_PTR _caretPosition = 0;
    std::wstring _renderedPreedit;
    size_t _renderedPreeditPrefixLength = 0;

    BOOL _hasWildcardIncludedInKeystrokeBuffer;

    LANGID _langid;
    GUID _guidProfile;
    TfClientId _tfClientId;

    CMetasequoiaImeArray<_KEYSTROKE> _KeystrokeComposition;
    CMetasequoiaImeArray<_KEYSTROKE> _KeystrokeCandidate;
    CMetasequoiaImeArray<_KEYSTROKE> _KeystrokeCandidateWildcard;
    CMetasequoiaImeArray<_KEYSTROKE> _KeystrokeCandidateSymbol;
    CMetasequoiaImeArray<_KEYSTROKE> _KeystrokeSymbol;

    // Preserved key data
    class XPreservedKey
    {
      public:
        XPreservedKey();
        ~XPreservedKey();
        BOOL UninitPreservedKey(_In_ ITfThreadMgr *pThreadMgr);

      public:
        CMetasequoiaImeArray<TF_PRESERVEDKEY> TSFPreservedKeyTable;
        GUID Guid;
        LPCWSTR Description;
    };

    XPreservedKey _PreservedKey_IMEMode;
    XPreservedKey _PreservedKey_IMEMode02;
    XPreservedKey _PreservedKey_IMEMode03;
    XPreservedKey _PreservedKey_EnglishInputMode;
    XPreservedKey _PreservedKey_DoubleSingleByte;
    XPreservedKey _PreservedKey_Punctuation;

    // Punctuation data
    CMetasequoiaImeArray<CPunctuationPair> _PunctuationPair;
    CMetasequoiaImeArray<CPunctuationNestPair> _PunctuationNestPair;

    // Language bar data
    CLangBarItemButton *_pLanguageBar_IMEMode;
    CLangBarItemButton *_pLanguageBar_DoubleSingleByte;
    CLangBarItemButton *_pLanguageBar_Punctuation;

    // Compartment
    CCompartment *_pCompartmentConversion;
    CCompartmentEventSink *_pCompartmentConversionEventSink;
    CCompartmentEventSink *_pCompartmentKeyboardOpenEventSink;
    CCompartmentEventSink *_pCompartmentDoubleSingleByteEventSink;
    CCompartmentEventSink *_pCompartmentPunctuationEventSink;
    ITfThreadMgr *_pOwnerThreadMgr;
    HWND _ownerMsgWndHandle;
    CMetasequoiaIME *_pTextService;
    BOOL _keyboardOpen;
    BOOL _keyboardOpenKnown;
    BOOL _suppressKeyboardCloseCommit;
    // After Activate applies input.default_ime_mode, Chromium (and Electron)
    // often rewrites TF_CONVERSIONMODE_* (NATIVE / SYMBOL / FULLSHAPE) while
    // focus is still on the non-editable shell. ConversionModeCompartmentUpdated
    // would then mirror those bits into OPENCLOSE and the punctuation /
    // fullwidth compartments. Defend by reasserting private state until the
    // user explicitly chooses a mode (Shift / langbar / FTB / preserved key).
    BOOL _defendConfiguredImeMode;
    BOOL _hasPendingImeModeAfterCompositionCommit;
    BOOL _pendingImeModeAfterCompositionCommit;

    // Configuration data.
    // Not bit-fields: BOOL is a signed int, so a one-bit field holds only -1 and 0 and every
    // `= TRUE` here stored -1 (C4463). Nothing compares these against TRUE today, but a
    // getter that returns -1 for "true" is a trap, and four ints cost nothing.
    BOOL _isWildcard;
    BOOL _isDisableWildcardAtFirst;
    BOOL _isKeystrokeSort;
    BOOL _isComLessMode;
    CCandidateRange _candidateListIndexRange;
    UINT _candidateListPhraseModifier;
    UINT _candidateWndWidth;

    static const int OUT_OF_FILE_INDEX = -1;
};
