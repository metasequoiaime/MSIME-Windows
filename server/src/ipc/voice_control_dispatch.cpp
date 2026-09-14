#include "voice_control_dispatch.h"
#include "contracts/windows_ipc.h"
#include "../voice-input/voice_input_service.h"
#include <limits>

namespace FanyNamedPipe
{
bool ParseVoiceControl(std::wstring_view frame, std::uint64_t client_id, std::uint64_t activation_epoch,
                       std::uint64_t generation, VoiceControlAction &action)
{
    constexpr std::wstring_view prefix = L"MSIME_VOICE|";
    if (frame.size() > FanyImeVoiceControl::MaxMessageChars || frame.substr(0, prefix.size()) != prefix || !client_id ||
        !activation_epoch || !generation)
        return false;
    frame.remove_prefix(prefix.size());
    std::uint64_t fields[4]{};
    for (unsigned index = 0; index < 4; ++index)
    {
        const auto separator = frame.find(L'|');
        if ((index < 3) != (separator != std::wstring_view::npos))
            return false;
        const auto field = frame.substr(0, separator);
        if (field.empty())
            return false;
        for (const auto digit : field)
        {
            if (digit < L'0' || digit > L'9')
                return false;
            const auto value = static_cast<std::uint64_t>(digit - L'0');
            if (fields[index] > (std::numeric_limits<std::uint64_t>::max() - value) / 10)
                return false;
            fields[index] = fields[index] * 10 + value;
        }
        if (index < 3)
            frame.remove_prefix(separator + 1);
    }
    if (fields[0] < FanyImeVoiceControl::Start || fields[0] > FanyImeVoiceControl::Cancel || fields[1] != client_id ||
        fields[2] != activation_epoch || fields[3] != generation)
        return false;
    action = static_cast<VoiceControlAction>(fields[0]);
    return true;
}

bool DispatchVoiceControl(std::wstring_view frame, std::uint64_t client_id, std::uint64_t activation_epoch,
                          std::uint64_t generation)
{
    VoiceControlAction action{};
    if (!ParseVoiceControl(frame, client_id, activation_epoch, generation, action))
        return false;
    switch (action)
    {
    case VoiceControlAction::Start:
        VoiceInput::StartRecording();
        break;
    case VoiceControlAction::Stop:
        VoiceInput::StopRecording();
        break;
    case VoiceControlAction::Cancel:
        VoiceInput::CancelRecording();
        break;
    }
    return true;
}
} // namespace FanyNamedPipe
