#include "voice_control_dispatch.h"
#include "contracts/windows_ipc.h"
#include "../voice-input/voice_input_service.h"
#include <cwchar>
#include <cerrno>

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
    wchar_t *end = nullptr;
    std::wstring value(frame);
    const auto command = std::wcstoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != L'|' || command < FanyImeVoiceControl::Start ||
        command > FanyImeVoiceControl::Cancel)
        return false;
    const auto parse = [&](const wchar_t *begin, std::uint64_t expected, wchar_t **next) {
        errno = 0;
        const auto actual = std::wcstoull(begin, next, 10);
        return errno != ERANGE && actual == expected;
    };
    if (!parse(end + 1, client_id, &end) || *end != L'|' || !parse(end + 1, activation_epoch, &end) || *end != L'|' ||
        !parse(end + 1, generation, &end) || *end != L'\0')
        return false;
    action = static_cast<VoiceControlAction>(command);
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
