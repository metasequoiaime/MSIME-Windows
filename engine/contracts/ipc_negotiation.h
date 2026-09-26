#pragma once

#include "windows_ipc.h"

// Main ClientHello reuses fields that legacy clients left zero/unused. No
// packet size, existing opcode or reverse-pipe handshake layout changes.
namespace FanyImeProtocol
{
constexpr std::uint32_t Magic = 0x4D534950; // MSIP
constexpr std::uint16_t Major = 1;
constexpr std::uint16_t Minor = 0;
constexpr std::uint32_t RequestIds = 1u << 0;
constexpr std::uint32_t FocusEpochs = 1u << 1;
constexpr std::uint32_t FramedVoice = 1u << 2;
// Optional: Ctrl+Shift+F KeyEvent toggles the configured character set without
// changing composition. A consumer must opt in only after implementing it.
constexpr std::uint32_t CharacterSetShortcut = 1u << 3;
// Optional: the Server answers the Backspace that would empty the remaining
// pinyin of an in-progress word with a CompositionRestored reply instead of
// discarding the whole composition. A client must opt in only after it can
// rebuild its keystroke buffer from that payload.
constexpr std::uint32_t CompositionRestore = 1u << 4;
// Optional: the Server renders IMESwitch / PuncSwitch / DoubleSingleByteSwitch
// as transient caret badges anchored at point[] and understands HideCaretState.
// These events are user-shortcut notifications only; authoritative mode state
// still travels in StatusSnapshot. A client must not send them unless the
// Server acknowledged this bit.
constexpr std::uint32_t CaretStateIndicator = 1u << 5;
constexpr std::uint32_t Capabilities = RequestIds | FocusEpochs | FramedVoice;
constexpr std::uint32_t RequiredCapabilities = RequestIds | FocusEpochs;

struct Negotiation
{
    bool accepted = false;
    bool legacy = false;
    std::uint32_t capabilities = 0;
};

inline FanyImeNamedpipeData Hello(std::uint64_t client, std::uint64_t request,
                                  std::uint32_t capabilities = Capabilities)
{
    FanyImeNamedpipeData hello{};
    hello.event_type = FanyImePipeEventType::ClientHello;
    hello.client_id = client;
    hello.request_id = request;
    hello.keycode = Magic;
    hello.wch = Major;
    hello.point[0] = Minor;
    hello.point[1] = static_cast<int>(RequiredCapabilities);
    hello.modifiers_down = capabilities;
    return hello;
}

inline Negotiation Negotiate(const FanyImeNamedpipeData &hello, std::uint32_t capabilities = Capabilities)
{
    if (hello.event_type != FanyImePipeEventType::ClientHello || hello.client_id == 0)
        return {};
    // Existing installed DLLs use this exact unversioned hello shape. Retain
    // their established protocol during an upgrade without sending a new ACK.
    if (hello.keycode == 0 && hello.wch == 0 && hello.request_id == 0 && hello.modifiers_down == 0)
        return {true, true, capabilities};
    if (hello.keycode != Magic || hello.wch != Major || hello.request_id == 0 ||
        hello.request_id == FANY_IME_NO_REQUEST_ID || hello.point[0] < 0 || hello.point[1] < 0)
        return {};
    const auto required = static_cast<std::uint32_t>(hello.point[1]);
    const auto common = hello.modifiers_down & capabilities;
    if ((required & common) != required || (common & RequiredCapabilities) != RequiredCapabilities)
        return {};
    return {true, false, common};
}

inline FanyImeNamedpipeDataToTsf Reply(const FanyImeNamedpipeData &hello, Negotiation result)
{
    FanyImeNamedpipeDataToTsf reply{};
    reply.msg_type = result.accepted ? FanyImeReplyType::ProtocolReady : FanyImeReplyType::ProtocolMismatch;
    reply.request_id = hello.request_id;
    reply.candidate_string[0] = Major;
    reply.candidate_string[1] = Minor;
    reply.candidate_string[2] = static_cast<FanyImeWireChar>(result.capabilities & 0xFFFFu);
    reply.candidate_string[3] = static_cast<FanyImeWireChar>(result.capabilities >> 16);
    reply.candidate_string[4] = static_cast<FanyImeWireChar>(Magic & 0xFFFFu);
    reply.candidate_string[5] = static_cast<FanyImeWireChar>(Magic >> 16);
    return reply;
}

inline bool IsNegotiationReply(std::uint32_t type)
{
    return type == FanyImeReplyType::ProtocolReady || type == FanyImeReplyType::ProtocolMismatch;
}

inline std::uint32_t ReplyCapabilities(const FanyImeNamedpipeDataToTsf &reply)
{
    return static_cast<std::uint32_t>(reply.candidate_string[2]) |
           (static_cast<std::uint32_t>(reply.candidate_string[3]) << 16);
}

constexpr bool IsCharacterSetShortcut(std::uint32_t keycode, std::uint32_t modifiers)
{
    return keycode == 'F' && (modifiers & 7u) == 3u;
}

inline bool AcceptReply(const FanyImeNamedpipeDataToTsf &reply, std::uint64_t expected_request)
{
    const auto capabilities = ReplyCapabilities(reply);
    const auto magic = static_cast<std::uint32_t>(reply.candidate_string[4]) |
                       (static_cast<std::uint32_t>(reply.candidate_string[5]) << 16);
    return expected_request != 0 && reply.request_id == expected_request &&
           reply.msg_type == FanyImeReplyType::ProtocolReady && magic == Magic && reply.candidate_string[0] == Major &&
           (capabilities & RequiredCapabilities) == RequiredCapabilities;
}
} // namespace FanyImeProtocol
