#include "skin/candidate_skin_catalog.h"

#include <toml++/toml.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string_view>
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

bool ReadEnum(const toml::table &table, const char *key, const std::vector<std::string> &allowed, std::string &out)
{
    if (!table.contains(key))
    {
        return true;
    }
    std::string text;
    if (!ReadString(table, key, text, 32, true) || std::find(allowed.begin(), allowed.end(), text) == allowed.end())
    {
        return false;
    }
    out = text;
    return true;
}

bool ReadResource(const toml::table &table, const char *key, std::string &out)
{
    return !table.contains(key) || (ReadString(table, key, out, 256, true) && IsSafeRelativeResource(out));
}

// 皮肤颜色会被拼进 WebView2 的 CSS 声明，只放行颜色值会用到的字符，挡住 `;`、`{}` 之类能跳出声明的写法。
bool ReadCssColor(const toml::table &table, const char *key, std::string &out)
{
    return ReadString(table, key, out, 80, false) && std::all_of(out.begin(), out.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '#' || ch == '(' || ch == ')' || ch == ',' || ch == '.' || ch == '%' ||
                      ch == ' ' || ch == '-' || ch == '/';
           });
}

bool ReadColors(const toml::table *table, CandidateColors &out)
{
    if (!table)
    {
        return true;
    }
    if (!ReadCssColor(*table, "accent", out.accent) || !ReadCssColor(*table, "selected", out.selected) ||
        !ReadCssColor(*table, "hover", out.hover) || !ReadCssColor(*table, "surface", out.surface) ||
        !ReadCssColor(*table, "border", out.border) || !ReadCssColor(*table, "text", out.text) ||
        !ReadCssColor(*table, "number", out.number) || !ReadCssColor(*table, "translation", out.translation) ||
        !ReadCssColor(*table, "candidate_text", out.candidateText) ||
        !ReadCssColor(*table, "preedit_text", out.preeditText) ||
        !ReadCssColor(*table, "preedit_caret", out.preeditCaret) ||
        !ReadCssColor(*table, "selected_text", out.selectedText) ||
        !ReadCssColor(*table, "selected_number", out.selectedNumber) ||
        !ReadCssColor(*table, "selected_translation", out.selectedTranslation) ||
        !ReadCssColor(*table, "selected_bar", out.selectedBar) ||
        !ReadCssColor(*table, "preedit_background", out.preeditBackground) ||
        !ReadCssColor(*table, "preedit_divider", out.preeditDivider))
    {
        return false;
    }
    if (const toml::node *menuNode = table->get("menu"))
    {
        const auto *menu = menuNode->as_table();
        if (!menu || !ReadCssColor(*menu, "background", out.menuBackground) ||
            !ReadCssColor(*menu, "border", out.menuBorder) || !ReadCssColor(*menu, "text", out.menuText) ||
            !ReadCssColor(*menu, "hover", out.menuHover))
        {
            return false;
        }
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

// 字体族会被拼进 WebView2 的 font-family（加引号）与脚本字符串，挡住引号、反斜杠、分号、花括号、尖括号和控制字符；
// 字体名可以是中文，所以非 ASCII 字节一律放行。
bool ReadFontFamily(const toml::table &table, const char *key, std::string &out)
{
    if (!table.contains(key))
    {
        return true;
    }
    if (!ReadString(table, key, out, 64, true))
    {
        return false;
    }
    return std::all_of(out.begin(), out.end(), [](unsigned char ch) {
        return ch >= 0x80 || (ch >= 0x20 && ch != 0x7f &&
                              std::string_view("\"'\\,;{}<>`").find(static_cast<char>(ch)) == std::string_view::npos);
    });
}

bool ReadOptionalBounded(const toml::table &table, const char *key, double maximum, std::optional<double> &out)
{
    if (!table.contains(key))
    {
        return true;
    }
    const double value = BoundedNumber(table, key, maximum);
    if (value < 0.0)
    {
        return false;
    }
    out = value;
    return true;
}

bool ReadOptionalBool(const toml::table &table, const char *key, std::optional<bool> &out)
{
    const toml::node *node = table.get(key);
    if (!node)
    {
        return true;
    }
    const auto *flag = node->as_boolean();
    if (!flag)
    {
        return false;
    }
    out = flag->get();
    return true;
}

// 可选的文本键：不写可以，写了就必须是 1 到 maximum 字节的字符串，与跨平台客户端的 optional_string 一致。
bool ReadOptionalText(const toml::table &table, const char *key, std::string &out, size_t maximum)
{
    return !table.contains(key) || ReadString(table, key, out, maximum, true);
}

// Read via the wide path and parse the text. toml::parse_file(manifest.string()) would run the
// path through the ANSI code page: skins live under the user profile, so a non-ASCII (e.g.
// Chinese) user name corrupts it, and on a code page that cannot represent the characters
// path::string() throws right past the callers' toml handlers.
std::optional<toml::table> ParseManifest(const std::filesystem::path &manifest)
{
    std::ifstream input(manifest, std::ios::binary);
    if (!input)
    {
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return toml::parse(text);
}

bool ReadToolbarColors(const toml::node *node, ToolbarColors &out)
{
    if (!node)
    {
        return true;
    }
    const auto *table = node->as_table();
    return table && ReadCssColor(*table, "background", out.background) && ReadCssColor(*table, "border", out.border) &&
           ReadCssColor(*table, "handle", out.handle) && ReadCssColor(*table, "divider", out.divider) &&
           ReadCssColor(*table, "icon", out.icon) && ReadCssColor(*table, "hover", out.hover);
}

// 跨平台客户端的全局主题：metasequoiaime/msime 的 crates/client-core/src/skin/theme.rs 里的
// BUILTIN_THEMES。社区皮肤包按客户端的规则校验，base 写的是这五个主题之一或
// system，所以这里逐字照抄那张表，改配色要两边一起改。
struct GlobalThemeBase
{
    std::string_view id;
    // 内置主题固定明暗：shuishan、night、ink 只有深色，light、paper 只有浅色。
    bool dark;
    std::string_view panel;
    std::string_view accent;
    std::string_view text;
    // 表里的 kb.sub：序号与翻译等次要文字。
    std::string_view secondary;
};

constexpr GlobalThemeBase kGlobalThemeBases[] = {
    {"shuishan", true, "#2A2B27", "#7FE08E", "#FFFFFF", "#9FB5A3"},
    {"light", false, "#FFFFFF", "#005FB8", "#1A1A1A", "#6A6F76"},
    {"paper", false, "#F7F5F0", "#2C7A4B", "#1A1E1B", "#6E6A5E"},
    {"night", true, "#16262F", "#4FD1C5", "#E6F1F4", "#86A6B0"},
    {"ink", true, "#1A1A1A", "#FFFFFF", "#9A9A9A", "#9A9A9A"},
};
// BUILTIN_CANDIDATE_BORDER、SELECTED_ALPHA、HOVER_ALPHA：内置主题的边框色，以及选中行、悬停行在强调色、文字色上叠的不透明度。
constexpr std::string_view kGlobalThemeBorder = "#0000001F";
constexpr std::string_view kGlobalThemeSelectedAlpha = "24";
constexpr std::string_view kGlobalThemeHoverAlpha = "0F";
// 客户端的 system 画的是宿主自己的原生配色，在 Windows 上就是 fluent。
constexpr std::string_view kGlobalThemeSystem = "system";

// 全局主题 ID（加上 custom）在跨平台客户端里不能当皮肤 ID，这里同样保留，同一个皮肤文件夹在每个平台上才都能加载。
bool IsGlobalThemeId(const std::string &id)
{
    for (std::string_view theme : {"system", "shuishan", "light", "paper", "night", "ink", "custom"})
    {
        if (theme == id)
        {
            return true;
        }
    }
    return false;
}

const GlobalThemeBase *FindGlobalThemeBase(const std::string &id)
{
    for (const auto &base : kGlobalThemeBases)
    {
        if (base.id == id)
        {
            return &base;
        }
    }
    return nullptr;
}

bool ParseColorChannel(std::string_view text, unsigned &out)
{
    if (!text.empty() && text.front() == '+')
    {
        text.remove_prefix(1);
    }
    if (text.empty() || text.size() > 3 ||
        !std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
    {
        return false;
    }
    out = static_cast<unsigned>(std::stoul(std::string(text)));
    return out <= 255;
}

// theme.rs 的 normalized_color：读成 #RRGGBB 或
// #RRGGBBAA（大写），读不懂的返回空串。客户端把读不懂的颜色当作没写，换算全局主题时这里也一样。
std::string NormalizedColor(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    std::string lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower == "transparent")
    {
        return "#00000000";
    }
    if (!lower.empty() && lower.front() == '#')
    {
        std::string hex = lower.substr(1);
        if (!std::all_of(hex.begin(), hex.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
        {
            return {};
        }
        std::transform(hex.begin(), hex.end(), hex.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if (hex.size() == 3)
        {
            return std::string{'#', hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
        }
        return hex.size() == 6 || hex.size() == 8 ? "#" + hex : std::string{};
    }
    const bool withAlpha = lower.rfind("rgba(", 0) == 0;
    if ((!withAlpha && lower.rfind("rgb(", 0) != 0) || lower.back() != ')')
    {
        return {};
    }
    const size_t open = lower.find('(');
    std::vector<std::string_view> parts;
    std::string_view arguments = std::string_view(lower).substr(open + 1, lower.size() - open - 2);
    for (size_t comma; (comma = arguments.find(',')) != std::string_view::npos; arguments.remove_prefix(comma + 1))
    {
        parts.push_back(arguments.substr(0, comma));
    }
    parts.push_back(arguments);
    if (parts.size() != (withAlpha ? 4u : 3u))
    {
        return {};
    }
    for (auto &part : parts)
    {
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())))
            part.remove_prefix(1);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())))
            part.remove_suffix(1);
    }
    unsigned channels[3]{};
    for (size_t i = 0; i < 3; ++i)
    {
        if (!ParseColorChannel(parts[i], channels[i]))
        {
            return {};
        }
    }
    char buffer[10];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", channels[0], channels[1], channels[2]);
    if (!withAlpha)
    {
        return buffer;
    }
    std::string_view alphaText = parts[3];
    if (!alphaText.empty() && alphaText.front() == '+')
    {
        alphaText.remove_prefix(1);
    }
    double alpha = 0.0;
    const auto [end, ec] = std::from_chars(alphaText.data(), alphaText.data() + alphaText.size(), alpha);
    if (alphaText.empty() || ec != std::errc() || end != alphaText.data() + alphaText.size() || !std::isfinite(alpha) ||
        alpha < 0.0 || alpha > 1.0)
    {
        return {};
    }
    char alphaBuffer[3];
    std::snprintf(alphaBuffer, sizeof(alphaBuffer), "%02X", static_cast<unsigned>(std::lround(alpha * 255.0)));
    return std::string(buffer) + alphaBuffer;
}

void FillColor(std::string &slot, const std::string &value)
{
    if (slot.empty())
    {
        slot = value;
    }
}

// 把 base 是全局主题的包换算成 Windows 画得出的 fluent 包，画出来与客户端 theme::resolve 一致：
// 主题固定明暗，所以只取包里那一种明暗的配色，深浅两套都画它；客户端读的八个颜色先按 normalized_color
// 规范化，没写的再按主题补齐——选中行是强调色叠 SELECTED_ALPHA，悬停行是文字色叠
// HOVER_ALPHA，选中文字取强调色，选中序号与翻译取序号色。
// 右键菜单和悬浮工具栏在客户端也从同一套候选配色派生，这里一并补上，免得跟着 Windows 当前的明暗画成另一种底色。
// 包没有声明主题那一种明暗时，客户端不画它，这里清空 themes，让各处的 Supports 都判为不兼容。
void ApplyGlobalThemeBase(Package &package, const GlobalThemeBase &base)
{
    package.base = "fluent";
    const std::string mode = base.dark ? "dark" : "light";
    if (std::find(package.themes.begin(), package.themes.end(), mode) == package.themes.end())
    {
        package.themes.clear();
        return;
    }
    CandidateColors colors = base.dark ? package.dark : package.light;
    for (std::string *slot : {&colors.surface, &colors.border, &colors.text, &colors.number, &colors.accent,
                              &colors.selected, &colors.hover, &colors.translation})
    {
        *slot = NormalizedColor(*slot);
    }
    FillColor(colors.surface, std::string(base.panel));
    FillColor(colors.border, std::string(kGlobalThemeBorder));
    FillColor(colors.text, std::string(base.text));
    FillColor(colors.number, std::string(base.secondary));
    FillColor(colors.accent, std::string(base.accent));
    FillColor(colors.selected, colors.accent.substr(0, 7) + std::string(kGlobalThemeSelectedAlpha));
    FillColor(colors.hover, colors.text.substr(0, 7) + std::string(kGlobalThemeHoverAlpha));
    FillColor(colors.translation, colors.number);
    FillColor(colors.selectedText, colors.accent);
    FillColor(colors.selectedNumber, colors.number);
    FillColor(colors.menuBackground, colors.surface);
    FillColor(colors.menuBorder, colors.border);
    FillColor(colors.menuText, colors.text);
    FillColor(colors.menuHover, colors.hover);

    ToolbarColors toolbar = base.dark ? package.toolbarDark : package.toolbarLight;
    for (std::string *slot :
         {&toolbar.background, &toolbar.border, &toolbar.handle, &toolbar.divider, &toolbar.icon, &toolbar.hover})
    {
        *slot = NormalizedColor(*slot);
    }
    FillColor(toolbar.background, colors.surface);
    FillColor(toolbar.border, colors.border);
    FillColor(toolbar.handle, colors.number);
    FillColor(toolbar.divider, colors.border);
    FillColor(toolbar.icon, colors.text);
    FillColor(toolbar.hover, colors.hover);

    package.dark = colors;
    package.light = colors;
    package.toolbarDark = toolbar;
    package.toolbarLight = toolbar;
    package.themes = {"dark", "light"};
}

// 客户端的 is_image：按扩展名判断是不是它允许的图片类型。
bool IsClientImage(const std::string &name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos)
    {
        return false;
    }
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    static const std::vector<std::string> images = {"png", "jpg", "jpeg", "gif", "webp", "svg", "ico", "bmp", "avif"};
    return std::find(images.begin(), images.end(), ext) != images.end();
}
} // namespace

