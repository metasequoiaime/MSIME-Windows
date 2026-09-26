#pragma once

#include "window/caret_state_indicator_policy.h"

#include <string>
#include <windows.h>

// Transient badge near the text caret announcing a user-initiated input-state
// switch. All functions run on the UI thread that owns the badge window.
namespace CaretStateIndicator
{
// Heap-allocated by the IPC worker and owned by WM_SHOW_CARET_STATE.
struct ShowRequest
{
    FanyImeUi::CaretStateBadge badge;
    POINT caret;
    // Captured on the worker thread, which owns the UILess session state.
    bool uiLess = false;
};

bool Show(HWND hwnd, const FanyImeUi::CaretStateBadge &badge, POINT caret, bool topmost);
bool Reposition(HWND hwnd, POINT caret, bool topmost);
void Hide(HWND hwnd);
bool HandleTimer(HWND hwnd, WPARAM timerId);
void Paint(HWND hwnd, HDC dc);
} // namespace CaretStateIndicator
