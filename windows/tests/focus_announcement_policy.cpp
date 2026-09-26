#include "Thread/FocusAnnouncementPolicy.h"

int main()
{
    // Do not use assert: Release builds must execute these checks too.

    // Chromium swapping document managers between two editable documents in
    // the same window while the session stays up is typing, not a new field.
    if (ShouldAnnounceDocumentFocus(true, false, true, false))
        return 1;
    // A different focus window: another Win32 edit, a dialog, another app.
    if (!ShouldAnnounceDocumentFocus(true, true, true, false))
        return 2;
    // Same window, but focus passed through no editable document first: a
    // single-window host (Chromium, WPF) moving from one field to the next.
    if (!ShouldAnnounceDocumentFocus(true, false, false, false))
        return 3;
    // Coming back after the focus session really ended.
    if (!ShouldAnnounceDocumentFocus(true, false, true, true))
        return 4;
    // Focus on something that cannot take text never announces.
    if (ShouldAnnounceDocumentFocus(false, true, false, true))
        return 5;

    if (!ShouldAnnounceThreadFocusReturn(true, false))
        return 6;
    // Closing the Win+. panel returns thread focus without a field change.
    if (ShouldAnnounceThreadFocusReturn(true, true))
        return 7;
    if (ShouldAnnounceThreadFocusReturn(false, false))
        return 8;
    return 0;
}
