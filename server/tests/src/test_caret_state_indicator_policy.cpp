#include "tests/includes/test_framework.h"
#include "engine/contracts/ipc_negotiation.h"
#include "engine/contracts/windows_ipc.h"
#include "window/caret_state_indicator_policy.h"

TEST_CASE(caret_state_indicator_visibility_is_independent_of_floating_toolbar)
{
    REQUIRE(FanyImeUi::ShouldShowCaretStateIndicator(true, true, false, 50, 100));
    REQUIRE(!FanyImeUi::ShouldShowCaretStateIndicator(false, true, false, 50, 100));
    REQUIRE(!FanyImeUi::ShouldShowCaretStateIndicator(true, false, false, 50, 100));
    REQUIRE(!FanyImeUi::ShouldShowCaretStateIndicator(true, true, false, 0, 0));
    REQUIRE(!FanyImeUi::ShouldShowCaretStateIndicator(true, true, false, 50, -10000));
    REQUIRE(!FanyImeUi::ShouldShowCaretStateIndicator(true, true, false, 50, -100000));
}

TEST_CASE(caret_state_indicator_never_shows_for_uiless_hosts)
{
    // A UILess host (game) draws its own UI; the badge must not create a
    // topmost popup over it even with a valid anchor and the feature on.
    REQUIRE(!FanyImeUi::ShouldShowCaretStateIndicator(true, true, true, 50, 100));
}

TEST_CASE(caret_state_indicator_rejects_unresolved_anchor_sentinel)
{
    // The DLL writes {0, INVALID_Y} when it could not resolve the caret.
    REQUIRE(!FanyImeUi::IsUsableCaretAnchor(0, -100000)); // Global::INVALID_Y
    REQUIRE(!FanyImeUi::IsUsableCaretAnchor(0, 0));
    REQUIRE(FanyImeUi::IsUsableCaretAnchor(0, 1));
    REQUIRE(FanyImeUi::IsUsableCaretAnchor(-1200, 300)); // monitor left of primary
}

TEST_CASE(caret_state_indicator_upper_positions_clear_the_caret_line)
{
    REQUIRE_EQ(FanyImeUi::CaretStateIndicatorY(false, 200, 30, 24, 6), 140);
    REQUIRE_EQ(FanyImeUi::CaretStateIndicatorY(false, 400, 60, 48, 12), 280);
    REQUIRE_EQ(FanyImeUi::CaretStateIndicatorY(true, 200, 30, 24, 6), 206);
    REQUIRE_EQ(FanyImeUi::CaretStateIndicatorY(true, 200, 30, 200, 6), 206);
}

TEST_CASE(caret_state_indicator_flips_away_from_work_area_and_suppresses_when_neither_side_fits)
{
    using FanyImeUi::CaretStateIndicatorPlacementY;
    REQUIRE_EQ(*CaretStateIndicatorPlacementY(true, 170, 30, 24, 6, 0, 200), 110);
    REQUIRE_EQ(*CaretStateIndicatorPlacementY(false, 30, 30, 24, 6, 0, 200), 36);
    REQUIRE(!CaretStateIndicatorPlacementY(true, 50, 80, 24, 6, 0, 100));
    REQUIRE(!CaretStateIndicatorPlacementY(false, 50, 80, 24, 6, 0, 100));
}

TEST_CASE(caret_state_indicator_horizontal_positions_follow_badge_width)
{
    REQUIRE_EQ(FanyImeUi::kCaretStatePunctuationBadgeWidthDip, 94);
    constexpr int punctuationBadgeWidth = FanyImeUi::kCaretStatePunctuationBadgeWidthDip;
    REQUIRE_EQ(FanyImeUi::CaretStateIndicatorX("top-left", 200, punctuationBadgeWidth, 6), 100);
    REQUIRE_EQ(FanyImeUi::CaretStateIndicatorX("top", 200, punctuationBadgeWidth, 6), 153);
    REQUIRE_EQ(FanyImeUi::CaretStateIndicatorX("top-right", 200, punctuationBadgeWidth, 6), 206);
    REQUIRE_EQ(FanyImeUi::CaretStateIndicatorX("bottom", 200, punctuationBadgeWidth, 6), 100);
}

