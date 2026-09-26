#pragma once

#include <windows.h>

// Auto-repeat guard for a Backspace hold that started inside a composition.
//
// Windows marks every key-down after the first one of a hold with lParam bit 30
// (the "previous key state" bit, KF_REPEAT). The TSF key sinks see that bit, so
// a repeat can be told apart from a fresh press without tracking key-up events:
// hosts do not reliably deliver a key-up for a key this tip ate, and a real
// release is only ever followed by a new non-repeat key-down.
//
// A hold that began while composing keeps deleting the preedit. Once the
// composition is gone, the repeat would otherwise be handed back to the host
// and start deleting document text (#347). The IME state these helpers classify
// -- whether the hold is armed and whether the composition is still alive --
// lives in CMetasequoiaIME; the checks themselves stay pure so they can be
// unit tested.

inline bool IsAutoRepeat(LPARAM lParam)
{
    return (static_cast<ULONG_PTR>(lParam) & 0x40000000u) != 0;
}

inline bool IsFreshCapsLockKeyDown(WPARAM key, LPARAM lParam)
{
    // The Server hook may have already broadcast the new lock state. The
    // physical non-repeat key press is the edge, regardless of cached state.
    return key == VK_CAPITAL && !IsAutoRepeat(lParam);
}

inline bool ShouldApplyCapsLockActualKeyDownSideEffects(bool matchingTestKeyDownHandled, WPARAM key, LPARAM lParam)
{
    return !matchingTestKeyDownHandled && IsFreshCapsLockKeyDown(key, lParam);
}

inline bool ResultingCapsLockState(bool actualKeyDown, bool observedCapsLockState)
{
    return actualKeyDown ? observedCapsLockState : !observedCapsLockState;
}

inline bool ShouldSuppressBackspaceRepeat(bool armed, bool compositionActive, bool repeat)
{
    return armed && repeat && !compositionActive;
}
