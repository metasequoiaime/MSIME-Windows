#include "tests/includes/test_framework.h"

#include "skin/candidate_skin_catalog.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
constexpr const char *kManifestHead = R"(schema_version = 1
id = "gloss"
name = "Gloss"
version = "1.0.0"
base = "fluent"

[supports]
layouts = ["horizontal", "vertical"]
themes = ["dark", "light"]

[candidate_window]

[candidate_window.decoration]
)";

std::filesystem::path WriteSkin(const std::wstring &leaf, const std::string &candidateTables)
{
    namespace fs = std::filesystem;
    const fs::path skins_root =
        fs::temp_directory_path() / (L"msime-skin-colors-" + std::to_wstring(GetCurrentProcessId())) / leaf / L"skins";
    std::error_code ec;
    fs::remove_all(skins_root, ec);
    fs::create_directories(skins_root / L"gloss", ec);
    REQUIRE(!ec);
    std::ofstream manifest(skins_root / L"gloss" / L"skin.toml", std::ios::binary | std::ios::trunc);
    REQUIRE(manifest.is_open());
    manifest << kManifestHead << candidateTables;
    return skins_root;
}
} // namespace

// The translation after a candidate has its own manifest key so a skin can colour it independently of
// the candidate text; an unset key stays empty so the renderers keep deriving it from the text colour.
TEST_CASE(candidate_skin_catalog_reads_translation_color_per_theme)
{
    const auto root = WriteSkin(L"valid", R"(
[candidate.dark]
text = "#e0e0e0"
translation = "#e6a817"

[candidate.light]
text = "#202020"
)");
    std::string error;
    const auto package = CandidateSkinCatalog::Load(root, "gloss", &error);
    REQUIRE(error.empty());
    REQUIRE(package.has_value());
    REQUIRE_EQ(package->dark.translation, std::string("#e6a817"));
    REQUIRE_EQ(package->dark.text, std::string("#e0e0e0"));
    REQUIRE(package->light.translation.empty());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE(candidate_skin_catalog_rejects_non_string_translation_color)
{
    const auto root = WriteSkin(L"invalid", R"(
[candidate.dark]
translation = 42
)");
    std::string error;
    REQUIRE(!CandidateSkinCatalog::Load(root, "gloss", &error).has_value());
    REQUIRE(!error.empty());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
