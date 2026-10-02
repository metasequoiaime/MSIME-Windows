#include "tests/includes/test_framework.h"

#include "skin/candidate_skin_catalog.h"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{
std::filesystem::path TestRoot(const std::wstring &leaf)
{
    return std::filesystem::temp_directory_path() /
           (L"msime-skin-global-theme-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
}

// Writes `manifest` as <root>/<id>/skin.toml and every name in `files` beside it, each holding one byte.
std::filesystem::path WriteSkin(const std::wstring &leaf, const std::string &id, const std::string &manifest,
                                const std::vector<std::string> &files = {})
{
    namespace fs = std::filesystem;
    const fs::path root = TestRoot(leaf);
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path directory = root / fs::u8path(id);
    fs::create_directories(directory, ec);
    REQUIRE(!ec);
    for (const auto &name : files)
    {
        const fs::path file = directory / fs::u8path(name);
        fs::create_directories(file.parent_path(), ec);
        REQUIRE(!ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        REQUIRE(out.is_open());
        out << 'x';
    }
    std::ofstream out(directory / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << manifest;
    return root;
}

void RemoveSkin(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::remove_all(root.parent_path(), ec);
}

std::string Manifest(const std::string &base, const std::string &themes, const std::string &tables = {})
{
    return "schema_version = 1\nid = \"community\"\nname = \"Community\"\nversion = \"1.0\"\nbase = \"" + base +
           "\"\n[supports]\nlayouts = [\"horizontal\", \"vertical\"]\nthemes = " + themes + "\n[candidate_window]\n" +
           tables;
}

std::optional<CandidateSkinCatalog::Package> LoadManifest(const std::wstring &leaf, const std::string &manifest)
{
    const auto root = WriteSkin(leaf, "community", manifest);
    std::string error;
    auto package = CandidateSkinCatalog::Load(root, "community", &error);
    RemoveSkin(root);
    REQUIRE(package.has_value() == error.empty());
    return package;
}
} // namespace

// `system` is the cross-platform client's name for the host's native look, which on Windows is fluent; the
// package's own colours are left exactly as written.
TEST_CASE(candidate_skin_catalog_reads_system_base_as_fluent)
{
    const auto package = LoadManifest(L"system", Manifest("system", "[\"dark\", \"light\"]", R"(
[candidate.dark]
accent = "#e08aa8"
)"));
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->base, std::string("fluent"));
    REQUIRE_EQ(package->themes, (std::vector<std::string>{"dark", "light"}));
    REQUIRE_EQ(package->dark.accent, std::string("#e08aa8"));
    REQUIRE(package->dark.surface.empty());
    REQUIRE(package->light.accent.empty());
}

// A built-in global theme fixes the mode, so the client draws only the package palette for the theme's own
// mode. Windows draws that palette in both of its modes, with every slot the client derives filled in from
// the theme (theme.rs BUILTIN_THEMES and resolve()), so the candidate window, its menu and the toolbar look
// the same as on the other platforms whatever mode Windows is in.
TEST_CASE(candidate_skin_catalog_fills_a_global_theme_base_from_its_palette)
{
    // The rgba() value ends in `)"`, so the raw string needs its own delimiter.
    const auto package = LoadManifest(L"night", Manifest("night", "[\"dark\"]", R"toml(
[candidate.dark]
accent = "rgba(224, 138, 168, 0.5)"
text = "not a colour"
translation = "#abc"
[candidate.light]
accent = "#000000"
[toolbar.dark]
icon = "#e3eaff"
)toml"));
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->base, std::string("fluent"));
    REQUIRE_EQ(package->themes, (std::vector<std::string>{"dark", "light"}));
    const auto &colors = package->dark;
    // Package colours are normalized the way the client reads them; one it cannot read counts as unset.
    REQUIRE_EQ(colors.accent, std::string("#E08AA880"));
    REQUIRE_EQ(colors.text, std::string("#E6F1F4"));
    REQUIRE_EQ(colors.translation, std::string("#AABBCC"));
    REQUIRE_EQ(colors.surface, std::string("#16262F"));
    REQUIRE_EQ(colors.border, std::string("#0000001F"));
    REQUIRE_EQ(colors.number, std::string("#86A6B0"));
    // Derived slots follow the package's accent and the theme's text, alpha replaced.
    REQUIRE_EQ(colors.selected, std::string("#E08AA824"));
    REQUIRE_EQ(colors.hover, std::string("#E6F1F40F"));
    REQUIRE_EQ(colors.selectedText, std::string("#E08AA880"));
    REQUIRE_EQ(colors.selectedNumber, std::string("#86A6B0"));
    REQUIRE_EQ(colors.menuBackground, std::string("#16262F"));
    REQUIRE_EQ(colors.menuText, std::string("#E6F1F4"));
    // The light palette is the dark one: the client never draws night in light mode.
    REQUIRE_EQ(package->light.accent, colors.accent);
    REQUIRE_EQ(package->light.surface, colors.surface);
    REQUIRE_EQ(package->toolbarDark.icon, std::string("#E3EAFF"));
    REQUIRE_EQ(package->toolbarDark.background, std::string("#16262F"));
    REQUIRE_EQ(package->toolbarDark.hover, std::string("#E6F1F40F"));
    REQUIRE_EQ(package->toolbarLight.icon, package->toolbarDark.icon);
    REQUIRE_EQ(package->toolbarLight.background, package->toolbarDark.background);
}

TEST_CASE(candidate_skin_catalog_draws_a_light_global_theme_without_package_colours)
{
    const auto package = LoadManifest(L"paper", Manifest("paper", "[\"light\"]"));
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->base, std::string("fluent"));
    REQUIRE_EQ(package->themes, (std::vector<std::string>{"dark", "light"}));
    REQUIRE_EQ(package->dark.surface, std::string("#F7F5F0"));
    REQUIRE_EQ(package->dark.accent, std::string("#2C7A4B"));
    REQUIRE_EQ(package->dark.selected, std::string("#2C7A4B24"));
    REQUIRE_EQ(package->dark.translation, std::string("#6E6A5E"));
}

// The client draws nothing of a package in a mode it does not declare, and a built-in theme only ever has
// its own mode, so such a package is never drawn there and must not be drawn here either.
TEST_CASE(candidate_skin_catalog_never_draws_a_global_theme_base_its_package_does_not_declare)
{
    const auto package = LoadManifest(L"undeclared", Manifest("shuishan", "[\"light\"]"));
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->base, std::string("fluent"));
    REQUIRE(!CandidateSkinCatalog::Supports(*package, "horizontal", "dark"));
    REQUIRE(!CandidateSkinCatalog::Supports(*package, "horizontal", "light"));
}

