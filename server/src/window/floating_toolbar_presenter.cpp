#include "window/floating_toolbar_presenter.h"

#include "config/ime_config.h"
#include "defines/globals.h"
#include "global/globals.h"
#include "ipc/event_listener.h"
#include "ipc/ipc.h"
#include "log/ftb_diag_log.h"
#include "settings/settings_launcher.h"
#include "skin/candidate_skin_catalog.h"
#include "utils/common_utils.h"
#include "utils/window_utils.h"
#include "webview2/windows_webview2.h"
#include "window/floating_toolbar_skin.h"

#include "msimeui/Controls.h"
#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Layout.h"
#include "msimeui/Scene.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <d2d1.h>
#include <dwrite.h>
#include <filesystem>
#include <functional>
#include <string>
#include <windowsx.h>
#include <wrl/client.h>

using msimeui::PointF;
using msimeui::RectF;
using msimeui::SizeF;

namespace
{
D2D1_COLOR_F ColorFromRgb(UINT rgb, float alpha = 1.0f)
{
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

constexpr float kShadowPadLeft = 18.0f;
constexpr float kShadowPadTop = 16.0f;
constexpr float kShadowPadRight = 18.0f;
constexpr float kShadowPadBottom = 20.0f;
constexpr float kToolbarGlyphFontSizeFactor = 0.82f;
constexpr float kToolbarUnderlinedTextFontSizeFactor = 0.58f;
// Every toolbar icon carries a text fallback: "Segoe Fluent Icons" ships with
// Windows 11 only, and the Windows 10 build of "Segoe MDL2 Assets" may predate
// some of the IME-specific codepoints below. DirectWrite substitutes a font
// silently rather than failing, so without this the button renders as a blank
// box (issue #232).
struct ToolbarIcon
{
    wchar_t codepoint;
    const wchar_t *fallbackText;
};

constexpr ToolbarIcon kIconJa = {0xE7DE, L"日"};
constexpr ToolbarIcon kIconCn = {0xE982, L"中"};
constexpr ToolbarIcon kIconEn = {0xE983, L"英"};
constexpr ToolbarIcon kIconHalfWidth = {0xEC46, L"半"};
constexpr ToolbarIcon kIconFullWidth = {0xF138, L"全"};
constexpr ToolbarIcon kIconPuncEn = {0xF110, L","};
constexpr ToolbarIcon kIconPuncCn = {0xF111, L"。"};
constexpr ToolbarIcon kIconSimplified = {0xE88D, L"简"};
constexpr ToolbarIcon kIconTraditional = {0xE88C, L"繁"};
constexpr ToolbarIcon kIconEmoji = {0xE76E, L"表"};
constexpr ToolbarIcon kIconKeyboard = {0xE765, L"键"};
constexpr ToolbarIcon kIconSettings = {0xE713, L"设"};

float ToolbarUserScale()
{
    const float scale = static_cast<float>(GetConfiguredFloatingToolbarScale());
    const float fontFactor = static_cast<float>(GetConfiguredFloatingToolbarFontSize()) / 24.0f;
    const float combined = (scale > 0.0f ? scale : 1.0f) * (fontFactor > 0.0f ? fontFactor : 1.0f);
    return combined > 0.05f ? combined : 1.0f;
}

class ToolbarIconButton : public msimeui::Visual
{
  public:
    using ClickHandler = std::function<void()>;

    ToolbarIconButton(wchar_t glyph, std::wstring glyphFamily, std::wstring text, float size)
        : glyph_(glyph), glyphFamily_(std::move(glyphFamily)), text_(std::move(text)), size_(size)
    {
        SetWidth(size_);
        SetHeight(size_);
    }

    void SetOnClick(ClickHandler handler)
    {
        onClick_ = std::move(handler);
    }

    void SetColors(const D2D1_COLOR_F &glyph, const D2D1_COLOR_F &hover, float hoverRadius)
    {
        glyphColor_ = glyph;
        hoverFill_ = hover;
        hoverRadius_ = hoverRadius;
        InvalidateVisual();
    }

    void SetUnderline(bool underline)
    {
        underline_ = underline;
        InvalidateVisual();
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        (void)availableSize;
        return {size_, size_};
    }

    void Arrange(const msimeui::RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &deviceResources) override
    {
        ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
        if (!target)
        {
            return;
        }
        if (hovered_ || pressed_)
        {
            if (ID2D1SolidColorBrush *hover = deviceResources.GetSolidColorBrush(hoverFill_))
            {
                const float radius = std::min(hoverRadius_, size_ * 0.5f);
                const auto rounded = D2D1::RoundedRect(
                    D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height), radius,
                    radius);
                target->FillRoundedRectangle(rounded, hover);
            }
        }

        IDWriteFactory *factory = deviceResources.GetDWriteFactory();
        ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(glyphColor_);
        if (!factory || !brush)
        {
            return;
        }

        const bool useGlyph = glyph_ != 0 && !glyphFamily_.empty();
        const std::wstring family = useGlyph ? glyphFamily_ : std::wstring(msimeui::UiFontFamily());
        const float fontSize =
            size_ * (underline_ ? kToolbarUnderlinedTextFontSizeFactor : kToolbarGlyphFontSizeFactor);
        IDWriteTextFormat *format =
            deviceResources.GetTextFormat(family, fontSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER,
                                          DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!format)
        {
            return;
        }

        wchar_t glyphText[2] = {glyph_, L'\0'};
        const wchar_t *drawText = useGlyph ? glyphText : text_.c_str();
        const UINT32 length = useGlyph ? 1u : static_cast<UINT32>(text_.size());
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (FAILED(factory->CreateTextLayout(drawText, length, format, bounds_.width, bounds_.height, &layout)))
        {
            return;
        }
        target->DrawTextLayout(D2D1::Point2F(bounds_.x, bounds_.y), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (underline_ && bounds_.width > 4.0f)
        {
            const float inset = bounds_.width * 0.08f;
            const float y = bounds_.y + bounds_.height - std::max(2.0f, size_ * 0.12f);
            target->DrawLine(D2D1::Point2F(bounds_.x + inset, y), D2D1::Point2F(bounds_.x + bounds_.width - inset, y),
                             brush, std::max(1.0f, size_ * 0.06f));
        }
    }

    bool HitTest(const msimeui::PointF &point) const override
    {
        return point.x >= bounds_.x && point.x < bounds_.x + bounds_.width && point.y >= bounds_.y &&
               point.y < bounds_.y + bounds_.height;
    }

    bool OnMouseDown(const POINT &point, WPARAM keyState) override
    {
        (void)keyState;
        if (!window_)
        {
            return false;
        }
        pressed_ = HitTest(window_->ClientPixelsToDips(point));
        InvalidateVisual();
        return pressed_;
    }

    bool OnMouseUp(const POINT &point, WPARAM keyState) override
    {
        (void)keyState;
        if (!window_ || !pressed_)
        {
            return false;
        }
        const bool shouldClick = HitTest(window_->ClientPixelsToDips(point));
        pressed_ = false;
        InvalidateVisual();
        if (shouldClick && onClick_)
        {
            onClick_();
        }
        return true;
    }

    void OnMouseEnter() override
    {
        hovered_ = true;
        InvalidateVisual();
    }

    void OnMouseLeave() override
    {
        hovered_ = false;
        pressed_ = false;
        InvalidateVisual();
    }

  private:
    wchar_t glyph_ = 0;
    std::wstring glyphFamily_;
    std::wstring text_;
    float size_ = 24.0f;
    bool underline_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
    D2D1_COLOR_F glyphColor_ = D2D1::ColorF(0xFFFFFF);
    D2D1_COLOR_F hoverFill_ = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
    float hoverRadius_ = 6.0f;
    ClickHandler onClick_;
};

class ToolbarDragHandle : public msimeui::Visual
{
  public:
    ToolbarDragHandle(float width, float slotHeight, float barWidth, float barHeight, D2D1_COLOR_F color)
        : width_(width), slotHeight_(slotHeight), barWidth_(barWidth), barHeight_(barHeight), color_(color)
    {
        SetWidth(width_);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        (void)availableSize;
        return {width_, slotHeight_};
    }

    void Arrange(const msimeui::RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &deviceResources) override
    {
        ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
        ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color_);
        if (!target || !brush)
        {
            return;
        }
        const float x = bounds_.x + (bounds_.width - barWidth_) * 0.5f;
        const float y = bounds_.y + (bounds_.height - barHeight_) * 0.5f;
        const float radius = std::max(1.0f, barWidth_ * 0.8f);
        const auto rounded = D2D1::RoundedRect(D2D1::RectF(x, y, x + barWidth_, y + barHeight_), radius, radius);
        target->FillRoundedRectangle(rounded, brush);
    }

    bool OnMouseDown(const POINT &point, WPARAM keyState) override
    {
        (void)point;
        (void)keyState;
        return false;
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_SIZEALL);
    }

