#include "window/candidate_presenter.h"

#include "config/ime_config.h"
#include "defines/defines.h"
#include "defines/globals.h"
#include "global/globals.h"
#include "ipc/ipc.h"
#include "log/candidate_diag_log.h"
#include "skin/candidate_skin_catalog.h"
#include "utils/common_utils.h"
#include "utils/ime_utils.h"
#include "utils/window_utils.h"
#include "window/candidate_skin_palette.h"
#include "window/candidate_wheel_paging.h"
#include "window/ime_windows.h"

#include "msimeui/Controls.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Layout.h"
#include "msimeui/Scene.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include "engine/common/helpcode_utils.h"
#include "engine/core/scheme_type.h"
#include "engine/core/word_item.h"

#include <d2d1.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
D2D1_COLOR_F ColorFromRgb(UINT rgb, float alpha = 1.0f)
{
    return CandidateColorFromRgb(rgb, alpha);
}

D2D1_COLOR_F ParseCssColor(const std::string &text, D2D1_COLOR_F fallback)
{
    return ParseCandidateCssColor(text, fallback);
}

int AlignHostPixels(int value)
{
    constexpr int kBucket = 64;
    if (value < 1)
    {
        value = 1;
    }
    return ((value + kBucket - 1) / kBucket) * kBucket;
}

// Recompute half-screen DIP limits against `scale` when it differs from the
// monitor-derived scale, so clamping and window sizing stay on one scale
// (mirrors the webview path's rasterization-scale composition).
HalfScreenDipLimits ApplyScaleToHalfScreenLimits(POINT pt, FLOAT scale)
{
    HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForPoint(pt);
    if (scale > 0.0f)
    {
        const double monitorWidthPx = static_cast<double>((std::max)(1, limits.monitor.right - limits.monitor.left));
        const double monitorHeightPx = static_cast<double>((std::max)(1, limits.monitor.bottom - limits.monitor.top));
        limits.scale = scale;
        limits.maxWidthDip = (monitorWidthPx * 0.5) / static_cast<double>(scale);
        limits.maxHeightDip = (monitorHeightPx * 0.5) / static_cast<double>(scale);
    }
    return limits;
}

struct CandSkinTokens
{
    D2D1_COLOR_F surface{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F text{};
    D2D1_COLOR_F number = ParseCssColor("#e9e8e89d", ColorFromRgb(0xE9E8E8, 0.616f));
    D2D1_COLOR_F selected = ParseCssColor("#3e3e3eb9", ColorFromRgb(0x3E3E3E, 0.725f));
    D2D1_COLOR_F hover = ColorFromRgb(0x414141);
    D2D1_COLOR_F accent = ColorFromRgb(0x6B69D6);
    float radius = 6.0f;
    float borderWidth = 1.5f;
    float containerPad = 5.0f;
    bool showSelectedBar = true;
    // Row corner radius, selected-row text colors and context-menu palette.
    // Defaults are fluent dark; the fluent light branch and per-skin branches
    // override. rowTextSelected/rowLabelSelected alpha 0 keeps the normal
    // colors (bar-style selection, like the fluent CSS).
    float itemRadius = 4.0f;
    // > 0: rows touching the card's corners take this radius there instead.
    float outerItemRadius = 0.0f;
    // Row box geometry. Defaults suit the inset-highlight skins; full-bleed
    // skins (willow green) pad the rows themselves because the card does not.
    float itemGap = 2.0f;
    float itemExtraHeight = 2.0f;
    float itemPadLeft = 0.0f;
    float itemPadRight = 0.0f;
    msimeui::Thickness preeditMargin{};
    D2D1_COLOR_F rowTextSelected = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F rowLabelSelected = D2D1::ColorF(0, 0.0f);
    D2D1_COLOR_F menuFill = D2D1::ColorF(0x2D2D2D);
    D2D1_COLOR_F menuBorder = ParseCssColor("#9b9b9b2e", D2D1::ColorF(0x3A3A3A, 0.18f));
    D2D1_COLOR_F menuText = D2D1::ColorF(0xE9E8E8);
    D2D1_COLOR_F menuHover = ColorFromRgb(0x414141);
};

void ApplyPackageColors(const CandidateSkinCatalog::CandidateColors &colors, CandSkinTokens &tokens)
{
    if (!colors.accent.empty())
    {
        tokens.accent = ParseCssColor(colors.accent, tokens.accent);
    }
    if (!colors.selected.empty())
    {
        tokens.selected = ParseCssColor(colors.selected, tokens.selected);
    }
    if (!colors.hover.empty())
    {
        tokens.hover = ParseCssColor(colors.hover, tokens.hover);
    }
    if (!colors.number.empty())
    {
        tokens.number = ParseCssColor(colors.number, tokens.number);
    }
    if (colors.showSelectedBar.has_value())
    {
        tokens.showSelectedBar = *colors.showSelectedBar;
    }
}

std::wstring AssetRoot()
{
    return CommonUtils::get_ime_data_path_w();
}

constexpr float kShadowPadLeft = 32.0f;
constexpr float kShadowPadTop = 20.0f;
constexpr float kShadowPadRight = 32.0f;
constexpr float kShadowPadBottom = 40.0f;
constexpr float kCandidateMinWidthDip = 160.0f;
constexpr float kDecorationCardOverlap = 5.0f;

} // namespace

struct CandidatePresenter::Impl
{
    msimeui::DeviceResources resources;
    std::unique_ptr<msimeui::Window> window;
    std::shared_ptr<msimeui::StackPanel> root;
    std::shared_ptr<msimeui::Image> decoration;
    std::shared_ptr<msimeui::Card> card;
    std::shared_ptr<msimeui::Container> frame;
    std::shared_ptr<msimeui::StackPanel> body;
    std::shared_ptr<msimeui::TextBlock> preedit;
    std::shared_ptr<msimeui::CandidateList> list;
    std::shared_ptr<msimeui::Popup> contextMenu;
    std::shared_ptr<msimeui::Popup> contextSubmenu;
    std::shared_ptr<msimeui::MenuFlyoutItem> fixPositionItem;
    D2D1_COLOR_F menuFill = D2D1::ColorF(0x2D2D2D);
    D2D1_COLOR_F menuBorder = ParseCssColor("#9b9b9b2e", D2D1::ColorF(0x3A3A3A, 0.18f));
    D2D1_COLOR_F menuText = D2D1::ColorF(0xE9E8E8);
    D2D1_COLOR_F menuHover = D2D1::ColorF(0x414141);
    RECT hostRectBeforeMenu{};
    bool contextMenuOpen = false;
    bool contextSubmenuOpen = false;
    bool hostExpandedForMenu = false;
    size_t contextMenuPageIndex = 0;
};

CandidatePresenter::CandidatePresenter() = default;

