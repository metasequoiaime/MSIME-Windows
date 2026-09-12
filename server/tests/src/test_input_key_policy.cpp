#include "ipc/input_key_policy.h"
#include "tests/includes/test_framework.h"

TEST_CASE(word_to_character_uses_only_the_selected_unmodified_key_pair)
{
    using FanyImeIpc::WordToCharacterDirection;
    REQUIRE_EQ(WordToCharacterDirection(0xDB, '[', 0, true, false), -1);
    REQUIRE_EQ(WordToCharacterDirection(0xDD, ']', 0, true, false), 1);
    REQUIRE_EQ(WordToCharacterDirection(0xBD, '-', 0, true, true), -1);
    REQUIRE_EQ(WordToCharacterDirection(0xBB, '=', 0, true, true), 1);
    REQUIRE_EQ(WordToCharacterDirection(0xDB, '[', 0, true, true), 0);
    REQUIRE_EQ(WordToCharacterDirection(0xBD, '-', 0, true, false), 0);
    REQUIRE_EQ(WordToCharacterDirection(0xBD, '-', 0, false, true), 0);
    REQUIRE_EQ(WordToCharacterDirection(0xDB, '[', 0, false, false), 0);
    // Unicode U+ entry and shifted punctuation must not select a character.
    REQUIRE_EQ(WordToCharacterDirection(0xBB, '+', 1, true, true), 0);
    REQUIRE_EQ(WordToCharacterDirection(0xBD, '_', 1, true, true), 0);
    REQUIRE_EQ(WordToCharacterDirection(0xDB, '{', 1, true, false), 0);
    for (unsigned modifiers = 1; modifiers < 8; ++modifiers)
        REQUIRE_EQ(WordToCharacterDirection(0xBB, '=', modifiers, true, true), 0);
    // Host-drawn candidates carry an unrelated UI-less flag.
    REQUIRE_EQ(WordToCharacterDirection(0xBB, '=', FanyImeIpc::kModifierUiLess, true, true), 1);
    REQUIRE_EQ(WordToCharacterDirection('A', '=', 0, true, true), 0);
}

TEST_CASE(shift_variants_are_backend_independent_composition_reset_keys)
{
    REQUIRE(FanyImeIpc::IsBackendIndependentCompositionResetKey(0x10));
    REQUIRE(FanyImeIpc::IsBackendIndependentCompositionResetKey(0x1B));
    REQUIRE(FanyImeIpc::IsBackendIndependentCompositionResetKey(0xA0));
    REQUIRE(FanyImeIpc::IsBackendIndependentCompositionResetKey(0xA1));

    REQUIRE(!FanyImeIpc::IsBackendIndependentCompositionResetKey(0));
    REQUIRE(!FanyImeIpc::IsBackendIndependentCompositionResetKey('A'));
    REQUIRE(!FanyImeIpc::IsBackendIndependentCompositionResetKey(0x0D));
    REQUIRE(!FanyImeIpc::IsBackendIndependentCompositionResetKey(0x11));
}

TEST_CASE(english_ime_status_requires_backend_independent_composition_reset)
{
    REQUIRE(FanyImeIpc::ShouldResetCompositionForImeMode(false));
    REQUIRE(!FanyImeIpc::ShouldResetCompositionForImeMode(true));
}

TEST_CASE(enter_english_learning_does_not_conflict_with_shift_letter_special_modes)
{
    REQUIRE(FanyImeIpc::ShouldLearnEnteredEnglishWord(false, false, true, false));
    REQUIRE(FanyImeIpc::ShouldLearnEnteredEnglishWord(true, false, true, true));
    // K/U/T/E/M/J/Y modes and the temporary R-mode session all use this path.
    REQUIRE(FanyImeIpc::ShouldLearnEnteredEnglishWord(false, true, true, true));
    REQUIRE(FanyImeIpc::ShouldLearnEnteredEnglishWord(false, true, false, true));
    REQUIRE(!FanyImeIpc::ShouldLearnEnteredEnglishWord(false, false, true, true));
    REQUIRE(!FanyImeIpc::ShouldLearnEnteredEnglishWord(false, false, false, false));
}

TEST_CASE(numpad_digits_are_normalized_to_candidate_digit_keys)
{
    REQUIRE(FanyImeIpc::NormalizeNumpadDigitKey(0x60) == '0');
    REQUIRE(FanyImeIpc::NormalizeNumpadDigitKey(0x61) == '1');
    REQUIRE(FanyImeIpc::NormalizeNumpadDigitKey(0x69) == '9');

    REQUIRE(FanyImeIpc::NormalizeNumpadDigitKey('1') == '1');
    REQUIRE(FanyImeIpc::NormalizeNumpadDigitKey(0x6A) == 0x6A);
}

TEST_CASE(english_mode_toggle_requires_ctrl_shift_e)
{
    REQUIRE(FanyImeIpc::IsEnglishModeToggleKey('E', 0b00000011u));
    REQUIRE(!FanyImeIpc::IsEnglishModeToggleKey('E', 0b00000111u));
    REQUIRE(!FanyImeIpc::IsEnglishModeToggleKey('E', 0b00000110u));
    REQUIRE(!FanyImeIpc::IsEnglishModeToggleKey('E', 0b00000001u));
    REQUIRE(!FanyImeIpc::IsEnglishModeToggleKey('A', 0b00000011u));
}

TEST_CASE(composition_reply_includes_microsoft_shuangpin_ing_key)
{
    REQUIRE(FanyImeIpc::ShouldSendCompositionReply(false, false, true, false, false, false));
    REQUIRE(FanyImeIpc::ShouldSendCompositionReply(true, false, false, false, false, false));
    REQUIRE(FanyImeIpc::ShouldSendCompositionReply(false, false, false, true, false, false));
    REQUIRE(FanyImeIpc::ShouldSendCompositionReply(false, false, false, false, false, true));
    REQUIRE(!FanyImeIpc::ShouldSendCompositionReply(false, false, false, false, false, false));
}

TEST_CASE(temporary_r_mode_japanese_session_is_not_replaced_by_config_sync)
{
    REQUIRE(FanyImeIpc::InputSessionMatchesConfig(false, true, true));
    REQUIRE(FanyImeIpc::InputSessionMatchesConfig(true, false, false));
    REQUIRE(!FanyImeIpc::InputSessionMatchesConfig(false, false, true));
    REQUIRE(!FanyImeIpc::InputSessionMatchesConfig(false, true, false));
}