  private:
    float width_ = 10.0f;
    float slotHeight_ = 35.0f;
    float barWidth_ = 2.5f;
    float barHeight_ = 14.0f;
    D2D1_COLOR_F color_ = ColorFromRgb(0x8E8CD8);
};

class ToolbarDivider : public msimeui::Visual
{
  public:
    ToolbarDivider(float width, float height, D2D1_COLOR_F color) : width_(width), height_(height), color_(color)
    {
        SetWidth(width_);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        (void)availableSize;
        return {width_, height_};
    }

    void Arrange(const msimeui::RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(msimeui::DeviceResources &deviceResources) override
    {
        ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
        ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color_);
        if (!target || !brush || bounds_.height <= 0.0f)
        {
            return;
        }
        target->FillRectangle(D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height),
                              brush);
    }

  private:
    float width_ = 1.2f;
    float height_ = 35.0f;
    D2D1_COLOR_F color_ = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
};

void SendWorker(UINT type)
{
    SendToTsfWorkerThreadViaNamedpipe(type, L"");
}
} // namespace

struct FloatingToolbarPresenter::Impl
{
    msimeui::DeviceResources resources;
    std::unique_ptr<msimeui::Window> window;
    std::shared_ptr<msimeui::StackPanel> root;
    std::shared_ptr<msimeui::Card> card;
    std::shared_ptr<msimeui::Container> frame;
    std::shared_ptr<msimeui::Visual> dragLimit;
    RectF captionRect = {};
    D2D1_COLOR_F fill = ColorFromRgb(0x1A1A1A);
    D2D1_COLOR_F border = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
    D2D1_COLOR_F glyph = ColorFromRgb(0xFFFFFF);
    D2D1_COLOR_F hover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
    D2D1_COLOR_F divider = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
    D2D1_COLOR_F handle = ColorFromRgb(0x8E8CD8);
    float radius = 8.0f;
    float iconRadius = 6.0f;
    int cnEn = 1;
    int doubleSingleByte = 0;
    int punctuation = 1;
    int englishInputMode = 0;
    int capsLock = 0;
    int japaneseInputMode = 0;
};

