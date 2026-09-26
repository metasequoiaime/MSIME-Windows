#pragma once

#include "engine/contracts/windows_ipc.h"

#include <cstdint>
#include <optional>
#include <string>

namespace FanyImeUi
{
inline constexpr int kCaretStateBadgeHeightDip = 30;
inline constexpr int kCaretStateAdditionalCharacterWidthDip = 20;
inline constexpr int kCaretStatePunctuationSlotWidthDip = 64;
inline constexpr int kCaretStatePunctuationModeGapDip = 0;
inline constexpr int kCaretStatePunctuationModeSlotWidthDip = 30;
inline constexpr int kCaretStatePunctuationBadgeWidthDip =
    kCaretStatePunctuationSlotWidthDip + kCaretStatePunctuationModeGapDip + kCaretStatePunctuationModeSlotWidthDip;

// What one badge shows. `mode` is the optional trailing input-mode slot used
// by the punctuation badge ("，。" + "中"); single-state badges leave it empty.
struct CaretStateBadge
{
    std::wstring text;
    wchar_t mode = L'\0';

    bool HasModeSlot() const
    {
        return mode != L'\0';
    }
    bool operator==(const CaretStateBadge &other) const
    {
        return text == other.text && mode == other.mode;
    }
};

enum class CaretStateKind
{
    Width,
    CharacterSet,
};

// Global::INVALID_Y is -100000; anything this far above the virtual screen is
// treated as "no anchor".
inline constexpr int kInvalidAnchorY = -10000;

inline bool IsUsableCaretAnchor(int anchorX, int anchorY)
{
    // {0, INVALID_Y} marks an unresolved anchor; (0, 0) is what an absent one
    // degrades to and is never a real caret position in practice.
    return anchorY > kInvalidAnchorY && (anchorX != 0 || anchorY != 0);
}

inline bool ShouldShowCaretStateIndicator(bool indicatorEnabled, bool imeActive, bool uiLess, int anchorX, int anchorY)
{
    // UILess hosts (games) draw their own UI and must never receive an HWND.
    return indicatorEnabled && imeActive && !uiLess && IsUsableCaretAnchor(anchorX, anchorY);
}

inline int CaretStateIndicatorY(bool belowCaret, int anchorY, int indicatorHeight, int caretLineHeight, int gap)
{
    // The TSF anchor is GetTextExt.bottom, so an upper badge must also clear
    // the caret's text line. The lower position already starts below it.
    return belowCaret ? anchorY + gap : anchorY - indicatorHeight - caretLineHeight - gap;
}

inline std::optional<int> CaretStateIndicatorPlacementY(bool requestedBelow, int anchorY, int indicatorHeight,
                                                        int caretLineHeight, int gap, int workTop, int workBottom)
{
    const int above = CaretStateIndicatorY(false, anchorY, indicatorHeight, caretLineHeight, gap);
    const int below = CaretStateIndicatorY(true, anchorY, indicatorHeight, caretLineHeight, gap);
    const auto fits = [workTop, workBottom, indicatorHeight](int y) {
        return y >= workTop && y <= workBottom - indicatorHeight;
    };
    if (requestedBelow)
        return fits(below) ? std::optional<int>(below) : (fits(above) ? std::optional<int>(above) : std::nullopt);
    return fits(above) ? std::optional<int>(above) : (fits(below) ? std::optional<int>(below) : std::nullopt);
}

// Configured badge positions: a vertical side of the caret line, then a
// horizontal alignment. "top" / "bottom" are centred on the caret.
inline constexpr const char *kCaretStatePositions[] = {"top-left",    "top",    "top-right",
                                                       "bottom-left", "bottom", "bottom-right"};
inline constexpr const char *kDefaultCaretStatePosition = "top-left";

inline bool IsValidCaretStatePosition(const std::string &position)
{
    for (const char *candidate : kCaretStatePositions)
    {
        if (position == candidate)
            return true;
    }
    return false;
}

inline bool IsBelowCaretPosition(const std::string &position)
{
    return position.rfind("bottom", 0) == 0;
}

inline int CaretStateIndicatorX(const std::string &position, int anchorX, int indicatorWidth, int gap)
{
    if (position == "top" || position == "bottom")
        return anchorX - indicatorWidth / 2;
    if (position == "top-right" || position == "bottom-right")
        return anchorX + gap;
    // "top-left" and "bottom-left" sit left of the caret.
    return anchorX - indicatorWidth - gap;
}

inline int CaretStateIndicatorTextWidth(int height, int scaledAdditionalCharacterWidth, int extraCharacters)
{
    return extraCharacters <= 0 ? height : height + scaledAdditionalCharacterWidth * extraCharacters;
}

inline int CaretStateBadgeWidthDip(const CaretStateBadge &badge)
{
    if (badge.HasModeSlot())
        return kCaretStatePunctuationBadgeWidthDip;
    const int extra = badge.text.size() > 1 ? static_cast<int>(badge.text.size()) - 1 : 0;
    return CaretStateIndicatorTextWidth(kCaretStateBadgeHeightDip, kCaretStateAdditionalCharacterWidthDip, extra);
}

inline wchar_t InputModeGlyph(bool imeEnabled, bool japaneseMode)
{
    return imeEnabled ? (japaneseMode ? L'日' : L'中') : L'英';
}

// Caps Lock makes letters English regardless of the IME mode.
inline wchar_t EffectiveInputModeGlyph(bool imeEnabled, bool japaneseMode, bool capsLockEnabled)
{
    return capsLockEnabled ? L'英' : InputModeGlyph(imeEnabled, japaneseMode);
}

enum class InputModeTrigger
{
    UserToggle,
    CapsLockEdge,
    FocusEntered,
};

// Decodes the IMESwitch wch field; values this Server does not know are
// treated as a plain toggle, as the contract promises older senders.
inline InputModeTrigger DecodeInputModeTrigger(std::uint32_t wireTrigger)
{
    if (wireTrigger == FanyImeCaretStateTrigger::CapsLockEdge)
        return InputModeTrigger::CapsLockEdge;
    if (wireTrigger == FanyImeCaretStateTrigger::FocusEntered)
        return InputModeTrigger::FocusEntered;
    return InputModeTrigger::UserToggle;
}

inline bool ShouldShowInputModeEvent(InputModeTrigger trigger, bool focusAnnouncementEnabled, bool capsLockEnabled,
                                     bool imeEnabled, bool japaneseMode)
{
    switch (trigger)
    {
    case InputModeTrigger::FocusEntered:
        // An announcement of the current mode, not a change; opt-in only.
        return focusAnnouncementEnabled;
    case InputModeTrigger::CapsLockEdge:
        // A Caps Lock edge matters only when it changes the effective glyph.
        return EffectiveInputModeGlyph(imeEnabled, japaneseMode, !capsLockEnabled) !=
               EffectiveInputModeGlyph(imeEnabled, japaneseMode, capsLockEnabled);
    case InputModeTrigger::UserToggle:
        break;
    }
    // A language toggle under Caps Lock does not change what letters produce.
    return !capsLockEnabled;
}

inline CaretStateBadge InputModeBadge(bool imeEnabled, bool japaneseMode, bool capsLockEnabled)
{
    return {std::wstring(1, EffectiveInputModeGlyph(imeEnabled, japaneseMode, capsLockEnabled))};
}

inline CaretStateBadge PunctuationBadge(bool punctuationEnabled, bool imeEnabled, bool japaneseMode)
{
    return {punctuationEnabled ? L"，。" : L",.", InputModeGlyph(imeEnabled, japaneseMode)};
}

inline wchar_t CaretStateGlyph(CaretStateKind kind, bool enabled)
{
    switch (kind)
    {
    case CaretStateKind::Width:
        return enabled ? L'全' : L'半';
    case CaretStateKind::CharacterSet:
        return enabled ? L'繁' : L'简';
    }
    return L'\0';
}

inline CaretStateBadge SingleStateBadge(CaretStateKind kind, bool enabled)
{
    return {std::wstring(1, CaretStateGlyph(kind, enabled))};
}
} // namespace FanyImeUi
