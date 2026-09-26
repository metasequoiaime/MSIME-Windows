#pragma once

#include <cstdint>
#include <string>

namespace FanyImeIpc
{
// A committing reply that misses the TSF deadline tears the pipe down. The keystrokes of the
// composition may then be replayed on the new connection, and the server sees the very same
// selection a second time. The commit itself is settled on the TSF side; the ranking write is
// not, so without this guard one pick would count twice and reorder the page a second time.
//
// A replay always arrives on a different (client_id, activation_epoch) than the original, and
// arrives quickly. A repeat on the same connection is a real second pick and still counts.
constexpr std::uint64_t kSelectionReplayWindowMs = 3000;

// Synchronization is supplied by the caller (the server worker thread).
class SelectionRankingReplayGuard
{
  public:
    // `key` identifies the ranking write (dictionary kind, context key, entry key, word).
    // Returns whether the ranking write should run.
    bool should_apply(const std::string &key, std::uint64_t client_id, std::uint64_t activation_epoch,
                      std::uint64_t now_ms)
    {
        const bool replay = valid_ && key == key_ &&
                            (client_id != client_id_ || activation_epoch != activation_epoch_) && now_ms >= at_ms_ &&
                            now_ms - at_ms_ <= kSelectionReplayWindowMs;
        valid_ = true;
        key_ = key;
        client_id_ = client_id;
        activation_epoch_ = activation_epoch;
        at_ms_ = now_ms;
        return !replay;
    }

  private:
    bool valid_ = false;
    std::string key_;
    std::uint64_t client_id_ = 0;
    std::uint64_t activation_epoch_ = 0;
    std::uint64_t at_ms_ = 0;
};
} // namespace FanyImeIpc