CandidatePresenter &CandidatePresenter::Instance()
{
    static CandidatePresenter instance;
    return instance;
}

bool CandidatePresenter::IsBound() const
{
    return bound_;
}

bool CandidatePresenter::Bind(HWND hwnd)
{
    hwnd_ = hwnd;
    if (!hwnd)
    {
        bound_ = false;
        impl_.reset();
        return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->window = std::make_unique<msimeui::Window>(L"metasequoiaime_windows", L"cand", 1, 1);
    impl_->window->AdoptExistingHwnd(hwnd);
    impl_->window->SetStealFocusOnClick(false);
    bound_ = impl_->resources.EnsureForComposition(hwnd);
    RebuildScene();
    CAND_DIAG_LOGF(L"candidate-d2d bind hwnd={:#x} composition={}", reinterpret_cast<uintptr_t>(hwnd), bound_ ? 1 : 0);
    return bound_;
}

void CandidatePresenter::RebuildScene()
{
    if (!impl_ || !impl_->window)
    {
        return;
    }
    lastSkinFingerprint_.clear();
    impl_->root = std::make_shared<msimeui::StackPanel>(0.0f);
    impl_->decoration = std::make_shared<msimeui::Image>(L"");
    impl_->decoration->SetHorizontalAlignment(msimeui::HorizontalAlignment::Trailing);
    impl_->preedit = std::make_shared<msimeui::TextBlock>(L"", 14.0f, D2D1::ColorF(0xF5F5F5));
    impl_->preedit->SetTextLayoutPadding({0.0f, 1.0f, 0.0f, 1.0f});
    impl_->preedit->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
    impl_->list = std::make_shared<msimeui::CandidateList>(28.0f);
    impl_->list->SetOnItemActivated([this](size_t index) { CommitItem(index); });
    impl_->list->SetOnContextMenu(
        [this](size_t index, const POINT &clientPoint) { ShowItemContextMenu(index, clientPoint); });
    impl_->body = std::make_shared<msimeui::StackPanel>(2.0f);
    impl_->body->SetPadding({0.0f, 0.0f, 0.0f, 0.0f});
    impl_->body->AddChild(impl_->preedit);
    impl_->body->AddChild(impl_->list);
    msimeui::Brush brush;
    brush.fill = D2D1::ColorF(0x202020);
    brush.stroke = ParseCssColor("#9b9b9b2e", D2D1::ColorF(0x3A3A3A, 0.18f));
    brush.strokeWidth = 1.5f;
    brush.radiusX = 6.0f;
    brush.radiusY = 6.0f;
    impl_->card = std::make_shared<msimeui::Card>(brush, 5.0f);
    impl_->card->SetHorizontalAlignment(msimeui::HorizontalAlignment::Leading);
    impl_->card->AddChild(impl_->body);
    impl_->root->SetHorizontalContentAlignment(msimeui::HorizontalAlignment::Leading);
    impl_->frame = std::make_shared<msimeui::Container>();
    impl_->frame->SetPadding({kShadowPadLeft, kShadowPadTop, kShadowPadRight, kShadowPadBottom});
    impl_->frame->SetChild(impl_->card);
    impl_->root->AddChild(impl_->decoration);
    impl_->root->AddChild(impl_->frame);
    auto scene = std::make_unique<msimeui::Scene>();
    scene->SetRoot(impl_->root);
    impl_->window->SetScene(std::move(scene));
}

void CandidatePresenter::ApplySkin()
{
    if (!impl_ || !impl_->list || !impl_->preedit || !impl_->window)
    {
        return;
    }
    const std::string skinId = GetConfiguredCandidateSkin();
    // Resolve the effective light/dark mode before the cache check: with
    // theme_cand = "follow" the raw setting is unchanged when theme_mode or
    // the Windows theme flips, but the resolved colors are what we render
    // with, so the resolved mode must participate in the fingerprint.
    const bool candLight = ResolveConfiguredTheme(GetConfiguredThemeCand()) == "light";
    std::ostringstream fingerprint;
    fingerprint << skinId << '|' << GetConfiguredThemeCand() << '|' << (candLight ? 'L' : 'D') << '|'
                << GetConfiguredCandidateFont() << '|' << GetConfiguredCandidateFontSize() << '|'
                << GetConfiguredCandidateWindowPreeditFontSize() << '|' << GetConfiguredCandidateWindowLayout() << '|'
                << GetConfiguredCandidateTextColor() << '|' << GetConfiguredCandidateWindowPreeditStyle();
    fingerprint << '|' << GetConfiguredCandidateEnglishFont();
    for (const auto &font : GetConfiguredCandidateFallbackFonts())
        fingerprint << '|' << font.size() << ':' << font;
    const std::string skinKey = fingerprint.str();
    if (skinKey == lastSkinFingerprint_ && impl_->card)
    {
        return;
    }
    lastSkinFingerprint_ = skinKey;
    CandSkinTokens tokens;
    if (candLight)
    {
        tokens.number = D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f, 0.55f);
        tokens.selected = ColorFromRgb(0xE8E8E8);
        tokens.hover = ColorFromRgb(0xECECEC);
        tokens.menuFill = ColorFromRgb(0xFFFFFF);
        tokens.menuBorder = D2D1::ColorF(0, 0.12f);
        tokens.menuText = ColorFromRgb(0x1A1A1A);
        tokens.menuHover = ColorFromRgb(0xECECEC);
    }
    // Built-in skin palettes mirror ui-html/webview2/candwnd/skins/<skin>/;
    // the WebView2 CSS is the reference for both the light and dark values.
    if (skinId == "wechat")
    {
        tokens.borderWidth = 1.0f;
        tokens.radius = 5.0f;
        tokens.containerPad = 2.0f;
        tokens.accent = ColorFromRgb(0x07C160);
        tokens.selected = ColorFromRgb(0x07C160);
        tokens.showSelectedBar = false;
        tokens.rowTextSelected = ColorFromRgb(0xFFFFFF);
        tokens.rowLabelSelected = ColorFromRgb(0xFFFFFF);
        if (candLight)
        {
            tokens.hover = D2D1::ColorF(7.0f / 255.0f, 193.0f / 255.0f, 96.0f / 255.0f, 0.14f);
            tokens.number = ColorFromRgb(0x757575);
            tokens.menuFill = ColorFromRgb(0xFFFFFF);
            tokens.menuBorder = ColorFromRgb(0xD9D9D9);
            tokens.menuText = ColorFromRgb(0x333333);
            tokens.menuHover = ColorFromRgb(0xEEEEEE);
        }
        else
        {
            tokens.hover = D2D1::ColorF(7.0f / 255.0f, 193.0f / 255.0f, 96.0f / 255.0f, 0.32f);
            tokens.number = ColorFromRgb(0x858585);
            tokens.menuFill = ColorFromRgb(0x1F1F1F);
            tokens.menuBorder = ColorFromRgb(0x343434);
            tokens.menuText = ColorFromRgb(0xD0D0D0);
            tokens.menuHover = ColorFromRgb(0x2A2A2A);
        }
    }
    else if (skinId == "willow_green")
    {
        tokens.borderWidth = 0.0f;
        tokens.radius = 9.0f;
        tokens.containerPad = 0.0f;
        // 高亮整行（竖排）/整列（横排）铺满、左右贴着卡片边，行本身是直角；CSS 靠
        // 容器 clip-path 裁出窗口圆角。D2D 的 Card 不裁剪子控件，所以改由列表把
        // 落在卡片四角上的那几个角画成卡片的 9px，其余角保持直角。
        tokens.itemRadius = 0.0f;
        tokens.outerItemRadius = 9.0f;
        tokens.itemGap = 0.0f;
        // 行内边距对齐 CSS 的 .row.cand padding（加上 .text 的左内边距），预编辑行对齐 .row.pinyin。
        if (GetConfiguredCandidateWindowLayout() == "horizontal")
        {
            tokens.itemExtraHeight = 10.0f;
            tokens.itemPadLeft = 6.0f;
            tokens.itemPadRight = 10.0f;
            tokens.preeditMargin = {3.0f, 6.0f, 5.0f, 1.0f};
        }
        else
        {
            tokens.itemExtraHeight = 8.0f;
            tokens.itemPadLeft = 9.0f;
            tokens.itemPadRight = 14.0f;
            tokens.preeditMargin = {3.0f, 6.0f, 7.0f, 2.0f};
        }
        tokens.showSelectedBar = false;
        tokens.rowTextSelected = ColorFromRgb(0xFFFFFF);
        tokens.rowLabelSelected = ColorFromRgb(0xFFFFFF);
        if (candLight)
        {
            tokens.accent = ColorFromRgb(0x58B980);
            tokens.selected = ColorFromRgb(0x58B980);
            tokens.hover = D2D1::ColorF(88.0f / 255.0f, 185.0f / 255.0f, 128.0f / 255.0f, 0.16f);
            tokens.number = ColorFromRgb(0x686F6A);
            tokens.menuFill = ColorFromRgb(0xFBFCFA);
            tokens.menuBorder = ColorFromRgb(0xD8DED9);
            tokens.menuText = ColorFromRgb(0x343936);
            tokens.menuHover = ColorFromRgb(0xE9EEEA);
        }
        else
        {
            tokens.accent = ColorFromRgb(0x65C98D);
            tokens.selected = ColorFromRgb(0x65C98D);
            tokens.hover = D2D1::ColorF(101.0f / 255.0f, 201.0f / 255.0f, 141.0f / 255.0f, 0.22f);
            tokens.number = ColorFromRgb(0xA6ABA7);
            tokens.menuFill = ColorFromRgb(0x343635);
            tokens.menuBorder = ColorFromRgb(0x454845);
            tokens.menuText = ColorFromRgb(0xE0E2DF);
            tokens.menuHover = ColorFromRgb(0x414441);
        }
    }
    else if (skinId == "graphite")
    {
        tokens.borderWidth = 1.0f;
        tokens.radius = 3.0f;
        tokens.containerPad = 5.0f;
        tokens.itemRadius = 2.0f;
        tokens.selected = D2D1::ColorF(0, 0.0f);
        tokens.showSelectedBar = false;
        if (candLight)
        {
            tokens.accent = ColorFromRgb(0x5F6B7A);
            tokens.hover = D2D1::ColorF(31.0f / 255.0f, 41.0f / 255.0f, 55.0f / 255.0f, 0.055f);
            tokens.number = ColorFromRgb(0x8993A1);
            tokens.rowTextSelected = ColorFromRgb(0x111827);
            tokens.rowLabelSelected = ColorFromRgb(0x111827);
            tokens.menuFill = ColorFromRgb(0xFFFFFF);
            tokens.menuBorder = ColorFromRgb(0xDFE3E8);
            tokens.menuText = ColorFromRgb(0x374151);
            tokens.menuHover = ColorFromRgb(0xF1F3F5);
        }
        else
        {
            tokens.accent = ColorFromRgb(0x8993A0);
            tokens.hover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f);
            tokens.number = ColorFromRgb(0x707987);
            tokens.rowTextSelected = ColorFromRgb(0xF1F3F5);
            tokens.rowLabelSelected = ColorFromRgb(0xF1F3F5);
            tokens.menuFill = ColorFromRgb(0x23272C);
            tokens.menuBorder = ColorFromRgb(0x3A4047);
            tokens.menuText = ColorFromRgb(0xC7CDD5);
            tokens.menuHover = ColorFromRgb(0x30353B);
        }
    }
    else if (skinId == "autumn_osmanthus")
    {
        tokens.borderWidth = 0.0f;
        tokens.radius = 10.0f;
        tokens.containerPad = 5.0f;
        // 高亮块内缩在卡片里：普通角 6px，贴着卡片四角的那几个角与卡片同为 10px
        // （首项顶部两角、尾项底部两角），对应 CSS 的 --ao-radius / --ao-item-radius。
        tokens.itemRadius = 6.0f;
        tokens.outerItemRadius = 10.0f;
        tokens.showSelectedBar = false;
        if (candLight)
        {
            tokens.accent = ColorFromRgb(0xE6A817);
            tokens.selected = ColorFromRgb(0xFFE399);
            tokens.hover = D2D1::ColorF(255.0f / 255.0f, 227.0f / 255.0f, 153.0f / 255.0f, 0.55f);
            tokens.number = ColorFromRgb(0x5B727B);
            tokens.rowTextSelected = ColorFromRgb(0x1A1A1A);
            tokens.rowLabelSelected = ColorFromRgb(0x1A1A1A);
            tokens.menuFill = ColorFromRgb(0xE8F5F7);
            tokens.menuBorder = ColorFromRgb(0xBCD8DE);
            tokens.menuText = ColorFromRgb(0x1F3138);
            tokens.menuHover = ColorFromRgb(0xD2E8EC);
        }
        else
        {
            tokens.accent = ColorFromRgb(0xF97D0A);
            tokens.selected = ColorFromRgb(0xF97D0A);
            tokens.hover = D2D1::ColorF(249.0f / 255.0f, 125.0f / 255.0f, 10.0f / 255.0f, 0.30f);
            tokens.number = ColorFromRgb(0xE1E8EC);
            tokens.rowTextSelected = ColorFromRgb(0xFFFFFF);
            tokens.rowLabelSelected = ColorFromRgb(0xFFFFFF);
            tokens.menuFill = ColorFromRgb(0x6F8491);
            tokens.menuBorder = ColorFromRgb(0x8CA0AC);
            tokens.menuText = ColorFromRgb(0xF5F8FA);
            tokens.menuHover = ColorFromRgb(0x637885);
        }
    }

    const std::wstring skinsRoot = AssetRoot() + L"\\skins";
    std::optional<CandidateSkinCatalog::Package> package;
    if (!CandidateSkinCatalog::IsBuiltIn(skinId))
    {
        package = CandidateSkinCatalog::Load(std::filesystem::path(skinsRoot), skinId);
        if (package)
        {
            ApplyPackageColors(candLight ? package->light : package->dark, tokens);
        }
    }
    const CandidateSkinCatalog::CandidateColors *packageColors =
        package ? &(candLight ? package->light : package->dark) : nullptr;
    const CandidateSkinPalette palette = ResolveCandidateSkinPalette(
        skinId, candLight, GetConfiguredCandidateTextColor(), packageColors, package ? package->base : std::string{});
    tokens.surface = palette.surface;
    tokens.border = palette.border;
    tokens.text = palette.text;
    // 阴影跟皮肤走，与 WebView2 端同一参照：四套内置皮肤与自定义包共用的皮肤 CSS
    // （ui-html/webview2/candwnd/skins/*/horizontal_*.css）带同一对 box-shadow——
    // light `8px 10px 24px rgba(0,0,0,.18), 2px 3px 8px rgba(0,0,0,.10)`，dark α .34/.22；
    // σ = blur ÷ 2。默认皮肤（webview 端只有 body HTML，无 box-shadow）直接关阴影。
    if (impl_->card)
    {
        if (CandidateSkinCatalog::IsBuiltIn(skinId) || package)
        {
            // 暂不解析自定义包内 CSS 的 box-shadow，先沿用内置皮肤的标准对，后续可加。
            const std::vector<msimeui::ShadowPass> shadowPasses = {
                {12.0f, candLight ? 0.18f : 0.34f, 8.0f, 10.0f},
                {4.0f, candLight ? 0.10f : 0.22f, 2.0f, 3.0f},
            };
            impl_->card->SetShadowEnabled(true);
            impl_->card->SetShadowPasses(shadowPasses);
        }
        else
        {
            impl_->card->SetShadowEnabled(false);
        }
    }

    msimeui::Theme theme = msimeui::ThemeManager::GetCurrent();
    theme.textInputFontFamily = string_to_wstring(GetConfiguredCandidateFont());
    if (theme.textInputFontFamily.empty())
    {
        theme.textInputFontFamily = L"Microsoft YaHei UI";
    }
    theme.uiFontFamily = theme.textInputFontFamily;
    theme.surface = tokens.surface;
    theme.border = tokens.border;
    theme.textPrimary = palette.text;
    theme.textSecondary = tokens.number;
    theme.primary = tokens.accent;
    theme.windowBackground = D2D1::ColorF(0, 0.0f);
    msimeui::ThemeManager::SetCurrent(theme);

    const float fontSize = static_cast<float>((std::max)(12, GetConfiguredCandidateFontSize()));
    const float preeditSize = static_cast<float>((std::max)(12, GetConfiguredCandidateWindowPreeditFontSize()));
    msimeui::CandidateList::Appearance appearance;
    appearance.fontFamily = string_to_wstring(ResolveSystemFontFamilyForCss(GetConfiguredCandidateEnglishFont()));
    for (const auto &font : GetConfiguredCandidateFallbackFontFamilies())
        appearance.fallbackFontFamilies.push_back(string_to_wstring(font));
    appearance.itemHeight = fontSize * 1.35f + tokens.itemExtraHeight;
    appearance.itemGap = tokens.itemGap;
    appearance.fontSize = fontSize;
    appearance.labelFontSize = fontSize * 0.8f;
    appearance.annotationFontSize = fontSize;
    appearance.contentPadLeft = tokens.itemPadLeft;
    appearance.contentPadRight = tokens.itemPadRight;
    appearance.textPadLeft = 5.0f;
    appearance.labelGap = 1.5f;
    appearance.selectedBarWidth = 3.0f;
    appearance.selectedBarHeight = fontSize * 0.85f;
    appearance.showSelectedBar = tokens.showSelectedBar;
    appearance.selectedBarColor = tokens.accent;
    appearance.cornerRadius = tokens.itemRadius;
    // 预编辑行隐藏时列表顶边才贴着卡片顶边，与 CSS 的 .preedit-hidden 同一判据。
    const bool preeditHidden = GetConfiguredCandidateWindowPreeditStyle() == "empty";
    appearance.outerCornerRadius = tokens.outerItemRadius;
    appearance.outerTopCornersEnabled = preeditHidden;
    // 隐藏的预编辑行高度为 0，但 body 仍在它和列表之间留 2px 行距；贴角皮肤把它抵掉，
    // 让首项贴着卡片的内容顶边（秋桂四边内边距一致，杨柳青高亮直抵卡片顶边）。
    if (preeditHidden)
        impl_->preedit->SetMargin({0.0f, 0.0f, 0.0f, tokens.outerItemRadius > 0.0f ? -2.0f : 0.0f});
    else
        impl_->preedit->SetMargin(tokens.preeditMargin);
    appearance.textColor = theme.textPrimary;
    appearance.labelColor = tokens.number;
    appearance.annotationColor = theme.textPrimary;
    appearance.rowFillSelected = tokens.selected;
    appearance.rowFillHover = tokens.hover;
    appearance.rowFillPressed = tokens.selected;
    appearance.rowTextSelected = tokens.rowTextSelected;
    appearance.rowLabelSelected = tokens.rowLabelSelected;
    impl_->menuFill = tokens.menuFill;
    impl_->menuBorder = tokens.menuBorder;
    impl_->menuText = tokens.menuText;
    impl_->menuHover = tokens.menuHover;
    impl_->list->SetAppearance(appearance);
    impl_->list->SetOrientation(GetConfiguredCandidateWindowLayout() == "horizontal"
                                    ? msimeui::CandidateList::Orientation::Horizontal
                                    : msimeui::CandidateList::Orientation::Vertical);
    impl_->preedit->SetFontFamily(appearance.fontFamily);
    impl_->preedit->SetFallbackFontFamilies(appearance.fallbackFontFamilies);
    impl_->preedit->SetFontSize(preeditSize);
    impl_->preedit->SetColor(theme.textPrimary);
    impl_->preedit->SetCaretColor(tokens.accent);
    impl_->preedit->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    impl_->preedit->SetTextLayoutPadding({5.0f, 0.0f, 5.0f, 0.0f});

    decorationTopDip_ = 0.0f;
    decorationWidthDip_ = 0.0f;
    std::wstring decorationPath;
    if (package)
    {
        decorationTopDip_ = static_cast<float>(package->decorationTopDip);
        decorationWidthDip_ = static_cast<float>(package->decorationWidthDip);
        if (!package->preview.empty())
        {
            decorationPath =
                skinsRoot + L"\\" + string_to_wstring(package->id) + L"\\" + string_to_wstring(package->preview);
        }
    }
    if (decorationPath.empty() || decorationTopDip_ <= 0.0f)
    {
        impl_->decoration->SetHeight(0.0f);
        impl_->decoration->ClearWidth();
        impl_->decoration->SetSource(L"");
        impl_->decoration->SetMargin({0.0f, 0.0f, 0.0f, 0.0f});
    }
    else
    {
        impl_->decoration->SetSource(decorationPath);
        impl_->decoration->SetHeight(decorationTopDip_);
        if (decorationWidthDip_ > 0.0f)
        {
            impl_->decoration->SetWidth(decorationWidthDip_);
        }
        impl_->decoration->SetStretch(msimeui::ImageStretch::Uniform);
        impl_->decoration->SetMargin({0.0f, 0.0f, 0.0f, -(kShadowPadTop + kDecorationCardOverlap)});
    }

    msimeui::Brush brush;
    brush.fill = tokens.surface;
    brush.stroke = tokens.border;
    brush.strokeWidth = tokens.borderWidth;
    brush.radiusX = tokens.radius;
    brush.radiusY = tokens.radius;
    if (impl_->card)
    {
        impl_->card->SetBrush(brush);
        impl_->card->SetPadding(tokens.containerPad);
        impl_->card->SetMinWidth(kCandidateMinWidthDip);
    }
}

