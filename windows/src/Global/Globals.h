#pragma once

#include "Private.h"
#include "define.h"
#include "MetasequoiaIMEBaseStructure.h"
#include <iostream>
#include <string>
#include <wrl.h>
#include <unordered_set>

void DllAddRef();
void DllRelease();

using namespace Microsoft::WRL;

namespace Global
{
//---------------------------------------------------------------------
// inline
//---------------------------------------------------------------------

inline void SafeRelease(_In_ IUnknown *punk)
{
    if (punk != nullptr)
    {
        punk->Release();
    }
}

inline void QuickVariantInit(_Inout_ VARIANT *pvar)
{
    pvar->vt = VT_EMPTY;
}

inline void QuickVariantClear(_Inout_ VARIANT *pvar)
{
    switch (pvar->vt)
    {
    // some ovbious VTs that don't need to call VariantClear.
    case VT_EMPTY:
    case VT_NULL:
    case VT_I2:
    case VT_I4:
    case VT_R4:
    case VT_R8:
    case VT_CY:
    case VT_DATE:
    case VT_I1:
    case VT_UI1:
    case VT_UI2:
    case VT_UI4:
    case VT_I8:
    case VT_UI8:
    case VT_INT:
    case VT_UINT:
    case VT_BOOL:
        break;

        // Call release for VT_UNKNOWN.
    case VT_UNKNOWN:
        SafeRelease(pvar->punkVal);
        break;

    default:
        // we call OleAut32 for other VTs.
        VariantClear(pvar);
        break;
    }
    pvar->vt = VT_EMPTY;
}

//+---------------------------------------------------------------------------
//
// IsTooSimilar
//
//  Return TRUE if the colors cr1 and cr2 are so similar that they
//  are hard to distinguish. Used for deciding to use reverse video
//  selection instead of system selection colors.
//
//----------------------------------------------------------------------------

inline BOOL IsTooSimilar(COLORREF cr1, COLORREF cr2)
{
    if ((cr1 | cr2) & 0xFF000000) // One color and/or the other isn't RGB, so algorithm doesn't apply
    {
        return FALSE;
    }

    LONG DeltaR = abs(GetRValue(cr1) - GetRValue(cr2));
    LONG DeltaG = abs(GetGValue(cr1) - GetGValue(cr2));
    LONG DeltaB = abs(GetBValue(cr1) - GetBValue(cr2));

    return DeltaR + DeltaG + DeltaB < 80;
}

//---------------------------------------------------------------------
// extern
//---------------------------------------------------------------------
extern HINSTANCE dllInstanceHandle;

extern LONG dllRefCount;

extern CRITICAL_SECTION CS;
extern HFONT defaultlFontHandle; // Global font object we use everywhere

extern const CLSID MetasequoiaIMECLSID;
extern const GUID MetasequoiaIMEGuidProfile;
extern const GUID MetasequoiaIMEGuidImeModePreserveKey;
extern const GUID MetasequoiaIMEGuidImeModePreserveKey02;
extern const GUID MetasequoiaIMEGuidImeModePreserveKey03;
extern const GUID MetasequoiaIMEGuidEnglishInputModePreserveKey;
extern const GUID MetasequoiaIMEGuidDoubleSingleBytePreserveKey;
extern const GUID MetasequoiaIMEGuidPunctuationPreserveKey;

LRESULT CALLBACK ThreadKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
BOOL CheckModifiers(UINT uModCurrent, UINT uMod);
BOOL UpdateModifiers(WPARAM wParam, LPARAM lParam);

extern thread_local USHORT ModifiersValue;
extern thread_local BOOL IsShiftKeyDownOnly;
extern thread_local BOOL IsControlKeyDownOnly;
extern thread_local BOOL IsAltKeyDownOnly;
extern thread_local BOOL PureShiftKeyDown;
extern thread_local BOOL PureShiftKeyUp;

extern const GUID MetasequoiaIMEGuidCompartmentDoubleSingleByte;
extern const GUID MetasequoiaIMEGuidCompartmentPunctuation;

extern const WCHAR FullWidthCharTable[];
extern const struct _PUNCTUATION PunctuationTable[23];
extern const std::unordered_set<WCHAR> CommitWithHighlightedCandPunc;

extern const GUID MetasequoiaIMEGuidLangBarIMEMode;
extern const GUID MetasequoiaIMEGuidLangBarDoubleSingleByte;
extern const GUID MetasequoiaIMEGuidLangBarPunctuation;

extern const GUID MetasequoiaIMEGuidDisplayAttributeInput;
extern const GUID MetasequoiaIMEGuidDisplayAttributeConverted;

extern const GUID MetasequoiaIMEGuidCandUIElement;

extern const WCHAR UnicodeByteOrderMark;
extern const WCHAR KeywordDelimiter;
extern const WCHAR StringDelimiter;

extern const WCHAR ImeModeDescription[];
extern const WCHAR ImeModeDescription02[];
extern const WCHAR ImeModeDescription03[];
extern const WCHAR EnglishInputModeDescription[];
extern const int ImeModeOnIcoIndex;
extern const int ImeModeOffIcoIndex;

extern const WCHAR DoubleSingleByteDescription[];
extern const int DoubleSingleByteOnIcoIndex;
extern const int DoubleSingleByteOffIcoIndex;

extern const WCHAR PunctuationDescription[];
extern const int PunctuationOnIcoIndex;
extern const int PunctuationOffIcoIndex;

extern const WCHAR LangbarImeModeDescription[];
extern const WCHAR LangbarDoubleSingleByteDescription[];
extern const WCHAR LangbarPunctuationDescription[];

inline std::vector<std::wstring> WStringCandidateList;
inline std::wstring FindKeyCode;

inline POINT PointDTO = {0, 0};
inline std::wstring CandidateWString = L"";
} // namespace Global
