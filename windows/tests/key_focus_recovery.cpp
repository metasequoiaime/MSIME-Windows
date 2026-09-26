#include "Key/KeyFocusRecovery.h"

int main()
{
    if (!ShouldRecoverNamedpipeOnKeyFocus(true, false, true))
        return 1;
    if (ShouldRecoverNamedpipeOnKeyFocus(false, false, true))
        return 2;
    if (ShouldRecoverNamedpipeOnKeyFocus(true, true, true))
        return 3;
    if (ShouldRecoverNamedpipeOnKeyFocus(true, false, false))
        return 4;
    return 0;
}