const std::vector<std::string> &BuiltInIds()
{
    static const std::vector<std::string> ids = {"fluent",       "wechat",           "graphite",
                                                 "willow_green", "autumn_osmanthus", "microsoft"};
    return ids;
}

bool IsBuiltIn(const std::string &id)
{
    const auto &ids = BuiltInIds();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
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

bool IsBackupFolder(const std::string &folder)
{
    constexpr std::string_view kSuffix = ".bak";
    if (folder.size() < kSuffix.size())
    {
        return false;
    }
    return std::equal(kSuffix.begin(), kSuffix.end(), folder.end() - kSuffix.size(),
                      [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
}

bool Supports(const Package &package, const std::string &layout, const std::string &theme)
{
    return std::find(package.layouts.begin(), package.layouts.end(), layout) != package.layouts.end() &&
           std::find(package.themes.begin(), package.themes.end(), theme) != package.themes.end();
}

std::optional<Package> Load(const std::filesystem::path &skinsRoot, const std::string &id, std::string *error)
{
    if (!IsSafeId(id) || IsBuiltIn(id) || id == kDefaultSkinsFolder || IsGlobalThemeId(id))
    {
        SetError(error, "目录名不是有效的外部皮肤 ID");
        return std::nullopt;
    }
    const std::filesystem::path directory = skinsRoot / std::filesystem::u8path(id);
    const std::filesystem::path manifest = directory / L"skin.toml";
    std::error_code sizeEc;
    if (const auto size = std::filesystem::file_size(manifest, sizeEc); !sizeEc && size > kMaxManifestBytes)
    {
        SetError(error, "skin.toml 超过 64 KiB");
        return std::nullopt;
    }
    try
    {
        const std::optional<toml::table> parsed = ParseManifest(manifest);
        if (!parsed)
        {
            SetError(error, "缺少或无法解析 skin.toml");
            return std::nullopt;
        }
        const toml::table &root = *parsed;
        const toml::node *schemaVersion = root.get("schema_version");
        if (!schemaVersion || !schemaVersion->as_integer() || schemaVersion->as_integer()->get() != 1)
        {
            SetError(error, "仅支持 schema_version 1");
            return std::nullopt;
        }
        Package package;
        if (!ReadString(root, "id", package.id, 64, true) || package.id != id ||
            !ReadString(root, "name", package.name, 80, true) ||
            !ReadString(root, "version", package.version, 32, true) ||
            !ReadOptionalText(root, "author", package.author, 120) ||
            !ReadOptionalText(root, "description", package.description, 500) ||
            !ReadString(root, "base", package.base, 32, true) ||
            !(IsBuiltIn(package.base) || package.base == kGlobalThemeSystem || FindGlobalThemeBase(package.base)))
        {
            SetError(error, "manifest 的基本信息无效");
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
        // preview 是皮肤列表里的预览图，写了就必须指向包内已有的文件或目录，与跨平台客户端一致。
        std::string preview;
        std::error_code previewEc;
        if (!ReadOptionalText(root, "preview", preview, 256) ||
            (!preview.empty() && (!IsSafeRelativeResource(preview) ||
                                  !std::filesystem::exists(directory / std::filesystem::u8path(preview), previewEc))))
        {
            SetError(error, "preview 无效");
            return std::nullopt;
        }
        // 装饰图是可选的，规则与跨平台客户端一致：没有这张表、或两个尺寸都是 0（不写算 0）就没有装饰；两个尺寸必须同为
        // 0 或同大于 0。有尺寸时 image 可以不写，改用 preview（是图片时）；两者都没有就不画装饰。
        if (const toml::node *decorationNode = window->get("decoration"))
        {
            const auto *decoration = decorationNode->as_table();
            if (!decoration || !ReadResource(*decoration, "image", package.decorationImage) ||
                (!package.decorationImage.empty() && !IsClientImage(package.decorationImage)) ||
                !ReadEnum(*decoration, "align", {"left", "center", "right"}, package.decorationAlign))
            {
                SetError(error, "candidate_window.decoration 无效");
                return std::nullopt;
            }
            package.decorationTopDip = BoundedNumber(*decoration, "top_inset_dip", 500.0);
            package.decorationWidthDip = BoundedNumber(*decoration, "width_dip", 1000.0);
            if (package.decorationTopDip < 0.0 || package.decorationWidthDip < 0.0 ||
                (package.decorationTopDip == 0.0) != (package.decorationWidthDip == 0.0) ||
                (package.decorationTopDip == 0.0 && !package.decorationImage.empty()))
            {
                SetError(error, "candidate_window.decoration 尺寸无效");
                return std::nullopt;
            }
            if (package.decorationTopDip > 0.0 && package.decorationImage.empty())
            {
                if (!preview.empty() && IsClientImage(preview) &&
                    std::filesystem::is_regular_file(directory / std::filesystem::u8path(preview), previewEc))
                {
                    package.decorationImage = preview;
                }
                else
                {
                    package.decorationTopDip = 0.0;
                    package.decorationWidthDip = 0.0;
                }
            }
        }
        if (window->contains("corner_radius_dip"))
        {
            const double radius = BoundedNumber(*window, "corner_radius_dip", 32.0);
            if (radius < 0.0)
            {
                SetError(error, "candidate_window.corner_radius_dip 超出范围");
                return std::nullopt;
            }
            package.cornerRadiusDip = radius;
        }
        if (!ReadOptionalBounded(*window, "border_width_dip", 4.0, package.borderWidthDip))
        {
            SetError(error, "candidate_window.border_width_dip 超出范围");
            return std::nullopt;
        }
        if (!ReadOptionalBounded(*window, "item_corner_radius_dip", 16.0, package.itemCornerRadiusDip))
        {
            SetError(error, "candidate_window.item_corner_radius_dip 超出范围");
            return std::nullopt;
        }
        if (!ReadEnum(*window, "shadow", {"none", "soft", "strong"}, package.shadow))
        {
            SetError(error, "candidate_window.shadow 只能是 none、soft 或 strong");
            return std::nullopt;
        }
        if (!ReadFontFamily(*window, "font_family", package.fontFamily))
        {
            SetError(error, "candidate_window.font_family 无效");
            return std::nullopt;
        }
        if (!ReadOptionalBool(*window, "page_arrows", package.pageArrows))
        {
            SetError(error, "candidate_window.page_arrows 必须是 true 或 false");
            return std::nullopt;
        }
        if (const toml::node *backgroundNode = window->get("background"))
        {
            const auto *background = backgroundNode->as_table();
            if (!background || !background->contains("image") ||
                !ReadResource(*background, "image", package.backgroundImage) ||
                !IsClientImage(package.backgroundImage) ||
                !ReadEnum(*background, "fit", {"cover", "contain", "stretch"}, package.backgroundFit))
            {
                SetError(error, "candidate_window.background 无效");
                return std::nullopt;
            }
            if (background->contains("opacity"))
            {
                package.backgroundOpacity = BoundedNumber(*background, "opacity", 1.0);
                if (package.backgroundOpacity < 0.0)
                {
                    SetError(error, "candidate_window.background.opacity 超出范围");
                    return std::nullopt;
                }
            }
        }
        // [candidate] 与其中的 dark、light 写了就必须是表，与跨平台客户端一致。
        if (const toml::node *candidateNode = root.get("candidate"))
        {
            const auto *candidate = candidateNode->as_table();
            const toml::node *dark = candidate ? candidate->get("dark") : nullptr;
            const toml::node *light = candidate ? candidate->get("light") : nullptr;
            if (!candidate || (dark && !dark->is_table()) || (light && !light->is_table()) ||
                !ReadColors(dark ? dark->as_table() : nullptr, package.dark) ||
                !ReadColors(light ? light->as_table() : nullptr, package.light))
            {
                SetError(error, "candidate 配色无效");
                return std::nullopt;
            }
        }
        if (const toml::node *toolbarNode = root.get("toolbar"))
        {
            const auto *toolbar = toolbarNode->as_table();
            if (!toolbar || !ReadToolbarColors(toolbar->get("dark"), package.toolbarDark) ||
                !ReadToolbarColors(toolbar->get("light"), package.toolbarLight))
            {
                SetError(error, "toolbar 配色无效");
                return std::nullopt;
            }
            if (toolbar->contains("corner_radius_dip"))
            {
                const double radius = BoundedNumber(*toolbar, "corner_radius_dip", 32.0);
                if (radius < 0.0)
                {
                    SetError(error, "toolbar.corner_radius_dip 超出范围");
                    return std::nullopt;
                }
                package.toolbarCornerRadiusDip = radius;
            }
        }
        // [license] 只是声明，Windows 不读它的内容，但按跨平台客户端的规则校验，同一个包在每个平台上的加载结果才一致。
        if (const toml::node *licenseNode = root.get("license"))
        {
            const auto *license = licenseNode->as_table();
            std::string text;
            if (!license || !ReadOptionalText(*license, "code", text, 120) ||
                !ReadOptionalText(*license, "assets", text, 120) || !ReadOptionalText(*license, "source", text, 500))
            {
                SetError(error, "license 无效");
                return std::nullopt;
            }
        }
        // toolbar_stylesheet 是跨平台客户端的工具栏样式表，Windows 不加载它，但同样要求它是包根目录里一个已有的 .css
        // 文件。
        std::string stylesheet;
        std::error_code stylesheetEc;
        if (!ReadOptionalText(root, "toolbar_stylesheet", stylesheet, 128) ||
            (!stylesheet.empty() &&
             (!IsSafeRelativeResource(stylesheet) || stylesheet.find('/') != std::string::npos ||
              stylesheet.size() <= 4 || stylesheet.compare(stylesheet.size() - 4, 4, ".css") != 0 ||
              !std::filesystem::is_regular_file(directory / std::filesystem::u8path(stylesheet), stylesheetEc))))
        {
            SetError(error, "toolbar_stylesheet 无效");
            return std::nullopt;
        }
        // base 写的是跨平台全局主题时，换算成 fluent 再交给渲染端，渲染端只认内置皮肤 ID。
        if (package.base == kGlobalThemeSystem)
        {
            package.base = "fluent";
        }
        else if (const GlobalThemeBase *theme = FindGlobalThemeBase(package.base))
        {
            ApplyGlobalThemeBase(package, *theme);
        }
        std::error_code ec;
        if (!package.decorationImage.empty() &&
            !std::filesystem::is_regular_file(directory / std::filesystem::u8path(package.decorationImage), ec))
        {
            SetError(error, "找不到 candidate_window.decoration.image 文件");
            return std::nullopt;
        }
        if (!package.backgroundImage.empty() &&
            !std::filesystem::is_regular_file(directory / std::filesystem::u8path(package.backgroundImage), ec))
        {
            SetError(error, "找不到 candidate_window.background.image 文件");
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
        // <id>.bak 是安装包覆盖随包皮肤前留下的旧版本：manifest 的 id 与目录名对不上，
        // 不跳过就会在设置页被列成一条损坏的皮肤。
        if (folder == kDefaultSkinsFolder || IsBackupFolder(folder))
        {
            continue;
        }
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

std::optional<DefaultSkin> LoadDefault(const std::filesystem::path &skinsRoot, const std::string &id,
                                       std::string *error)
{
    if (!IsBuiltIn(id))
    {
        SetError(error, "不是内置皮肤 ID");
        return std::nullopt;
    }
    const std::filesystem::path manifest =
        skinsRoot / std::filesystem::u8path(kDefaultSkinsFolder) / std::filesystem::u8path(id) / L"skin.toml";
    try
    {
        const std::optional<toml::table> parsed = ParseManifest(manifest);
        if (!parsed)
        {
            SetError(error, "缺少或无法解析 skin.toml");
            return std::nullopt;
        }
        const toml::table &root = *parsed;
        DefaultSkin skin;
        if (root["schema_version"].value_or(0) != 1 || !ReadString(root, "id", skin.id, 64, true) || skin.id != id ||
            !ReadString(root, "name", skin.name, 80, false))
        {
            SetError(error, "manifest 的基本信息无效");
            return std::nullopt;
        }
        if (const toml::node *windowNode = root.get("candidate_window"))
        {
            const auto *window = windowNode->as_table();
            if (!window || !ReadOptionalBool(*window, "page_arrows", skin.pageArrows))
            {
                SetError(error, "candidate_window.page_arrows 必须是 true 或 false");
                return std::nullopt;
            }
        }
        return skin;
    }
    catch (const std::exception &)
    {
        SetError(error, "skin.toml 不是有效的 TOML manifest");
        return std::nullopt;
    }
}

bool ResolvePageArrows(const std::filesystem::path &skinsRoot, const std::string &skinId, const Package *package)
{
    if (package && package->pageArrows)
    {
        return *package->pageArrows;
    }
    const std::string base = package ? package->base : skinId;
    // 配置里的皮肤既不是内置、也没能作为外部皮肤加载时，渲染端回退到 fluent，这里跟着回退。
    const auto defaults = LoadDefault(skinsRoot, IsBuiltIn(base) ? base : "fluent");
    return defaults && defaults->pageArrows ? *defaults->pageArrows : kDefaultPageArrows;
}
} // namespace CandidateSkinCatalog