FloatingToolbarPresenter::FloatingToolbarPresenter() = default;

FloatingToolbarPresenter &FloatingToolbarPresenter::Instance()
{
    static FloatingToolbarPresenter instance;
    return instance;
}

bool FloatingToolbarPresenter::IsBound() const
{
    return bound_;
}

bool FloatingToolbarPresenter::Bind(HWND hwnd)
{
    hwnd_ = hwnd;
    if (!hwnd)
    {
        bound_ = false;
        impl_.reset();
        return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->window = std::make_unique<msimeui::Window>(L"metasequoiaime_windows", L"ftb", 1, 1);
    impl_->window->AdoptExistingHwnd(hwnd);
    impl_->window->SetStealFocusOnClick(false);
    bound_ = impl_->resources.EnsureForComposition(hwnd);
    impl_->japaneseInputMode = GetConfiguredInputMode() == "japanese" ? 1 : 0;
    ApplyTheme();
    FTB_DIAG_LOGF(L"ftb-d2d bind hwnd={:#x} composition={}", reinterpret_cast<uintptr_t>(hwnd), bound_ ? 1 : 0);
    return bound_;
}

void FloatingToolbarPresenter::RebuildScene()
{
    if (!impl_ || !impl_->window)
    {
        return;
    }

    const float s = ToolbarUserScale();
    const float barHeight = 35.0f * s;
    const float iconSize = 26.0f * s;
    const float gap = 5.0f * s;
    const float padLeft = 8.0f * s;
    const float padRight = 4.0f * s;
    const float radius = impl_->radius * s;
    const float iconRadius = impl_->iconRadius * s;

    auto row = std::make_shared<msimeui::HorizontalStackPanel>(gap);
    row->SetVerticalContentAlignment(msimeui::VerticalAlignment::Center);
    row->SetPadding({0.0f, 0.0f, padRight, 0.0f});
    row->SetHeight(barHeight);

    auto handle =
        std::make_shared<ToolbarDragHandle>(padLeft + 10.0f * s, barHeight, 2.5f * s, 14.0f * s, impl_->handle);
    auto divider = std::make_shared<ToolbarDivider>(1.2f * s, barHeight, impl_->divider);
    auto leading = std::make_shared<msimeui::HorizontalStackPanel>(2.0f * s);
    leading->SetVerticalContentAlignment(msimeui::VerticalAlignment::Center);
    leading->AddChild(handle);
    leading->AddChild(divider);
    impl_->dragLimit = divider;
    auto icons = std::make_shared<msimeui::HorizontalStackPanel>(gap);
    icons->SetVerticalContentAlignment(msimeui::VerticalAlignment::Center);

    auto addText = [&](std::wstring text, bool underline, ToolbarIconButton::ClickHandler click) {
        auto button = std::make_shared<ToolbarIconButton>(0, L"", std::move(text), iconSize);
        button->SetUnderline(underline);
        button->SetColors(impl_->glyph, impl_->hover, iconRadius);
        button->SetOnClick(std::move(click));
        icons->AddChild(button);
        return button;
    };
    auto addGlyph = [&](const ToolbarIcon &icon, ToolbarIconButton::ClickHandler click) {
        const msimeui::IconGlyph resolved = msimeui::ResolveIconGlyph(icon.codepoint);
        if (!resolved.family)
        {
            return addText(icon.fallbackText, false, std::move(click));
        }
        auto button = std::make_shared<ToolbarIconButton>(resolved.codepoint, resolved.family, L"", iconSize);
        button->SetColors(impl_->glyph, impl_->hover, iconRadius);
        button->SetOnClick(std::move(click));
        icons->AddChild(button);
        return button;
    };

    if (impl_->capsLock == 1)
    {
        addText(L"A", false, [this]() {
            if (impl_->englishInputMode == 1)
            {
                FanyNamedPipe::EnqueueExitEnglishInputModeTask();
            }
            else if (impl_->cnEn == 1)
            {
                SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToEn);
            }
            else
            {
                SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToCn);
            }
        });
    }
    else if (impl_->cnEn != 1)
    {
        addGlyph(kIconEn, []() { SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToCn); });
    }
    else if (impl_->englishInputMode == 1)
    {
        addText(L"En", true, []() { FanyNamedPipe::EnqueueExitEnglishInputModeTask(); });
    }
    else if (impl_->japaneseInputMode == 1)
    {
        addGlyph(kIconJa, []() { SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToEn); });
    }
    else
    {
        addGlyph(kIconCn, []() { SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToEn); });
    }

    const FloatingToolbarItemsConfig &items = GetConfiguredFloatingToolbarItems();
    if (items.fullwidth)
    {
        if (impl_->doubleSingleByte == 1)
        {
            addGlyph(kIconFullWidth,
                     []() { SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToHalfwidth); });
        }
        else
        {
            addGlyph(kIconHalfWidth,
                     []() { SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToFullwidth); });
        }
    }
    if (items.punctuation)
    {
        if (impl_->punctuation == 1)
        {
            addGlyph(kIconPuncCn, []() { SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToPuncEn); });
        }
        else
        {
            addGlyph(kIconPuncEn, []() { SendWorker(Global::DataFromServerMsgTypeToTsfWorkerThread::SwitchToPuncCn); });
        }
    }
    if (items.character_set)
    {
        const bool traditional = GetConfiguredCharacterSet() == "traditional";
        addGlyph(traditional ? kIconTraditional : kIconSimplified, []() {
            const std::string next = GetConfiguredCharacterSet() == "traditional" ? "simplified" : "traditional";
            if (SetConfiguredCharacterSet(next))
            {
                PostSettingsConfig();
            }
        });
    }
    if (items.emoji)
    {
        addGlyph(kIconEmoji, []() { OpenEmojiPanelApplication(); });
    }
    if (items.screen_keyboard)
    {
        addGlyph(kIconKeyboard, []() { OpenKeyboardPanelApplication(); });
    }
    if (items.settings)
    {
        addGlyph(kIconSettings, []() { OpenSettingsApplication(); });
    }

    row->AddChild(leading);
    row->AddChild(icons);

    msimeui::Brush brush;
    brush.fill = impl_->fill;
    brush.stroke = impl_->border;
    brush.strokeWidth = 1.4f;
    brush.radiusX = radius;
    brush.radiusY = radius;
    impl_->card = std::make_shared<msimeui::Card>(brush, 0.0f);
    impl_->card->SetShadowScale(0.45f);
    impl_->card->AddChild(row);
    impl_->frame = std::make_shared<msimeui::Container>();
    impl_->frame->SetPadding({kShadowPadLeft, kShadowPadTop, kShadowPadRight, kShadowPadBottom});
    impl_->frame->SetChild(impl_->card);
    impl_->root = std::make_shared<msimeui::StackPanel>(0.0f);
    impl_->root->AddChild(impl_->frame);
    auto scene = std::make_unique<msimeui::Scene>();
    scene->SetRoot(impl_->root);
    impl_->window->SetScene(std::move(scene));
}

