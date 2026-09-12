#pragma once

#include <cstdint>

namespace FanyImeIpc
{
inline constexpr uint32_t kVirtualKeyShift = 0x10;
inline constexpr uint32_t kVirtualKeyEscape = 0x1B;
inline constexpr uint32_t kVirtualKeyLeftShift = 0xA0;
inline constexpr uint32_t kVirtualKeyRightShift = 0xA1;
inline constexpr uint32_t kVirtualKeyNumpad0 = 0x60;
inline constexpr uint32_t kVirtualKeyNumpad9 = 0x69;
inline constexpr uint32_t kModifierShift = 0b00000001u;
inline constexpr uint32_t kModifierControl = 0b00000010u;
inline constexpr uint32_t kModifierAlt = 0b00000100u;
inline constexpr uint32_t kModifierUiLess = 0x80000000u;
inline constexpr uint32_t kEnglishModeToggleModifiers = kModifierShift | kModifierControl;
inline constexpr uint32_t kKeyModifierMask = kModifierShift | kModifierControl | kModifierAlt;

constexpr bool IsEnglishModeToggleKey(uint32_t keycode, uint32_t modifiers_down)
{
    return keycode == static_cast<uint32_t>('E') && (modifiers_down & kKeyModifierMask) == kEnglishModeToggleModifiers;
}

// Return -1 / +1 for the first / last Han character, or zero for an ordinary key.
constexpr int WordToCharacterDirection(uint32_t keycode, uint32_t character, uint32_t modifiers, bool enabled,
                                       bool minus_equal)
{
    if (!enabled || (modifiers & kKeyModifierMask) != 0)
        return 0;
    if (minus_equal)
        return keycode == 0xBD && character == '-' ? -1 : keycode == 0xBB && character == '=' ? 1 : 0;
    return keycode == 0xDB && character == '[' ? -1 : keycode == 0xDD && character == ']' ? 1 : 0;
}

// The TSF side treats numpad digits exactly like the corresponding candidate
// digit. Canonicalize them at the Server boundary so every downstream policy
// sees the same key code and, crucially, produces a reply for the request.
constexpr uint32_t NormalizeNumpadDigitKey(uint32_t keycode)
{
    return keycode >= kVirtualKeyNumpad0 && keycode <= kVirtualKeyNumpad9
               ? static_cast<uint32_t>('0') + (keycode - kVirtualKeyNumpad0)
               : keycode;
}

// TSF locally consumes these keys and completes/cancels its composition. The
// Server must reset every backend without producing a reply.
constexpr bool IsBackendIndependentCompositionResetKey(uint32_t keycode)
{
    return keycode == kVirtualKeyShift || keycode == kVirtualKeyEscape || keycode == kVirtualKeyLeftShift ||
           keycode == kVirtualKeyRightShift;
}

constexpr bool ShouldResetCompositionForImeMode(bool chinese_mode)
{
    return !chinese_mode;
}

// Enter commits the raw composition instead of choosing a special-mode
// candidate. Therefore a Shift+letter wake key must not by itself prevent an
// otherwise non-pinyin English word (for example "Metasequoia") from being
// learned. The database layer still validates the final string.
constexpr bool ShouldLearnEnteredEnglishWord(bool dedicated_english_mode, bool shift_letter_special_mode,
                                             bool chinese_scheme, bool all_complete_pinyin)
{
    return dedicated_english_mode || shift_letter_special_mode || (chinese_scheme && !all_complete_pinyin);
}

constexpr bool InputSessionMatchesConfig(bool configured_scheme_matches, bool temporary_r_mode_active,
                                         bool session_is_japanese)
{
    return configured_scheme_matches || (temporary_r_mode_active && session_is_japanese);
}

constexpr bool ShouldSendCompositionReply(bool is_alpha_key, bool is_manual_pinyin_separator,
                                          bool is_microsoft_shuangpin_ing_key, bool is_unicode_hex_digit,
                                          bool is_unicode_plus, bool is_number_point)
{
    return is_alpha_key || is_manual_pinyin_separator || is_microsoft_shuangpin_ing_key || is_unicode_hex_digit ||
           is_unicode_plus || is_number_point;
}
} // namespace FanyImeIpc
