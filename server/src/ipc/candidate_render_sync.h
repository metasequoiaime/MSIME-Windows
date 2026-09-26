#pragma once

#include <cstdint>

namespace FanyImeIpc
{
// A digit/space selection settles against the live page_words, but the user is looking at the
// asynchronously painted CandidatePageSnapshot. Pin-frequency reorders a page after every commit, so
// a selection that runs ahead of the painted frame commits a candidate the user never saw.
//
// The wait bound is load-bearing: the TSF side reads a committing reply with
// FANY_IME_COMMIT_REPLY_TIMEOUT_MS (300ms, windows/src/IPC/Ipc.h) and on a miss tears the pipe
// down and makes the user choose again. The wait is woken by the render echo, so a condition
// variable timeout (rounded up to one timer tick, ~16ms) is the only slack on top of this bound;
// together with candidate resolution it must stay well inside that budget. The frequency write
// runs after the reply and no longer competes for it. On a slow machine (battery, WebView2 paint)
// a paint regularly takes longer than 30ms, and settling against the unpainted page is exactly the
// "committed something I did not see" bug, so the bound leaves room for a slow paint. On timeout
// the caller continues with the current data and the diagnostic log records the miss. Do not
// raise this without re-deriving the TSF budget.
constexpr int kCandidateSelectionRenderWaitMaxMs = 80;

// Header-only pure policy so tests can pin it without linking the Windows/server stack.
// Generation 0 means "never published/rendered". A host-drawn (UI-less) or invisible candidate
// window has no on-screen list to match either, so none of those cases waits.
constexpr bool ShouldWaitForCandidateRender(std::uint64_t rendered, std::uint64_t current, bool uiless,
                                            bool visible) noexcept
{
    if (uiless || !visible)
    {
        return false;
    }
    if (current == 0 || rendered == 0)
    {
        return false;
    }
    return rendered < current;
}
} // namespace FanyImeIpc
