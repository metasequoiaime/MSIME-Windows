#pragma once
#include <cstdint>
#include <string_view>

namespace FanyNamedPipe
{
enum class VoiceControlAction : std::uint32_t
{
    Start = 1,
    Stop = 2,
    Cancel = 3
};

// Parses the bounded UTF-16 control frame and authenticates its lease fields.
// Execution is deliberately supplied by the caller so this layer has no
// access to global voice state and remains straightforward to test.
bool ParseVoiceControl(std::wstring_view frame, std::uint64_t client_id, std::uint64_t activation_epoch,
                       std::uint64_t generation, VoiceControlAction &action);
bool DispatchVoiceControl(std::wstring_view frame, std::uint64_t client_id, std::uint64_t activation_epoch,
                          std::uint64_t generation);
} // namespace FanyNamedPipe