TEST_CASE(candidate_skin_catalog_rejects_unknown_and_custom_bases)
{
    REQUIRE(!LoadManifest(L"custom", Manifest("custom", "[\"dark\"]")).has_value());
    REQUIRE(!LoadManifest(L"unknown", Manifest("sepia", "[\"dark\"]")).has_value());
    REQUIRE(!LoadManifest(L"case", Manifest("Night", "[\"dark\"]")).has_value());
}

// Packages from the community library are validated by the cross-platform client's rules
// (crates/client-core/src/skin/catalog.rs load() in metasequoiaime/msime). data/client_dialect.json is
// msime-cloud's internal/skins/testdata/client_dialect.json, the case table both the client's Go port and its
// seed script are pinned to; scripts/sync-client-dialect.py refreshes the copy. Every package the client accepts
// must load here too, or a skin shared from another platform would silently vanish from the Windows list.
// Windows stays free to accept more than the client does, so the rejected cases are not checked.
TEST_CASE(candidate_skin_catalog_loads_every_package_the_client_accepts)
{
    std::ifstream input(MSIME_CLIENT_DIALECT_FIXTURE_PATH, std::ios::binary);
    REQUIRE(input.is_open());
    const auto fixture = nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
    size_t accepted = 0;
    for (const auto &item : fixture.at("cases"))
    {
        if (!item.at("reason").is_null())
        {
            continue;
        }
        const std::string name = item.at("name").get<std::string>();
        const std::string templateName = item.at("template").get<std::string>();
        std::string id = "sample";
        std::string manifest = item.value("manifest", std::string{});
        std::vector<std::string> files;
        if (templateName != "raw")
        {
            const auto &tpl = fixture.at("templates").at(templateName);
            id = tpl.at("id").get<std::string>();
            manifest = tpl.at("manifest").get<std::string>();
            files = tpl.at("files").get<std::vector<std::string>>();
        }
        id = item.value("id", id);
        for (size_t at; (at = manifest.find("{id}")) != std::string::npos;)
        {
            manifest.replace(at, 4, id);
        }
        for (const auto &edit : item.value("replace", nlohmann::json::array()))
        {
            const std::string from = edit.at(0).get<std::string>();
            const size_t at = manifest.find(from);
            if (at == std::string::npos)
            {
                throw std::runtime_error(name + ": replace target not in manifest");
            }
            manifest.replace(at, from.size(), edit.at(1).get<std::string>());
        }
        manifest = item.value("prepend", std::string{}) + manifest + item.value("append", std::string{});
        if (const size_t padTo = item.value("pad_to", size_t{0}); padTo > 0)
        {
            manifest += "#" + std::string(padTo - manifest.size() - 1, 'x');
        }
        if (item.contains("files"))
        {
            files = item.at("files").get<std::vector<std::string>>();
        }

        const auto root = WriteSkin(L"client-dialect", id, manifest, files);
        std::string error;
        const auto package = CandidateSkinCatalog::Load(root, id, &error);
        RemoveSkin(root);
        if (!package)
        {
            throw std::runtime_error(name + ": rejected: " + error);
        }
        if (!CandidateSkinCatalog::IsBuiltIn(package->base))
        {
            throw std::runtime_error(name + ": base " + package->base + " is not a built-in skin");
        }
        ++accepted;
    }
    // Guards against a fixture that silently lost its cases or changed shape.
    REQUIRE(accepted >= 30);
}