TEST_CASE(caret_state_indicator_badge_width_follows_structure_not_string_length)
{
    using FanyImeUi::CaretStateBadgeWidthDip;
    REQUIRE_EQ(CaretStateBadgeWidthDip({L"中"}), 30);
    REQUIRE_EQ(CaretStateBadgeWidthDip({L"全"}), 30);
    REQUIRE_EQ(CaretStateBadgeWidthDip({L"abc"}), 70);
    REQUIRE_EQ(CaretStateBadgeWidthDip(FanyImeUi::PunctuationBadge(true, true, false)), 94);
    REQUIRE_EQ(CaretStateBadgeWidthDip(FanyImeUi::PunctuationBadge(false, false, false)), 94);
}

TEST_CASE(caret_state_indicator_maps_input_mode_to_one_glyph)
{
    using FanyImeUi::EffectiveInputModeGlyph;
    using FanyImeUi::InputModeGlyph;
    REQUIRE_EQ(InputModeGlyph(true, false), L'中');
    REQUIRE_EQ(InputModeGlyph(false, false), L'英');
    REQUIRE_EQ(InputModeGlyph(true, true), L'日');
    REQUIRE_EQ(EffectiveInputModeGlyph(true, false, true), L'英');
    REQUIRE_EQ(EffectiveInputModeGlyph(true, false, false), L'中');
    REQUIRE_EQ(EffectiveInputModeGlyph(true, true, false), L'日');
}

TEST_CASE(caret_state_indicator_uses_effective_mode_for_caps_and_language_actions)
{
    struct ModeCase
    {
        bool imeEnabled;
        bool japaneseMode;
        wchar_t authoritativeGlyph;
    };
    constexpr ModeCase modes[] = {
        {false, false, L'英'},
        {true, false, L'中'},
        {true, true, L'日'},
    };

    for (const auto &mode : modes)
    {
        for (const bool capsEnabled : {false, true})
        {
            const bool capsEdgeShouldShow = mode.authoritativeGlyph != L'英';
            REQUIRE_EQ(FanyImeUi::ShouldShowInputModeEvent(true, capsEnabled, mode.imeEnabled, mode.japaneseMode),
                       capsEdgeShouldShow);
            const auto badge = FanyImeUi::InputModeBadge(mode.imeEnabled, mode.japaneseMode, capsEnabled);
            REQUIRE(!badge.HasModeSlot());
            REQUIRE(badge.text == std::wstring(1, capsEnabled ? L'英' : mode.authoritativeGlyph));

            REQUIRE_EQ(FanyImeUi::ShouldShowInputModeEvent(false, capsEnabled, mode.imeEnabled, mode.japaneseMode),
                       !capsEnabled);
        }
    }
}

