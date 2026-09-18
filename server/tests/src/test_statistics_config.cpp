#include "tests/includes/test_framework.h"

#include "config/ime_config.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace
{
class ScopedEnv
{
  public:
    ScopedEnv(const wchar_t *name, const std::wstring &value) : name_(name)
    {
        wchar_t buffer[32768];
        const DWORD length = GetEnvironmentVariableW(name, buffer, 32768);
        had_previous_ = length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        previous_.assign(buffer, length);
        SetEnvironmentVariableW(name, value.c_str());
    }
    ~ScopedEnv()
    {
        SetEnvironmentVariableW(name_.c_str(), had_previous_ ? previous_.c_str() : nullptr);
    }

    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

  private:
    std::wstring name_;
    std::wstring previous_;
    bool had_previous_ = false;
};

// LOCALAPPDATA alone does not decide where config.toml lives: an installed product records its
// data directory in HKLM, which outranks the profile. Pin the config directory explicitly so these
// cases stay isolated on a machine that has the IME installed.
class ScopedConfigLocation
{
  public:
    explicit ScopedConfigLocation(const std::filesystem::path &config_dir)
        : local_app_data_(L"LOCALAPPDATA", config_dir.parent_path().wstring()),
          config_dir_(L"METASEQUOIA_IME_CONFIG_DIR", config_dir.wstring()),
          data_dir_(L"METASEQUOIA_IME_DATA_DIR", config_dir.wstring())
    {
    }

  private:
    ScopedEnv local_app_data_;
    ScopedEnv config_dir_;
    ScopedEnv data_dir_;
};

std::filesystem::path MakeProfileRoot(const wchar_t *name)
{
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (std::wstring(name) + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    REQUIRE(!ec);
    return root;
}

void WriteText(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(output));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string ReadText(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void SeedShippedTemplate(const std::filesystem::path &config_dir)
{
    std::error_code ec;
    std::filesystem::copy_file(MSIME_DEFAULT_CONFIG_PATH, config_dir / L"config.default.toml",
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE(!ec);
}
} // namespace

TEST_CASE(statistics_config_defaults_on_for_a_fresh_install)
{
    const std::filesystem::path root = MakeProfileRoot(L"msime-stats-config-全新");
    const std::filesystem::path config_dir = root / L"metasequoiaime";
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    SeedShippedTemplate(config_dir);

    // The key must be in the file the installer ships: a template without it is dropped on the
    // next upgrade merge, and the user's choice would silently revert to the code default.
    const std::string template_text = ReadText(config_dir / L"config.default.toml");
    REQUIRE(template_text.find("[statistics]") != std::string::npos);

    {
        const ScopedConfigLocation location(config_dir);
        InitImeConfig();
        REQUIRE(GetConfiguredStatisticsEnabled());
    }

    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_config_key_missing_reads_the_code_default)
{
    const std::filesystem::path root = MakeProfileRoot(L"msime-stats-config-缺键");
    const std::filesystem::path config_dir = root / L"metasequoiaime";
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    // A template without the section cannot re-add the key during the upgrade merge, so this is
    // the read-side default and nothing else.
    WriteText(config_dir / L"config.default.toml", "[general]\nfloating_toolbar = true\n");
    WriteText(config_dir / L"config.toml", "[general]\nfloating_toolbar = false\n");

    {
        const ScopedConfigLocation location(config_dir);
        InitImeConfig();
        REQUIRE(GetConfiguredStatisticsEnabled());
    }

    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_config_round_trips_through_config_toml)
{
    const std::filesystem::path root = MakeProfileRoot(L"msime-stats-config-往返");
    const std::filesystem::path config_dir = root / L"metasequoiaime";
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    SeedShippedTemplate(config_dir);

    {
        const ScopedConfigLocation location(config_dir);
        InitImeConfig();
        REQUIRE(GetConfiguredStatisticsEnabled());

        REQUIRE(SetConfiguredStatisticsEnabled(false));
        REQUIRE(!GetConfiguredStatisticsEnabled());
        REQUIRE(ReadText(config_dir / L"config.toml").find("enabled = false") != std::string::npos);

        InitImeConfig();
        REQUIRE(!GetConfiguredStatisticsEnabled());

        REQUIRE(SetConfiguredStatisticsEnabled(true));
        InitImeConfig();
        REQUIRE(GetConfiguredStatisticsEnabled());
    }

    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_config_creates_the_section_when_the_user_file_lacks_it)
{
    const std::filesystem::path root = MakeProfileRoot(L"msime-stats-config-补段");
    const std::filesystem::path config_dir = root / L"metasequoiaime";
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    SeedShippedTemplate(config_dir);
    // An older profile: no [statistics] section in the user's copy.
    WriteText(config_dir / L"config.toml", "[general]\nfloating_toolbar = true\n");
    WriteText(config_dir / L"config.base.toml", ReadText(config_dir / L"config.default.toml"));

    {
        const ScopedConfigLocation location(config_dir);
        InitImeConfig();
        REQUIRE(SetConfiguredStatisticsEnabled(false));
        const std::string text = ReadText(config_dir / L"config.toml");
        REQUIRE(text.find("[statistics]") != std::string::npos);
        REQUIRE(text.find("enabled = false") != std::string::npos);

        InitImeConfig();
        REQUIRE(!GetConfiguredStatisticsEnabled());
    }

    std::filesystem::remove_all(root, ec);
}

TEST_CASE(statistics_config_write_failure_keeps_the_cached_value)
{
    const std::filesystem::path root = MakeProfileRoot(L"msime-stats-config-写失败");
    const std::filesystem::path config_dir = root / L"metasequoiaime";
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    SeedShippedTemplate(config_dir);
    // The config directory path is a regular file, so no config write can succeed.
    const std::filesystem::path blocker = root / L"not-a-directory";
    WriteText(blocker, "x");

    {
        const ScopedConfigLocation location(config_dir);
        InitImeConfig();
        // A known-good cached state first: a failed load leaves the globals untouched, so the test
        // must not depend on whatever the previous case left behind.
        REQUIRE(GetConfiguredStatisticsEnabled());

        const ScopedConfigLocation blocked(blocker);
        InitImeConfig();
        REQUIRE(!SetConfiguredStatisticsEnabled(false));
        // The in-memory switch must not flip when the file was not written: the next reload would
        // otherwise silently revert while the DLL was already told to stop collecting.
        REQUIRE(GetConfiguredStatisticsEnabled());
    }

    std::filesystem::remove_all(root, ec);
}
