#pragma once

enum class SchemeType
{
    Quanpin,
    Shuangpin,
    Wubi,
    JapaneseRomaji,
    JapaneseKana,
};

// True for every Japanese input method (romaji conversion and direct JIS kana).
inline bool IsJapaneseScheme(SchemeType scheme)
{
    return scheme == SchemeType::JapaneseRomaji || scheme == SchemeType::JapaneseKana;
}
