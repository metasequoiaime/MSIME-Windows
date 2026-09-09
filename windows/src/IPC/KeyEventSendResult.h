#pragma once

// Local transport outcome, not a serialized IPC field.
enum class KeyEventSendResult
{
    Sent,
    DefinitelyNotSent,
    DeliveryAmbiguous,
};

// A necessary condition for local fallback, not permission to replay a key.
// The caller must still validate focus, composition and ownership.
constexpr bool IsDefinitelyNotSent(KeyEventSendResult result) noexcept
{
    return result == KeyEventSendResult::DefinitelyNotSent;
}
