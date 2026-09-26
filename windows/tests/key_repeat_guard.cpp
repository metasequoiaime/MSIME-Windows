#include "Key/KeyRepeatGuard.h"

int main()
{
    // Do not use assert: Release builds must execute these checks too.

    // Only bit 30 marks a repeat. Bit 31 (up/context) and the scan-code bits
    // must not leak into the decision.
    if (IsAutoRepeat(0))
        return 1;
    if (!IsAutoRepeat(static_cast<LPARAM>(0x40000000u)))
        return 2;
    if (IsAutoRepeat(static_cast<LPARAM>(0x80000000u)))
        return 3;
    if (!IsAutoRepeat(static_cast<LPARAM>(0x40000000u | 0x80000000u)))
        return 4;
    if (IsAutoRepeat(static_cast<LPARAM>(0x001d0001u)))
        return 5;
    if (!IsAutoRepeat(static_cast<LPARAM>(0x001d0001u | 0x40000000u)))
        return 6;

    // The physical key transition remains the edge whether the Server hook's
    // asynchronous state broadcast arrives before or after this test-key call.
    if (!IsFreshCapsLockKeyDown(VK_CAPITAL, 0))
        return 7;
    if (IsFreshCapsLockKeyDown(VK_CAPITAL, 0x40000000))
        return 8;
    if (IsFreshCapsLockKeyDown('A', 0))
        return 9;

    // The actual callback is the fallback for hosts that omit TestKeyDown,
    // while a matching test callback suppresses the duplicate side effects.
    if (ShouldApplyCapsLockActualKeyDownSideEffects(true, VK_CAPITAL, 0))
        return 10;
    if (!ShouldApplyCapsLockActualKeyDownSideEffects(false, VK_CAPITAL, 0))
        return 11;
    if (ShouldApplyCapsLockActualKeyDownSideEffects(false, VK_CAPITAL, 0x40000000))
        return 12;
    if (ShouldApplyCapsLockActualKeyDownSideEffects(false, 'A', 0))
        return 13;

    // TestKeyDown sees the old toggle bit; OnKeyDown sees the new one.
    if (!ResultingCapsLockState(false, false))
        return 14;
    if (ResultingCapsLockState(false, true))
        return 15;
    if (!ResultingCapsLockState(true, true))
        return 16;
    if (ResultingCapsLockState(true, false))
        return 17;

    // Exhaustive armed x compositionActive x repeat matrix.
    for (int mask = 0; mask < 8; ++mask)
    {
        const bool armed = (mask & 1) != 0;
        const bool compositionActive = (mask & 2) != 0;
        const bool repeat = (mask & 4) != 0;
        const bool expected = armed && repeat && !compositionActive;
        if (ShouldSuppressBackspaceRepeat(armed, compositionActive, repeat) != expected)
        {
            return 20 + mask;
        }
    }

    // A fresh press (no repeat bit) is never swallowed, armed or not: the new
    // hold re-evaluates the guard and restores ordinary Backspace.
    if (ShouldSuppressBackspaceRepeat(true, false, false))
        return 30;
    if (ShouldSuppressBackspaceRepeat(true, true, false))
        return 31;

    // A hold that never touched a composition is not guarded even when the
    // composition disappeared for another reason.
    if (ShouldSuppressBackspaceRepeat(false, false, true))
        return 32;

    // While the composition is still alive the repeat belongs to it.
    if (ShouldSuppressBackspaceRepeat(true, true, true))
        return 33;

    return 0;
}
