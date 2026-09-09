#include "IPC/KeyEventSendResult.h"

int main()
{
    // Do not use assert: Release builds must execute these checks too.
    if (IsDefinitelyNotSent(KeyEventSendResult::Sent))
        return 1;
    if (!IsDefinitelyNotSent(KeyEventSendResult::DefinitelyNotSent))
        return 2;
    if (IsDefinitelyNotSent(KeyEventSendResult::DeliveryAmbiguous))
        return 3;
    // An unknown local result must not accidentally authorize fallback.
    if (IsDefinitelyNotSent(static_cast<KeyEventSendResult>(-1)))
        return 4;
    return 0;
}
