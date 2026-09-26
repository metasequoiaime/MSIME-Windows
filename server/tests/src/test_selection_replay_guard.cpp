#include "ipc/selection_replay_guard.h"
#include "tests/includes/test_framework.h"

TEST_CASE(selection_replay_guard_skips_a_replay_on_a_new_connection)
{
    FanyImeIpc::SelectionRankingReplayGuard guard;
    REQUIRE(guard.should_apply("p|ni|ni|A", 1, 1, 1000));
    // Reconnected and replayed within the window: the pick already counted.
    REQUIRE(!guard.should_apply("p|ni|ni|A", 2, 1, 1300));
    REQUIRE(!guard.should_apply("p|ni|ni|A", 2, 5, 1400));
}

TEST_CASE(selection_replay_guard_counts_real_repeats)
{
    FanyImeIpc::SelectionRankingReplayGuard guard;
    REQUIRE(guard.should_apply("p|ni|ni|A", 1, 1, 1000));
    // Same connection: the user picked the same candidate again.
    REQUIRE(guard.should_apply("p|ni|ni|A", 1, 1, 1200));
    // A different selection on a new connection is not a replay.
    REQUIRE(guard.should_apply("p|ni|ni|B", 2, 1, 1300));
    // The same selection long after the last one is not a replay either.
    REQUIRE(guard.should_apply("p|ni|ni|B", 3, 1, 1300 + FanyImeIpc::kSelectionReplayWindowMs + 1));
}