std::uint64_t CandidatePresenter::FillItemsFromUi()
{
    const Global::CandidatePageSnapshotPtr page = Global::LoadCandidatePageSnapshot();
    std::vector<msimeui::CandidateList::Item> items;
    for (size_t i = 0; i < page->page_views.size(); ++i)
    {
        const auto &view = page->page_views[i];
        msimeui::CandidateList::Item item;
        item.label = std::to_wstring(i + 1);
        item.text = string_to_wstring(view.text + view.badge);
        item.annotation = string_to_wstring(view.annotation);
        if (GetConfiguredCandidateTranslationsEnabled())
            item.translation = string_to_wstring(view.translation);
        items.push_back(std::move(item));
    }
    ignoreSelectionCallback_ = true;
    impl_->list->SetItems(std::move(items));
    impl_->list->SetSelectedIndex(static_cast<size_t>((std::max)(0, page->selected_index_in_page)));
    ignoreSelectionCallback_ = false;
    return page->generation;
}

void CandidatePresenter::CommitItem(size_t pageIndex)
{
    if (!hwnd_)
    {
        return;
    }
    PostMessageW(hwnd_, WM_COMMIT_CANDIDATE, static_cast<WPARAM>(pageIndex + 1), 0);
}

namespace
{
constexpr float kContextMenuWidthDip = 108.0f;
constexpr float kContextSubmenuWidthDip = 96.0f;

void StyleContextPopup(msimeui::Popup &popup, const D2D1_COLOR_F &fill, const D2D1_COLOR_F &border, float width)
{
    popup.SetMatchAnchorWidth(false);
    popup.SetWidth(width);
    popup.SetPadding({2.0f, 2.0f, 2.0f, 2.0f});
    popup.SetBackgroundFill(fill);
    popup.SetBorderColor(border);
    popup.SetCornerRadius(6.0f);
    popup.SetShadowEnabled(true);
    popup.SetOffset(0.0f, 0.0f);
    popup.SetConstrainToViewport(true);
}
} // namespace

