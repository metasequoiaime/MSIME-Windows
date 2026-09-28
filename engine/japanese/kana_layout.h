#pragma once

#include <cstdint>
#include <string>

namespace japanese
{
// JIS 106/109 "kana input" layout (the same physical-key arrangement used by
// ATOK and MS-IME's かな入力 mode). vk is a Windows virtual-key code; the
// mapping is by physical key position so it is independent of the active OS
// keyboard layout. Returns 0 when the key is not a direct kana key.
//
// shift selects the shifted legend: small kana (ぁぅぇ…), voiced kana
// (だば…), を and the long sound mark ー.
char32_t MapJisKanaKey(std::uint32_t vk, bool shift);

// The dedicated dakuten (゛) / handakuten (゜) keys. They are returned as the
// combining-mark sentinels below rather than a kana, so the caller can apply
// them to the last kana already in the composition.
constexpr char32_t kDakutenMark = U'゛';
constexpr char32_t kHandakutenMark = U'゜';

// Apply a standalone voicing mark to the last kana of a UTF-8 hiragana
// string: か->が, は->ば (dakuten), は->ぱ (handakuten), う->ヴ. Returns true
// when the ending kana accepted the mark.
bool ApplyVoicingMark(std::string &hiraganaUtf8, char32_t mark);

// Remove one trailing Unicode code point (UTF-8 aware) from s.
void PopLastCodePoint(std::string &s);
} // namespace japanese
