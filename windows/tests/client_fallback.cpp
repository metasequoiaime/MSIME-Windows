#include "IPC/Ipc.h"

#include <cassert>

int main()
{
    assert(!IsDefinitelyNotSent(KeyEventSendResult::Sent));
    assert(IsDefinitelyNotSent(KeyEventSendResult::DefinitelyNotSent));
    assert(!IsDefinitelyNotSent(KeyEventSendResult::DeliveryAmbiguous));
    return 0;
}