void CandidatePresenter::ShowItemContextMenu(size_t pageIndex, POINT clientPoint)
{
    if (!hwnd_ || !impl_ || !impl_->list || !impl_->window)
    {
        return;
    }

    CloseContextMenu(true);
    impl_->contextMenuPageIndex = pageIndex;

    // A mouse callback has no posted-message handoff at all, so it must not walk the worker's live page either.
    const Global::CandidatePageSnapshotPtr page = Global::LoadCandidatePageSnapshot();
    std::wstring word;
    if (pageIndex < page->page_words.size())
    {
        word = page->page_words[pageIndex];
    }
    size_t codePoints = 0;
    for (size_t i = 0; i < word.size(); ++i)
    {
        ++codePoints;
        if (i + 1 < word.size() && IS_HIGH_SURROGATE(word[i]) && IS_LOW_SURROGATE(word[i + 1]))
        {
            ++i;
        }
    }
    const bool showDelete = codePoints != 1;
    const WPARAM oneBased = static_cast<WPARAM>(pageIndex + 1);

    auto makeItem = [this](const std::wstring &text, bool hasSubmenu, std::function<void()> onClick) {
        auto item = std::make_shared<msimeui::MenuFlyoutItem>(text, hasSubmenu);
        item->SetColors(impl_->menuText, impl_->menuHover);
        if (onClick)
        {
            item->SetOnClick(std::move(onClick));
        }
        return item;
    };

    auto stack = std::make_shared<msimeui::StackPanel>(0.0f);
    auto pin = makeItem(L"置顶", false, [this, oneBased]() {
        CloseContextMenu(true);
        PostMessageW(hwnd_, WM_PIN_TO_TOP_CANDIDATE, oneBased, 0);
    });
    pin->SetOnHover([this](bool hovered) {
        if (hovered)
        {
            CloseFixSubmenu();
        }
    });
    impl_->fixPositionItem = makeItem(L"固定排位", true, {});
    impl_->fixPositionItem->SetOnHover([this](bool hovered) {
        if (hovered)
        {
            OpenFixSubmenu();
        }
    });
    stack->AddChild(pin);
    stack->AddChild(impl_->fixPositionItem);
    if (showDelete)
    {
        auto del = makeItem(L"删除", false, [this, oneBased]() {
            CloseContextMenu(true);
            PostMessageW(hwnd_, WM_DELETE_CANDIDATE, oneBased, 0);
        });
        del->SetOnHover([this](bool hovered) {
            if (hovered)
            {
                CloseFixSubmenu();
            }
        });
        stack->AddChild(del);
    }

    auto subStack = std::make_shared<msimeui::StackPanel>(0.0f);
    for (int position = 1; position <= 5; ++position)
    {
        const std::wstring label = L"第 " + std::to_wstring(position) + L" 位";
        subStack->AddChild(makeItem(label, false, [this, oneBased, position]() {
            CloseContextMenu(true);
            PostMessageW(hwnd_, WM_FIX_CANDIDATE_POSITION, oneBased, position);
        }));
    }
    auto separator = std::make_shared<msimeui::MenuSeparator>();
    separator->SetColor(D2D1::ColorF(impl_->menuText.r, impl_->menuText.g, impl_->menuText.b, 0.125f));
    subStack->AddChild(separator);
    subStack->AddChild(makeItem(L"取消固定", false, [this, oneBased]() {
        CloseContextMenu(true);
        PostMessageW(hwnd_, WM_CLEAR_CANDIDATE_POSITION, oneBased, 0);
    }));

    impl_->contextMenu = std::make_shared<msimeui::Popup>(stack);
    StyleContextPopup(*impl_->contextMenu, impl_->menuFill, impl_->menuBorder, kContextMenuWidthDip);
    impl_->contextSubmenu = std::make_shared<msimeui::Popup>(subStack);
    StyleContextPopup(*impl_->contextSubmenu, impl_->menuFill, impl_->menuBorder, kContextSubmenuWidthDip);

    const msimeui::PointF anchor = impl_->window->ClientPixelsToDips(clientPoint);
    impl_->contextMenu->SetAnchorRect({anchor.x, anchor.y, 1.0f, 1.0f});
    ExpandHostForMenu(clientPoint);

    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        scene->AddPopup(impl_->contextMenu, [this]() { CloseContextMenu(true); });
        impl_->contextMenuOpen = true;
    }
    Present();
}

