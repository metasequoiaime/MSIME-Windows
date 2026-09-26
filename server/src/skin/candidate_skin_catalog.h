#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace CandidateSkinCatalog
{
struct CandidateColors
{
    std::string accent;
    std::string selected;
    std::string hover;
    std::string surface;
    std::string border;
    std::string text;
    std::string number;
    // 候选后的翻译文本。留空时沿用候选文字色并降到 62% 不透明度；设置后按原值绘制，选中行也不改。
    std::string translation;
    std::optional<bool> showSelectedBar;
};

struct Package
{
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string base = "fluent";
    std::string toolbarStylesheet;
    std::string preview;
    std::vector<std::string> layouts;
    std::vector<std::string> themes;
    double minWidthDip = 0.0;
    double decorationTopDip = 0.0;
    double decorationWidthDip = 0.0;
    CandidateColors dark;
    CandidateColors light;
};

struct Issue
{
    std::string folder;
    std::string reason;
};

struct ScanResult
{
    std::vector<Package> packages;
    std::vector<Issue> issues;
};

bool IsBuiltIn(const std::string &id);
bool IsSafeId(const std::string &id);
bool Supports(const Package &package, const std::string &layout, const std::string &theme);
std::optional<Package> Load(const std::filesystem::path &skinsRoot, const std::string &id,
                            std::string *error = nullptr);
ScanResult Scan(const std::filesystem::path &skinsRoot);
} // namespace CandidateSkinCatalog
