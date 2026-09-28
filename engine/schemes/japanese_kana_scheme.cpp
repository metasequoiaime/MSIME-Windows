#include "japanese_kana_scheme.h"
#include "../japanese/kana_layout.h"

namespace
{
void AppendUtf8(std::string &out, char32_t cp)
{
    if (cp < 0x80)
        out.push_back(static_cast<char>(cp));
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool IsKanaChar(char32_t cp)
{
    // Hiragana block, the long sound mark ー and the katakana ヴ (う+゛).
    return (cp >= 0x3041 && cp <= 0x3096) || cp == 0x30FC || cp == 0x30F4;
}

std::string FilterKana(const std::string &s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = c;
        int extra = 0;
        if ((c & 0xE0) == 0xC0)
        {
            cp = c & 0x1F;
            extra = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            cp = c & 0x0F;
            extra = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            cp = c & 0x07;
            extra = 3;
        }
        for (int k = 1; k <= extra && i + static_cast<std::size_t>(k) < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        if (IsKanaChar(cp))
        {
            out.append(s, i, static_cast<std::size_t>(extra) + 1);
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    return out;
}
} // namespace

void JapaneseKanaScheme::reset()
{
    raw_input_.clear();
    key_strokes_.clear();
}

void JapaneseKanaScheme::handle_key(ImeKeyCode vk, ImeModifierMask modifiers_down, ImeCharacter wch)
{
    (void)wch;
    if (vk == ImeKey::Backspace)
    {
        japanese::PopLastCodePoint(raw_input_);
        if (!key_strokes_.empty())
            key_strokes_.pop_back();
        return;
    }
    if (vk == ImeKey::Escape || vk == ImeKey::Return)
    {
        reset();
        return;
    }

    const bool bare_shift = (modifiers_down & 0x1u) != 0 && (modifiers_down & 0x6u) == 0;
    const char32_t mapped = japanese::MapJisKanaKey(vk, bare_shift);
    if (mapped == 0)
        return;

    key_strokes_.push_back(KeyStroke{vk, modifiers_down, wch});
    if (mapped == japanese::kDakutenMark || mapped == japanese::kHandakutenMark)
        japanese::ApplyVoicingMark(raw_input_, mapped);
    else
        AppendUtf8(raw_input_, mapped);
}

QueryRequest JapaneseKanaScheme::build_request() const
{
    QueryRequest request;
    request.scheme = type();
    // raw already is hiragana, so cased/raw/normalized/segmentation are identical.
    request.raw_input_with_cases = raw_input_;
    request.raw_input = raw_input_;
    request.normalized_input = raw_input_;
    request.raw_segmentation = raw_input_;
    request.normalized_segmentation = raw_input_;
    request.segmentation = raw_input_;
    request.key_strokes = key_strokes_;
    request.valid = !raw_input_.empty();
    return request;
}

std::string JapaneseKanaScheme::get_preedit() const
{
    return raw_input_;
}

SchemeType JapaneseKanaScheme::type() const
{
    return SchemeType::JapaneseKana;
}

void JapaneseKanaScheme::set_raw_input(const std::string &raw_input, const std::string &raw_input_with_cases)
{
    (void)raw_input;
    raw_input_ = FilterKana(raw_input_with_cases.empty() ? raw_input : raw_input_with_cases);
    key_strokes_.clear();
}
