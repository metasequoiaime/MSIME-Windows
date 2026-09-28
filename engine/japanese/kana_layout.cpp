#include "kana_layout.h"

#include <unordered_map>

namespace japanese
{
namespace
{
// Windows virtual-key codes for the symbol positions (mirrored from
// Windows.h so this file stays layout-only).
constexpr std::uint32_t kOem1 = 0xBA;      // ; :  -> れ
constexpr std::uint32_t kOemMinus = 0xBD;  // - _ -> ほ / ー
constexpr std::uint32_t kOem3 = 0xC0;      // ^ ~ -> へ / べ
constexpr std::uint32_t kOem4 = 0xDB;      // [ { -> ゛ (dakuten)
constexpr std::uint32_t kOem5 = 0xDC;      // \ | -> ろ
constexpr std::uint32_t kOem6 = 0xDD;      // ] } -> ゜ (handakuten)
constexpr std::uint32_t kOem7 = 0xDE;      // ' " -> け / げ
constexpr std::uint32_t kOemComma = 0xBC;  // , < -> ね
constexpr std::uint32_t kOemPeriod = 0xBE; // . > -> る
constexpr std::uint32_t kOem2 = 0xBF;      // / ? -> め

const std::unordered_map<std::uint32_t, char32_t> &UnshiftedTable()
{
    static const std::unordered_map<std::uint32_t, char32_t> table = {
        {'1', U'ぬ'},   {'2', U'ふ'},   {'3', U'あ'},          {'4', U'う'},
        {'5', U'え'},   {'6', U'お'},   {'7', U'や'},          {'8', U'ゆ'},
        {'9', U'よ'},   {'0', U'わ'},   {kOemMinus, U'ほ'},    {kOem3, U'へ'},
        {'Q', U'た'},   {'W', U'て'},   {'E', U'い'},          {'R', U'す'},
        {'T', U'か'},   {'Y', U'ん'},   {'U', U'な'},          {'I', U'に'},
        {'O', U'ら'},   {'P', U'せ'},   {kOem4, kDakutenMark}, {kOem6, kHandakutenMark},
        {'A', U'ち'},   {'S', U'と'},   {'D', U'し'},          {'F', U'は'},
        {'G', U'き'},   {'H', U'く'},   {'J', U'ま'},          {'K', U'の'},
        {'L', U'り'},   {kOem1, U'れ'}, {kOem7, U'け'},        {'Z', U'つ'},
        {'X', U'さ'},   {'C', U'そ'},   {'V', U'ひ'},          {'B', U'こ'},
        {'N', U'み'},   {'M', U'も'},   {kOemComma, U'ね'},    {kOemPeriod, U'る'},
        {kOem2, U'め'}, {kOem5, U'ろ'},
    };
    return table;
}

// Only the shifted legends that differ from the unshifted one. Keys without an
// entry (ん な に ら ま の り れ ね る め ろ …) fall back to the base table.
const std::unordered_map<std::uint32_t, char32_t> &ShiftedTable()
{
    static const std::unordered_map<std::uint32_t, char32_t> table = {
        {'2', U'ぶ'}, {'3', U'ぁ'}, {'4', U'ぅ'}, {'5', U'ぇ'},       {'6', U'ぉ'},   {'7', U'ゃ'},
        {'8', U'ゅ'}, {'9', U'ょ'}, {'0', U'を'}, {kOemMinus, U'ー'}, {kOem3, U'べ'}, {'Q', U'だ'},
        {'W', U'で'}, {'E', U'ぃ'}, {'R', U'ず'}, {'T', U'が'},       {'P', U'ぜ'},   {'A', U'ぢ'},
        {'S', U'ど'}, {'D', U'じ'}, {'F', U'ば'}, {'G', U'ぎ'},       {'H', U'ぐ'},   {kOem7, U'げ'},
        {'Z', U'っ'}, {'X', U'ざ'}, {'C', U'ぞ'}, {'V', U'び'},       {'B', U'ご'},
    };
    return table;
}

std::u32string DecodeUtf8(const std::string &s)
{
    std::u32string out;
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
        for (int k = 1; k <= extra && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += static_cast<std::size_t>(extra) + 1;
    }
    return out;
}

void EncodeUtf8(std::string &out, char32_t cp)
{
    if (cp < 0x80)
    {
        out.push_back(static_cast<char>(cp));
    }
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
} // namespace

char32_t MapJisKanaKey(std::uint32_t vk, bool shift)
{
    if (shift)
    {
        const auto &shifted = ShiftedTable();
        if (auto it = shifted.find(vk); it != shifted.end())
            return it->second;
    }
    const auto &base = UnshiftedTable();
    if (auto it = base.find(vk); it != base.end())
        return it->second;
    return 0;
}

bool ApplyVoicingMark(std::string &hiraganaUtf8, char32_t mark)
{
    static const std::unordered_map<char32_t, char32_t> dakuten = {
        {U'か', U'が'}, {U'き', U'ぎ'}, {U'く', U'ぐ'}, {U'け', U'げ'}, {U'こ', U'ご'}, {U'さ', U'ざ'}, {U'し', U'じ'},
        {U'す', U'ず'}, {U'せ', U'ぜ'}, {U'そ', U'ぞ'}, {U'た', U'だ'}, {U'ち', U'ぢ'}, {U'つ', U'づ'}, {U'て', U'で'},
        {U'と', U'ど'}, {U'は', U'ば'}, {U'ひ', U'び'}, {U'ふ', U'ぶ'}, {U'へ', U'べ'}, {U'ほ', U'ぼ'}, {U'う', U'ヴ'},
    };
    static const std::unordered_map<char32_t, char32_t> handakuten = {
        {U'は', U'ぱ'}, {U'ひ', U'ぴ'}, {U'ふ', U'ぷ'}, {U'へ', U'ぺ'}, {U'ほ', U'ぽ'},
    };
    auto cps = DecodeUtf8(hiraganaUtf8);
    if (cps.empty())
        return false;
    const auto &table = (mark == kHandakutenMark) ? handakuten : dakuten;
    auto it = table.find(cps.back());
    if (it == table.end())
        return false;
    cps.back() = it->second;
    hiraganaUtf8.clear();
    for (char32_t cp : cps)
        EncodeUtf8(hiraganaUtf8, cp);
    return true;
}

void PopLastCodePoint(std::string &s)
{
    while (!s.empty())
    {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) // continuation bytes follow the lead byte
            break;
    }
}
} // namespace japanese
