#include "input_session.h"
#include <algorithm>
#include <cctype>

namespace metasequoia
{
std::string InputSession::editing_text() const
{
    if (dedicated_english_mode_)
        return dedicated_english_preedit_;
    if (local_input_mode_ == LocalInputMode::TemporaryJapanese)
        return "R" + engine_.get_request().raw_input_with_cases;
    if (local_input_mode_ != LocalInputMode::None)
        return local_preedit_;
    const auto &value = engine_.get_request();
    return value.raw_input_with_cases.empty() ? value.raw_input : value.raw_input_with_cases;
}

std::size_t InputSession::caret_position() const
{
    const auto size = editing_text().size();
    return std::min(caret_.value_or(size), size);
}

KeyResult InputSession::edit_at_caret(Command command)
{
    auto text = editing_text();
    auto caret = caret_position();
    // Local-mode markers are commands, not editable payload. Backspace at the
    // end of a marker-only composition retains the existing cancel behavior.
    const std::size_t begin = local_input_mode_ == LocalInputMode::None ? 0 : 1;
    switch (command)
    {
    case Command::MoveLeft:
        caret = caret > begin ? caret - 1 : begin;
        break;
    case Command::MoveRight:
        caret = std::min(caret + 1, text.size());
        break;
    case Command::MoveHome:
        caret = begin;
        break;
    case Command::MoveEnd:
        caret = text.size();
        break;
    case Command::Backspace:
        if (caret <= begin)
            return {true, std::nullopt, std::nullopt};
        text.erase(--caret, 1);
        return replace_editing_text(std::move(text), caret);
    case Command::DeleteForward:
        if (caret == text.size())
            return {true, std::nullopt, std::nullopt};
        text.erase(caret, 1);
        return replace_editing_text(std::move(text), caret);
    default:
        return {};
    }
    caret_ = caret;
    return {true, std::nullopt, std::nullopt};
}

KeyResult InputSession::insert_at_caret(char character)
{
    auto text = editing_text();
    const auto caret = caret_position();
    const bool lower = character >= 'a' && character <= 'z';
    const bool upper = character >= 'A' && character <= 'Z';
    bool accepted = lower || upper;
    if (!dedicated_english_mode_)
    {
        switch (local_input_mode_)
        {
        case LocalInputMode::Unicode:
            accepted = (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
                       (character >= 'A' && character <= 'F') ||
                       (character == '+' && caret == 1 && text.find('+') == std::string::npos);
            break;
        case LocalInputMode::QuickPhrase:
            accepted = lower;
            break;
        case LocalInputMode::DateTime:
            accepted = false;
            break;
        case LocalInputMode::Number:
            accepted =
                (character >= '0' && character <= '9') || (character == '.' && text.find('.') == std::string::npos);
            break;
        case LocalInputMode::None:
            accepted = lower || (upper && ((scheme() == SchemeType::Quanpin && quanpin_helpcode_enabled_) ||
                                           (scheme() == SchemeType::Shuangpin && shuangpin_helpcode_enabled_)));
            if (character == ';' && scheme() == SchemeType::Shuangpin && shuangpin_profile_.name == "microsoft")
            {
                const auto separator = caret == 0 ? std::string::npos : text.rfind('\'', caret - 1);
                const auto start = separator == std::string::npos ? 0 : separator + 1;
                accepted = (caret - start) % 2 == 1;
            }
            if (character == '\'' && scheme() != SchemeType::Wubi)
                accepted = caret > 0;
            break;
        case LocalInputMode::Emoji:
        case LocalInputMode::Kaomoji:
        case LocalInputMode::TemporaryJapanese:
            accepted = accepted || character == '\'';
            break;
        default:
            break;
        }
    }
    if (!accepted)
        return {};
    if (character == '\'' && ((caret > 0 && text[caret - 1] == '\'') || text[caret] == '\''))
        return {true, std::nullopt, std::nullopt};
    text.insert(caret, 1, character);
    return replace_editing_text(std::move(text), caret + 1);
}

KeyResult InputSession::replace_editing_text(std::string text, std::size_t caret)
{
    std::optional<std::string> diagnostic;
    if (dedicated_english_mode_)
    {
        dedicated_english_preedit_ = std::move(text);
        update_dedicated_english_candidates();
    }
    else if (local_input_mode_ != LocalInputMode::None && local_input_mode_ != LocalInputMode::TemporaryJapanese)
    {
        local_preedit_ = std::move(text);
        diagnostic = update_local_candidates();
    }
    else
    {
        const auto payload = local_input_mode_ == LocalInputMode::TemporaryJapanese ? text.substr(1) : text;
        auto normalized = payload;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        set_pinyin_sequence(normalized);
        set_pinyin_sequence_with_cases(payload);
        apply_pending_sequence();
        if (local_input_mode_ == LocalInputMode::TemporaryJapanese)
        {
            local_preedit_ = "R" + engine_.get_preedit();
            local_candidates_ = engine_.get_candidates();
        }
    }
    caret_ = caret;
    online_requests_.invalidate();
    discard_abandoned_phrase_progress();
    return {true, std::nullopt, std::move(diagnostic)};
}
} // namespace metasequoia
