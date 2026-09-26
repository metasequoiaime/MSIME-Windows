#include "skin/candidate_skin_catalog.h"

#include <toml++/toml.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace CandidateSkinCatalog
{
namespace
{
void SetError(std::string *error, const std::string &message)
{
    if (error)
    {
        *error = message;
    }
}

bool IsSafeFileName(const std::string &name, const std::string &extension)
{
    if (name.empty() || name.size() > 128 || name.size() <= extension.size() ||
        name.substr(name.size() - extension.size()) != extension)
    {
        return false;
    }
    return std::all_of(name.begin(), name.end(),
                       [](unsigned char ch) { return std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-'; });
}

bool IsSafeRelativeResource(const std::string &name)
{
    if (name.empty() || name.size() > 256 || name.front() == '/' || name.front() == '\\' ||
        name.find('\\') != std::string::npos)
    {
        return false;
    }
    if (!std::all_of(name.begin(), name.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '/' || ch == '.' || ch == '_' || ch == '-';
        }))
    {
        return false;
    }
    std::filesystem::path path(name);
    if (path.is_absolute())
    {
        return false;
    }
    for (const auto &part : path)
    {
        if (part == ".." || part == "." || part.empty())
        {
            return false;
        }
    }
    return true;
}

bool ReadString(const toml::table &table, const char *key, std::string &out, size_t maximum, bool required)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return !required;
    }
    const auto *value = node->as_string();
    if (!value)
    {
        return false;
    }
    out = value->get();
    return (!required || !out.empty()) && out.size() <= maximum;
}

bool ReadEnumArray(const toml::table &table, const char *key, const std::vector<std::string> &allowed,
                   std::vector<std::string> &out)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return false;
    }
    const auto *array = node->as_array();
    if (!array || array->empty())
    {
        return false;
    }
    for (const auto &item : *array)
    {
        const auto *value = item.as_string();
        if (!value)
        {
            return false;
        }
        const std::string text = value->get();
        if (std::find(allowed.begin(), allowed.end(), text) == allowed.end() ||
            std::find(out.begin(), out.end(), text) != out.end())
        {
            return false;
        }
        out.push_back(text);
    }
    return true;
}

double BoundedNumber(const toml::table &table, const char *key, double maximum)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return 0.0;
    }
    if (const auto *floating = node->as_floating_point())
    {
        const double value = floating->get();
        return std::isfinite(value) && value >= 0.0 && value <= maximum ? value : -1.0;
    }
    if (const auto *integer = node->as_integer())
    {
        const double value = static_cast<double>(integer->get());
        return value >= 0.0 && value <= maximum ? value : -1.0;
    }
    return -1.0;
}

bool ReadColors(const toml::table *table, CandidateColors &out)
{
    if (!table)
    {
        return true;
    }
    if ((table->contains("accent") && !ReadString(*table, "accent", out.accent, 80, false)) ||
        (table->contains("selected") && !ReadString(*table, "selected", out.selected, 80, false)) ||
        (table->contains("hover") && !ReadString(*table, "hover", out.hover, 80, false)) ||
        (table->contains("surface") && !ReadString(*table, "surface", out.surface, 80, false)) ||
        (table->contains("border") && !ReadString(*table, "border", out.border, 80, false)) ||
        (table->contains("text") && !ReadString(*table, "text", out.text, 80, false)) ||
        (table->contains("number") && !ReadString(*table, "number", out.number, 80, false)))
    {
        return false;
    }
    if (const toml::node *bar = table->get("show_selected_bar"))
    {
        const auto *flag = bar->as_boolean();
        if (!flag)
        {
            return false;
        }
        out.showSelectedBar = flag->get();
    }
    return true;
}
} // namespace

bool IsBuiltIn(const std::string &id)
{
    return id == "fluent" || id == "wechat" || id == "graphite" || id == "willow_green" || id == "autumn_osmanthus";
}

bool IsSafeId(const std::string &id)
{
    if (id.empty() || id.size() > 64 || !std::isalnum(static_cast<unsigned char>(id.front())))
    {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return std::islower(ch) || std::isdigit(ch) || ch == '.' || ch == '_' || ch == '-';
    });
}

bool Supports(const Package &package, const std::string &layout, const std::string &theme)
{
    return std::find(package.layouts.begin(), package.layouts.end(), layout) != package.layouts.end() &&
           std::find(package.themes.begin(), package.themes.end(), theme) != package.themes.end();
}