void CandidatePresenter::CloseContextMenu(bool restoreHost)
{
    if (!impl_ || !impl_->window)
    {
        return;
    }

    // ShowFromGlobalState and Hide call this on every keystroke, so the hover
    // teardown and the repaint below only run when a menu is actually up.
    const bool hadPopup = impl_->contextMenu || impl_->contextSubmenu;
    if (hadPopup)
    {
        // Window caches the hovered and focused Visual as raw pointers. They
        // point into the popup destroyed a few lines below, so they have to be
        // dropped first or the next mouse message dispatches through a dangling
        // pointer.
        impl_->window->DispatchImportedMessage(WM_MOUSELEAVE, 0, 0);
        impl_->window->FocusVisual(nullptr);
    }

    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        if (impl_->contextSubmenu)
        {
            scene->RemovePopup(impl_->contextSubmenu.get(), false);
        }
        if (impl_->contextMenu)
        {
            scene->RemovePopup(impl_->contextMenu.get(), false);
        }
    }
    impl_->contextMenuOpen = false;
    impl_->contextSubmenuOpen = false;
    impl_->fixPositionItem.reset();
    impl_->contextMenu.reset();
    impl_->contextSubmenu.reset();
    if (restoreHost)
    {
        RestoreHostAfterMenu();
    }
    else
    {
        impl_->hostExpandedForMenu = false;
    }
    if (hadPopup)
    {
        Present();
    }
}

