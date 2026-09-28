#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace japanese
{
// One built-in internet-slang abbreviation entry.
struct SlangEntry
{
    std::string code;    // typed romaji abbreviation, lowercase (e.g. "ktkr")
    std::string surface; // candidate text shown and committed (e.g. "キタコレ")
    std::string gloss;   // full form / meaning previewed next to the candidate
};

// One katakana loanword entry: the original foreign word plus a short language
// tag displayed with it, e.g. コーヒー -> "koffie（荷）", アルバイト -> "Arbeit（德）".
struct LoanEntry
{
    std::string surface;  // katakana surface as it appears in candidates
    std::string original; // original word in the source language
    std::string lang;     // short source-language tag: 英/德/法/荷/葡/西/意/俄
};

// Built-in internet slang abbreviations (w, ktkr, ggrks, ...). The Japanese
// candidate provider injects an entry as an extra candidate when the raw
// romaji input equals the abbreviation exactly.
const std::vector<SlangEntry> &InternetSlangEntries();

// Built-in katakana loanword etymology table.
const std::vector<LoanEntry> &LoanwordEntries();

// Preview gloss for a Japanese candidate surface:
//   * katakana loanwords -> the original foreign word with a language tag
//                            (コーヒー -> "koffie（荷）")
//   * slang surfaces     -> their full form (キタコレ -> "来たこれ")
// Returns an empty string when nothing is known. Lookup falls back to a
// long-vowel-normalized key so コンピュータ / コンピューター share one entry.
std::string LookUpCandidateGloss(std::string_view surface);
} // namespace japanese
