#include "Globals.h"
#include "Private.h"
#include "resource.h"
#include "define.h"
#include "MetasequoiaIMEBaseStructure.h"
#include <unordered_set>
#include <windows.h>
#include <fstream>
#include <string>
#include <ctime>
#include "FanyUtils.h"

namespace Global
{
HINSTANCE dllInstanceHandle;

LONG dllRefCount = -1;

CRITICAL_SECTION CS;
HFONT defaultlFontHandle; // Global font object we use everywhere

//---------------------------------------------------------------------
// MetasequoiaIME CLSID
//---------------------------------------------------------------------
// {E3062E9A-D834-4637-8958-ED8CFA427D01}
extern const CLSID MetasequoiaIMECLSID = {0xe3062e9a, 0xd834, 0x4637, {0x89, 0x58, 0xed, 0x8c, 0xfa, 0x42, 0x7d, 0x01}};

//---------------------------------------------------------------------
// Profile GUID
//---------------------------------------------------------------------
// {4D59B1B4-D503-44AE-9259-BAD9BB2778AB}
extern const GUID MetasequoiaIMEGuidProfile = {
    0x4d59b1b4, 0xd503, 0x44ae, {0x92, 0x59, 0xba, 0xd9, 0xbb, 0x27, 0x78, 0xab}};

//---------------------------------------------------------------------
// PreserveKey GUID
//---------------------------------------------------------------------
// {34764E82-AE6D-4F71-BB3A-96799AECE466}
extern const GUID MetasequoiaIMEGuidImeModePreserveKey = {
    0x34764e82, 0xae6d, 0x4f71, {0xbb, 0x3a, 0x96, 0x79, 0x9a, 0xec, 0xe4, 0x66}};

// {748C1D81-246B-4849-921F-143BA2BED3F5}
extern const GUID MetasequoiaIMEGuidImeModePreserveKey02 = {
    0x748c1d81, 0x246b, 0x4849, {0x92, 0x1f, 0x14, 0x3b, 0xa2, 0xbe, 0xd3, 0xf5}};

// {B7E4F2A1-9C3D-4E8F-A1B2-C3D4E5F60718}
extern const GUID MetasequoiaIMEGuidImeModePreserveKey03 = {
    0xb7e4f2a1, 0x9c3d, 0x4e8f, {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18}};

// {D625C0B1-5A8F-4CC4-9C65-6C536BFF2D91}
extern const GUID MetasequoiaIMEGuidEnglishInputModePreserveKey = {
    0xd625c0b1, 0x5a8f, 0x4cc4, {0x9c, 0x65, 0x6c, 0x53, 0x6b, 0xff, 0x2d, 0x91}};

// {4393748A-89DC-485C-A7F7-5FA232CEC70B}
extern const GUID MetasequoiaIMEGuidDoubleSingleBytePreserveKey = {
    0x4393748a, 0x89dc, 0x485c, {0xa7, 0xf7, 0x5f, 0xa2, 0x32, 0xce, 0xc7, 0x0b}};

// {628DDA3B-38D8-4521-BDD4-85CA38F475B8}
extern const GUID MetasequoiaIMEGuidPunctuationPreserveKey = {
    0x628dda3b, 0x38d8, 0x4521, {0xbd, 0xd4, 0x85, 0xca, 0x38, 0xf4, 0x75, 0xb8}};

//---------------------------------------------------------------------
// Compartments
//---------------------------------------------------------------------
// {851BC7CB-8395-4FA6-9C95-DB6EFC2E648E}
extern const GUID MetasequoiaIMEGuidCompartmentDoubleSingleByte = {
    0x851bc7cb, 0x8395, 0x4fa6, {0x9c, 0x95, 0xdb, 0x6e, 0xfc, 0x2e, 0x64, 0x8e}};

// {58DA9E0F-88B2-426F-91C8-802C9B4D9115}
extern const GUID MetasequoiaIMEGuidCompartmentPunctuation = {
    0x58da9e0f, 0x88b2, 0x426f, {0x91, 0xc8, 0x80, 0x2c, 0x9b, 0x4d, 0x91, 0x15}};

//---------------------------------------------------------------------
// LanguageBars
//---------------------------------------------------------------------

// {94B8FD94-E918-4667-93BE-57A49D35B02D}
extern const GUID MetasequoiaIMEGuidLangBarIMEMode = {
    0x94b8fd94, 0xe918, 0x4667, {0x93, 0xbe, 0x57, 0xa4, 0x9d, 0x35, 0xb0, 0x2d}};

// {3E044725-9617-402E-B113-9865AD9B4F8E}
extern const GUID MetasequoiaIMEGuidLangBarDoubleSingleByte = {
    0x3e044725, 0x9617, 0x402e, {0xb1, 0x13, 0x98, 0x65, 0xad, 0x9b, 0x4f, 0x8e}};

// {596E7EE3-B629-4895-A5B0-C60A82B47A04}
extern const GUID MetasequoiaIMEGuidLangBarPunctuation = {
    0x596e7ee3, 0xb629, 0x4895, {0xa5, 0xb0, 0xc6, 0x0a, 0x82, 0xb4, 0x7a, 0x04}};

// {688746FF-BAF2-4153-93ED-96943436422F}
extern const GUID MetasequoiaIMEGuidDisplayAttributeInput = {
    0x688746ff, 0xbaf2, 0x4153, {0x93, 0xed, 0x96, 0x94, 0x34, 0x36, 0x42, 0x2f}};

// {1E2209EA-13CD-4550-8A8F-B352E9744DF2}
extern const GUID MetasequoiaIMEGuidDisplayAttributeConverted = {
    0x1e2209ea, 0x13cd, 0x4550, {0x8a, 0x8f, 0xb3, 0x52, 0xe9, 0x74, 0x4d, 0xf2}};

//---------------------------------------------------------------------
// UI element
//---------------------------------------------------------------------

// {9FFF12AA-B5EE-4477-A1AA-A4BF5F7B2447}
extern const GUID MetasequoiaIMEGuidCandUIElement = {
    0x9fff12aa, 0xb5ee, 0x4477, {0xa1, 0xaa, 0xa4, 0xbf, 0x5f, 0x7b, 0x24, 0x47}};

//---------------------------------------------------------------------
// Unicode byte order mark
//---------------------------------------------------------------------
extern const WCHAR UnicodeByteOrderMark = 0xFEFF;

//---------------------------------------------------------------------
// dictionary table delimiter
//---------------------------------------------------------------------
extern const WCHAR KeywordDelimiter = L'=';
extern const WCHAR StringDelimiter = L'\"';

//---------------------------------------------------------------------
// defined item in setting file table [PreservedKey] section
//---------------------------------------------------------------------
extern const WCHAR ImeModeDescription[] = L"Chinese/English input (Shift)";
extern const WCHAR ImeModeDescription02[] = L"Chinese/English input (Ctrl+Alt+Space)";
extern const WCHAR ImeModeDescription03[] = L"Chinese/English input (Ctrl)";
extern const WCHAR EnglishInputModeDescription[] = L"English candidate input (Ctrl+Shift+E)";
extern const int ImeModeOnIcoIndex = IME_MODE_ON_ICON_INDEX;
extern const int ImeModeOffIcoIndex = IME_MODE_OFF_ICON_INDEX;

extern const WCHAR DoubleSingleByteDescription[] = L"Double/Single byte (Ctrl+Shift+Space)";
extern const int DoubleSingleByteOnIcoIndex = IME_DOUBLE_ON_INDEX;
extern const int DoubleSingleByteOffIcoIndex = IME_DOUBLE_OFF_INDEX;

extern const WCHAR PunctuationDescription[] = L"Chinese/English punctuation (Ctrl+.)";
extern const int PunctuationOnIcoIndex = IME_PUNCTUATION_ON_INDEX;
extern const int PunctuationOffIcoIndex = IME_PUNCTUATION_OFF_INDEX;

//---------------------------------------------------------------------
// defined item in setting file table [LanguageBar] section
//---------------------------------------------------------------------
extern const WCHAR LangbarImeModeDescription[] = L"Conversion mode";
extern const WCHAR LangbarDoubleSingleByteDescription[] = L"Character width";
extern const WCHAR LangbarPunctuationDescription[] = L"Punctuation";

//---------------------------------------------------------------------
// defined full width characters for Double/Single byte conversion
//---------------------------------------------------------------------
extern const WCHAR FullWidthCharTable[] = {
    0x3000, // Full width space
    0xFF01, // ！
    0xFF02, // ＂
    0xFF03, // ＃
    0xFF04, // ＄
    0xFF05, // ％
    0xFF06, // ＆
    0xFF07, // ＇
    0xFF08, // （
    0xFF09, // ）
    0xFF0A, // ＊
    0xFF0B, // ＋
    0xFF0C, // ，
    0xFF0D, // －
    0xFF0E, // ．
    0xFF0F, // ／

    0xFF10, // ０
    0xFF11, // １
    0xFF12, // ２
    0xFF13, // ３
    0xFF14, // ４
    0xFF15, // ５
    0xFF16, // ６
    0xFF17, // ７
    0xFF18, // ８
    0xFF19, // ９
    0xFF1A, // ：
    0xFF1B, // ；
    0xFF1C, // ＜
    0xFF1D, // ＝
    0xFF1E, // ＞
    0xFF1F, // ？

    0xFF20, // ＠
    0xFF21, // Ａ
    0xFF22, // Ｂ
    0xFF23, // Ｃ
    0xFF24, // Ｄ
    0xFF25, // Ｅ
    0xFF26, // Ｆ
    0xFF27, // Ｇ
    0xFF28, // Ｈ
    0xFF29, // Ｉ
    0xFF2A, // Ｊ
    0xFF2B, // Ｋ
    0xFF2C, // Ｌ
    0xFF2D, // Ｍ
    0xFF2E, // Ｎ
    0xFF2F, // Ｏ

    0xFF30, // Ｐ
    0xFF31, // Ｑ
    0xFF32, // Ｒ
    0xFF33, // Ｓ
    0xFF34, // Ｔ
    0xFF35, // Ｕ
    0xFF36, // Ｖ
    0xFF37, // Ｗ
    0xFF38, // Ｘ
    0xFF39, // Ｙ
    0xFF3A, // Ｚ
    0xFF3B, // ［
    0xFF3C, // ＼
    0xFF3D, // ］
    0xFF3E, // ＾
    0xFF3F, // ＿

    0xFF40, // ｀
    0xFF41, // ａ
    0xFF42, // ｂ
    0xFF43, // ｃ
    0xFF44, // ｄ
    0xFF45, // ｅ
    0xFF46, // ｆ
    0xFF47, // ｇ
    0xFF48, // ｈ
    0xFF49, // ｉ
    0xFF4A, // ｊ
    0xFF4B, // ｋ
    0xFF4C, // ｌ
    0xFF4D, // ｍ
    0xFF4E, // ｎ
    0xFF4F, // ｏ

    0xFF50, // ｐ
    0xFF51, // ｑ
    0xFF52, // ｒ
    0xFF53, // ｓ
    0xFF54, // ｔ
    0xFF55, // ｕ
    0xFF56, // ｖ
    0xFF57, // ｗ
    0xFF58, // ｘ
    0xFF59, // ｙ
    0xFF5A, // ｚ
    0xFF5B, // ｛
    0xFF5C, // ｜
    0xFF5D, // ｝
    0xFF5E  // ～
};

//---------------------------------------------------------------------
// defined punctuation characters
//---------------------------------------------------------------------
// '<' and '>' are deliberately absent: they belong to the nest pair set up in
// SetupPunctuationPair, and GetPunctuation searches this table first. Listing them here
// would shadow the nest pair, and the nested marks would never be produced.
extern const struct _PUNCTUATION PunctuationTable[23] = {
    {L'`', L"·"},   // ·
    {L'~', L"~"},   // ~
    {L'!', L"！"},  // ！
    {L'@', L"@"},   // @
    {L'#', L"#"},   // #
    {L'$', L"￥"},  // ￥
    {L'%', L"%"},   // %
    {L'^', L"……"},  // ……
    {L'&', L"&"},   // &
    {L'*', L"*"},   // *
    {L'(', L"（"},  // （
    {L')', L"）"},  // ）
    {L'_', L"——"},  // ——
    {L'[', L"【"},  // 【
    {L']', L"】"},  // 】
    {L'{', L"{"},   // {
    {L'}', L"}"},   // }
    {L'\\', L"、"}, // 、
    {L';', L"；"},  // ；
    {L':', L"："},  // ：
    {L',', L"，"},  // ，
    {L'.', L"。"},  // 。
    {L'?', L"？"},  // ？
};

//
// Will commit the highlighted candidate string with a punctuation character.
//
extern const std::unordered_set<WCHAR> CommitWithHighlightedCandPunc = {
    L'`',  //
    L'!',  //
    L'@',  //
    L'#',  //
    L'$',  //
    L'%',  //
    L'^',  //
    L'&',  //
    L'*',  //
    L'(',  //
    L')',  //
    L'-',  //
    L'_',  //
    L'=',  //
    L'+',  //
    L'[',  //
    L']',  //
    L'\\', //
    L';',  //
    L':',  //
    L'\'', //
    L'"',  //
    L',',  //
    L'<',  //
    L'.',  //
    L'>',  //
    L'?'   //
};

//+---------------------------------------------------------------------------
//
// CheckModifiers
//
//----------------------------------------------------------------------------

#define TF_MOD_ALLALT (TF_MOD_RALT | TF_MOD_LALT | TF_MOD_ALT)
#define TF_MOD_ALLCONTROL (TF_MOD_RCONTROL | TF_MOD_LCONTROL | TF_MOD_CONTROL)
#define TF_MOD_ALLSHIFT (TF_MOD_RSHIFT | TF_MOD_LSHIFT | TF_MOD_SHIFT)
#define TF_MOD_RLALT (TF_MOD_RALT | TF_MOD_LALT)
#define TF_MOD_RLCONTROL (TF_MOD_RCONTROL | TF_MOD_LCONTROL)
#define TF_MOD_RLSHIFT (TF_MOD_RSHIFT | TF_MOD_LSHIFT)

#define CheckMod(m0, m1, mod)                                                                                          \
    if (m1 & TF_MOD_##mod##)                                                                                           \
    {                                                                                                                  \
        if (!(m0 & TF_MOD_##mod##))                                                                                    \
        {                                                                                                              \
            return FALSE;                                                                                              \
        }                                                                                                              \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        if ((m1 ^ m0) & TF_MOD_RL##mod##)                                                                              \
        {                                                                                                              \
            return FALSE;                                                                                              \
        }                                                                                                              \
    }

BOOL CheckModifiers(UINT modCurrent, UINT mod)
{
    mod &= ~TF_MOD_ON_KEYUP;

    if (mod & TF_MOD_IGNORE_ALL_MODIFIER)
    {
        return TRUE;
    }

    if (modCurrent == mod)
    {
        return TRUE;
    }

    if (modCurrent && !mod)
    {
        return FALSE;
    }

    CheckMod(modCurrent, mod, ALT);
    CheckMod(modCurrent, mod, SHIFT);
    CheckMod(modCurrent, mod, CONTROL);

    return TRUE;
}

//+---------------------------------------------------------------------------
//
// UpdateModifiers
//
//    wParam - virtual-key code
//    lParam - [0-15]  Repeat count
//  [16-23] Scan code
//  [24]    Extended key
//  [25-28] Reserved
//  [29]    Context code
//  [30]    Previous key state
//  [31]    Transition state
//----------------------------------------------------------------------------

thread_local USHORT ModifiersValue = 0;
thread_local BOOL IsShiftKeyDownOnly = FALSE;
thread_local BOOL IsControlKeyDownOnly = FALSE;
thread_local BOOL IsAltKeyDownOnly = FALSE;
thread_local BOOL PureShiftKeyDown = FALSE;
thread_local BOOL PureShiftKeyUp = FALSE;

BOOL UpdateModifiers(WPARAM wParam, LPARAM lParam)
{
    // high-order bit : key down
    // low-order bit  : toggled
    SHORT sksMenu = GetKeyState(VK_MENU);
    SHORT sksCtrl = GetKeyState(VK_CONTROL);
    SHORT sksShft = GetKeyState(VK_SHIFT);

    PureShiftKeyUp = FALSE;

    switch (wParam & 0xff)
    {
    case VK_MENU:
        // is VK_MENU down?
        if (sksMenu & 0x8000)
        {
            // is extended key?
            if (lParam & 0x01000000)
            {
                ModifiersValue |= (TF_MOD_RALT | TF_MOD_ALT);
            }
            else
            {
                ModifiersValue |= (TF_MOD_LALT | TF_MOD_ALT);
            }

            // is previous key state up?
            if (!(lParam & 0x40000000))
            {
                // is VK_CONTROL and VK_SHIFT up?
                if (!(sksCtrl & 0x8000) && !(sksShft & 0x8000))
                {
                    IsAltKeyDownOnly = TRUE;
                }
                else
                {
                    IsShiftKeyDownOnly = FALSE;
                    IsControlKeyDownOnly = FALSE;
                    IsAltKeyDownOnly = FALSE;
                }
            }
        }
        break;

    case VK_CONTROL:
        // is VK_CONTROL down?
        if (sksCtrl & 0x8000)
        {
            // is extended key?
            if (lParam & 0x01000000)
            {
                ModifiersValue |= (TF_MOD_RCONTROL | TF_MOD_CONTROL);
            }
            else
            {
                ModifiersValue |= (TF_MOD_LCONTROL | TF_MOD_CONTROL);
            }

            // is previous key state up?
            if (!(lParam & 0x40000000))
            {
                // is VK_SHIFT and VK_MENU up?
                if (!(sksShft & 0x8000) && !(sksMenu & 0x8000))
                {
                    IsControlKeyDownOnly = TRUE;
                }
                else
                {
                    IsShiftKeyDownOnly = FALSE;
                    IsControlKeyDownOnly = FALSE;
                    IsAltKeyDownOnly = FALSE;
                }
            }
        }
        break;

    case VK_SHIFT: {
        // is VK_SHIFT down?
        if (sksShft & 0x8000)
        {
            PureShiftKeyDown = TRUE;
            // is scan code 0x36(right shift)?
            if (((lParam >> 16) & 0x00ff) == 0x36)
            {
                ModifiersValue |= (TF_MOD_RSHIFT | TF_MOD_SHIFT);
            }
            else
            {
                ModifiersValue |= (TF_MOD_LSHIFT | TF_MOD_SHIFT);
            }

            // is previous key state up?
            if (!(lParam & 0x40000000))
            {
                // is VK_MENU and VK_CONTROL up?
                if (!(sksMenu & 0x8000) && !(sksCtrl & 0x8000))
                {
                    IsShiftKeyDownOnly = TRUE;
                }
                else
                {
                    IsShiftKeyDownOnly = FALSE;
                    IsControlKeyDownOnly = FALSE;
                    IsAltKeyDownOnly = FALSE;
                }
            }
        }
        else
        {
            if (PureShiftKeyDown)
                PureShiftKeyUp = TRUE;
        }
        break;
    }

    default:
        IsShiftKeyDownOnly = FALSE;
        IsControlKeyDownOnly = FALSE;
        IsAltKeyDownOnly = FALSE;
        PureShiftKeyDown = FALSE;
        break;
    }

    if (!(sksMenu & 0x8000))
    {
        ModifiersValue &= ~TF_MOD_ALLALT;
    }
    if (!(sksCtrl & 0x8000))
    {
        ModifiersValue &= ~TF_MOD_ALLCONTROL;
    }
    if (!(sksShft & 0x8000))
    {
        ModifiersValue &= ~TF_MOD_ALLSHIFT;
    }

    return TRUE;
}

//---------------------------------------------------------------------
// override CompareElements
//---------------------------------------------------------------------
BOOL CompareElements(LCID locale, const CStringRange *pElement1, const CStringRange *pElement2)
{
    return (CStringRange::Compare(locale, (CStringRange *)pElement1, (CStringRange *)pElement2) == CSTR_EQUAL) ? TRUE
                                                                                                               : FALSE;
}
} // namespace Global
