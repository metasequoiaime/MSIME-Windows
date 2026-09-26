#pragma once

// A KeyEvent packet that carries a caret anchor also writes point[]; an
// unresolved anchor is still written, as {0, INVALID_Y}.
inline constexpr unsigned KeyEventPayloadWriteMask(bool includeCaretAnchor)
{
    return includeCaretAnchor ? 0b001111u : 0b000111u;
}

inline constexpr bool IsUsableCaretExtent(int left, int top, int right, int bottom)
{
    // A collapsed TSF range is a vertical caret and may legitimately have zero
    // width. Clipped extents are still usable: the badge is clamped on screen.
    return right >= left && bottom > top;
}