void CandidatePresenter::OpenFixSubmenu()
{
    if (!impl_ || !impl_->window || !impl_->contextSubmenu || !impl_->fixPositionItem || impl_->contextSubmenuOpen)
    {
        return;
    }
    const msimeui::RectF anchor = impl_->fixPositionItem->GetBounds();
    if (anchor.width < 1.0f || anchor.height < 1.0f)
    {
        return;
    }
    impl_->contextSubmenu->SetAnchorRect(anchor);
    impl_->contextSubmenu->SetOffset(anchor.width - 2.0f, -anchor.height);
    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        scene->AddPopup(impl_->contextSubmenu, [this]() { impl_->contextSubmenuOpen = false; });
        impl_->contextSubmenuOpen = true;
    }
}

void CandidatePresenter::CloseFixSubmenu()
{
    if (!impl_ || !impl_->window || !impl_->contextSubmenu || !impl_->contextSubmenuOpen)
    {
        return;
    }
    // The submenu visual outlives this call (contextSubmenu keeps it alive for
    // a later reopen), but it leaves the scene here, so the cached hover has to
    // go with it. Focus stays where it is: the parent menu is still open.
    impl_->window->DispatchImportedMessage(WM_MOUSELEAVE, 0, 0);
    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        scene->RemovePopup(impl_->contextSubmenu.get(), false);
    }
    impl_->contextSubmenuOpen = false;
    Present();
}

void CandidatePresenter::ExpandHostForMenu(POINT clientPoint)
{
    if (!hwnd_ || !impl_ || !impl_->window)
    {
        return;
    }
    RECT windowRect{};
    RECT clientRect{};
    GetWindowRect(hwnd_, &windowRect);
    GetClientRect(hwnd_, &clientRect);
    if (!impl_->hostExpandedForMenu)
    {
        impl_->hostRectBeforeMenu = windowRect;
        impl_->hostExpandedForMenu = true;
    }

    const float dpi = impl_->window->GetDpi();
    const msimeui::PointF anchor = impl_->window->ClientPixelsToDips(clientPoint);
    const msimeui::SizeF clientDip = impl_->window->ClientPixelsToDips(SIZE{clientRect.right, clientRect.bottom});
    const float needWidth =
        (std::max)(clientDip.width, anchor.x + kContextMenuWidthDip + kContextSubmenuWidthDip + 16.0f);
    const float needHeight = (std::max)(clientDip.height, (std::max)(anchor.y + 168.0f, 176.0f));
    const int widthPx = (std::max)(static_cast<int>(windowRect.right - windowRect.left),
                                   static_cast<int>(std::ceil(needWidth * dpi / 96.0f)));
    const int heightPx = (std::max)(static_cast<int>(windowRect.bottom - windowRect.top),
                                    static_cast<int>(std::ceil(needHeight * dpi / 96.0f)));
    if (widthPx == windowRect.right - windowRect.left && heightPx == windowRect.bottom - windowRect.top)
    {
        return;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, windowRect.left, windowRect.top, widthPx, heightPx,
                 SWP_NOACTIVATE | SWP_NOZORDER);
}