std::optional<Package> Load(const std::filesystem::path &skinsRoot, const std::string &id, std::string *error)
{
    if (!IsSafeId(id) || IsBuiltIn(id))
    {
        SetError(error, "目录名不是有效的外部皮肤 ID");
        return std::nullopt;
    }
    const std::filesystem::path directory = skinsRoot / std::filesystem::u8path(id);
    const std::filesystem::path manifest = directory / L"skin.toml";
    try
    {
        // Read via the wide path and parse the text. toml::parse_file(manifest.string()) would run the
        // path through the ANSI code page: skins live under the user profile, so a non-ASCII (e.g.
        // Chinese) user name corrupts it, and on a code page that cannot represent the characters
        // path::string() throws right past this try block's toml handlers.
        std::ifstream input(manifest, std::ios::binary);
        if (!input)
        {
            SetError(error, "缺少或无法解析 skin.toml");
            return std::nullopt;
        }
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const toml::table root = toml::parse(text);
        if (root["schema_version"].value_or(0) != 1)
        {
            SetError(error, "仅支持 schema_version 1");
            return std::nullopt;
        }
        Package package;
        if (!ReadString(root, "id", package.id, 64, true) || package.id != id ||
            !ReadString(root, "name", package.name, 80, true) ||
            !ReadString(root, "version", package.version, 32, true) ||
            !ReadString(root, "author", package.author, 120, false) ||
            !ReadString(root, "description", package.description, 500, false) ||
            !ReadString(root, "base", package.base, 32, true) || !IsBuiltIn(package.base))
        {
            SetError(error, "manifest 的基本信息无效");
            return std::nullopt;
        }
        if (root.contains("toolbar_stylesheet") &&
            (!ReadString(root, "toolbar_stylesheet", package.toolbarStylesheet, 128, true) ||
             !IsSafeFileName(package.toolbarStylesheet, ".css")))
        {
            SetError(error, "toolbar_stylesheet 文件名无效");
            return std::nullopt;
        }
        if (root.contains("preview") &&
            (!ReadString(root, "preview", package.preview, 256, false) || !IsSafeRelativeResource(package.preview)))
        {
            SetError(error, "preview 必须是皮肤目录内的相对路径");
            return std::nullopt;
        }
        const auto *supports = root["supports"].as_table();
        if (!supports || !ReadEnumArray(*supports, "layouts", {"horizontal", "vertical"}, package.layouts) ||
            !ReadEnumArray(*supports, "themes", {"dark", "light"}, package.themes))
        {
            SetError(error, "supports.layouts 或 supports.themes 无效");
            return std::nullopt;
        }
        const auto *window = root["candidate_window"].as_table();
        if (!window)
        {
            SetError(error, "缺少 candidate_window");
            return std::nullopt;
        }
        package.minWidthDip = BoundedNumber(*window, "min_width_dip", 1000.0);
        if (package.minWidthDip < 0.0)
        {
            SetError(error, "candidate_window.min_width_dip 超出范围");
            return std::nullopt;
        }
        const auto *decoration = (*window)["decoration"].as_table();
        if (!decoration)
        {
            SetError(error, "缺少 candidate_window.decoration");
            return std::nullopt;
        }
        package.decorationTopDip = BoundedNumber(*decoration, "top_inset_dip", 500.0);
        package.decorationWidthDip = BoundedNumber(*decoration, "width_dip", 1000.0);
        if (package.decorationTopDip < 0.0 || package.decorationWidthDip < 0.0 ||
            ((package.decorationTopDip == 0.0) != (package.decorationWidthDip == 0.0)))
        {
            SetError(error, "decoration 尺寸无效");
            return std::nullopt;
        }
        const auto *candidate = root["candidate"].as_table();
        if (candidate && (!ReadColors((*candidate)["dark"].as_table(), package.dark) ||
                          !ReadColors((*candidate)["light"].as_table(), package.light)))
        {
            SetError(error, "candidate 配色无效");
            return std::nullopt;
        }
        std::error_code ec;
        if (!package.toolbarStylesheet.empty() &&
            !std::filesystem::is_regular_file(directory / std::filesystem::u8path(package.toolbarStylesheet), ec))
        {
            SetError(error, "找不到 toolbar_stylesheet 文件");
            return std::nullopt;
        }
        return package;
    }
    catch (const toml::parse_error &)
    {
        SetError(error, "缺少或无法解析 skin.toml");
        return std::nullopt;
    }
    catch (const std::exception &)
    {
        SetError(error, "skin.toml 不是有效的 TOML manifest");
        return std::nullopt;
    }
}

ScanResult Scan(const std::filesystem::path &skinsRoot)
{
    ScanResult result;
    std::error_code ec;
    if (!std::filesystem::exists(skinsRoot, ec))
    {
        return result;
    }
    for (std::filesystem::directory_iterator it(skinsRoot, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_directory(ec))
        {
            continue;
        }
        const std::string folder = it->path().filename().u8string();
        std::string error;
        auto package = Load(skinsRoot, folder, &error);
        if (package)
        {
            result.packages.push_back(std::move(*package));
        }
        else
        {
            result.issues.push_back({folder, error});
        }
    }
    if (ec)
    {
        result.issues.push_back({"skins", "无法完整读取皮肤目录"});
    }
    std::sort(result.packages.begin(), result.packages.end(),
              [](const Package &a, const Package &b) { return a.name < b.name; });
    std::sort(result.issues.begin(), result.issues.end(),
              [](const Issue &a, const Issue &b) { return a.folder < b.folder; });
    return result;
}
} // namespace CandidateSkinCatalog
