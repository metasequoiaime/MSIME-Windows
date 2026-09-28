#include "japanese_romaji_scheme.h"
#include "../japanese/romaji_converter.h"
#include <algorithm>
#include <cctype>

namespace
{
bool IsRomajiKey(ImeKeyCode vk)
{
    return vk >= 'A' && vk <= 'Z';
}
} // namespace

void JapaneseRomajiScheme::reset()
{
    raw_input_.clear();
    key_strokes_.clear();
}

void JapaneseRomajiScheme::handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    if (vk == ImeKey::Backspace)
    {
        if (!raw_input_.empty())
            raw_input_.pop_back();
        if (!key_strokes_.empty())
            key_strokes_.pop_back();
        return;
    }
    if (vk == ImeKey::Escape || vk == ImeKey::Return)
    {
        reset();
        return;
    }
    if (vk == ImeKey::Apostrophe && wch == u'\'')
    {
        raw_input_.push_back('\'');
        key_strokes_.push_back(KeyStroke{vk, modifiers_down, wch});
        return;
    }
    // '-' 是长音符（ー）的罗马字写法，不是翻页键，罗马字表里已有对应条目。
    if (vk == ImeKey::Minus && wch == u'-')
    {
        raw_input_.push_back('-');
        key_strokes_.push_back(KeyStroke{vk, modifiers_down, wch});
        return;
    }
    if (!IsRomajiKey(vk))
        return;

    key_strokes_.push_back(KeyStroke{vk, modifiers_down, wch});
    if ((wch >= u'a' && wch <= u'z') || (wch >= u'A' && wch <= u'Z'))
        raw_input_.push_back(static_cast<char>(wch));
    else
        raw_input_.push_back(static_cast<char>(vk + ('a' - 'A')));
}

void JapaneseRomajiScheme::set_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    raw_input_ = raw_input_with_cases.empty() ? raw_input : raw_input_with_cases;
    raw_input_.erase(std::remove_if(raw_input_.begin(), raw_input_.end(),
                                    [](unsigned char ch) { return !std::isalpha(ch) && ch != '\'' && ch != '-'; }),
                     raw_input_.end());
    key_strokes_.clear();
}

QueryRequest JapaneseRomajiScheme::build_request() const
{
    QueryRequest request;
    request.scheme = type();
    request.raw_input_with_cases = raw_input_;
    request.raw_input = raw_input_;
    std::transform(request.raw_input.begin(), request.raw_input.end(), request.raw_input.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    request.normalized_input = request.raw_input;
    const auto converted = japanese::ConvertRomaji(request.raw_input);
    request.raw_segmentation = request.raw_input_with_cases;
    request.normalized_segmentation = converted.hiragana + converted.pending;
    request.segmentation = request.normalized_segmentation;
    request.key_strokes = key_strokes_;
    request.valid = !request.raw_input.empty();
    return request;
}

std::string JapaneseRomajiScheme::get_preedit() const
{
    // The inline composition (and, on Enter, the committed text) must show the
    // live kana conversion rather than the typed romaji. Fully converted mora
    // become hiragana; the still-unmatched romaji tail stays as letters, e.g.
    // "kannk" -> "かんk".
    std::string lower = raw_input_;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto converted = japanese::ConvertRomaji(lower);
    return converted.hiragana + converted.pending;
}

SchemeType JapaneseRomajiScheme::type() const
{
    return SchemeType::JapaneseRomaji;
}
