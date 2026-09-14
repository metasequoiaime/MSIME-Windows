#include "ipc/voice_control_dispatch.h"
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
unsigned calls = 0;
unsigned last_action = 0;
} // namespace
// Link-time substitutes: tests never start microphones or send network requests.
namespace VoiceInput
{
void StartRecording()
{
    ++calls;
    last_action = 1;
}
void StopRecording()
{
    ++calls;
    last_action = 2;
}
void CancelRecording()
{
    ++calls;
    last_action = 3;
}
} // namespace VoiceInput

int main()
{
    using namespace FanyNamedPipe;
    for (unsigned command = 1; command <= 3; ++command)
    {
        const auto frame = L"MSIME_VOICE|" + std::to_wstring(command) + L"|7|11|13";
        if (!DispatchVoiceControl(frame, 7, 11, 13) || last_action != command)
            return EXIT_FAILURE;
    }
    const auto accepted = calls;
    for (const auto *frame :
         {L"MSIME_VOICE|1|7|11|14", L"MSIME_VOICE|1|8|11|13", L"MSIME_VOICE|1|7|12|13", L"MSIME_VOICE|+1|7|11|13",
          L"MSIME_VOICE|1| 7|11|13", L"MSIME_VOICE|1|7|11|13|", L"MSIME_VOICE|1|18446744073709551616|11|13"})
        if (DispatchVoiceControl(frame, 7, 11, 13))
            return EXIT_FAILURE;
    std::wstring nul = L"MSIME_VOICE|1|7|11|13";
    nul.push_back(0);
    nul += L"ignored";
    if (DispatchVoiceControl(nul, 7, 11, 13) || calls != accepted)
        return EXIT_FAILURE;
    std::cout << "Voice control boundary and dispatch tests passed\n";
}
