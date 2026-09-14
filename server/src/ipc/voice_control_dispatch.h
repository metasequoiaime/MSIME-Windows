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

// Checks bounded frame syntax against caller-supplied expected lease fields.
// This is NOT peer authentication: the caller must authenticate the connection
// and keep the lease valid until the queued lifecycle command executes.
bool ParseVoiceControl(std::wstring_view frame, std::uint64_t client_id, std::uint64_t activation_epoch,
                       std::uint64_t generation, VoiceControlAction &action);
bool DispatchVoiceControl(std::wstring_view frame, std::uint64_t client_id, std::uint64_t activation_epoch,
                          std::uint64_t generation);
} // namespace FanyNamedPipe