void CandidatePresenter::RestoreHostAfterMenu()
{
    if (!hwnd_ || !impl_ || !impl_->hostExpandedForMenu)
    {
        return;
    }
    const RECT &rc = impl_->hostRectBeforeMenu;
    SetWindowPos(hwnd_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    impl_->hostExpandedForMenu = false;
}

void CandidatePresenter::PlaceAndShow(POINT caret, float widthDip, float heightDip, float cardLeftDip, float cardTopDip,
                                      const ResolvedCandidateScale &resolved)
{
    FLOAT scale = resolved.scale;
    CandidateScaleSource scaleSource = resolved.source;
    if (scale <= 0.0f)
    {
        const ResolvedCandidateScale now = ResolveCandidateScaleForCaret(caret);
        scale = now.scale;
        scaleSource = now.source;
    }
    if (scale <= 0.0f)
    {
        scale = 1.0f;
        scaleSource = CandidateScaleSource::Monitor;
    }
    const HalfScreenDipLimits limits = ApplyScaleToHalfScreenLimits(caret, scale);
    widthDip = static_cast<float>(ClampWidthDipToHalfScreen(widthDip, limits));
    heightDip = static_cast<float>(ClampHeightDipToHalfScreen(heightDip, limits));
    lastLayoutWidthDip_ = widthDip;
    lastLayoutHeightDip_ = heightDip;
    cardLeftDip = (std::max)(0.0f, cardLeftDip);
    cardTopDip = (std::max)(0.0f, (std::min)(cardTopDip, heightDip));
    float cardWidthDip = (std::max)(widthDip - cardLeftDip - kShadowPadRight, 1.0f);
    float cardHeightDip = (std::max)(heightDip - cardTopDip - kShadowPadBottom, 1.0f);
    if (impl_->card)
    {
        const msimeui::RectF card = impl_->card->GetBounds();
        if (card.width > 1.0f)
        {
            cardWidthDip = card.width;
        }
        if (card.height > 1.0f)
        {
            cardHeightDip = card.height;
        }
    }
    auto properPos = std::make_shared<std::pair<int, int>>();
    // Position the opaque card, not the decoration or drop shadow around it.
    AdjustCandidateWindowPosition(&caret, {cardWidthDip, cardHeightDip}, properPos, scale, cardWidthDip);
    int widthPx = AlignHostPixels((std::max)(1, static_cast<int>(std::ceil(widthDip * scale))));
    int heightPx = AlignHostPixels((std::max)(1, static_cast<int>(std::ceil(heightDip * scale))));
    widthPx = (std::max)(widthPx, lastHostWidthPx_);
    heightPx = (std::max)(heightPx, lastHostHeightPx_);
    lastHostWidthPx_ = widthPx;
    lastHostHeightPx_ = heightPx;
    const int cardLeftPx = static_cast<int>(std::lround(cardLeftDip * scale));
    const int cardTopPx = static_cast<int>(std::lround(cardTopDip * scale));
    const int cardHeightPx = static_cast<int>(std::lround(cardHeightDip * scale));
    int x = properPos->first - cardLeftPx;
    int y = properPos->second - cardTopPx;
    const MonitorCoordinates monitor = GetMonitorCoordinatesFromPoint(caret);
    // Clamp the opaque card, not the host window — same rule the vertical branch
    // below already follows. The host carries transparent shadow padding, is
    // rounded up to a 64px bucket and only ever grows while typing, so clamping
    // *its* right edge shoved the card left by all of that slack. AdjustCandidate-
    // WindowPosition had already parked the card flush against the screen edge;
    // deleting a character kept the stale (larger) host width, so the card's left
    // edge froze and its right edge drifted further from the edge on every delete.
    const int cardWidthPx = (std::max)(1, static_cast<int>(std::ceil(cardWidthDip * scale)));
    if (x + cardLeftPx + cardWidthPx > monitor.right)
    {
        x = monitor.right - cardLeftPx - cardWidthPx - 2;
    }
    if (x + cardLeftPx < monitor.left)
    {
        x = monitor.left + 2 - cardLeftPx;
    }
    const int cardBottom = y + cardTopPx + cardHeightPx;
    if (cardBottom > monitor.bottom)
    {
        y = monitor.bottom - cardTopPx - cardHeightPx - 2;
    }
    // Allow the mascot to clip above the work area so the card stays on the caret.
    if (y + cardTopPx < monitor.top)
    {
        y = monitor.top + 2 - cardTopPx;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, widthPx, heightPx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // Rendering must use the same scale the window was just sized with: the ui
    // override replaces GetDpiForWindow (144 inside RDP) so DIPs map exactly
    // onto the pixels computed above. Must land before EnsureForComposition —
    // a freshly created target reads it through DpiForHwnd().
    const FLOAT dpiOverride = scale * 96.0f;
    impl_->window->SetDpiOverride(dpiOverride);
    impl_->resources.SetDpiOverride(dpiOverride);
    impl_->resources.EnsureForComposition(hwnd_);
    Present();
    SetCandidateHostCloaked(false);
    // Acceptance trace for the RDP candidate-scale fix: which scale authority
    // won, and the system's own (potentially diverging) DPI values. Coordinates
    // and DPI only — never user input, per the diagnostic logging red lines.
    CAND_DIAG_LOGF(L"candidate-d2d place source={} scale={:.3f} hwnd_dpi={} system_dpi={} remote={} caret=({},{}) "
                   L"size_px=({},{})",
                   scaleSource == CandidateScaleSource::RdpForeground ? L"rdp-foreground" : L"monitor", scale,
                   GetDpiForWindow(hwnd_), GetDpiForSystem(), GetSystemMetrics(SM_REMOTESESSION) ? 1 : 0, caret.x,
                   caret.y, widthPx, heightPx);
}

void CandidatePresenter::ShowFromGlobalState()
{
    ShowFromGlobalState(POINT{Global::Point[0], Global::Point[1]});
}

void CandidatePresenter::ShowFromGlobalState(POINT caret)
{
    if (!bound_ || !hwnd_ || !impl_ || !impl_->root)
    {
        return;
    }
    // A composition can start before the host reports a usable text extent
    // (first focus, or resuming after a long idle). The caret then arrives as
    // (0, INVALID_Y), which placement would clamp into the monitor's work area.
    // Hide even an already-visible host until a real anchor arrives. Preserve
    // the show request so MoveCandidate / the settle timer can place it later.
    if (caret.y == Global::INVALID_Y)
    {
        CloseContextMenu(false);
        SetCandidateHostCloaked(true);
        ::is_global_wnd_cand_shown = true;
        Global::candidate_window_rendered_visible.store(true, std::memory_order_relaxed);
        CAND_DIAG_LOGF(L"candidate-d2d show deferred: no usable caret anchor ({},{})", caret.x, caret.y);
        return;
    }
    CloseContextMenu(false);
    ApplySkin();
    std::wstring preedit;
    size_t caretIndex = 0;
    if (GetConfiguredCandidateWindowPreeditStyle() != "empty")
    {
        preedit = GetPreeditWithCaretMarker();
        const size_t marker = preedit.find(L'\uE000');
        if (marker != std::wstring::npos)
        {
            caretIndex = marker;
            preedit.erase(marker, 1);
        }
        else
        {
            caretIndex = preedit.size();
        }
    }
    impl_->preedit->SetText(preedit);
    if (preedit.empty() && GetConfiguredCandidateWindowPreeditStyle() == "empty")
    {
        impl_->preedit->ClearCaret();
        impl_->preedit->SetHeight(0.0f);
    }
    else
    {
        impl_->preedit->ClearHeight();
        impl_->preedit->SetCaretIndex(caretIndex);
    }
    const std::uint64_t renderedGeneration = FillItemsFromUi();
    impl_->list->SetHoverEnabled(hoverArmed_);
    // Resolve the scale once per show and thread it through measure, clamping,
    // window sizing and rendering DPI — a foreground window changing mid-show
    // must not split measure (clamped at one scale) from placement (another).
    const ResolvedCandidateScale scale = ResolveCandidateScaleForCaret(caret);
    const HalfScreenDipLimits limits = ApplyScaleToHalfScreenLimits(caret, scale.scale);
    const float maxW = limits.maxWidthDip > 1.0 ? static_cast<float>(limits.maxWidthDip) : 480.0f;
    const float maxH = limits.maxHeightDip > 1.0 ? static_cast<float>(limits.maxHeightDip) : 640.0f;
    impl_->root->InvalidateMeasure();
    const msimeui::SizeF measured = impl_->root->MeasureInLayout({maxW, maxH});
    float widthDip = (std::max)(measured.width, kCandidateMinWidthDip + kShadowPadLeft + kShadowPadRight);
    float heightDip = (std::max)(measured.height, 36.0f);
    impl_->root->InvalidateArrange();
    impl_->root->ArrangeInLayout({0.0f, 0.0f, widthDip, heightDip});
    hoverArmed_ = false;
    if (!GetCursorPos(&hoverBaseline_))
    {
        hoverBaseline_ = {};
    }
    float cardLeftDip = 0.0f;
    float cardTopDip = 0.0f;
    if (impl_->card)
    {
        const msimeui::RectF card = impl_->card->GetBounds();
        cardLeftDip = card.x;
        cardTopDip = card.y;
    }
    PlaceAndShow(caret, widthDip, heightDip, cardLeftDip, cardTopDip, scale);
    ::is_global_wnd_cand_shown = true;
    Global::candidate_window_rendered_visible.store(true, std::memory_order_relaxed);
    // PlaceAndShow paints synchronously, so the page is on screen now. Echo the generation of the
    // snapshot that was actually rendered (not the latest published one) so a pending digit/space
    // selection can settle against the same page the user is looking at.
    Global::rendered_candidate_page_generation.store(renderedGeneration, std::memory_order_release);
}

void CandidatePresenter::Hide()
{
    if (!hwnd_)
    {
        return;
    }
    CloseContextMenu(false);
    ::is_global_wnd_cand_shown = false;
    Global::candidate_window_rendered_visible.store(false, std::memory_order_relaxed);
    hoverArmed_ = false;
    wheelDeltaAccumulator_ = 0;
    if (impl_ && impl_->list)
    {
        impl_->list->SetHoverEnabled(false);
    }
    SetCandidateHostCloaked(true);
    lastHostWidthPx_ = 0;
    lastHostHeightPx_ = 0;
    SetWindowPos(hwnd_, nullptr, 0, Global::INVALID_Y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CandidatePresenter::Present()
{
    if (!bound_ || !hwnd_ || !impl_ || !impl_->window)
    {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT width = static_cast<UINT>((std::max)(1L, rc.right));
    const UINT height = static_cast<UINT>((std::max)(1L, rc.bottom));
    if (!impl_->resources.EnsureForComposition(hwnd_))
    {
        return;
    }
    impl_->resources.Resize(width, height);
    ID2D1RenderTarget *target = impl_->resources.GetRenderTarget();
    if (!target)
    {
        return;
    }
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    if (msimeui::Scene *scene = impl_->window->GetScene())
    {
        const float dpi = impl_->window->GetDpi();
        float layoutW = msimeui::PixelsToDips(static_cast<float>(width), dpi);
        float layoutH = msimeui::PixelsToDips(static_cast<float>(height), dpi);
        // Host HWND is larger than the card (shadow padding, 64px buckets, and a
        // grow-only size while typing). Stretching the scene to that HWND fills
        // the rounded card with empty space. Keep layout at the measured DIP size.
        if (!impl_->hostExpandedForMenu && lastLayoutWidthDip_ > 0.0f && lastLayoutHeightDip_ > 0.0f)
        {
            layoutW = lastLayoutWidthDip_;
            layoutH = lastLayoutHeightDip_;
        }
        scene->EnsureLayout({layoutW, layoutH});
        scene->Render(impl_->resources);
    }
    const HRESULT hr = target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        impl_->resources.DiscardTarget();
        return;
    }
    impl_->resources.Present();
}

void CandidatePresenter::ArmHoverIfPointerMoved()
{
    POINT now{};
    if (!GetCursorPos(&now))
    {
        return;
    }
    const int dx = now.x - hoverBaseline_.x;
    const int dy = now.y - hoverBaseline_.y;
    if (dx * dx + dy * dy < 4)
    {
        return;
    }
    hoverArmed_ = true;
    if (impl_ && impl_->list)
    {
        impl_->list->SetHoverEnabled(true);
    }
}

bool CandidatePresenter::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!bound_ || !impl_ || !impl_->window)
    {
        return false;
    }
    switch (message)
    {
    case WM_ERASEBKGND:
        return true;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Present();
        EndPaint(hwnd_, &ps);
        return true;
    }
    case WM_SIZE:
        if (::is_global_wnd_cand_shown)
        {
            impl_->resources.Resize(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
            Present();
        }
        return true;
    case WM_MOUSEMOVE:
        ArmHoverIfPointerMoved();
        if (!hoverArmed_ && !(impl_ && impl_->contextMenuOpen))
        {
            return true;
        }
        impl_->window->DispatchImportedMessage(message, wParam, lParam);
        Present();
        return true;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MOUSELEAVE:
        impl_->window->DispatchImportedMessage(message, wParam, lParam);
        Present();
        return true;
    case WM_MOUSEWHEEL: {
        if (!GetConfiguredPagingMouseWheelEnabled())
        {
            // Leave the message to DefWindowProc, exactly as before the feature
            // existed. WM_PAGE_CANDIDATE checks the same setting for both hosts;
            // this early-out only keeps a disabled wheel from being swallowed.
            wheelDeltaAccumulator_ = 0;
            return false;
        }
        if (!::is_global_wnd_cand_shown)
        {
            // Hide() only parks the host off-screen, so a wheel message posted
            // just before the page went away can still arrive here.
            wheelDeltaAccumulator_ = 0;
            return true;
        }
        if (impl_->contextMenuOpen)
        {
            // Scrolling dismisses the open context menu and consumes the event.
            wheelDeltaAccumulator_ = 0;
            CloseContextMenu(true);
            return true;
        }
        const CandidateWheel::PagingSteps steps =
            CandidateWheel::ConsumeWheelDelta(wheelDeltaAccumulator_, GET_WHEEL_DELTA_WPARAM(wParam), WHEEL_DELTA);
        // Route through the host WndProc like every other candidate UI action
        // instead of reaching into the IPC layer from the window layer.
        if (steps.page_up > 0)
        {
            PostMessageW(hwnd_, WM_PAGE_CANDIDATE, CANDIDATE_PAGE_PREVIOUS, steps.page_up);
        }
        if (steps.page_down > 0)
        {
            PostMessageW(hwnd_, WM_PAGE_CANDIDATE, CANDIDATE_PAGE_NEXT, steps.page_down);
        }
        return true;
    }
    default:
        return false;
    }
}

CandidatePresenter::~CandidatePresenter() = default;
