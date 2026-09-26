#include "ipc/candidate_render_sync.h"
#include "tests/includes/test_framework.h"

TEST_CASE(candidate_render_sync_waits_only_when_a_painted_page_lags_the_published_page)
{
    using FanyImeIpc::ShouldWaitForCandidateRender;

    // Already painted: no wait, so normal typing pays nothing.
    REQUIRE(!ShouldWaitForCandidateRender(7, 7, false, true));
    // A newer page was published while the painted frame is older: wait for the paint.
    REQUIRE(ShouldWaitForCandidateRender(6, 7, false, true));
    REQUIRE(ShouldWaitForCandidateRender(1, 2, false, true));
    // Generation 0 means nothing has been published or painted yet.
    REQUIRE(!ShouldWaitForCandidateRender(0, 7, false, true));
    REQUIRE(!ShouldWaitForCandidateRender(7, 0, false, true));
    REQUIRE(!ShouldWaitForCandidateRender(0, 0, false, true));
    // Host-drawn (UI-less) candidate windows have no on-screen list to match.
    REQUIRE(!ShouldWaitForCandidateRender(6, 7, true, true));
    // A hidden candidate window has no on-screen list to match either.
    REQUIRE(!ShouldWaitForCandidateRender(6, 7, false, false));
    // A render echoed ahead of the published page is not a lag (page cleared and republished).
    REQUIRE(!ShouldWaitForCandidateRender(8, 7, false, true));
}

TEST_CASE(candidate_render_sync_wait_is_bounded_to_a_visible_stall)
{
    // The bound is a UX contract and a transport constraint: a wedged UI thread may delay a
    // committed keystroke by at most this, and the wait must stay well inside the TSF commit reply
    // timeout (300ms, windows/src/IPC/Ipc.h FANY_IME_COMMIT_REPLY_TIMEOUT_MS) or TSF tears the pipe
    // down and the user has to choose again.
    REQUIRE_EQ(FanyImeIpc::kCandidateSelectionRenderWaitMaxMs, 80);
}