TEST_CASE(caret_state_indicator_uses_the_ime_switch_packet_caps_snapshot)
{
    const auto capsOnPacket = FanyImePipeFlags::EncodeImeSwitchCapsLockSnapshot(true);
    const auto capsOffPacket = FanyImePipeFlags::EncodeImeSwitchCapsLockSnapshot(false);

    // Later global state has already moved to Caps off, but this ordinary
    // language event happened while Caps was on and must remain suppressed.
    const auto ordinaryPacketCapsState = FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(capsOnPacket);
    REQUIRE(ordinaryPacketCapsState.has_value());
    REQUIRE(!FanyImeUi::ShouldShowInputModeEvent(false, *ordinaryPacketCapsState, true, false));

    // Conversely, a queued Caps-off edge restores the authoritative Chinese
    // glyph even if a newer keydown has already turned the global state on.
    const auto capsEdgePacketState = FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(capsOffPacket);
    REQUIRE(capsEdgePacketState.has_value());
    REQUIRE(FanyImeUi::ShouldShowInputModeEvent(true, *capsEdgePacketState, true, false));
    REQUIRE(FanyImeUi::InputModeBadge(true, false, *capsEdgePacketState).text == L"中");

    // A packet without the Present bit is not an authoritative snapshot.
    REQUIRE(!FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(0).has_value());
    REQUIRE(
        !FanyImePipeFlags::DecodeImeSwitchCapsLockSnapshot(FanyImePipeFlags::ImeSwitchCapsSnapshotEnabled).has_value());
    // The snapshot bits never collide with the UILess routing flag.
    REQUIRE_EQ(capsOnPacket & FanyImePipeFlags::UiLess, 0u);
}

TEST_CASE(caret_state_indicator_combines_punctuation_and_input_mode)
{
    using FanyImeUi::PunctuationBadge;
    REQUIRE(PunctuationBadge(true, true, false) == (FanyImeUi::CaretStateBadge{L"，。", L'中'}));
    REQUIRE(PunctuationBadge(false, true, false) == (FanyImeUi::CaretStateBadge{L",.", L'中'}));
    REQUIRE(PunctuationBadge(true, false, false) == (FanyImeUi::CaretStateBadge{L"，。", L'英'}));
    REQUIRE(PunctuationBadge(false, false, false) == (FanyImeUi::CaretStateBadge{L",.", L'英'}));
    REQUIRE(PunctuationBadge(true, true, true) == (FanyImeUi::CaretStateBadge{L"，。", L'日'}));
    REQUIRE(PunctuationBadge(false, true, true) == (FanyImeUi::CaretStateBadge{L",.", L'日'}));
    REQUIRE(PunctuationBadge(true, true, false).HasModeSlot());
}

TEST_CASE(caret_state_indicator_maps_single_state_switches_to_one_glyph)
{
    using FanyImeUi::CaretStateKind;
    using FanyImeUi::SingleStateBadge;
    REQUIRE(SingleStateBadge(CaretStateKind::Width, true).text == L"全");
    REQUIRE(SingleStateBadge(CaretStateKind::Width, false).text == L"半");
    REQUIRE(SingleStateBadge(CaretStateKind::CharacterSet, false).text == L"简");
    REQUIRE(SingleStateBadge(CaretStateKind::CharacterSet, true).text == L"繁");
    REQUIRE(!SingleStateBadge(CaretStateKind::Width, true).HasModeSlot());
}

TEST_CASE(caret_state_indicator_capability_is_optional_and_negotiated)
{
    // A versioned client that does not advertise the bit must not get it, so
    // it never sends badge-only events that an old Server would misapply.
    const auto withoutBadge = FanyImeProtocol::Hello(7, 19, FanyImeProtocol::Capabilities);
    const auto negotiatedWithout =
        FanyImeProtocol::Negotiate(withoutBadge, FanyImeProtocol::Capabilities | FanyImeProtocol::CaretStateIndicator);
    REQUIRE(negotiatedWithout.accepted);
    REQUIRE_EQ(negotiatedWithout.capabilities & FanyImeProtocol::CaretStateIndicator, 0u);

    const auto withBadge =
        FanyImeProtocol::Hello(7, 20, FanyImeProtocol::Capabilities | FanyImeProtocol::CaretStateIndicator);
    REQUIRE((FanyImeProtocol::Negotiate(withBadge).capabilities & FanyImeProtocol::CaretStateIndicator) == 0u);
    REQUIRE((FanyImeProtocol::Negotiate(withBadge, FanyImeProtocol::Capabilities | FanyImeProtocol::CaretStateIndicator)
                 .capabilities &
             FanyImeProtocol::CaretStateIndicator) != 0u);
}
