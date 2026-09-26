#pragma once

inline bool ShouldRecoverNamedpipeOnKeyFocus(bool foreground, bool connected, bool focusStateOwner)
{
    return foreground && !connected && focusStateOwner;
}
