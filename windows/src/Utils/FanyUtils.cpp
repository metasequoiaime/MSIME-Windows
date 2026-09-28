#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include "Define.h"
#include "Globals.h"
#include "FanyUtils.h"
#include "Ipc.h"
#include <utf8cpp/utf8.h>
#include <fmt/xchar.h>

using namespace std;

namespace FanyUtils
{
namespace
{
std::string TrimAscii(const std::string &value)
{
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r'))
    {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r'))
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string UnquoteTomlBasicString(const std::string &value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool ParseTomlBool(const std::string &raw, bool fallback)
{
    const std::string value = to_lower_copy(UnquoteTomlBasicString(TrimAscii(raw)));
    if (value == "true" || value == "1")
    {
        return true;
    }
    if (value == "false" || value == "0")
    {
        return false;
    }
    return fallback;
}

// Build a wide path and open it as such. A narrow std::string path would be opened through the
// ANSI code page, which cannot round-trip a non-ASCII (e.g. Chinese) user profile path on a
// non-UTF-8 system, so the TSF would read the wrong file or fail to find the config.
// GetEnvironmentVariableW rather than _wgetenv: the CRT variant is deprecated (C4996) because
// it hands out a pointer into a buffer another thread can invalidate, and this DLL runs inside
// arbitrary hosts.
std::wstring EnvironmentValue(const wchar_t *name)
{
    std::wstring value(MAX_PATH, L'\0');
    DWORD length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (length > value.size())
    {
        // The variable is longer than MAX_PATH; length is now the required size including the NUL.
        value.resize(length);
        length = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    }
    if (length == 0 || length > value.size())
    {
        return {};
    }
    value.resize(length);
    return value;
}

// The data directory the user picked during setup. This DLL is also built 32-bit and loads into
// 32-bit hosts, so KEY_WOW64_64KEY is mandatory: without it the read is redirected to
// Wow6432Node, where the 64-bit setup never wrote anything.
std::wstring InstalledDataDir()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Metasequoia\\MetasequoiaIME", 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    {
        return {};
    }

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, L"DataDir", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ ||
        bytes < sizeof(wchar_t))
    {
        RegCloseKey(key);
        return {};
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    const LSTATUS status =
        RegQueryValueExW(key, L"DataDir", nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS)
    {
        return {};
    }
    // REG_SZ carries no guaranteed terminator and the stored length may or may not include one.
    value.resize(std::wcslen(value.c_str()));
    return value;
}

// Same resolution order as the Server (server/src/utils/ime_paths.cpp) and the engine
// (engine/core/data_path.h): environment override, then the location chosen during setup, then
// the default under LocalAppData. All three must agree or the TSF reads a different config than
// the one the Server writes.
std::filesystem::path SharedDataDirectory()
{
    const std::wstring fromEnv = EnvironmentValue(L"METASEQUOIA_IME_DATA_DIR");
    if (!fromEnv.empty() && std::filesystem::path(fromEnv).is_absolute())
    {
        return fromEnv;
    }

    const std::wstring installed = InstalledDataDir();
    if (!installed.empty() && std::filesystem::path(installed).is_absolute())
    {
        return installed;
    }

    const std::wstring localAppData = EnvironmentValue(L"LOCALAPPDATA");
    if (localAppData.empty())
    {
        return {};
    }
    return std::filesystem::path(localAppData) / L"metasequoiaime";
}

std::filesystem::path SharedConfigPath()
{
    const std::filesystem::path directory = SharedDataDirectory();
    if (directory.empty())
    {
        return {};
    }
    return directory / L"config.toml";
}
} // namespace

BOOL ReadConfiguredDefaultImeModeChinese()
{
    const std::filesystem::path configPath = SharedConfigPath();
    if (configPath.empty())
    {
        return TRUE;
    }

    std::ifstream input(configPath);
    if (!input)
    {
        return TRUE;
    }

    bool inInputSection = false;
    std::string line;
    while (std::getline(input, line))
    {
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line = line.substr(0, comment);
        }
        line = TrimAscii(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            inInputSection = (line == "[input]");
            continue;
        }
        if (!inInputSection)
        {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = TrimAscii(line.substr(0, eq));
        if (key != "default_ime_mode")
        {
            continue;
        }
        const std::string value = to_lower_copy(UnquoteTomlBasicString(TrimAscii(line.substr(eq + 1))));
        return value != "english";
    }
    return TRUE;
}

int ReadConfiguredPunctuationLock()
{
    const std::filesystem::path configPath = SharedConfigPath();
    if (configPath.empty())
    {
        return Global::PunctuationLock::Follow;
    }

    std::ifstream input(configPath);
    if (!input)
    {
        return Global::PunctuationLock::Follow;
    }

    bool inInputSection = false;
    std::string line;
    while (std::getline(input, line))
    {
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line = line.substr(0, comment);
        }
        line = TrimAscii(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            inInputSection = (line == "[input]");
            continue;
        }
        if (!inInputSection)
        {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = TrimAscii(line.substr(0, eq));
        if (key != "punctuation_lock")
        {
            continue;
        }
        const std::string value = to_lower_copy(UnquoteTomlBasicString(TrimAscii(line.substr(eq + 1))));
        if (value == "chinese")
        {
            return Global::PunctuationLock::AlwaysChinese;
        }
        if (value == "english")
        {
            return Global::PunctuationLock::AlwaysEnglish;
        }
        return Global::PunctuationLock::Follow;
    }
    return Global::PunctuationLock::Follow;
}

void RefreshPunctuationLockFromConfig()
{
    Global::PunctuationLockMode.store(ReadConfiguredPunctuationLock(), std::memory_order_relaxed);
}

BOOL ReadConfiguredJapaneseInputMode()
{
    const std::filesystem::path configPath = SharedConfigPath();
    if (configPath.empty())
    {
        return FALSE;
    }

    std::ifstream input(configPath);
    if (!input)
    {
        return FALSE;
    }

    bool inInputSection = false;
    std::string line;
    while (std::getline(input, line))
    {
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line = line.substr(0, comment);
        }
        line = TrimAscii(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            inInputSection = (line == "[input]");
            continue;
        }
        if (!inInputSection)
        {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = TrimAscii(line.substr(0, eq));
        if (key != "mode")
        {
            continue;
        }
        const std::string value = to_lower_copy(UnquoteTomlBasicString(TrimAscii(line.substr(eq + 1))));
        return value == "japanese";
    }
    return FALSE;
}

// Reads [input] japanese_punctuation: TRUE (default) means Japanese input uses
// Japanese punctuation (、。「」『』); FALSE uses ASCII.
BOOL ReadConfiguredJapanesePunctuation()
{
    const std::filesystem::path configPath = SharedConfigPath();
    if (configPath.empty())
    {
        return TRUE;
    }

    std::ifstream input(configPath);
    if (!input)
    {
        return TRUE;
    }

    bool inInputSection = false;
    std::string line;
    while (std::getline(input, line))
    {
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line = line.substr(0, comment);
        }
        line = TrimAscii(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            inInputSection = (line == "[input]");
            continue;
        }
        if (!inInputSection)
        {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = TrimAscii(line.substr(0, eq));
        if (key != "japanese_punctuation")
        {
            continue;
        }
        const std::string value = to_lower_copy(TrimAscii(line.substr(eq + 1)));
        return value != "false";
    }
    return TRUE;
}

// Reads [input] japanese_schema: "kana" selects the JIS direct-kana layout,
// anything else (default) is romaji conversion.
BOOL ReadConfiguredJapaneseKanaLayout()
{
    const std::filesystem::path configPath = SharedConfigPath();
    if (configPath.empty())
    {
        return FALSE;
    }

    std::ifstream input(configPath);
    if (!input)
    {
        return FALSE;
    }

    bool inInputSection = false;
    std::string line;
    while (std::getline(input, line))
    {
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line = line.substr(0, comment);
        }
        line = TrimAscii(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            inInputSection = (line == "[input]");
            continue;
        }
        if (!inInputSection)
        {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = TrimAscii(line.substr(0, eq));
        if (key != "japanese_schema")
        {
            continue;
        }
        const std::string value = to_lower_copy(UnquoteTomlBasicString(TrimAscii(line.substr(eq + 1))));
        return value == "kana" ? TRUE : FALSE;
    }
    return FALSE;
}

namespace
{
SwitchLanguageHotkeys ParseSwitchLanguageHotkeys(const std::filesystem::path &configPath)
{
    SwitchLanguageHotkeys result;
    std::ifstream input(configPath);
    if (!input)
    {
        return result;
    }

    bool inKeybindings = false;
    bool sawShift = false;
    bool sawCtrl = false;
    bool sawCtrlAltSpace = false;
    std::string line;
    while (std::getline(input, line))
    {
        const size_t comment = line.find('#');
        if (comment != std::string::npos)
        {
            line = line.substr(0, comment);
        }
        line = TrimAscii(line);
        if (line.empty())
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            inKeybindings = (line == "[keybindings]");
            continue;
        }
        if (!inKeybindings)
        {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = TrimAscii(line.substr(0, eq));
        const std::string raw = line.substr(eq + 1);
        if (key == "switch_language_shift")
        {
            result.shift = ParseTomlBool(raw, true);
            sawShift = true;
        }
        else if (key == "switch_language_ctrl")
        {
            result.ctrl = ParseTomlBool(raw, false);
            sawCtrl = true;
        }
        else if (key == "switch_language_ctrl_alt_space")
        {
            result.ctrl_alt_space = ParseTomlBool(raw, true);
            sawCtrlAltSpace = true;
        }
        else if (key == "toggle_character_set_ctrl_shift_f")
        {
            result.character_set_ctrl_shift_f = ParseTomlBool(raw, true);
        }
        else if (key == "switch_language" && !sawShift && !sawCtrlAltSpace)
        {
            // Legacy array: switch_language = ["Ctrl+Space", "Shift"]
            result.shift = raw.find("Shift") != std::string::npos;
            result.ctrl_alt_space =
                raw.find("Ctrl+Alt+Space") != std::string::npos || raw.find("Ctrl+Space") != std::string::npos;
        }
    }
    (void)sawCtrl;
    return result;
}

// The key sink asks for these hotkeys on every key down and key up, on the host's UI thread.
// Resolving the path (environment + registry) and parsing the whole config.toml each time cost
// up to four file reads per keystroke. The path cannot change for the life of the host process
// (the environment is fixed and DataDir only changes on reinstall), so it is resolved once; the
// file is re-parsed only when its timestamp or size changes, checked at most once per interval
// so a settings change still lands within a second. Everything here is constant-initialized:
// this DLL avoids global constructors and is built with /Zc:threadSafeInit-.
constexpr ULONGLONG kHotkeyConfigRecheckMs = 1000;

struct HotkeyConfigCache
{
    bool pathResolved = false;
    bool haveStamp = false;
    ULONGLONG checkedTick = 0;
    FILETIME lastWrite = {};
    DWORD sizeLow = 0;
    DWORD sizeHigh = 0;
    SwitchLanguageHotkeys value;
};

SRWLOCK g_hotkeyConfigLock = SRWLOCK_INIT;
HotkeyConfigCache g_hotkeyConfig;
// Heap-allocated on first use and intentionally never freed, so it needs no global destructor.
std::filesystem::path *g_hotkeyConfigPath = nullptr;
} // namespace

SwitchLanguageHotkeys ReadConfiguredSwitchLanguageHotkeys()
{
    AcquireSRWLockExclusive(&g_hotkeyConfigLock);
    SwitchLanguageHotkeys result = g_hotkeyConfig.value;
    try
    {
        if (!g_hotkeyConfig.pathResolved)
        {
            g_hotkeyConfig.pathResolved = true;
            const std::filesystem::path configPath = SharedConfigPath();
            if (!configPath.empty())
            {
                g_hotkeyConfigPath = new std::filesystem::path(configPath);
            }
        }

        const ULONGLONG now = GetTickCount64();
        const bool due = !g_hotkeyConfig.haveStamp || now - g_hotkeyConfig.checkedTick >= kHotkeyConfigRecheckMs;
        if (g_hotkeyConfigPath != nullptr && due)
        {
            g_hotkeyConfig.checkedTick = now;
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (GetFileAttributesExW(g_hotkeyConfigPath->c_str(), GetFileExInfoStandard, &attributes))
            {
                const bool changed = !g_hotkeyConfig.haveStamp ||
                                     CompareFileTime(&attributes.ftLastWriteTime, &g_hotkeyConfig.lastWrite) != 0 ||
                                     attributes.nFileSizeLow != g_hotkeyConfig.sizeLow ||
                                     attributes.nFileSizeHigh != g_hotkeyConfig.sizeHigh;
                if (changed)
                {
                    g_hotkeyConfig.value = ParseSwitchLanguageHotkeys(*g_hotkeyConfigPath);
                    g_hotkeyConfig.lastWrite = attributes.ftLastWriteTime;
                    g_hotkeyConfig.sizeLow = attributes.nFileSizeLow;
                    g_hotkeyConfig.sizeHigh = attributes.nFileSizeHigh;
                    g_hotkeyConfig.haveStamp = true;
                }
            }
            else
            {
                // A missing config means defaults, as the uncached read returned; the stamp
                // stays unset so the file is picked up as soon as the Server writes it.
                g_hotkeyConfig.value = SwitchLanguageHotkeys{};
                g_hotkeyConfig.haveStamp = false;
            }
        }
        result = g_hotkeyConfig.value;
    }
    catch (...)
    {
        // Keep the last known hotkeys; a read failure must not take the key path down.
    }
    ReleaseSRWLockExclusive(&g_hotkeyConfigLock);
    return result;
}

void SendKeys(std::wstring pinyin)
{
    for (wchar_t ch : pinyin)
    {
        INPUT in[2]{};

        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;

        in[1] = in[0];
        in[1].ki.dwFlags |= KEYEVENTF_KEYUP;

        UINT sent = SendInput(2, in, sizeof(INPUT));
        if (sent != 2)
        {
        }
    }
}

std::wstring string_to_wstring(const std::string &str)
{
    std::u16string utf16result;
    utf8::utf8to16(str.begin(), str.end(), std::back_inserter(utf16result));
    return std::wstring(utf16result.begin(), utf16result.end());
}

std::string wstring_to_string(const std::wstring &wstr)
{
    std::string result;
    utf8::utf16to8(wstr.begin(), wstr.end(), std::back_inserter(result));
    return result;
}

std::string to_lower_copy(const std::string &str)
{
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::wstring GetCurrentProcessName()
{
    TCHAR fullPath[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, fullPath, MAX_PATH) == 0)
        return L"";

    std::wstring wfullPath(fullPath);
    size_t pos = wfullPath.find_last_of(L"\\/");
    std::wstring wname = (pos != std::wstring::npos) ? wfullPath.substr(pos + 1) : wfullPath;
    return wname;
}

/**
 * @brief Count UTF-8 chars
 *
 * @param str
 * @return string::size_type
 */
string::size_type count_utf8_chars(const string &str)
{
    return utf8::distance(str.begin(), str.end());
}
} // namespace FanyUtils
