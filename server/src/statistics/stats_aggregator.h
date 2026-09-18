#pragma once

#include "engine/contracts/windows_ipc.h"
#include "statistics/stats_store.h"

#include <cstddef>
#include <cstdint>

namespace Statistics
{
// Validates one TSF statistics frame and folds it into the store.
//
// A frame whose header disagrees with its payload is dropped whole instead of being parsed
// best-effort: misreading the record stride would file counts under the wrong hour, and the data
// is not worth guessing about. Every rejection only loses counts -- the caller is a read-only
// transport, so there is nothing to report back to the DLL.
class StatsAggregator
{
  public:
    explicit StatsAggregator(StatsStore &store);

    // connected_process_id comes from the pipe handle; 0 skips the writer-identity check (tests).
    bool HandleFrame(const unsigned char *frame, std::size_t size, std::uint32_t connected_process_id = 0);

  private:
    StatsStore &store_;
};
} // namespace Statistics
