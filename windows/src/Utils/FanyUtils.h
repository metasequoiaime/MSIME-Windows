#pragma once
#include <Windows.h>
#include <string>

namespace FanyUtils
{
void SendKeys(std::wstring pinyin);
std::wstring string_to_wstring(const std::string &str);
std::string wstring_to_string(const std::wstring &wstr);
std::string to_lower_copy(const std::string &str);
std::wstring GetCurrentProcessName();
std::string::size_type count_utf8_chars(const std::string &str);
// Read input.default_ime_mode from the shared config.toml in the IME data directory
// (%LOCALAPPDATA%\metasequoiaime by default; the installer may place it on another volume).
// Returns TRUE for Chinese (default), FALSE for English.
BOOL ReadConfiguredDefaultImeModeChinese();
// Read input.mode from the shared config. TRUE when Japanese input is active.
BOOL ReadConfiguredJapaneseInputMode();
// Read input.japanese_punctuation from the shared config. TRUE (default) means
// Japanese input uses Japanese punctuation (、。「」『』); FALSE uses ASCII.
BOOL ReadConfiguredJapanesePunctuation();
// Read input.japanese_schema from the shared config. TRUE when it is "kana"
// (JIS direct-kana layout); FALSE for "romaji" (default).
BOOL ReadConfiguredJapaneseKanaLayout();
// Read input.punctuation_lock from shared config.toml.
// 0 = follow IME, 1 = always Chinese punctuation, 2 = always English punctuation.
int ReadConfiguredPunctuationLock();
// Refresh the in-process lock cache from config.toml.
void RefreshPunctuationLockFromConfig();

struct SwitchLanguageHotkeys
{
    bool shift = true;
    bool ctrl = false;
    bool ctrl_alt_space = true;
    bool character_set_ctrl_shift_f = true;
};
// Read keybindings.switch_language_* from shared config.toml.
SwitchLanguageHotkeys ReadConfiguredSwitchLanguageHotkeys();
} // namespace FanyUtils
