#include "tests/includes/test_framework.h"

#include "config/ime_config.h"
#include "session/session_factory.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

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

std::filesystem::path MakeProfileRoot()
{
    return std::filesystem::temp_directory_path() / (L"msime-配置测试-" + std::to_wstring(GetCurrentProcessId()));
}

void SeedTemplate(const std::filesystem::path &data_dir)
{
    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    REQUIRE(!ec);
    std::filesystem::copy_file(MSIME_DEFAULT_CONFIG_PATH, data_dir / L"config.default.toml",
                               std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE(!ec);
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
} // namespace

TEST_CASE(shuangpin_config_round_trip_drives_the_product_session_factory)
{
    namespace fs = std::filesystem;
    const fs::path unique_root = MakeProfileRoot() / L"shuangpin";
    const fs::path local_app_data = unique_root / L"本地";
    const fs::path data_dir = local_app_data / L"metasequoiaime";
    SeedTemplate(data_dir);

    {
        ScopedEnv local_app_data_env(L"LOCALAPPDATA", local_app_data.wstring());
        InitImeConfig();
        REQUIRE_EQ(GetConfiguredShuangpinSchema(), std::string("xiaohe"));
        REQUIRE(SetConfiguredInputMode("chinese"));
        REQUIRE(SetConfiguredInputScheme("shuangpin"));
        REQUIRE(SetConfiguredShuangpinHelpcodeEnabled(false));

        const auto input = [](IInputSession &session, const std::string &keys) {
            for (const char ch : keys)
            {
                const UINT vk = ch == ';' ? VK_OEM_1 : static_cast<UINT>(ch - ('a' - 'A'));
                session.handle_key(vk, 0, static_cast<WCHAR>(ch));
            }
        };

        auto previous_session = CreateInputSessionFromConfig();
        input(*previous_session, "nihc");
        REQUIRE_EQ(previous_session->get_quanpin(), std::string("nihao"));

        REQUIRE(SetConfiguredShuangpinSchema("jiajia"));
        const auto saved = ReadText(data_dir / L"config.toml");
        REQUIRE(saved.find("shuangpin_schema = \"jiajia\"") != std::string::npos);
        REQUIRE(!SetConfiguredShuangpinSchema("unsupported"));
        REQUIRE_EQ(GetConfiguredShuangpinSchema(), std::string("jiajia"));
        REQUIRE_EQ(ReadText(data_dir / L"config.toml"), saved);

        // This is the same factory that the ReloadInputSession task queued by
        // ApplyConfiguredShuangpinSchema uses. Exercise real decoding/candidates,
        // while HWND notifications and queue dispatch remain host integration tests.
        auto session = CreateInputSessionFromConfig();
        REQUIRE_EQ(session->current_scheme_type(), SchemeType::Shuangpin);
        REQUIRE(session->get_pinyin_sequence().empty());
        input(*session, "nihd");
        REQUIRE_EQ(session->get_quanpin(), std::string("nihao"));
        session->recompute_candidates();
        const auto &candidates = session->get_candidates();
        REQUIRE(std::any_of(candidates.begin(), candidates.end(),
                            [](const auto &candidate) { return candidate.word == "你好"; }));

        // Reload from disk after changing the in-memory choice via a real setter.
        REQUIRE(SetConfiguredShuangpinSchema("microsoft"));
        WriteText(data_dir / L"config.toml", saved);
        InitImeConfig();
        REQUIRE_EQ(GetConfiguredShuangpinSchema(), std::string("jiajia"));
        session = CreateInputSessionFromConfig();
        input(*session, "nihd");
        REQUIRE_EQ(session->get_quanpin(), std::string("nihao"));

        // Existing persisted IDs must still construct their original profiles;
        // switching away from Jiajia must also restore Microsoft's semicolon key.
        const std::vector<std::pair<std::string, std::string>> cases{
            {"xiaohe", "xl"},
            {"ziranma", "xd"},
            {"shoudao", "xx"},
            {"microsoft", "m;"},
        };
        for (const auto &[schema, keys] : cases)
        {
            REQUIRE(SetConfiguredShuangpinSchema(schema));
            InitImeConfig();
            REQUIRE_EQ(GetConfiguredShuangpinSchema(), schema);
            session = CreateInputSessionFromConfig();
            input(*session, keys);
            REQUIRE_EQ(session->get_quanpin(), schema == "microsoft" ? std::string("ming") : std::string("xiang"));
        }
        REQUIRE(SetConfiguredShuangpinSchema("xiaohe"));
    }

    std::error_code ec;
    fs::remove_all(unique_root, ec);
}

TEST_CASE(config_round_trips_under_non_ascii_profile_path)
{
    namespace fs = std::filesystem;
    const fs::path unique_root = MakeProfileRoot();
    const fs::path local_app_data = unique_root / L"本地";
    const fs::path data_dir = local_app_data / L"metasequoiaime";

    std::error_code ec;
    fs::remove_all(unique_root, ec);
    SeedTemplate(data_dir);

    {
        ScopedEnv local_app_data_env(L"LOCALAPPDATA", local_app_data.wstring());

        InitImeConfig();
        REQUIRE(fs::exists(data_dir / L"config.toml"));

        REQUIRE(SetConfiguredInputMode("japanese"));
        REQUIRE(SetConfiguredInputScheme("wubi"));

        InitImeConfig();
        REQUIRE_EQ(GetConfiguredInputMode(), std::string("japanese"));
        REQUIRE_EQ(GetConfiguredInputSchemeName(), std::string("wubi"));

        const std::vector<std::string> fonts = {"SimSun", "Font#1", "Font]2", "Font\\\"3", "微软雅黑"};
        REQUIRE(SetConfiguredCandidateFallbackFonts(fonts));
        InitImeConfig();
        REQUIRE(GetConfiguredCandidateFallbackFonts() == fonts);
        auto reordered = fonts;
        std::reverse(reordered.begin(), reordered.end());
        REQUIRE(SetConfiguredCandidateFallbackFonts(reordered));
        InitImeConfig();
        REQUIRE(GetConfiguredCandidateFallbackFonts() == reordered);
        REQUIRE(!SetConfiguredCandidateFallbackFonts({"bad\nfont"}));
        REQUIRE(GetConfiguredCandidateFallbackFonts() == reordered);
        REQUIRE(SetConfiguredCandidateFallbackFonts({}));
        InitImeConfig();
        REQUIRE(GetConfiguredCandidateFallbackFonts().empty());
    }

    fs::remove_all(unique_root, ec);
}

TEST_CASE(config_recovers_unparseable_file_and_saves)
{
    namespace fs = std::filesystem;
    const fs::path unique_root = MakeProfileRoot() / L"损坏";
    const fs::path local_app_data = unique_root / L"本地";
    const fs::path data_dir = local_app_data / L"metasequoiaime";

    std::error_code ec;
    fs::remove_all(unique_root, ec);
    SeedTemplate(data_dir);
    WriteText(data_dir / L"config.toml", "this is not toml {{{");
    WriteText(data_dir / L"config.base.toml", ReadText(data_dir / L"config.default.toml"));

    {
        ScopedEnv local_app_data_env(L"LOCALAPPDATA", local_app_data.wstring());
        InitImeConfig();
        REQUIRE(SetConfiguredInputMode("japanese"));
        InitImeConfig();
        REQUIRE_EQ(GetConfiguredInputMode(), std::string("japanese"));
    }

    fs::remove_all(unique_root, ec);
}

TEST_CASE(config_overwrites_readonly_file)
{
    namespace fs = std::filesystem;
    const fs::path unique_root = MakeProfileRoot() / L"只读";
    const fs::path local_app_data = unique_root / L"本地";
    const fs::path data_dir = local_app_data / L"metasequoiaime";

    std::error_code ec;
    fs::remove_all(unique_root, ec);
    SeedTemplate(data_dir);

    {
        ScopedEnv local_app_data_env(L"LOCALAPPDATA", local_app_data.wstring());
        InitImeConfig();
        const fs::path config_path = data_dir / L"config.toml";
        REQUIRE(SetFileAttributesW(config_path.c_str(), FILE_ATTRIBUTE_READONLY));
        REQUIRE(SetConfiguredInputMode("japanese"));
        InitImeConfig();
        REQUIRE_EQ(GetConfiguredInputMode(), std::string("japanese"));
    }

    fs::remove_all(unique_root, ec);
}

TEST_CASE(config_migrates_legacy_acp_mangled_path)
{
    namespace fs = std::filesystem;
    const fs::path unique_root = MakeProfileRoot() / L"遗留";
    const fs::path local_app_data = unique_root / L"本地";
    const fs::path data_dir = local_app_data / L"metasequoiaime";

    std::error_code ec;
    fs::remove_all(unique_root, ec);
    SeedTemplate(data_dir);

    fs::path mangled_dir;
    try
    {
        mangled_dir = fs::path(data_dir.u8string());
    }
    catch (...)
    {
        mangled_dir.clear();
    }
    if (mangled_dir.empty() || mangled_dir == data_dir)
    {
        fs::remove_all(unique_root, ec);
        return;
    }

    fs::create_directories(mangled_dir, ec);
    REQUIRE(!ec);
    const std::string stock = ReadText(data_dir / L"config.default.toml");
    const std::string from = "mode = \"chinese\"";
    const auto pos = stock.find(from);
    REQUIRE(pos != std::string::npos);
    WriteText(data_dir / L"config.toml", stock + "\n# leftover-installer-marker\n");
    std::string leftover = stock;
    leftover.replace(pos, from.size(), "mode = \"japanese\"");
    WriteText(mangled_dir / L"config.toml", leftover);

    {
        ScopedEnv local_app_data_env(L"LOCALAPPDATA", local_app_data.wstring());
        InitImeConfig();
        REQUIRE_EQ(GetConfiguredInputMode(), std::string("japanese"));
        REQUIRE(SetConfiguredInputScheme("wubi"));
        InitImeConfig();
        REQUIRE_EQ(GetConfiguredInputMode(), std::string("japanese"));
        REQUIRE_EQ(GetConfiguredInputSchemeName(), std::string("wubi"));
    }

    fs::remove_all(unique_root, ec);
}

// 全拼纠错的两个开关必须能在全新安装上落盘（出厂模板没有 [quanpin] 段，首次写入要能创建它），
// 重启（重新 InitImeConfig）后保持；旧的单一 autocorrect 键已废弃，即使配置文件里还留着它、
// 甚至只有它，纠错也必须保持默认关闭（R2：废弃不迁移）。
TEST_CASE(quanpin_autocorrect_keys_persist_and_legacy_key_stays_ignored)
{
    namespace fs = std::filesystem;
    const fs::path unique_root =
        fs::temp_directory_path() / (L"msime-纠错配置测试-" + std::to_wstring(GetCurrentProcessId()));
    const fs::path local_app_data = unique_root / L"profile";
    const fs::path data_dir = local_app_data / L"metasequoiaime";

    std::error_code ec;
    fs::remove_all(unique_root, ec);
    fs::create_directories(data_dir, ec);
    REQUIRE(!ec);
    // 出厂模板没有 [quanpin] 段：这正是全新安装后第一次开开关的真实起点。
    fs::copy_file(MSIME_DEFAULT_CONFIG_PATH, data_dir / L"config.default.toml", fs::copy_options::overwrite_existing,
                  ec);
    REQUIRE(!ec);

    {
        ScopedEnv local_app_data_env(L"LOCALAPPDATA", local_app_data.wstring());

        InitImeConfig();
        REQUIRE(fs::exists(data_dir / L"config.toml"));

        // 缺失段上的首次写入不能失败；重读磁盘后两个开关独立保持。
        REQUIRE(SetConfiguredQuanpinAutocorrectTransposition(true));
        REQUIRE(SetConfiguredQuanpinAutocorrectNeighbor(false));
        InitImeConfig();
        REQUIRE(GetConfiguredQuanpinAutocorrectTransposition());
        REQUIRE(!GetConfiguredQuanpinAutocorrectNeighbor());

        // 旧键（哪怕显式 true）不得再影响纠错状态：清掉新键、只留旧键后重读，两者都必须默认关。
        auto config_text = std::string("[quanpin]\nautocorrect = true\n");
        {
            std::ofstream config(data_dir / L"config.toml", std::ios::binary | std::ios::trunc);
            REQUIRE(static_cast<bool>(config));
            config.write(config_text.data(), static_cast<std::streamsize>(config_text.size()));
        }
        InitImeConfig();
        REQUIRE(!GetConfiguredQuanpinAutocorrectTransposition());
        REQUIRE(!GetConfiguredQuanpinAutocorrectNeighbor());
    }

    fs::remove_all(unique_root, ec);
}