void FloatingToolbarPresenter::ApplyTheme()
{
    if (!impl_)
    {
        return;
    }
    const bool light = ResolveConfiguredTheme(GetConfiguredThemeFtb()) == "light";
    // 与 WebView2 端选工具栏页面的规则一致：跟随候选窗皮肤，外部皮肤用其 base。
    std::string skinId = GetConfiguredCandidateSkin();
    if (!CandidateSkinCatalog::IsBuiltIn(skinId))
    {
        const auto package =
            CandidateSkinCatalog::Load(std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins", skinId);
        skinId = package ? package->base : "fluent";
    }
    const FloatingToolbarSkin skin = ResolveFloatingToolbarSkin(skinId, light);
    impl_->fill = skin.fill;
    impl_->border = skin.border;
    impl_->glyph = skin.glyph;
    impl_->hover = skin.hover;
    impl_->divider = skin.divider;
    impl_->handle = skin.handle;
    impl_->radius = skin.radius;
    impl_->iconRadius = skin.iconRadius;
    ApplyAppearance();
}

void FloatingToolbarPresenter::ApplyAppearance()
{
    RebuildScene();
    RelayoutHost();
}

void FloatingToolbarPresenter::RelayoutHost(FLOAT scaleOverride)
{
    if (!bound_ || !hwnd_ || !impl_ || !impl_->root)
    {
        return;
    }
    impl_->root->InvalidateMeasure();
    const SizeF measured = impl_->root->MeasureInLayout({2000.0f, 240.0f});
    impl_->root->InvalidateArrange();
    impl_->root->ArrangeInLayout({0.0f, 0.0f, measured.width, measured.height});
    ::FTB_CONTENT_WIDTH_DIP = measured.width;
    ::FTB_CONTENT_HEIGHT_DIP = measured.height;
    ::FTB_WND_WIDTH = static_cast<int>(std::ceil(measured.width));
    ::FTB_WND_HEIGHT = static_cast<int>(std::ceil(measured.height));

    FLOAT scale = scaleOverride > 0.0f ? scaleOverride : GetWindowScale(hwnd_);
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    RECT current{};
    GetWindowRect(hwnd_, &current);
    const HalfScreenDipLimits limits = QueryHalfScreenDipLimitsForPoint({current.left, current.top});
    const float widthDip = static_cast<float>(ClampWidthDipToHalfScreen(static_cast<double>(measured.width), limits));
    const float heightDip =
        static_cast<float>(ClampHeightDipToHalfScreen(static_cast<double>(measured.height), limits));
    const int widthPx = (std::max)(1, static_cast<int>(std::ceil(widthDip * scale)));
    const int heightPx = (std::max)(1, static_cast<int>(std::ceil(heightDip * scale)));
    int posX = current.left;
    int posY = current.top;
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (monitor && GetMonitorInfo(monitor, &monitorInfo))
    {
        const int maxX = static_cast<int>(monitorInfo.rcMonitor.right) - widthPx;
        const int maxY = static_cast<int>(monitorInfo.rcMonitor.bottom) - heightPx;
        posX = (std::max)(static_cast<int>(monitorInfo.rcMonitor.left), (std::min)(posX, maxX));
        posY = (std::max)(static_cast<int>(monitorInfo.rcMonitor.top), (std::min)(posY, maxY));
    }
    SetWindowPos(hwnd_, nullptr, posX, posY, widthPx, heightPx, SWP_NOZORDER | SWP_NOACTIVATE);
    impl_->resources.EnsureForComposition(hwnd_);
    if (impl_->card && impl_->dragLimit)
    {
        const RectF card = impl_->card->GetBounds();
        const RectF limit = impl_->dragLimit->GetBounds();
        impl_->captionRect = {0.0f, card.y, limit.x + limit.width, card.height};
    }
    Present();
}

bool FloatingToolbarPresenter::HitCaptionDrag(POINT clientPoint) const
{
    if (!bound_ || !impl_ || !impl_->window || impl_->captionRect.width <= 0.0f)
    {
        return false;
    }
    const PointF dip = impl_->window->ClientPixelsToDips(clientPoint);
    return dip.x >= impl_->captionRect.x && dip.x < impl_->captionRect.x + impl_->captionRect.width &&
           dip.y >= impl_->captionRect.y && dip.y < impl_->captionRect.y + impl_->captionRect.height;
}

void FloatingToolbarPresenter::SyncUi(int cnEn, int doubleSingleByte, int punctuation, int englishInputMode,
                                      int capsLock, int japaneseInputMode)
{
    if (!impl_)
    {
        return;
    }
    impl_->cnEn = cnEn;
    impl_->doubleSingleByte = doubleSingleByte;
    impl_->punctuation = punctuation;
    impl_->englishInputMode = englishInputMode;
    impl_->capsLock = capsLock;
    impl_->japaneseInputMode = japaneseInputMode;
    if (!bound_)
    {
        return;
    }
    ApplyAppearance();
}

void FloatingToolbarPresenter::Present()
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
        scene->EnsureLayout({msimeui::PixelsToDips(static_cast<float>(width), dpi),
                             msimeui::PixelsToDips(static_cast<float>(height), dpi)});
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

bool FloatingToolbarPresenter::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
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
        impl_->resources.Resize(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
        Present();
        return true;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSELEAVE:
        impl_->window->DispatchImportedMessage(message, wParam, lParam);
        Present();
        return true;
    default:
        return false;
    }
}

FloatingToolbarPresenter::~FloatingToolbarPresenter() = default;
