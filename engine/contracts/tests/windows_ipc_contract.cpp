#include "../ipc_negotiation.h"
#include "../voice_composition_pipe.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>

#define CHECK(expression)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expression))                                                                                             \
        {                                                                                                              \
            std::cerr << #expression << " at " << __LINE__ << '\n';                                                    \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (false)

int main()
{
    // Wire bytes, not just matching C++ declarations: existing v1 DLLs write
    // these offsets on both x86 and x64. Changes must not move UTF-16 data.
    std::array<unsigned char, 304> bytes{};
    bytes[0] = 10; // ClientHello
    bytes[8] = 7;  // client id
    FanyImeNamedpipeData legacy{};
    std::memcpy(&legacy, bytes.data(), bytes.size());
    CHECK(legacy.client_id == 7);
    CHECK(FanyImePipeEventType::HideCaretState == 16);
    CHECK(FanyImePipeEventType::HideCaretState != FanyImePipeEventType::HideCandidateWnd);
    CHECK(!FanyImePipeEventType::IsRouteDeactivation(FanyImePipeEventType::HideCaretState));
    CHECK(FanyImeProtocol::CaretStateIndicator == (1u << 5));
    CHECK((FanyImeProtocol::CaretStateIndicator & FanyImeProtocol::RequiredCapabilities) == 0);
    // CapsLockEdge keeps the VK_CAPITAL value the first Servers matched on.
    CHECK(FanyImeCaretStateTrigger::UserToggle == 0);
    CHECK(FanyImeCaretStateTrigger::CapsLockEdge == 0x14);
    CHECK(FanyImeCaretStateTrigger::FocusEntered != FanyImeCaretStateTrigger::UserToggle);
    CHECK(FanyImeCaretStateTrigger::FocusEntered != FanyImeCaretStateTrigger::CapsLockEdge);
    CHECK(FanyImeProtocol::Negotiate(legacy).legacy);
    CHECK(FanyImeProtocol::Negotiate(legacy).accepted);

    const auto capsOffSnapshot = FanyImePipeFlags::EncodeImeSwitchCapsLockSnapshot(false);
    const auto capsOnSnapshot = FanyImePipeFlags::EncodeImeSwitchCapsLockSnapshot(true);
    CHECK((FanyImePipeFlags::ImeSwitchCapsSnapshotPresent & FanyImePipeFlags::UiLess) == 0);
    CHECK((FanyImePipeFlags::ImeSwitchCapsSnapshotEnabled & FanyImePipeFlags::UiLess) == 0);
    CHECK((FanyImePipeFlags::ImeSwitchCapsSnapshotPresent & 0xffu) == 0);
    CHECK((FanyImePipeFlags::ImeSwitchCapsSnapshotEnabled & 0xffu) == 0);
    CHECK(FanyImePipeFlags::HasImeSwitchCapsLockSnapshot(capsOffSnapshot));
    CHECK(!FanyImePipeFlags::ImeSwitchCapsLockSnapshotEnabled(capsOffSnapshot));
    CHECK(FanyImePipeFlags::HasImeSwitchCapsLockSnapshot(capsOnSnapshot));
    CHECK(FanyImePipeFlags::ImeSwitchCapsLockSnapshotEnabled(capsOnSnapshot));
    CHECK(!FanyImePipeFlags::HasImeSwitchCapsLockSnapshot(0)); // old DLL packet
    CHECK(!FanyImePipeFlags::ImeSwitchCapsLockSnapshotEnabled(FanyImePipeFlags::ImeSwitchCapsSnapshotEnabled));
    CHECK(FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(capsOffSnapshot).has_value());
    CHECK(!*FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(capsOffSnapshot));
    CHECK(*FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(capsOnSnapshot));
    CHECK(!FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(0).has_value());
    CHECK(
        !FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(FanyImePipeFlags::ImeSwitchCapsSnapshotEnabled).has_value());

    auto hello = FanyImeProtocol::Hello(7, 19);
    auto result = FanyImeProtocol::Negotiate(hello);
    CHECK(result.accepted && !result.legacy);
    auto reply = FanyImeProtocol::Reply(hello, result);
    CHECK(FanyImeProtocol::AcceptReply(reply, 19));
    CHECK(!FanyImeProtocol::AcceptReply(reply, 18)); // stale reconnect ACK
    CHECK(!FanyImeProtocol::AcceptReply(reply, 0));

    const auto shortcutCapabilities = FanyImeProtocol::Capabilities | FanyImeProtocol::CharacterSetShortcut;
    const auto shortcutHello = FanyImeProtocol::Hello(7, 20, shortcutCapabilities);
    const auto oldServer = FanyImeProtocol::Negotiate(shortcutHello);
    CHECK(oldServer.accepted);
    CHECK((oldServer.capabilities & FanyImeProtocol::CharacterSetShortcut) == 0);
    const auto newServer = FanyImeProtocol::Negotiate(shortcutHello, shortcutCapabilities);
    CHECK(newServer.accepted);
    CHECK((newServer.capabilities & FanyImeProtocol::CharacterSetShortcut) != 0);
    const auto shortcutReply = FanyImeProtocol::Reply(shortcutHello, newServer);
    CHECK(FanyImeProtocol::AcceptReply(shortcutReply, 20));
    CHECK(FanyImeProtocol::ReplyCapabilities(shortcutReply) == shortcutCapabilities);
    CHECK((FanyImeProtocol::Negotiate(hello, shortcutCapabilities).capabilities &
           FanyImeProtocol::CharacterSetShortcut) == 0); // old client/new server
    CHECK(FanyImeProtocol::IsCharacterSetShortcut('F', 3));
    CHECK(FanyImeProtocol::IsCharacterSetShortcut('F', 0x80000003u));
    for (unsigned modifiers = 0; modifiers < 8; ++modifiers)
        CHECK(FanyImeProtocol::IsCharacterSetShortcut('F', modifiers) == (modifiers == 3));
    CHECK(!FanyImeProtocol::IsCharacterSetShortcut('E', 3));

    // Backspace retraction is negotiated separately: an old Server must keep
    // its current behavior, and an old DLL must never receive CompositionRestored.
    const auto restoreCapabilities = FanyImeProtocol::Capabilities | FanyImeProtocol::CompositionRestore;
    const auto restoreHello = FanyImeProtocol::Hello(7, 21, restoreCapabilities);
    const auto oldServerForRestore = FanyImeProtocol::Negotiate(restoreHello);
    CHECK(oldServerForRestore.accepted);
    CHECK((oldServerForRestore.capabilities & FanyImeProtocol::CompositionRestore) == 0);
    const auto newServerForRestore = FanyImeProtocol::Negotiate(restoreHello, restoreCapabilities);
    CHECK(newServerForRestore.accepted);
    CHECK((newServerForRestore.capabilities & FanyImeProtocol::CompositionRestore) != 0);
    CHECK(FanyImeProtocol::AcceptReply(FanyImeProtocol::Reply(restoreHello, newServerForRestore), 21));
    CHECK(FanyImeProtocol::ReplyCapabilities(FanyImeProtocol::Reply(restoreHello, newServerForRestore)) ==
          restoreCapabilities);
    CHECK((FanyImeProtocol::Negotiate(hello, restoreCapabilities).capabilities & FanyImeProtocol::CompositionRestore) ==
          0); // old client/new server
    CHECK(FanyImeReplyType::CompositionRestored == 14);
    CHECK(FanyImeReplyType::MaxKnown == FanyImeReplyType::CompositionRestored);
    CHECK(FanyImeReplyType::TransportUnavailable > FanyImeReplyType::MaxKnown);

    hello.wch += 1;
    result = FanyImeProtocol::Negotiate(hello);
    CHECK(!result.accepted);
    CHECK(!FanyImeProtocol::AcceptReply(FanyImeProtocol::Reply(hello, result), 19));
    hello = FanyImeProtocol::Hello(7, 19);
    hello.point[1] |= 1u << 20; // client requires an unknown capability
    CHECK(!FanyImeProtocol::Negotiate(hello).accepted);
    hello = FanyImeProtocol::Hello(7, 19);
    hello.modifiers_down |= 1u << 20; // unknown optional capabilities are ignored
    CHECK(FanyImeProtocol::Negotiate(hello).accepted);
    hello.modifiers_down &= ~FanyImeProtocol::FocusEpochs;
    CHECK(!FanyImeProtocol::Negotiate(hello).accepted);
    hello = FanyImeProtocol::Hello(7, 19);
    hello.keycode ^= 1;
    CHECK(!FanyImeProtocol::Negotiate(hello).accepted);
    hello = FanyImeProtocol::Hello(7, 0);
    CHECK(!FanyImeProtocol::Negotiate(hello).accepted);

    FanyImeNamedpipeDataToTsf old_server{};
    old_server.msg_type = FanyImeReplyType::PipeReady;
    old_server.request_id = 19;
    CHECK(!FanyImeProtocol::AcceptReply(old_server, 19));
    // Corrupt/missing negotiated capabilities cannot authorize the new DLL.
    reply.candidate_string[2] = 0;
    CHECK(!FanyImeProtocol::AcceptReply(reply, 19));

    CHECK(FanyImeWorkerReplyType::SwitchToEn == FanyImeWorkerReplyType::SwitchToEnglish);
    CHECK(FanyImeWorkerReplyType::CommitCandidate == FanyImeWorkerReplyType::CommitCurCandidate);
    CHECK(FanyImeWorkerReplyType::CommitCandidateAndContinue == 27);
    CHECK(FanyImeWorkerReplyType::MaxKnown == FanyImeWorkerReplyType::CommitCandidateAndContinue);
    const std::wstring voice(1000, L'x');
    const auto frames = FanyImeVoiceCompositionPipe::EncodeSnapshot(voice, 7);
    CHECK(FanyImeVoiceCompositionPipe::AssembleFrames(frames) == voice);
    auto incomplete = frames;
    incomplete.erase(incomplete.begin());
    CHECK(FanyImeVoiceCompositionPipe::AssembleFrames(incomplete).empty());
    // A middle frame legally carries flags 0, so an unterminated packet must be rejected on the chunk, not accepted
    // because data[0] happens to be a NUL.
    std::array<wchar_t, FanyImeVoiceCompositionPipe::kPacketChars> packet{};
    packet.fill(L'x');
    packet[0] = 0; // middle frame
    packet[1] = 7; // generation
    CHECK(!FanyImeVoiceCompositionPipe::ParseFrame(packet.data()).valid);
    packet[FanyImeVoiceCompositionPipe::kPacketChars - 1] = 0;
    const auto middle = FanyImeVoiceCompositionPipe::ParseFrame(packet.data());
    CHECK(middle.valid && !middle.first && !middle.last && middle.generation == 7);
    CHECK(middle.chunk == std::wstring(FanyImeVoiceCompositionPipe::kMaxChunkChars, L'x'));
    const auto blank = FanyImeVoiceCompositionPipe::EncodeSnapshot(std::wstring(), 7);
    CHECK(blank.size() == 1);
    std::array<wchar_t, FanyImeVoiceCompositionPipe::kPacketChars> header{};
    std::copy(blank[0].begin(), blank[0].end(), header.begin());
    const auto empty = FanyImeVoiceCompositionPipe::ParseFrame(header.data());
    CHECK(empty.valid && empty.first && empty.last && empty.chunk.empty());

    // Statistics frames: fixed 28-byte header plus 24-byte events; the Go stats
    // tool mirrors these numbers in internal/frames, so both sides must change
    // together.
    CHECK(FANY_IME_STATS_MAGIC == 0x54415453);
    CHECK(FANY_IME_STATS_VERSION == 1);
    CHECK(sizeof(FanyImeStatsBatchHeader) == 28);
    CHECK(sizeof(FanyImeStatsEvent) == 24);
    CHECK(offsetof(FanyImeStatsEvent, timestamp_utc_ft) == 0);
    CHECK(offsetof(FanyImeStatsEvent, cjk) == 8);
    CHECK(sizeof(FanyImeStatsBatchHeader) + 256 * sizeof(FanyImeStatsEvent) <= FANY_IME_STATS_MAX_FRAME_BYTES);
    std::cout << "Windows IPC layout, negotiation, upgrade, voice and stats contracts passed\n";
}
