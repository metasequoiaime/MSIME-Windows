#pragma once
#include <Windows.h>
#include <string>

namespace FanyUtils
{
std::string GetIMEDataDirPath();
void SendKeys(std::wstring pinyin);
std::wstring string_to_wstring(const std::string &str);
std::string wstring_to_string(const std::wstring &wstr);
std::string to_lower_copy(const std::string &str);
std::wstring GetCurrentProcessName();
std::string::size_type count_utf8_chars(const std::string &str);
// Read input.default_ime_mode from %LOCALAPPDATA%\metasequoiaime\config.toml.
// Returns TRUE for Chinese (default), FALSE for English.
BOOL ReadConfiguredDefaultImeModeChinese();
// Read input.mode from the shared config. TRUE when Japanese input is active.
BOOL ReadConfiguredJapaneseInputMode();
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
