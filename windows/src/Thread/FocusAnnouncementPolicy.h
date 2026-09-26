#pragma once

// When moving focus into a text field should announce the current input mode.
//
// Chromium replaces its document manager on almost every edit, so a document
// change alone is not a field change. A field change is one of:
// - a different focus window (Win32 edits, dialogs and other windows each own
//   one);
// - editable focus arriving after a gap with no editable document, which is
//   how single-window hosts blur one field before focusing the next;
// - a new focus session after the previous one really ended.
inline bool ShouldAnnounceDocumentFocus(bool editable, bool focusWindowChanged, bool previousFocusEditable,
                                        bool focusSessionStarted)
{
    return editable && (focusWindowChanged || !previousFocusEditable || focusSessionStarted);
}

// Thread focus also returns after the Win+. panel (TextInputHost) closes; that
// is not the user moving to another field.
inline bool ShouldAnnounceThreadFocusReturn(bool threadFocusWasLost, bool lostToTextInputHost)
{
    return threadFocusWasLost && !lostToTextInputHost;
}
