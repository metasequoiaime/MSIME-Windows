#include "msimeui/Controls.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <d2d1_3.h>
#include <limits>
#include <utility>
#include <wincodec.h>
#include <shlwapi.h>
#include <wrl/client.h>

namespace msimeui
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr D2D1_DRAW_TEXT_OPTIONS kColorClipTextOptions =
    static_cast<D2D1_DRAW_TEXT_OPTIONS>(D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);

SizeF ReadImageSize(const std::wstring &filePath)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (filePath.empty() ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf()))) ||
        FAILED(factory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
    {
        return {};
    }

    UINT width = 0;
    UINT height = 0;
    return SUCCEEDED(frame->GetSize(&width, &height)) ? SizeF{static_cast<float>(width), static_cast<float>(height)}
                                                      : SizeF{};
}

IDWriteFactory *GetSharedDWriteFactory()
{
    static ComPtr<IDWriteFactory> factory;
    if (!factory)
    {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(factory.GetAddressOf()));
    }

    return factory.Get();
}

constexpr float kControlCornerRadius = 12.0f;
constexpr float kControlPaddingX = 16.0f;
constexpr float kControlPaddingY = 10.0f;
constexpr float kCheckBoxIndicatorSize = 20.0f;
constexpr float kSliderTrackHeight = 6.0f;
constexpr float kSliderThumbRadius = 9.0f;
constexpr float kListItemCornerRadius = 14.0f;
constexpr float kTreeIndent = 22.0f;
constexpr float kTreeRowLeadingPadding = 8.0f;
constexpr float kTreeGuideLineOffset = 15.0f;
constexpr float kTreeExpanderSize = 14.0f;
constexpr float kTreeTextGapAfterExpander = 10.0f;
constexpr float kTabCornerRadius = 12.0f;
constexpr float kAccordionCornerRadius = 14.0f;
constexpr float kAccordionHeaderHorizontalPadding = 20.0f;
constexpr float kComboBoxItemGap = 6.0f;
constexpr float kCandidateItemGap = 1.0f;

RectF MakeInsetRect(const RectF &rect, float insetX, float insetY)
{
    return {rect.x + insetX, rect.y + insetY, std::max(rect.width - insetX * 2.0f, 0.0f),
            std::max(rect.height - insetY * 2.0f, 0.0f)};
}

SizeF DeflateSizeLocal(const SizeF &size, const Thickness &thickness)
{
    return {std::max(size.width - thickness.left - thickness.right, 0.0f),
            std::max(size.height - thickness.top - thickness.bottom, 0.0f)};
}

RectF DeflateRectLocal(const RectF &rect, const Thickness &thickness)
{
    return {rect.x + thickness.left, rect.y + thickness.top,
            std::max(rect.width - thickness.left - thickness.right, 0.0f),
            std::max(rect.height - thickness.top - thickness.bottom, 0.0f)};
}

bool PointInRect(const RectF &rect, const PointF &point)
{
    return point.x >= rect.x && point.x <= (rect.x + rect.width) && point.y >= rect.y &&
           point.y <= (rect.y + rect.height);
}

bool PointInRoundedRect(const RectF &rect, float radius, const PointF &point)
{
    if (!PointInRect(rect, point))
    {
        return false;
    }

    const float clampedRadius = std::min(radius, std::min(rect.width, rect.height) * 0.5f);
    if (clampedRadius <= 0.0f)
    {
        return true;
    }

    const float left = rect.x;
    const float top = rect.y;
    const float right = rect.x + rect.width;
    const float bottom = rect.y + rect.height;

    if ((point.x >= left + clampedRadius && point.x <= right - clampedRadius) ||
        (point.y >= top + clampedRadius && point.y <= bottom - clampedRadius))
    {
        return true;
    }

    const float centerX = point.x < left + clampedRadius ? left + clampedRadius : right - clampedRadius;
    const float centerY = point.y < top + clampedRadius ? top + clampedRadius : bottom - clampedRadius;
    const float dx = point.x - centerX;
    const float dy = point.y - centerY;
    return (dx * dx + dy * dy) <= (clampedRadius * clampedRadius);
}

SizeF MeasureText(IDWriteFactory *factory, const std::wstring &text, float fontSize, bool bold, float maxWidth)
{
    if (!factory)
    {
        return {maxWidth, std::ceil(fontSize * 1.5f)};
    }

    ComPtr<IDWriteTextFormat> format;
    if (FAILED(factory->CreateTextFormat(
            L"Segoe UI", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", format.GetAddressOf())))
    {
        return {maxWidth, std::ceil(fontSize * 1.5f)};
    }

    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), maxWidth,
                                         std::numeric_limits<float>::max(), layout.GetAddressOf())))
    {
        return {maxWidth, std::ceil(fontSize * 1.5f)};
    }

    DWRITE_TEXT_METRICS metrics = {};
    if (FAILED(layout->GetMetrics(&metrics)))
    {
        return {maxWidth, std::ceil(fontSize * 1.5f)};
    }

    return {std::ceil(metrics.widthIncludingTrailingWhitespace), std::ceil(metrics.height)};
}

ComPtr<IDWriteTextLayout> CreateCachedTextLayout(IDWriteFactory *factory, const std::wstring &fontFamily,
                                                 const std::wstring &text, float fontSize,
                                                 DWRITE_FONT_WEIGHT fontWeight, float width, float height,
                                                 DWRITE_TEXT_ALIGNMENT textAlignment,
                                                 DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment,
                                                 DWRITE_WORD_WRAPPING wordWrapping,
                                                 const std::vector<std::wstring> &fallbackFamilies = {})
{
    ComPtr<IDWriteTextLayout> layout;
    if (!factory)
    {
        return layout;
    }

    ComPtr<IDWriteTextFormat> format;
    struct TextFormatKey
    {
        std::wstring family;
        std::vector<std::wstring> fallbackFamilies;
        float size = 0.0f;
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        DWRITE_TEXT_ALIGNMENT textAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        DWRITE_WORD_WRAPPING wordWrapping = DWRITE_WORD_WRAPPING_NO_WRAP;
        ComPtr<IDWriteTextFormat> format;
    };
    static std::vector<TextFormatKey> formatCache;
    for (auto &entry : formatCache)
    {
        if (entry.family == fontFamily && entry.fallbackFamilies == fallbackFamilies && entry.size == fontSize &&
            entry.weight == fontWeight && entry.textAlignment == textAlignment &&
            entry.paragraphAlignment == paragraphAlignment && entry.wordWrapping == wordWrapping && entry.format)
        {
            format = entry.format;
            break;
        }
    }
    if (!format)
    {
        if (FAILED(factory->CreateTextFormat(fontFamily.c_str(), nullptr, fontWeight, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", format.GetAddressOf())))
        {
            if (FAILED(factory->CreateTextFormat(UiFontFallbackFamily(), nullptr, fontWeight, DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", format.GetAddressOf())))
            {
                return layout;
            }
        }
        format->SetTextAlignment(textAlignment);
        format->SetParagraphAlignment(paragraphAlignment);
        format->SetWordWrapping(wordWrapping);
        ApplyFontFallback(factory, format.Get(), fallbackFamilies);
        TextFormatKey entry;
        entry.fallbackFamilies = fallbackFamilies;
        entry.family = fontFamily;
        entry.size = fontSize;
        entry.weight = fontWeight;
        entry.textAlignment = textAlignment;
        entry.paragraphAlignment = paragraphAlignment;
        entry.wordWrapping = wordWrapping;
        entry.format = format;
        formatCache.push_back(std::move(entry));
    }

    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), width, height,
                                         layout.GetAddressOf())))
    {
        layout.Reset();
    }

    return layout;
}

float EstimateTrayLabelWidth(const std::wstring &text)
{
    ComPtr<IDWriteTextLayout> layout = CreateCachedTextLayout(
        GetSharedDWriteFactory(), UiFontFamily(), text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, 4096.0f, 30.0f,
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!layout)
    {
        return 14.0f * static_cast<float>(text.size());
    }
    DWRITE_TEXT_METRICS metrics = {};
    if (FAILED(layout->GetMetrics(&metrics)))
    {
        return 14.0f * static_cast<float>(text.size());
    }
    return std::ceil(metrics.widthIncludingTrailingWhitespace);
}

void DrawTextBlock(DeviceResources &deviceResources, const std::wstring &text, float fontSize, bool bold,
                   D2D1_COLOR_F color, const RectF &rect, DWRITE_TEXT_ALIGNMENT textAlignment)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    IDWriteTextFormat *format = deviceResources.GetTextFormat(
        theme.uiFontFamily, fontSize, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, textAlignment,
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color);
    if (!format || !brush)
    {
        return;
    }
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
                      D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height), brush);
}

void FillRoundedRect(DeviceResources &deviceResources, const RectF &bounds, float radius, D2D1_COLOR_F fill,
                     D2D1_COLOR_F stroke, float strokeWidth)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(fill);
    ID2D1SolidColorBrush *strokeBrush = deviceResources.GetSolidColorBrush(stroke);
    if (!fillBrush || !strokeBrush)
    {
        return;
    }

    const auto rounded = D2D1::RoundedRect(
        D2D1::RectF(bounds.x, bounds.y, bounds.x + bounds.width, bounds.y + bounds.height), radius, radius);
    target->FillRoundedRectangle(rounded, fillBrush);
    if (strokeWidth > 0.0f)
    {
        target->DrawRoundedRectangle(rounded, strokeBrush, strokeWidth);
    }
}

// Per-corner radii in top-left, top-right, bottom-right, bottom-left order.
void FillRoundedRectCorners(DeviceResources &deviceResources, const RectF &bounds, const float (&radii)[4],
                            D2D1_COLOR_F fill)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(fill);
    if (!target || !fillBrush)
    {
        return;
    }

    const float limit = std::max(std::min(bounds.width, bounds.height) * 0.5f, 0.0f);
    const float tl = std::clamp(radii[0], 0.0f, limit);
    const float tr = std::clamp(radii[1], 0.0f, limit);
    const float br = std::clamp(radii[2], 0.0f, limit);
    const float bl = std::clamp(radii[3], 0.0f, limit);
    const float left = bounds.x;
    const float top = bounds.y;
    const float right = bounds.x + bounds.width;
    const float bottom = bounds.y + bounds.height;

    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(geometry.GetAddressOf())) ||
        FAILED(geometry->Open(sink.GetAddressOf())))
    {
        return;
    }

    const auto arcTo = [&sink](float x, float y, float radius) {
        if (radius > 0.0f)
        {
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(radius, radius), 0.0f,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        }
    };
    sink->BeginFigure(D2D1::Point2F(left + tl, top), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(right - tr, top));
    arcTo(right, top + tr, tr);
    sink->AddLine(D2D1::Point2F(right, bottom - br));
    arcTo(right - br, bottom, br);
    sink->AddLine(D2D1::Point2F(left + bl, bottom));
    arcTo(left, bottom - bl, bl);
    sink->AddLine(D2D1::Point2F(left, top + tl));
    arcTo(left + tl, top, tl);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (SUCCEEDED(sink->Close()))
    {
        target->FillGeometry(geometry.Get(), fillBrush);
    }
}

void DrawPopupShadow(DeviceResources &deviceResources, const RectF &bounds, float radius)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    struct ShadowLayer
    {
        float spread;
        float offsetY;
        float alpha;
    };

    static constexpr ShadowLayer kLayers[] = {
        {1.0f, 1.0f, 0.08f},
        {4.0f, 4.0f, 0.045f},
        {8.0f, 9.0f, 0.02f},
    };

    for (const ShadowLayer &layer : kLayers)
    {
        const RectF shadowRect = {bounds.x - layer.spread, bounds.y - layer.spread + layer.offsetY,
                                  bounds.width + layer.spread * 2.0f, bounds.height + layer.spread * 2.0f};
        const D2D1_COLOR_F shadowColor = D2D1::ColorF(0x000000, layer.alpha);
        ID2D1SolidColorBrush *shadowBrush = deviceResources.GetSolidColorBrush(shadowColor);
        if (!shadowBrush)
        {
            continue;
        }

        const auto rounded = D2D1::RoundedRect(
            D2D1::RectF(shadowRect.x, shadowRect.y, shadowRect.x + shadowRect.width, shadowRect.y + shadowRect.height),
            radius + layer.spread, radius + layer.spread);
        target->FillRoundedRectangle(rounded, shadowBrush);
    }
}

void DrawLabel(DeviceResources &deviceResources, const std::wstring &text, float fontSize, bool bold,
               D2D1_COLOR_F color, const RectF &rect, DWRITE_TEXT_ALIGNMENT alignment,
               DWRITE_PARAGRAPH_ALIGNMENT paragraphAlignment,
               DWRITE_WORD_WRAPPING wrapping = DWRITE_WORD_WRAPPING_NO_WRAP)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    IDWriteTextFormat *format = deviceResources.GetTextFormat(
        theme.uiFontFamily, fontSize, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, alignment,
        paragraphAlignment, wrapping);
    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color);
    if (!format || !brush)
    {
        return;
    }

    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
                      D2D1::RectF(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height), brush);
}

class ComboBoxPopupContent : public Visual
{
  public:
    using SelectHandler = std::function<void(size_t index)>;

    explicit ComboBoxPopupContent(float itemHeight) : itemHeight_(itemHeight)
    {
    }

    void SetItems(const std::vector<std::wstring> &items)
    {
        items_ = items;
        if (hoveredIndex_ >= items_.size())
        {
            hoveredIndex_ = static_cast<size_t>(-1);
        }
        if (pressedIndex_ >= items_.size())
        {
            pressedIndex_ = static_cast<size_t>(-1);
        }
        InvalidateMeasure();
        InvalidateVisual();
    }

    void SetSelectedIndex(size_t index)
    {
        selectedIndex_ = index;
        InvalidateVisual();
    }

    void SetHighlightedIndex(size_t index)
    {
        hoveredIndex_ = index < items_.size() ? index : static_cast<size_t>(-1);
        pressedIndex_ = static_cast<size_t>(-1);
        InvalidateVisual();
    }

    void SetOnSelect(SelectHandler handler)
    {
        onSelect_ = std::move(handler);
    }

    SizeF Measure(const SizeF &availableSize) override
    {
        float maxWidth = 160.0f;
        for (const auto &item : items_)
        {
            const SizeF measured =
                MeasureText(GetSharedDWriteFactory(), item, 15.0f, false, std::max(availableSize.width - 28.0f, 1.0f));
            maxWidth = std::max(maxWidth, measured.width + 28.0f);
        }

        if (items_.empty())
        {
            return {std::min(availableSize.width, maxWidth), 0.0f};
        }

        return {std::min(availableSize.width, maxWidth), itemHeight_ * static_cast<float>(items_.size()) +
                                                             kComboBoxItemGap * static_cast<float>(items_.size() - 1)};
    }

    void Arrange(const RectF &finalRect) override
    {
        bounds_ = finalRect;
    }

    void Render(DeviceResources &deviceResources) override
    {
        ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
        if (!target)
        {
            return;
        }

        const Theme &theme = ThemeManager::GetCurrent();
        for (size_t index = 0; index < items_.size(); ++index)
        {
            const RectF row = ItemRect(index);
            const bool selected = index == selectedIndex_;
            const bool hovered = index == hoveredIndex_;
            const bool pressed = index == pressedIndex_;

            if (selected || hovered || pressed)
            {
                FillRoundedRect(deviceResources, row, 10.0f,
                                pressed ? theme.primarySoftPressed
                                        : (selected ? theme.primarySoft : theme.surfaceMuted),
                                selected ? theme.primaryFocus : theme.border, selected ? 1.5f : 1.0f);
            }

            DrawLabel(deviceResources, items_[index], 15.0f, selected, theme.textPrimary,
                      {row.x + 14.0f, row.y, std::max(row.width - 28.0f, 0.0f), row.height},
                      DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    bool HitTest(const PointF &point) const override
    {
        return PointInRect(bounds_, point);
    }

    bool IsFocusable() const override
    {
        return true;
    }

    bool OnMouseDown(const POINT &point, WPARAM keyState) override
    {
        (void)keyState;
        if (!window_)
        {
            return false;
        }

        const PointF dipPoint = window_->ClientPixelsToDips(point);
        pressedIndex_ = HitTestItem(dipPoint);
        if (pressedIndex_ != static_cast<size_t>(-1))
        {
            hoveredIndex_ = pressedIndex_;
            InvalidateVisual();
            return true;
        }

        return false;
    }

    bool OnMouseUp(const POINT &point, WPARAM keyState) override
    {
        (void)keyState;
        if (!window_)
        {
            return false;
        }

        const size_t pressedIndex = pressedIndex_;
        pressedIndex_ = static_cast<size_t>(-1);
        const PointF dipPoint = window_->ClientPixelsToDips(point);
        hoveredIndex_ = HitTestItem(dipPoint);
        InvalidateVisual();

        if (pressedIndex == static_cast<size_t>(-1))
        {
            return false;
        }

        if (pressedIndex == hoveredIndex_ && onSelect_)
        {
            onSelect_(pressedIndex);
        }
        return true;
    }

    bool OnMouseMove(const POINT &point, WPARAM keyState) override
    {
        (void)keyState;
        if (!window_)
        {
            return false;
        }

        const size_t hovered = HitTestItem(window_->ClientPixelsToDips(point));
        if (hovered != hoveredIndex_)
        {
            hoveredIndex_ = hovered;
            InvalidateVisual();
        }
        return hovered != static_cast<size_t>(-1);
    }

    HCURSOR GetCursor() const override
    {
        return LoadCursor(nullptr, IDC_HAND);
    }

  private:
    RectF ItemRect(size_t index) const
    {
        return {bounds_.x, bounds_.y + (itemHeight_ + kComboBoxItemGap) * static_cast<float>(index), bounds_.width,
                itemHeight_};
    }

    size_t HitTestItem(const PointF &point) const
    {
        if (!HitTest(point) || items_.empty())
        {
            return static_cast<size_t>(-1);
        }

        const float localY = point.y - bounds_.y;
        const float stride = itemHeight_ + kComboBoxItemGap;
        const size_t index = static_cast<size_t>(localY / stride);
        const float withinItem = localY - stride * static_cast<float>(index);
        if (withinItem > itemHeight_)
        {
            return static_cast<size_t>(-1);
        }
        return index < items_.size() ? index : static_cast<size_t>(-1);
    }

    std::vector<std::wstring> items_;
    float itemHeight_ = 40.0f;
    size_t selectedIndex_ = static_cast<size_t>(-1);
    size_t hoveredIndex_ = static_cast<size_t>(-1);
    size_t pressedIndex_ = static_cast<size_t>(-1);
    SelectHandler onSelect_;
};
} // namespace

Image::Image(std::wstring filePath) : filePath_(std::move(filePath))
{
}

void Image::SetSource(std::wstring filePath)
{
    if (filePath_ == filePath)
    {
        return;
    }
    filePath_ = std::move(filePath);
    naturalSize_ = {};
    naturalSizeLoaded_ = false;
    InvalidateMeasure();
}

const std::wstring &Image::GetSource() const
{
    return filePath_;
}

void Image::SetStretch(ImageStretch stretch)
{
    if (stretch_ == stretch)
    {
        return;
    }
    stretch_ = stretch;
    InvalidateVisual();
}

void Image::SetOpacity(float opacity)
{
    const float clamped = std::clamp(opacity, 0.0f, 1.0f);
    if (opacity_ == clamped)
    {
        return;
    }
    opacity_ = clamped;
    InvalidateVisual();
}

void Image::SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE interpolationMode)
{
    if (interpolationMode_ == interpolationMode)
    {
        return;
    }
    interpolationMode_ = interpolationMode;
    InvalidateVisual();
}

void Image::LoadNaturalSize()
{
    if (!naturalSizeLoaded_)
    {
        naturalSize_ = ReadImageSize(filePath_);
        naturalSizeLoaded_ = true;
    }
}

SizeF Image::Measure(const SizeF &availableSize)
{
    LoadNaturalSize();
    float width = naturalSize_.width;
    float height = naturalSize_.height;
    if (HasExplicitHeight() && !HasExplicitWidth() && naturalSize_.height > 0.0f)
    {
        width = naturalSize_.width * (explicitHeight_ / naturalSize_.height);
    }
    else if (HasExplicitWidth() && !HasExplicitHeight() && naturalSize_.width > 0.0f)
    {
        height = naturalSize_.height * (explicitWidth_ / naturalSize_.width);
    }
    return {std::min(width, availableSize.width), std::min(height, availableSize.height)};
}

void Image::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Image::Render(DeviceResources &deviceResources)
{
    D2D1_SIZE_F sourceSize = {};
    ID2D1Bitmap *bitmap = deviceResources.GetBitmapFromFile(filePath_, &sourceSize);
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!bitmap || !target || sourceSize.width <= 0.0f || sourceSize.height <= 0.0f || bounds_.width <= 0.0f ||
        bounds_.height <= 0.0f)
    {
        return;
    }

    RectF destination = bounds_;
    if (stretch_ == ImageStretch::None)
    {
        destination.width = std::min(sourceSize.width, bounds_.width);
        destination.height = std::min(sourceSize.height, bounds_.height);
    }
    else if (stretch_ == ImageStretch::Uniform || stretch_ == ImageStretch::UniformToFill)
    {
        const float scaleX = bounds_.width / sourceSize.width;
        const float scaleY = bounds_.height / sourceSize.height;
        const float scale = stretch_ == ImageStretch::Uniform ? std::min(scaleX, scaleY) : std::max(scaleX, scaleY);
        destination.width = sourceSize.width * scale;
        destination.height = sourceSize.height * scale;
        destination.x = bounds_.x + (bounds_.width - destination.width) * 0.5f;
        destination.y = bounds_.y + (bounds_.height - destination.height) * 0.5f;
    }

    const auto destinationRect = D2D1::RectF(destination.x, destination.y, destination.x + destination.width,
                                             destination.y + destination.height);
    if (stretch_ == ImageStretch::UniformToFill || stretch_ == ImageStretch::None)
    {
        target->PushAxisAlignedClip(
            D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    target->DrawBitmap(bitmap, destinationRect, opacity_, interpolationMode_);
    if (stretch_ == ImageStretch::UniformToFill || stretch_ == ImageStretch::None)
    {
        target->PopAxisAlignedClip();
    }
}

Button::Button(std::wstring text, float height) : text_(std::move(text)), preferredHeight_(height)
{
}

void Button::SetOnClick(ClickHandler handler)
{
    onClick_ = std::move(handler);
}

void Button::InvalidateTextLayoutCache()
{
    cachedTextLayout_.Reset();
    cachedFontFamily_.clear();
    cachedLayoutWidth_ = -1.0f;
}

Popup::Popup(std::shared_ptr<Visual> child) : child_(std::move(child))
{
    AdoptChild(child_);
    SetPadding({12.0f, 12.0f, 12.0f, 12.0f});
}

void Popup::SetAnchorRect(const RectF &anchorRect)
{
    anchorRect_ = anchorRect;
}

void Popup::SetPlacement(PopupPlacement placement)
{
    placement_ = placement;
}

void Popup::SetOffset(float x, float y)
{
    offsetX_ = x;
    offsetY_ = y;
}

void Popup::SetMatchAnchorWidth(bool matchAnchorWidth)
{
    matchAnchorWidth_ = matchAnchorWidth;
}

void Popup::SetConstrainToViewport(bool constrainToViewport)
{
    constrainToViewport_ = constrainToViewport;
}

void Popup::SetBackgroundFill(const D2D1_COLOR_F &fill)
{
    backgroundFill_ = fill;
    InvalidateVisual();
}

void Popup::SetBorderColor(const D2D1_COLOR_F &border)
{
    borderColor_ = border;
    InvalidateVisual();
}

void Popup::SetCornerRadius(float radius)
{
    cornerRadius_ = std::max(radius, 0.0f);
    InvalidateVisual();
}

void Popup::SetShadowEnabled(bool enabled)
{
    shadowEnabled_ = enabled;
    InvalidateVisual();
}

SizeF Popup::Measure(const SizeF &availableSize)
{
    const SizeF inner = DeflateSizeLocal(availableSize, padding_);
    if (!child_)
    {
        return {padding_.left + padding_.right, padding_.top + padding_.bottom};
    }

    const SizeF childSize = child_->MeasureInLayout(inner);
    return {childSize.width + padding_.left + padding_.right, childSize.height + padding_.top + padding_.bottom};
}

void Popup::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (child_)
    {
        child_->ArrangeInLayout(DeflateRectLocal(bounds_, padding_));
    }
}

void Popup::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    if (shadowEnabled_)
    {
        DrawPopupShadow(deviceResources, bounds_, cornerRadius_);
    }

    FillRoundedRect(deviceResources, bounds_, cornerRadius_, backgroundFill_, borderColor_, 1.0f);
    if (child_)
    {
        child_->Render(deviceResources);
    }
}

void Popup::Attach(Window *window)
{
    Visual::Attach(window);
    if (child_)
    {
        child_->Attach(window);
    }
}

Visual *Popup::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    if (child_)
    {
        if (Visual *hit = child_->FindVisualAt(point))
        {
            return hit;
        }
    }

    return this;
}

Visual *Popup::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    return child_ ? child_->FindFocusableAt(point) : nullptr;
}

Visual *Popup::FindFirstFocusableDescendant()
{
    return child_ ? child_->FindFirstFocusableDescendant() : nullptr;
}

bool Popup::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool Popup::KeepsPopupsOpenOnClick() const
{
    return true;
}

MenuFlyoutItem::MenuFlyoutItem(std::wstring text, bool hasSubmenu) : text_(std::move(text)), hasSubmenu_(hasSubmenu)
{
    SetHeight(24.0f);
}

void MenuFlyoutItem::SetOnClick(ClickHandler handler)
{
    onClick_ = std::move(handler);
}

void MenuFlyoutItem::SetOnHover(HoverHandler handler)
{
    onHover_ = std::move(handler);
}

void MenuFlyoutItem::SetColors(const D2D1_COLOR_F &text, const D2D1_COLOR_F &hoverFill)
{
    textColor_ = text;
    hoverFill_ = hoverFill;
    InvalidateVisual();
}

void MenuFlyoutItem::SetHasSubmenu(bool hasSubmenu)
{
    hasSubmenu_ = hasSubmenu;
    InvalidateVisual();
}

void MenuFlyoutItem::SetLeadingSvg(std::string svgUtf8)
{
    leadingSvg_ = std::move(svgUtf8);
    svgDocument_.Reset();
    InvalidateMeasure();
    InvalidateVisual();
}

void MenuFlyoutItem::SetTrailingToggle(bool show)
{
    if (showToggle_ == show)
    {
        return;
    }
    showToggle_ = show;
    InvalidateMeasure();
    InvalidateVisual();
}

void MenuFlyoutItem::SetToggleOn(bool on)
{
    if (toggleOn_ == on)
    {
        return;
    }
    toggleOn_ = on;
    InvalidateVisual();
}

bool MenuFlyoutItem::IsToggleOn() const
{
    return toggleOn_;
}

bool MenuFlyoutItem::UsesTrayLayout() const
{
    return !leadingSvg_.empty() || showToggle_;
}

RectF MenuFlyoutItem::ToggleHitRect() const
{
    constexpr float kToggleWidth = 32.0f;
    constexpr float kToggleHeight = 16.0f;
    return {bounds_.x + bounds_.width - 6.0f - kToggleWidth, bounds_.y + (bounds_.height - kToggleHeight) * 0.5f,
            kToggleWidth, kToggleHeight};
}

SizeF MenuFlyoutItem::Measure(const SizeF &availableSize)
{
    if (UsesTrayLayout())
    {
        constexpr float kPad = 5.6f;
        constexpr float kIcon = 18.0f;
        constexpr float kIconGap = 8.0f;
        constexpr float kToggleWidth = 32.0f;
        constexpr float kToggleGap = 8.0f;
        // HTML .menu { min-width: 12em } inherits body 16px, minus 6px item margins.
        constexpr float kMinItemWidth = 12.0f * 16.0f - 12.0f;
        float width = kPad + kIcon + kIconGap + EstimateTrayLabelWidth(text_);
        width += showToggle_ ? (kToggleGap + kToggleWidth) : kPad;
        width = (std::max)(width, kMinItemWidth);
        if (availableSize.width > 1.0f)
        {
            width = (std::min)(width, availableSize.width);
        }
        return {width, 30.0f};
    }
    const float chevron = hasSubmenu_ ? 16.0f : 0.0f;
    const float width = (std::min)((std::max)(availableSize.width, 88.0f + chevron), 220.0f);
    return {width, 24.0f};
}

void MenuFlyoutItem::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void MenuFlyoutItem::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const bool tray = UsesTrayLayout();
    const float hoverRadius = tray ? 6.0f : 4.0f;
    if (hovered_ || pressed_)
    {
        FillRoundedRect(deviceResources, bounds_, hoverRadius, hoverFill_, hoverFill_, 0.0f);
    }

    const wchar_t *fontFamily = UiFontFamily();
    const float pad = tray ? 5.6f : 4.0f;
    const float iconSize = leadingSvg_.empty() ? 0.0f : 18.0f;
    const float iconGap = leadingSvg_.empty() ? 0.0f : 8.0f;
    const float chevronReserve = hasSubmenu_ ? 16.0f : (showToggle_ ? 36.0f : (tray ? 4.0f : 4.0f));
    const float textX = bounds_.x + pad + iconSize + iconGap;
    const float textWidth = (std::max)(bounds_.width - (textX - bounds_.x) - chevronReserve, 1.0f);
    if (!textLayout_ || cachedLayoutWidth_ != textWidth || cachedFontFamily_ != fontFamily)
    {
        textLayout_ =
            CreateCachedTextLayout(deviceResources.GetDWriteFactory(), fontFamily, text_, 14.0f,
                                   DWRITE_FONT_WEIGHT_NORMAL, textWidth, bounds_.height, DWRITE_TEXT_ALIGNMENT_LEADING,
                                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        cachedLayoutWidth_ = textWidth;
        cachedFontFamily_ = fontFamily;
    }

    if (ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(textColor_))
    {
        if (!leadingSvg_.empty())
        {
            const float iconY = bounds_.y + (bounds_.height - iconSize) * 0.5f;
            const RectF iconRect = {bounds_.x + pad, iconY, iconSize, iconSize};
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", static_cast<int>(textColor_.r * 255.0f + 0.5f),
                          static_cast<int>(textColor_.g * 255.0f + 0.5f),
                          static_cast<int>(textColor_.b * 255.0f + 0.5f));
            std::string tinted = leadingSvg_;
            for (size_t pos = 0; (pos = tinted.find("currentColor", pos)) != std::string::npos;)
            {
                tinted.replace(pos, 12, hex);
                pos += 7;
            }
            ComPtr<ID2D1DeviceContext5> dc5;
            if (SUCCEEDED(target->QueryInterface(IID_PPV_ARGS(&dc5))))
            {
                if (!svgDocument_ || svgContext_ != dc5.Get() || svgTintCache_ != tinted)
                {
                    svgDocument_.Reset();
                    ComPtr<IStream> stream;
                    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE *>(tinted.data()),
                                                    static_cast<UINT>(tinted.size())));
                    ComPtr<ID2D1SvgDocument> document;
                    if (stream && SUCCEEDED(dc5->CreateSvgDocument(stream.Get(), D2D1::SizeF(iconSize, iconSize),
                                                                   document.GetAddressOf())))
                    {
                        svgDocument_ = document;
                        svgContext_ = dc5.Get();
                        svgTintCache_ = tinted;
                    }
                }
                if (svgDocument_)
                {
                    ComPtr<ID2D1SvgDocument> document;
                    svgDocument_.As(&document);
                    if (document)
                    {
                        document->SetViewportSize(D2D1::SizeF(iconSize, iconSize));
                        D2D1_MATRIX_3X2_F previous = D2D1::Matrix3x2F::Identity();
                        dc5->GetTransform(&previous);
                        dc5->SetTransform(previous * D2D1::Matrix3x2F::Translation(iconRect.x, iconRect.y));
                        dc5->DrawSvgDocument(document.Get());
                        dc5->SetTransform(previous);
                    }
                }
            }
        }
        if (textLayout_)
        {
            target->DrawTextLayout(D2D1::Point2F(textX, bounds_.y), textLayout_.Get(), brush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (hasSubmenu_)
        {
            const float cx = bounds_.x + bounds_.width - 8.0f;
            const float cy = bounds_.y + bounds_.height * 0.5f;
            const float dx = 3.25f;
            const float dy = 4.0f;
            target->DrawLine(D2D1::Point2F(cx - dx, cy - dy), D2D1::Point2F(cx + 1.0f, cy), brush, 1.25f);
            target->DrawLine(D2D1::Point2F(cx + 1.0f, cy), D2D1::Point2F(cx - dx, cy + dy), brush, 1.25f);
        }
    }

    if (showToggle_)
    {
        const RectF track = ToggleHitRect();
        const D2D1_COLOR_F off = D2D1::ColorF(textColor_.r > 0.5f ? 0x555555 : 0xC8C8C8);
        const D2D1_COLOR_F on = D2D1::ColorF(0x8E8CD8);
        FillRoundedRect(deviceResources, track, 8.0f, toggleOn_ ? on : off, toggleOn_ ? on : off, 0.0f);
        const float thumb = 14.0f;
        const float thumbX = track.x + 1.0f + (toggleOn_ ? 16.0f : 0.0f);
        const float thumbY = track.y + 1.0f;
        if (ID2D1SolidColorBrush *thumbBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0xFFFFFF)))
        {
            target->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(thumbX + thumb * 0.5f, thumbY + thumb * 0.5f), thumb * 0.5f, thumb * 0.5f),
                thumbBrush);
        }
    }
}

bool MenuFlyoutItem::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool MenuFlyoutItem::IsFocusable() const
{
    return true;
}

void MenuFlyoutItem::SetHovered(bool hovered)
{
    if (hovered_ == hovered)
    {
        return;
    }
    hovered_ = hovered;
    InvalidateVisual();
    if (onHover_)
    {
        onHover_(hovered_);
    }
}

bool MenuFlyoutItem::OnMouseDown(const POINT &point, WPARAM keyState)
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

bool MenuFlyoutItem::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }
    const bool shouldClick = HitTest(window_->ClientPixelsToDips(point));
    pressed_ = false;
    InvalidateVisual();
    if (shouldClick && onClick_ && !hasSubmenu_)
    {
        // A menu item's handler almost always closes the menu it lives in, which
        // drops the last reference to this item and destroys onClick_ while its
        // operator() is still on the stack. The handler body then reloads its
        // captures from freed storage the teardown has already recycled. Copy the
        // handler so the closure outlives the call; nothing below touches *this*.
        ClickHandler handler = onClick_;
        if (showToggle_)
        {
            if (PointInRect(ToggleHitRect(), window_->ClientPixelsToDips(point)))
            {
                toggleOn_ = !toggleOn_;
                InvalidateVisual();
                handler();
            }
        }
        else
        {
            handler();
        }
    }
    return true;
}

bool MenuFlyoutItem::OnMouseMove(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }
    SetHovered(HitTest(window_->ClientPixelsToDips(point)));
    return hovered_;
}

void MenuFlyoutItem::OnMouseLeave()
{
    pressed_ = false;
    SetHovered(false);
}

bool MenuFlyoutItem::KeepsPopupsOpenOnClick() const
{
    return true;
}

HCURSOR MenuFlyoutItem::GetCursor() const
{
    return LoadCursor(nullptr, IDC_ARROW);
}

void MenuSeparator::SetColor(const D2D1_COLOR_F &color)
{
    color_ = color;
    InvalidateVisual();
}

SizeF MenuSeparator::Measure(const SizeF &availableSize)
{
    return {(std::min)((std::max)(availableSize.width, 72.0f), 220.0f), 7.0f};
}

void MenuSeparator::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void MenuSeparator::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }
    if (ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color_))
    {
        const float y = bounds_.y + bounds_.height * 0.5f;
        target->DrawLine(D2D1::Point2F(bounds_.x + 5.0f, y), D2D1::Point2F(bounds_.x + bounds_.width - 5.0f, y), brush,
                         1.0f);
    }
}

bool MenuSeparator::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

void Popup::LayoutOverlay(const SizeF &viewportSize)
{
    const SizeF available = {std::max(viewportSize.width - 24.0f, 0.0f), std::max(viewportSize.height - 24.0f, 0.0f)};
    const SizeF measured = MeasureInLayout(available);
    const float width = matchAnchorWidth_ ? std::max(measured.width, anchorRect_.width) : measured.width;
    const float height = measured.height;

    float x = anchorRect_.x + offsetX_;
    if (constrainToViewport_ && x + width > viewportSize.width - 8.0f)
    {
        x = std::max(viewportSize.width - width - 8.0f, 8.0f);
    }

    float y = placement_ == PopupPlacement::AboveLeading ? (anchorRect_.y - height - offsetY_)
                                                         : (anchorRect_.y + anchorRect_.height + offsetY_);
    if (constrainToViewport_ && placement_ == PopupPlacement::AboveLeading && y < 8.0f)
    {
        y = anchorRect_.y + anchorRect_.height + offsetY_;
    }
    if (constrainToViewport_ && placement_ == PopupPlacement::BelowLeading && y + height > viewportSize.height - 8.0f)
    {
        const float aboveY = anchorRect_.y - height - offsetY_;
        if (aboveY >= 8.0f)
        {
            y = aboveY;
        }
    }

    if (constrainToViewport_)
    {
        y = std::clamp(y, 8.0f, std::max(viewportSize.height - height - 8.0f, 8.0f));
    }
    ArrangeInLayout({x, y, width, height});
}

PopupHost::PopupHost(std::shared_ptr<Visual> trigger, std::shared_ptr<Popup> popup)
    : trigger_(std::move(trigger)), popup_(std::move(popup))
{
    AdoptChild(trigger_);
}

SizeF PopupHost::Measure(const SizeF &availableSize)
{
    return trigger_ ? trigger_->MeasureInLayout(availableSize) : SizeF{};
}

void PopupHost::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (trigger_)
    {
        trigger_->ArrangeInLayout(finalRect);
    }
    if (popup_)
    {
        popup_->SetAnchorRect(bounds_);
    }
}

void PopupHost::Render(DeviceResources &deviceResources)
{
    if (trigger_)
    {
        trigger_->Render(deviceResources);
    }
}

void PopupHost::Attach(Window *window)
{
    Visual::Attach(window);
    if (trigger_)
    {
        trigger_->Attach(window);
    }
    if (popup_)
    {
        popup_->Attach(window);
    }
}

Visual *PopupHost::FindVisualAt(const PointF &point)
{
    return HitTest(point) ? this : nullptr;
}

Visual *PopupHost::FindFocusableAt(const PointF &point)
{
    (void)point;
    return nullptr;
}

Visual *PopupHost::FindFirstFocusableDescendant()
{
    return nullptr;
}

bool PopupHost::HitTest(const PointF &point) const
{
    return trigger_ ? trigger_->HitTest(point) : false;
}

bool PopupHost::OnMouseDown(const POINT &point, WPARAM keyState)
{
    if (!window_)
    {
        return false;
    }

    pressed_ = HitTest(window_->ClientPixelsToDips(point));
    if (pressed_ && trigger_)
    {
        trigger_->OnMouseDown(point, keyState);
    }
    return pressed_;
}

bool PopupHost::OnMouseUp(const POINT &point, WPARAM keyState)
{
    if (!window_ || !pressed_)
    {
        return false;
    }

    const bool shouldToggle = HitTest(window_->ClientPixelsToDips(point));
    if (trigger_)
    {
        trigger_->OnMouseUp(point, keyState);
    }
    pressed_ = false;
    if (shouldToggle)
    {
        TogglePopup();
    }
    return true;
}

HCURSOR PopupHost::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

bool PopupHost::KeepsPopupsOpenOnClick() const
{
    return true;
}

void PopupHost::OpenPopup()
{
    if (open_ || !window_ || !popup_)
    {
        return;
    }

    popup_->SetAnchorRect(bounds_);
    if (Scene *scene = window_->GetScene())
    {
        scene->AddPopup(popup_, [this]() { open_ = false; });
        open_ = true;
        if (Visual *focusTarget = popup_->FindFirstFocusableDescendant())
        {
            window_->FocusVisual(focusTarget);
        }
    }
}

void PopupHost::ClosePopup()
{
    if (!open_ || !window_ || !popup_)
    {
        return;
    }

    if (Scene *scene = window_->GetScene())
    {
        scene->RemovePopup(popup_.get(), false);
    }
    open_ = false;
}

void PopupHost::TogglePopup()
{
    if (open_)
    {
        ClosePopup();
    }
    else
    {
        OpenPopup();
    }
}

bool PopupHost::IsOpen() const
{
    return open_;
}

ContextMenuHost::ContextMenuHost(std::shared_ptr<Visual> trigger, std::shared_ptr<Popup> popup)
    : trigger_(std::move(trigger)), popup_(std::move(popup))
{
    AdoptChild(trigger_);
}

SizeF ContextMenuHost::Measure(const SizeF &availableSize)
{
    return trigger_ ? trigger_->MeasureInLayout(availableSize) : SizeF{};
}

void ContextMenuHost::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (trigger_)
    {
        trigger_->ArrangeInLayout(finalRect);
    }
}

void ContextMenuHost::Render(DeviceResources &deviceResources)
{
    if (trigger_)
    {
        trigger_->Render(deviceResources);
    }
}

void ContextMenuHost::Attach(Window *window)
{
    Visual::Attach(window);
    if (trigger_)
    {
        trigger_->Attach(window);
    }
    if (popup_)
    {
        popup_->Attach(window);
    }
}

Visual *ContextMenuHost::FindVisualAt(const PointF &point)
{
    return HitTest(point) ? this : nullptr;
}

Visual *ContextMenuHost::FindFocusableAt(const PointF &point)
{
    (void)point;
    return nullptr;
}

Visual *ContextMenuHost::FindFirstFocusableDescendant()
{
    return nullptr;
}

bool ContextMenuHost::HitTest(const PointF &point) const
{
    return trigger_ ? trigger_->HitTest(point) : false;
}

bool ContextMenuHost::OnContextMenu(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    if (!HitTest(dipPoint))
    {
        return false;
    }

    OpenPopupAt(dipPoint);
    return true;
}

HCURSOR ContextMenuHost::GetCursor() const
{
    return trigger_ ? trigger_->GetCursor() : LoadCursor(nullptr, IDC_ARROW);
}

bool ContextMenuHost::KeepsPopupsOpenOnClick() const
{
    return true;
}

void ContextMenuHost::OpenPopupAt(const PointF &anchorPoint)
{
    if (!window_ || !popup_)
    {
        return;
    }

    if (Scene *scene = window_->GetScene())
    {
        if (open_)
        {
            scene->RemovePopup(popup_.get(), false);
            open_ = false;
        }

        popup_->SetAnchorRect({anchorPoint.x, anchorPoint.y, 1.0f, 1.0f});
        scene->AddPopup(popup_, [this]() {
            open_ = false;
            InvalidateVisual();
        });
        open_ = true;
        if (Visual *focusTarget = popup_->FindFirstFocusableDescendant())
        {
            window_->FocusVisual(focusTarget);
        }
        InvalidateVisual();
    }
}

void ContextMenuHost::ClosePopup()
{
    if (!open_ || !window_ || !popup_)
    {
        return;
    }

    if (Scene *scene = window_->GetScene())
    {
        scene->RemovePopup(popup_.get(), false);
    }
    open_ = false;
    InvalidateVisual();
}

bool ContextMenuHost::IsOpen() const
{
    return open_;
}

ComboBox::ComboBox(float height) : preferredHeight_(height)
{
    auto popupContent = std::make_shared<ComboBoxPopupContent>(40.0f);
    popupContent->SetOnSelect([this](size_t index) {
        if (index >= items_.size())
        {
            return;
        }

        selectedIndex_ = index;
        SyncPopupState();
        NotifySelectionChanged();
        ClosePopup();
        InvalidateVisual();
    });
    popupContent_ = popupContent;

    popup_ = std::make_shared<Popup>(popupContent_);
    popup_->SetPlacement(PopupPlacement::BelowLeading);
    popup_->SetOffset(0.0f, 6.0f);
    popup_->SetConstrainToViewport(false);
}

void ComboBox::AddItem(std::wstring item)
{
    items_.push_back(std::move(item));
    if (selectedIndex_ == static_cast<size_t>(-1))
    {
        selectedIndex_ = 0;
    }
    SyncPopupState();
    InvalidateMeasure();
    InvalidateVisual();
}

void ComboBox::ClearItems()
{
    items_.clear();
    selectedIndex_ = static_cast<size_t>(-1);
    ClosePopup();
    SyncPopupState();
    InvalidateMeasure();
    InvalidateVisual();
}

void ComboBox::SetSelectedIndex(size_t index)
{
    if (index >= items_.size() || selectedIndex_ == index)
    {
        return;
    }

    selectedIndex_ = index;
    SyncPopupState();
    NotifySelectionChanged();
    InvalidateVisual();
}

size_t ComboBox::GetSelectedIndex() const
{
    return selectedIndex_;
}

const std::wstring &ComboBox::GetSelectedText() const
{
    static const std::wstring empty;
    return selectedIndex_ < items_.size() ? items_[selectedIndex_] : empty;
}

void ComboBox::SetOnSelectionChanged(SelectionChangedHandler handler)
{
    onSelectionChanged_ = std::move(handler);
}

SizeF ComboBox::Measure(const SizeF &availableSize)
{
    float textWidth = 140.0f;
    for (const auto &item : items_)
    {
        const SizeF measured =
            MeasureText(GetSharedDWriteFactory(), item, 15.0f, false, std::max(availableSize.width - 44.0f, 1.0f));
        textWidth = std::max(textWidth, measured.width + 44.0f);
    }

    return {std::min(availableSize.width, textWidth), preferredHeight_};
}

void ComboBox::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (popup_)
    {
        popup_->SetAnchorRect(bounds_);
    }
}

void ComboBox::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const D2D1_COLOR_F fill = pressed_ ? theme.surfaceMuted : theme.surface;
    const D2D1_COLOR_F stroke = focused_ || open_ ? theme.primaryFocus : theme.border;
    FillRoundedRect(deviceResources, bounds_, kControlCornerRadius, fill, stroke, focused_ || open_ ? 2.0f : 1.0f);

    const std::wstring text = GetSelectedText().empty() ? L"Select an option" : GetSelectedText();
    const D2D1_COLOR_F textColor = GetSelectedText().empty() ? theme.textSecondary : theme.textPrimary;
    DrawLabel(deviceResources, text, 15.0f, false, textColor,
              {bounds_.x + 16.0f, bounds_.y, std::max(bounds_.width - 44.0f, 0.0f), bounds_.height},
              DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(theme.textSecondary);
    if (!brush)
    {
        return;
    }
    const float centerX = bounds_.x + bounds_.width - 18.0f;
    const float centerY = bounds_.y + bounds_.height * 0.5f + (open_ ? -1.0f : 1.0f);
    const float direction = open_ ? -1.0f : 1.0f;
    target->DrawLine(D2D1::Point2F(centerX - 5.0f, centerY - 3.0f * direction),
                     D2D1::Point2F(centerX, centerY + 2.0f * direction), brush, 1.8f);
    target->DrawLine(D2D1::Point2F(centerX, centerY + 2.0f * direction),
                     D2D1::Point2F(centerX + 5.0f, centerY - 3.0f * direction), brush, 1.8f);
}

void ComboBox::Attach(Window *window)
{
    Visual::Attach(window);
    if (popup_)
    {
        popup_->Attach(window);
    }
}

bool ComboBox::HitTest(const PointF &point) const
{
    return PointInRoundedRect(bounds_, kControlCornerRadius, point);
}

bool ComboBox::IsFocusable() const
{
    return true;
}

void ComboBox::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool ComboBox::OnMouseDown(const POINT &point, WPARAM keyState)
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

bool ComboBox::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const bool shouldToggle = HitTest(window_->ClientPixelsToDips(point));
    pressed_ = false;
    InvalidateVisual();
    if (shouldToggle)
    {
        TogglePopup();
    }
    return true;
}

HCURSOR ComboBox::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

bool ComboBox::KeepsPopupsOpenOnClick() const
{
    return true;
}

void ComboBox::OpenPopup()
{
    if (open_ || !window_ || !popup_ || items_.empty())
    {
        return;
    }

    if (const auto popupContent = std::dynamic_pointer_cast<ComboBoxPopupContent>(popupContent_))
    {
        popupContent->SetHighlightedIndex(selectedIndex_);
    }

    popup_->SetAnchorRect(bounds_);
    if (Scene *scene = window_->GetScene())
    {
        scene->AddPopup(popup_, [this]() {
            open_ = false;
            InvalidateVisual();
        });
        open_ = true;
        if (Visual *focusTarget = popup_->FindFirstFocusableDescendant())
        {
            window_->FocusVisual(focusTarget);
        }
        InvalidateVisual();
    }
}

void ComboBox::ClosePopup()
{
    if (!open_ || !window_ || !popup_)
    {
        return;
    }

    if (Scene *scene = window_->GetScene())
    {
        scene->RemovePopup(popup_.get(), false);
    }
    open_ = false;
    InvalidateVisual();
}

void ComboBox::TogglePopup()
{
    if (open_)
    {
        ClosePopup();
    }
    else
    {
        OpenPopup();
    }
}

void ComboBox::NotifySelectionChanged()
{
    if (onSelectionChanged_ && selectedIndex_ < items_.size())
    {
        onSelectionChanged_(selectedIndex_, items_[selectedIndex_]);
    }
}

void ComboBox::SyncPopupState()
{
    if (const auto popupContent = std::dynamic_pointer_cast<ComboBoxPopupContent>(popupContent_))
    {
        popupContent->SetItems(items_);
        popupContent->SetSelectedIndex(selectedIndex_);
    }
}

SizeF Button::Measure(const SizeF &availableSize)
{
    const float textWidth = std::max(availableSize.width - kControlPaddingX * 2.0f, 1.0f);
    const Theme &theme = ThemeManager::GetCurrent();
    if (!cachedTextLayout_ || cachedLayoutWidth_ != textWidth || cachedFontFamily_ != theme.uiFontFamily)
    {
        cachedTextLayout_ = CreateCachedTextLayout(GetSharedDWriteFactory(), theme.uiFontFamily, text_, 16.0f,
                                                   DWRITE_FONT_WEIGHT_SEMI_BOLD, textWidth, preferredHeight_,
                                                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                                   DWRITE_WORD_WRAPPING_NO_WRAP);
        cachedLayoutWidth_ = textWidth;
        cachedFontFamily_ = theme.uiFontFamily;
    }

    SizeF measuredText = MeasureText(GetSharedDWriteFactory(), text_, 16.0f, true, textWidth);
    if (cachedTextLayout_)
    {
        DWRITE_TEXT_METRICS metrics = {};
        if (SUCCEEDED(cachedTextLayout_->GetMetrics(&metrics)))
        {
            measuredText = {std::ceil(metrics.widthIncludingTrailingWhitespace), std::ceil(metrics.height)};
        }
    }

    return {std::min(availableSize.width, measuredText.width + kControlPaddingX * 2.0f),
            std::max(preferredHeight_, measuredText.height + kControlPaddingY * 2.0f)};
}

void Button::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Button::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    FillRoundedRect(deviceResources, bounds_, kControlCornerRadius, GetFillColor(), GetStrokeColor(), 1.0f);

    const Theme &theme = ThemeManager::GetCurrent();
    const float textWidth = std::max(bounds_.width - kControlPaddingX * 2.0f, 1.0f);
    if (!cachedTextLayout_ || cachedLayoutWidth_ != textWidth || cachedFontFamily_ != theme.uiFontFamily)
    {
        cachedTextLayout_ = CreateCachedTextLayout(deviceResources.GetDWriteFactory(), theme.uiFontFamily, text_, 16.0f,
                                                   DWRITE_FONT_WEIGHT_SEMI_BOLD, textWidth, bounds_.height,
                                                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                                   DWRITE_WORD_WRAPPING_NO_WRAP);
        cachedLayoutWidth_ = textWidth;
        cachedFontFamily_ = theme.uiFontFamily;
    }

    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(GetTextColor());
    if (!cachedTextLayout_ || !brush)
    {
        return;
    }

    target->DrawTextLayout(D2D1::Point2F(bounds_.x + kControlPaddingX, bounds_.y), cachedTextLayout_.Get(), brush,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

bool Button::HitTest(const PointF &point) const
{
    return PointInRoundedRect(bounds_, kControlCornerRadius, point);
}

bool Button::IsFocusable() const
{
    return true;
}

void Button::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool Button::OnMouseDown(const POINT &point, WPARAM keyState)
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

bool Button::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const bool shouldClick = HitTest(window_->ClientPixelsToDips(point));
    pressed_ = false;
    InvalidateVisual();
    if (shouldClick)
    {
        OnClick();
    }
    return true;
}

HCURSOR Button::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

void Button::OnClick()
{
    if (onClick_)
    {
        // Same reason as MenuFlyoutItem::OnMouseUp: a handler that tears down the
        // panel holding this button destroys onClick_ mid-call.
        ClickHandler handler = onClick_;
        handler();
    }
}

D2D1_COLOR_F Button::GetFillColor() const
{
    const Theme &theme = ThemeManager::GetCurrent();
    return pressed_ ? theme.primaryPressed : theme.primary;
}

D2D1_COLOR_F Button::GetStrokeColor() const
{
    const Theme &theme = ThemeManager::GetCurrent();
    return theme.primaryPressed;
}

D2D1_COLOR_F Button::GetTextColor() const
{
    return ThemeManager::GetCurrent().textInverse;
}

CheckBox::CheckBox(std::wstring text, bool checked) : text_(std::move(text)), checked_(checked)
{
}

void CheckBox::SetOnChanged(ChangeHandler handler)
{
    onChanged_ = std::move(handler);
}

bool CheckBox::IsChecked() const
{
    return checked_;
}

void CheckBox::SetChecked(bool checked)
{
    if (checked_ == checked)
    {
        return;
    }

    checked_ = checked;
    InvalidateVisual();
}

SizeF CheckBox::Measure(const SizeF &availableSize)
{
    const float textWidth = std::max(availableSize.width - kCheckBoxIndicatorSize - 12.0f, 1.0f);
    const SizeF measuredText = MeasureText(GetSharedDWriteFactory(), text_, 15.0f, false, textWidth);
    return {std::min(availableSize.width, kCheckBoxIndicatorSize + 12.0f + measuredText.width),
            std::max(28.0f, measuredText.height)};
}

void CheckBox::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void CheckBox::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const float indicatorY = bounds_.y + (bounds_.height - kCheckBoxIndicatorSize) * 0.5f;
    const RectF indicator = {bounds_.x, indicatorY, kCheckBoxIndicatorSize, kCheckBoxIndicatorSize};
    FillRoundedRect(deviceResources, indicator, 6.0f, checked_ ? theme.primary : theme.surface,
                    focused_ ? theme.primaryFocusStrong : theme.border, focused_ ? 2.0f : 1.0f);

    if (checked_)
    {
        ID2D1SolidColorBrush *checkBrush = deviceResources.GetSolidColorBrush(theme.textInverse);
        if (!checkBrush)
        {
            return;
        }
        target->DrawLine(D2D1::Point2F(indicator.x + 5.0f, indicator.y + 11.0f),
                         D2D1::Point2F(indicator.x + 9.0f, indicator.y + 15.0f), checkBrush, 2.0f);
        target->DrawLine(D2D1::Point2F(indicator.x + 9.0f, indicator.y + 15.0f),
                         D2D1::Point2F(indicator.x + 15.0f, indicator.y + 6.0f), checkBrush, 2.0f);
    }

    const RectF textRect = {indicator.x + indicator.width + 12.0f, bounds_.y,
                            std::max(bounds_.width - indicator.width - 12.0f, 0.0f), bounds_.height};
    DrawTextBlock(deviceResources, text_, 15.0f, false, theme.textPrimary, textRect, DWRITE_TEXT_ALIGNMENT_LEADING);
}

bool CheckBox::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool CheckBox::IsFocusable() const
{
    return true;
}

void CheckBox::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool CheckBox::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    pressed_ = HitTest(window_->ClientPixelsToDips(point));
    return pressed_;
}

bool CheckBox::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const bool shouldToggle = HitTest(window_->ClientPixelsToDips(point));
    pressed_ = false;
    if (shouldToggle)
    {
        checked_ = !checked_;
        if (onChanged_)
        {
            onChanged_(checked_);
        }
    }

    InvalidateVisual();
    return true;
}

HCURSOR CheckBox::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

ProgressBar::ProgressBar(float height) : preferredHeight_(height)
{
}

void ProgressBar::SetValue(float value)
{
    value_ = std::clamp(value, 0.0f, 1.0f);
    InvalidateVisual();
}

float ProgressBar::GetValue() const
{
    return value_;
}

SizeF ProgressBar::Measure(const SizeF &availableSize)
{
    return {availableSize.width, preferredHeight_};
}

void ProgressBar::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void ProgressBar::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    FillRoundedRect(deviceResources, bounds_, preferredHeight_ * 0.5f, theme.track, theme.border, 1.0f);

    const RectF fillRect = {bounds_.x, bounds_.y, bounds_.width * value_, bounds_.height};
    if (fillRect.width > 0.0f)
    {
        FillRoundedRect(deviceResources, fillRect, preferredHeight_ * 0.5f, theme.success, theme.success, 1.0f);
    }
}

Slider::Slider(float minValue, float maxValue, float value, float height)
    : minValue_(minValue), maxValue_(std::max(maxValue, minValue)), preferredHeight_(height)
{
    SetValueInternal(value, false);
}

void Slider::SetOnChanged(ChangeHandler handler)
{
    onChanged_ = std::move(handler);
}

void Slider::SetValue(float value)
{
    SetValueInternal(value, false);
}

float Slider::GetValue() const
{
    return value_;
}

SizeF Slider::Measure(const SizeF &availableSize)
{
    return {availableSize.width, preferredHeight_};
}

void Slider::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Slider::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const RectF track = {bounds_.x, bounds_.y + (bounds_.height - kSliderTrackHeight) * 0.5f, bounds_.width,
                         kSliderTrackHeight};
    FillRoundedRect(deviceResources, track, kSliderTrackHeight * 0.5f, theme.track, theme.border, 1.0f);

    const RectF active = {track.x, track.y, track.width * NormalizedValue(), track.height};
    if (active.width > 0.0f)
    {
        FillRoundedRect(deviceResources, active, kSliderTrackHeight * 0.5f, theme.primary, theme.primary, 1.0f);
    }

    const float thumbCenterX = track.x + track.width * NormalizedValue();
    const float thumbCenterY = bounds_.y + bounds_.height * 0.5f;
    ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(theme.surface);
    ID2D1SolidColorBrush *strokeBrush =
        deviceResources.GetSolidColorBrush(focused_ || dragging_ ? theme.primary : theme.thumb);
    if (!fillBrush || !strokeBrush)
    {
        return;
    }
    target->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(thumbCenterX, thumbCenterY), kSliderThumbRadius, kSliderThumbRadius), fillBrush);
    target->DrawEllipse(
        D2D1::Ellipse(D2D1::Point2F(thumbCenterX, thumbCenterY), kSliderThumbRadius, kSliderThumbRadius), strokeBrush,
        focused_ || dragging_ ? 2.0f : 1.0f);
}

bool Slider::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool Slider::IsFocusable() const
{
    return true;
}

void Slider::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool Slider::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    dragging_ = UpdateFromPoint(point, true);
    return dragging_;
}

bool Slider::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!dragging_)
    {
        return false;
    }

    UpdateFromPoint(point, true);
    dragging_ = false;
    InvalidateVisual();
    return true;
}

bool Slider::OnMouseMove(const POINT &point, WPARAM keyState)
{
    if (!dragging_ || !(keyState & MK_LBUTTON))
    {
        return false;
    }

    return UpdateFromPoint(point, true);
}

HCURSOR Slider::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

float Slider::NormalizedValue() const
{
    const float range = maxValue_ - minValue_;
    if (range <= 0.0f)
    {
        return 0.0f;
    }

    return std::clamp((value_ - minValue_) / range, 0.0f, 1.0f);
}

void Slider::SetValueInternal(float value, bool notify)
{
    const float oldValue = value_;
    value_ = std::clamp(value, minValue_, maxValue_);
    InvalidateVisual();

    if (notify && oldValue != value_ && onChanged_)
    {
        onChanged_(value_);
    }
}

bool Slider::UpdateFromPoint(const POINT &point, bool notify)
{
    if (!window_ || bounds_.width <= 0.0f)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const float ratio = std::clamp((dipPoint.x - bounds_.x) / bounds_.width, 0.0f, 1.0f);
    SetValueInternal(minValue_ + (maxValue_ - minValue_) * ratio, notify);
    return true;
}

Separator::Separator(float height) : height_(height)
{
}

SizeF Separator::Measure(const SizeF &availableSize)
{
    return {availableSize.width, height_};
}

void Separator::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Separator::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0xE2E8F0));
    if (!brush)
    {
        return;
    }
    const float centerY = bounds_.y + bounds_.height * 0.5f;
    target->DrawLine(D2D1::Point2F(bounds_.x, centerY), D2D1::Point2F(bounds_.x + bounds_.width, centerY), brush,
                     std::max(bounds_.height, 1.0f));
}

ListView::ListView(float itemHeight) : itemHeight_(itemHeight)
{
}

void ListView::InvalidateLayoutCache()
{
    layoutCache_.clear();
}

void ListView::AddItem(Item item)
{
    items_.push_back(std::move(item));
    layoutCache_.push_back({});
    InvalidateMeasure();
}

void ListView::ClearItems()
{
    items_.clear();
    InvalidateLayoutCache();
    selectedIndex_ = 0;
    pressedIndex_ = static_cast<size_t>(-1);
    InvalidateMeasure();
}

void ListView::SetOnSelectionChanged(SelectionChangedHandler handler)
{
    onSelectionChanged_ = std::move(handler);
}

void ListView::SetSelectedIndex(size_t index)
{
    if (items_.empty())
    {
        selectedIndex_ = 0;
        return;
    }

    const size_t clamped = std::min(index, items_.size() - 1);
    if (selectedIndex_ == clamped)
    {
        return;
    }

    selectedIndex_ = clamped;
    if (onSelectionChanged_)
    {
        onSelectionChanged_(selectedIndex_);
    }
    if (window_)
    {
        window_->Relayout();
    }
}

size_t ListView::GetSelectedIndex() const
{
    return selectedIndex_;
}

SizeF ListView::Measure(const SizeF &availableSize)
{
    const float height = items_.empty() ? itemHeight_ : itemHeight_ * static_cast<float>(items_.size());
    return {availableSize.width, std::min(height, availableSize.height)};
}

void ListView::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void ListView::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    IDWriteFactory *factory = deviceResources.GetDWriteFactory();
    if (!target || !factory)
    {
        return;
    }

    if (layoutCache_.size() != items_.size())
    {
        layoutCache_.resize(items_.size());
    }

    const Theme &theme = ThemeManager::GetCurrent();
    for (size_t index = 0; index < items_.size(); ++index)
    {
        const float itemY = bounds_.y + itemHeight_ * static_cast<float>(index);
        const RectF itemRect = {bounds_.x, itemY, bounds_.width, itemHeight_ - 6.0f};
        const bool selected = index == selectedIndex_;
        const bool pressed = pressed_ && index == pressedIndex_;

        FillRoundedRect(deviceResources, itemRect, kListItemCornerRadius,
                        pressed ? D2D1::ColorF(0xDBEAFE) : (selected ? D2D1::ColorF(0xEFF6FF) : D2D1::ColorF(0xFFFFFF)),
                        selected ? D2D1::ColorF(0x60A5FA) : D2D1::ColorF(0xD6DCE5), selected || focused_ ? 2.0f : 1.0f);

        const RectF titleRect = {itemRect.x + 16.0f, itemRect.y + 10.0f, std::max(itemRect.width - 120.0f, 0.0f),
                                 22.0f};
        const RectF subtitleRect = {itemRect.x + 16.0f, itemRect.y + 34.0f, std::max(itemRect.width - 120.0f, 0.0f),
                                    18.0f};
        auto &cache = layoutCache_[index];
        if (cache.fontFamily != theme.uiFontFamily || cache.titleWidth != titleRect.width)
        {
            cache.titleLayout = CreateCachedTextLayout(factory, theme.uiFontFamily, items_[index].title, 15.0f,
                                                       DWRITE_FONT_WEIGHT_SEMI_BOLD, std::max(titleRect.width, 1.0f),
                                                       std::max(titleRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                                                       DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP);
            cache.titleWidth = titleRect.width;
            cache.fontFamily = theme.uiFontFamily;
        }
        if (cache.fontFamily != theme.uiFontFamily || cache.subtitleWidth != subtitleRect.width)
        {
            cache.subtitleLayout = CreateCachedTextLayout(
                factory, theme.uiFontFamily, items_[index].subtitle, 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                std::max(subtitleRect.width, 1.0f), std::max(subtitleRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP);
            cache.subtitleWidth = subtitleRect.width;
            cache.fontFamily = theme.uiFontFamily;
        }

        ID2D1SolidColorBrush *titleBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0x0F172A));
        ID2D1SolidColorBrush *subtitleBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0x64748B));
        if (cache.titleLayout && titleBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(titleRect.x, titleRect.y), cache.titleLayout.Get(), titleBrush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (cache.subtitleLayout && subtitleBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(subtitleRect.x, subtitleRect.y), cache.subtitleLayout.Get(),
                                   subtitleBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        if (!items_[index].badge.empty())
        {
            const RectF badgeRect = {itemRect.x + itemRect.width - 88.0f, itemRect.y + 18.0f, 72.0f, 28.0f};
            FillRoundedRect(deviceResources, badgeRect, 14.0f,
                            selected ? D2D1::ColorF(0x2563EB) : D2D1::ColorF(0xE2E8F0),
                            selected ? D2D1::ColorF(0x2563EB) : D2D1::ColorF(0xCBD5E1), 1.0f);
            if (cache.fontFamily != theme.uiFontFamily || cache.badgeWidth != badgeRect.width)
            {
                cache.badgeLayout = CreateCachedTextLayout(
                    factory, theme.uiFontFamily, items_[index].badge, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    std::max(badgeRect.width, 1.0f), std::max(badgeRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                cache.badgeWidth = badgeRect.width;
                cache.fontFamily = theme.uiFontFamily;
            }

            ID2D1SolidColorBrush *badgeBrush =
                deviceResources.GetSolidColorBrush(selected ? D2D1::ColorF(0xFFFFFF) : D2D1::ColorF(0x334155));
            if (cache.badgeLayout && badgeBrush)
            {
                target->DrawTextLayout(D2D1::Point2F(badgeRect.x, badgeRect.y), cache.badgeLayout.Get(), badgeBrush,
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }
}

bool ListView::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool ListView::IsFocusable() const
{
    return true;
}

void ListView::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool ListView::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    if (hit == static_cast<size_t>(-1))
    {
        return false;
    }

    pressed_ = true;
    pressedIndex_ = hit;
    InvalidateVisual();
    return true;
}

bool ListView::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    const size_t pressedIndex = pressedIndex_;
    pressed_ = false;
    pressedIndex_ = static_cast<size_t>(-1);

    if (hit != static_cast<size_t>(-1) && hit == pressedIndex)
    {
        SetSelectedIndex(hit);
    }

    InvalidateVisual();
    return true;
}

HCURSOR ListView::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

size_t ListView::HitTestItem(const PointF &point) const
{
    if (!HitTest(point) || items_.empty())
    {
        return static_cast<size_t>(-1);
    }

    const float relativeY = point.y - bounds_.y;
    const size_t index = static_cast<size_t>(relativeY / itemHeight_);
    return index < items_.size() ? index : static_cast<size_t>(-1);
}

CandidateList::CandidateList(float itemHeight)
{
    appearance_.itemHeight = itemHeight;
}

void CandidateList::InvalidateLayoutCache()
{
    layoutCache_.clear();
}

void CandidateList::AddItem(Item item)
{
    items_.push_back(std::move(item));
    layoutCache_.push_back({});
    InvalidateMeasure();
}

void CandidateList::SetItems(std::vector<Item> items)
{
    items_ = std::move(items);
    InvalidateLayoutCache();
    if (selectedIndex_ >= items_.size())
    {
        selectedIndex_ = items_.empty() ? 0 : items_.size() - 1;
    }
    pressedIndex_ = static_cast<size_t>(-1);
    hoveredIndex_ = static_cast<size_t>(-1);
    InvalidateMeasure();
}

void CandidateList::ClearItems()
{
    items_.clear();
    InvalidateLayoutCache();
    selectedIndex_ = 0;
    pressedIndex_ = static_cast<size_t>(-1);
    InvalidateMeasure();
}

void CandidateList::SetSelectedIndex(size_t index)
{
    if (items_.empty())
    {
        selectedIndex_ = 0;
        return;
    }

    const size_t clamped = std::min(index, items_.size() - 1);
    if (selectedIndex_ == clamped)
    {
        return;
    }

    selectedIndex_ = clamped;
    if (onSelectionChanged_)
    {
        onSelectionChanged_(selectedIndex_);
    }
    InvalidateVisual();
}

size_t CandidateList::GetSelectedIndex() const
{
    return selectedIndex_;
}

void CandidateList::SetOnSelectionChanged(SelectionChangedHandler handler)
{
    onSelectionChanged_ = std::move(handler);
}

void CandidateList::SetOnItemActivated(ItemActivatedHandler handler)
{
    onItemActivated_ = std::move(handler);
}

void CandidateList::SetOnContextMenu(ContextMenuHandler handler)
{
    onContextMenu_ = std::move(handler);
}

const CandidateList::Item *CandidateList::GetItem(size_t index) const
{
    return index < items_.size() ? &items_[index] : nullptr;
}

void CandidateList::SetAppearance(Appearance appearance)
{
    appearance_ = appearance;
    InvalidateLayoutCache();
    InvalidateMeasure();
}

void CandidateList::SetOrientation(Orientation orientation)
{
    orientation_ = orientation;
    InvalidateLayoutCache();
    InvalidateMeasure();
}

void CandidateList::SetHoverEnabled(bool enabled)
{
    hoverEnabled_ = enabled;
    if (!enabled)
    {
        hoveredIndex_ = static_cast<size_t>(-1);
    }
    InvalidateVisual();
}

float CandidateList::EstimateTextWidth(const std::wstring &text, float fontSize) const
{
    if (text.empty())
    {
        return 0.0f;
    }

    IDWriteFactory *factory = GetSharedDWriteFactory();
    const Theme &theme = ThemeManager::GetCurrent();
    const std::wstring &fontFamily =
        appearance_.fontFamily.empty() ? theme.textInputFontFamily : appearance_.fontFamily;
    ComPtr<IDWriteTextLayout> layout = CreateCachedTextLayout(
        factory, fontFamily, text, fontSize, DWRITE_FONT_WEIGHT_NORMAL, 4096.0f, std::max(fontSize * 2.0f, 1.0f),
        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP,
        appearance_.fallbackFontFamilies);
    if (!layout)
    {
        float width = 0.0f;
        for (const wchar_t ch : text)
        {
            width += (ch < 128) ? fontSize * 0.72f : fontSize;
        }
        return width;
    }

    DWRITE_TEXT_METRICS metrics = {};
    layout->GetMetrics(&metrics);
    DWRITE_OVERHANG_METRICS overhang = {};
    layout->GetOverhangMetrics(&overhang);
    return std::ceil(metrics.widthIncludingTrailingWhitespace + std::max(overhang.right, 0.0f) + 1.0f);
}

float CandidateList::MeasureTextHeight(const std::wstring &text, float fontSize, float width) const
{
    if (text.empty())
        return 0.0f;
    const Theme &theme = ThemeManager::GetCurrent();
    const auto &family = appearance_.fontFamily.empty() ? theme.textInputFontFamily : appearance_.fontFamily;
    const auto layout =
        CreateCachedTextLayout(GetSharedDWriteFactory(), family, text, fontSize, DWRITE_FONT_WEIGHT_NORMAL, width,
                               65536.0f, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                               DWRITE_WORD_WRAPPING_WRAP, appearance_.fallbackFontFamilies);
    DWRITE_TEXT_METRICS metrics{};
    if (layout && SUCCEEDED(layout->GetMetrics(&metrics)))
        return std::ceil(metrics.height);
    return std::ceil(EstimateTextWidth(text, fontSize) / width) * fontSize * 1.25f;
}

CandidateList::ItemGeometry CandidateList::MeasureItem(size_t index, float width) const
{
    const auto &item = items_[index];
    ItemGeometry geometry;
    const float labelX = appearance_.contentPadLeft + appearance_.textPadLeft;
    const float labelW = std::max(EstimateTextWidth(item.label, appearance_.labelFontSize), 9.0f);
    const float textX = labelX + labelW + appearance_.labelGap;
    const float contentWidth = std::max(width - textX - appearance_.contentPadRight, 1.0f);
    const float textW = std::min(std::max(EstimateTextWidth(item.text, appearance_.fontSize), 8.0f), contentWidth);
    const float textH = std::max(appearance_.itemHeight, MeasureTextHeight(item.text, appearance_.fontSize, textW));
    geometry.label = {labelX, 0.0f, labelW, appearance_.itemHeight};
    geometry.text = {textX, 0.0f, textW, textH};
    float height = textH;
    float lineEnd = textW;

    // 短字段保持原有并排方式；空间不足时让辅助码和翻译完整换行。
    if (!item.annotation.empty())
    {
        const float annotationW =
            std::min(EstimateTextWidth(item.annotation, appearance_.annotationFontSize), contentWidth);
        const bool inlineAnnotation = lineEnd + 4.0f + annotationW <= contentWidth;
        const float annotationH = std::max(
            appearance_.itemHeight, MeasureTextHeight(item.annotation, appearance_.annotationFontSize, annotationW));
        geometry.annotation = {textX + (inlineAnnotation ? lineEnd + 4.0f : 0.0f), inlineAnnotation ? 0.0f : height,
                               annotationW, annotationH};
        height = std::max(height, geometry.annotation.y + annotationH);
        lineEnd = geometry.annotation.x - textX + annotationW;
    }
    if (!item.translation.empty())
    {
        const float fontSize = appearance_.fontSize * 0.78f;
        const float gap = appearance_.fontSize * 0.65f;
        const float naturalTranslationWidth = EstimateTextWidth(item.translation, fontSize);
        const float translationW = std::min(naturalTranslationWidth, contentWidth);
        const bool inlineTranslation = orientation_ == Orientation::Vertical && geometry.annotation.y == 0.0f &&
                                       lineEnd + gap + translationW <= contentWidth;
        const float translationH =
            inlineTranslation
                ? textH
                : (naturalTranslationWidth <= contentWidth
                       ? fontSize * 1.25f
                       : std::max(fontSize * 1.25f, MeasureTextHeight(item.translation, fontSize, translationW)));
        geometry.translation = {textX + (inlineTranslation ? lineEnd + gap : 0.0f), inlineTranslation ? 0.0f : height,
                                translationW, translationH};
        height = std::max(height, geometry.translation.y + translationH);
    }
    geometry.bounds = {0.0f, 0.0f, width, height};
    return geometry;
}

RectF CandidateList::ItemRect(size_t index) const
{
    if (index >= itemGeometry_.size())
        return {};
    RectF rect = itemGeometry_[index].bounds;
    rect.x += bounds_.x;
    rect.y += bounds_.y;
    // 竖排：每行都应铺满列表实际宽度（列表已被拉伸到卡片内宽），使选中高亮与命中区域
    // 覆盖整行。这里直接用列表的最终布局宽度 bounds_，而非候选项自然宽度，从而不受测量/
    // 布局缓存影响（绘制前 Present 可能以不同可用宽度重新测量，把每项宽度还原为自然宽度）。
    // 右侧留白由外层卡片内边距提供，与左侧保持一致。
    if (orientation_ == Orientation::Vertical)
    {
        rect.x = bounds_.x;
        rect.width = bounds_.width;
    }
    return rect;
}

RectF CandidateList::GetItemBounds(size_t index) const
{
    return ItemRect(index);
}

SizeF CandidateList::Measure(const SizeF &availableSize)
{
    // 宽度变化也会改变每段文字的高度，不能继续使用上一轮的文字布局缓存。
    InvalidateLayoutCache();
    itemGeometry_.clear();
    const float availableWidth = std::max(availableSize.width, 1.0f);
    const float gap = appearance_.itemGap;
    const bool horizontal = orientation_ == Orientation::Horizontal;
    std::vector<float> widths;
    float maxWidth = 80.0f;
    for (const auto &item : items_)
    {
        const float textWidth = EstimateTextWidth(item.text, appearance_.fontSize) + 6.0f +
                                EstimateTextWidth(item.annotation, appearance_.annotationFontSize);
        const float translationWidth = EstimateTextWidth(item.translation, appearance_.fontSize * 0.78f);
        const float contentWidth =
            horizontal
                ? std::max(textWidth, translationWidth)
                : textWidth + (item.translation.empty() ? 0.0f : appearance_.fontSize * 0.65f + translationWidth);
        const float width = appearance_.contentPadLeft + appearance_.textPadLeft +
                            std::max(EstimateTextWidth(item.label, appearance_.labelFontSize), 9.0f) +
                            appearance_.labelGap + contentWidth + appearance_.contentPadRight;
        widths.push_back(std::min(width, availableWidth));
        maxWidth = std::max(maxWidth, width);
    }
    const float verticalWidth = std::min(maxWidth, availableWidth);
    float x = 0.0f;
    float y = 0.0f;
    float rowHeight = 0.0f;
    float measuredWidth = horizontal ? 0.0f : verticalWidth;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        auto geometry = MeasureItem(i, horizontal ? widths[i] : verticalWidth);
        if (horizontal && x > 0.0f && x + geometry.bounds.width > availableWidth)
        {
            y += rowHeight + gap;
            x = 0.0f;
            rowHeight = 0.0f;
        }
        geometry.bounds.x = x;
        geometry.bounds.y = y;
        itemGeometry_.push_back(geometry);
        measuredWidth = std::max(measuredWidth, x + geometry.bounds.width);
        rowHeight = std::max(rowHeight, geometry.bounds.height);
        if (horizontal)
            x += geometry.bounds.width + gap;
        else if (i + 1 < items_.size())
        {
            y += geometry.bounds.height + gap;
            rowHeight = 0.0f;
        }
    }
    layoutWidth_ = measuredWidth;
    const float height = items_.empty() ? appearance_.itemHeight : y + rowHeight;
    return {measuredWidth, std::min(height, availableSize.height)};
}

void CandidateList::Arrange(const RectF &finalRect)
{
    if (std::abs(finalRect.width - layoutWidth_) > 0.5f)
        Measure({finalRect.width, std::numeric_limits<float>::max()});
    bounds_ = finalRect;
}

void CandidateList::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    IDWriteFactory *factory = deviceResources.GetDWriteFactory();
    if (!target || !factory || itemGeometry_.size() != items_.size())
    {
        return;
    }

    if (layoutCache_.size() != items_.size())
    {
        layoutCache_.resize(items_.size());
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const std::wstring &fontFamily =
        appearance_.fontFamily.empty() ? theme.textInputFontFamily : appearance_.fontFamily;

    // The list's corners in list coordinates: its arranged width (the list is
    // stretched to its container) and the bottom of the last row of items.
    // Only item corners that really sit on them take outerCornerRadius, so a
    // full-bleed highlight never rounds a corner in the middle of the frame.
    const float contentRight = bounds_.width;
    float contentBottom = 0.0f;
    for (size_t index = 0; index < items_.size(); ++index)
    {
        const RectF itemRect = ItemRect(index);
        contentBottom = std::max(contentBottom, itemRect.y - bounds_.y + itemRect.height);
    }

    for (size_t index = 0; index < items_.size(); ++index)
    {
        const RectF itemRect = ItemRect(index);
        const bool selected = index == selectedIndex_;
        const bool hovered = hoverEnabled_ && index == hoveredIndex_;
        const bool pressed = pressed_ && index == pressedIndex_;

        if (selected || pressed || (hovered && !selected))
        {
            const D2D1_COLOR_F fill = pressed ? appearance_.rowFillPressed
                                              : (selected ? appearance_.rowFillSelected : appearance_.rowFillHover);
            if (fill.a > 0.001f)
            {
                if (appearance_.outerCornerRadius > 0.0f)
                {
                    constexpr float kEdgeEpsilon = 0.5f;
                    const float x = itemRect.x - bounds_.x;
                    const float y = itemRect.y - bounds_.y;
                    const bool top = appearance_.outerTopCornersEnabled && y <= kEdgeEpsilon;
                    const bool bottom = y + itemRect.height >= contentBottom - kEdgeEpsilon;
                    const bool left = x <= kEdgeEpsilon;
                    const bool right = x + itemRect.width >= contentRight - kEdgeEpsilon;
                    const float inner = appearance_.cornerRadius;
                    const float outer = appearance_.outerCornerRadius;
                    const float radii[4] = {top && left ? outer : inner, top && right ? outer : inner,
                                            bottom && right ? outer : inner, bottom && left ? outer : inner};
                    FillRoundedRectCorners(deviceResources, itemRect, radii, fill);
                }
                else
                {
                    FillRoundedRect(deviceResources, itemRect, appearance_.cornerRadius, fill, fill, 0.0f);
                }
            }

            if (appearance_.showSelectedBar && (selected || pressed))
            {
                const float barWidth = (std::max)(appearance_.selectedBarWidth, 3.0f);
                const float barHeight = (std::max)(appearance_.selectedBarHeight, barWidth * 2.0f);
                const float barY = itemRect.y + std::max((itemRect.height - barHeight) * 0.5f, 0.0f);
                const RectF selectedBar = {itemRect.x - barWidth * 0.5f, barY, barWidth, barHeight};
                FillRoundedRect(deviceResources, selectedBar, barWidth * 0.5f, appearance_.selectedBarColor,
                                appearance_.selectedBarColor, 0.0f);
            }
        }

        const float translationFontSize = appearance_.fontSize * 0.78f;
        const auto &geometry = itemGeometry_[index];
        const auto absolute = [&](RectF rect) {
            rect.x += itemRect.x;
            rect.y += itemRect.y;
            return rect;
        };
        const RectF labelRect = absolute(geometry.label);
        const RectF textRect = absolute(geometry.text);
        const RectF annotationRect = absolute(geometry.annotation);
        const RectF translationRect = absolute(geometry.translation);

        auto &cache = layoutCache_[index];
        if (cache.fontFamily != fontFamily || cache.labelWidth != labelRect.width)
        {
            cache.labelLayout = CreateCachedTextLayout(
                factory, fontFamily, items_[index].label, appearance_.labelFontSize, DWRITE_FONT_WEIGHT_NORMAL,
                std::max(labelRect.width, 1.0f), std::max(labelRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_WRAP, appearance_.fallbackFontFamilies);
            cache.labelWidth = labelRect.width;
            cache.fontFamily = fontFamily;
        }
        if (cache.fontFamily != fontFamily || cache.textWidth != textRect.width)
        {
            cache.textLayout = CreateCachedTextLayout(
                factory, fontFamily, items_[index].text, appearance_.fontSize, DWRITE_FONT_WEIGHT_NORMAL,
                std::max(textRect.width, 1.0f), std::max(textRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_WRAP, appearance_.fallbackFontFamilies);
            cache.textWidth = textRect.width;
            cache.fontFamily = fontFamily;
        }
        if (cache.fontFamily != fontFamily || cache.annotationWidth != annotationRect.width)
        {
            cache.annotationLayout = CreateCachedTextLayout(
                factory, fontFamily, items_[index].annotation, appearance_.annotationFontSize,
                DWRITE_FONT_WEIGHT_NORMAL, std::max(annotationRect.width, 1.0f), std::max(annotationRect.height, 1.0f),
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_WRAP,
                appearance_.fallbackFontFamilies);
            cache.annotationWidth = annotationRect.width;
            cache.fontFamily = fontFamily;
        }
        if (cache.fontFamily != fontFamily || cache.translationWidth != translationRect.width)
        {
            cache.translationLayout = CreateCachedTextLayout(
                factory, fontFamily, items_[index].translation, translationFontSize, DWRITE_FONT_WEIGHT_NORMAL,
                std::max(translationRect.width, 1.0f), std::max(translationRect.height, 1.0f),
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_WRAP,
                appearance_.fallbackFontFamilies);
            cache.translationWidth = translationRect.width;
            cache.fontFamily = fontFamily;
        }

        const bool highlighted = selected || pressed;
        const D2D1_COLOR_F &labelColor = highlighted && appearance_.rowLabelSelected.a > 0.001f
                                             ? appearance_.rowLabelSelected
                                             : appearance_.labelColor;
        const D2D1_COLOR_F &textColor =
            highlighted && appearance_.rowTextSelected.a > 0.001f ? appearance_.rowTextSelected : appearance_.textColor;
        ID2D1SolidColorBrush *labelBrush = deviceResources.GetSolidColorBrush(labelColor);
        ID2D1SolidColorBrush *textBrush = deviceResources.GetSolidColorBrush(textColor);
        // 辅助码与翻译在 CSS 里都是 .text 的子节点（.cand-content / .cand-translation），
        // 颜色继承自 .text，选中行的 `.first .text { color: ... }` 会一并覆盖它们。
        // D2D 端分开绘制，所以这里显式跟随选中行文字色，translation 再乘 CSS 的 opacity .62。
        const D2D1_COLOR_F &annotationColor = highlighted && appearance_.rowTextSelected.a > 0.001f
                                                  ? appearance_.rowTextSelected
                                                  : appearance_.annotationColor;
        ID2D1SolidColorBrush *annotationBrush = deviceResources.GetSolidColorBrush(annotationColor);
        D2D1_COLOR_F translationColor = appearance_.translationColor;
        if (translationColor.a <= 0.001f)
        {
            translationColor = annotationColor;
            translationColor.a *= 0.62f;
        }
        ID2D1SolidColorBrush *translationBrush = deviceResources.GetSolidColorBrush(translationColor);
        if (cache.labelLayout && labelBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(labelRect.x, labelRect.y), cache.labelLayout.Get(), labelBrush,
                                   kColorClipTextOptions);
        }
        if (cache.textLayout && textBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(textRect.x, textRect.y), cache.textLayout.Get(), textBrush,
                                   kColorClipTextOptions);
        }
        if (!items_[index].annotation.empty() && cache.annotationLayout && annotationBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(annotationRect.x, annotationRect.y), cache.annotationLayout.Get(),
                                   annotationBrush, kColorClipTextOptions);
        }
        if (!items_[index].translation.empty() && cache.translationLayout && translationBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(translationRect.x, translationRect.y), cache.translationLayout.Get(),
                                   translationBrush, kColorClipTextOptions);
        }
    }
}

bool CandidateList::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool CandidateList::IsFocusable() const
{
    return true;
}

void CandidateList::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool CandidateList::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    if (hit == static_cast<size_t>(-1))
    {
        return false;
    }

    pressed_ = true;
    pressedIndex_ = hit;
    InvalidateVisual();
    return true;
}

bool CandidateList::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    const size_t pressedIndex = pressedIndex_;
    pressed_ = false;
    pressedIndex_ = static_cast<size_t>(-1);

    if (hit != static_cast<size_t>(-1) && hit == pressedIndex)
    {
        SetSelectedIndex(hit);
        if (onItemActivated_)
        {
            onItemActivated_(hit);
        }
    }

    InvalidateVisual();
    return true;
}

bool CandidateList::OnContextMenu(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    if (hit == static_cast<size_t>(-1))
    {
        return false;
    }

    SetSelectedIndex(hit);
    InvalidateVisual();
    if (onContextMenu_)
    {
        onContextMenu_(hit, point);
    }
    return true;
}

bool CandidateList::OnMouseMove(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !hoverEnabled_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const size_t hit = HitTestItem(dipPoint);
    if (hoveredIndex_ == hit)
    {
        return hit != static_cast<size_t>(-1);
    }

    hoveredIndex_ = hit;
    InvalidateVisual();
    return hit != static_cast<size_t>(-1);
}

void CandidateList::OnMouseLeave()
{
    if (hoveredIndex_ == static_cast<size_t>(-1))
    {
        return;
    }

    hoveredIndex_ = static_cast<size_t>(-1);
    InvalidateVisual();
}

bool CandidateList::OnKeyDown(WPARAM key, LPARAM lParam)
{
    (void)lParam;
    if (items_.empty())
    {
        return false;
    }

    size_t next = selectedIndex_;
    switch (key)
    {
    case VK_UP:
    case VK_LEFT:
        next = selectedIndex_ == 0 ? items_.size() - 1 : selectedIndex_ - 1;
        break;
    case VK_DOWN:
    case VK_RIGHT:
        next = selectedIndex_ + 1 >= items_.size() ? 0 : selectedIndex_ + 1;
        break;
    case VK_HOME:
        next = 0;
        break;
    case VK_END:
        next = items_.size() - 1;
        break;
    case VK_PRIOR:
        next = selectedIndex_ > 3 ? selectedIndex_ - 3 : 0;
        break;
    case VK_NEXT:
        next = (std::min)(selectedIndex_ + 3, items_.size() - 1);
        break;
    default:
        return false;
    }
    SetSelectedIndex(next);
    return true;
}

HCURSOR CandidateList::GetCursor() const
{
    return LoadCursor(nullptr, IDC_ARROW);
}

size_t CandidateList::HitTestItem(const PointF &point) const
{
    if (!HitTest(point) || items_.empty())
    {
        return static_cast<size_t>(-1);
    }

    for (size_t i = 0; i < items_.size(); ++i)
    {
        const RectF rect = ItemRect(i);
        if (PointInRect(rect, point))
        {
            return i;
        }
    }
    return static_cast<size_t>(-1);
}

TreeView::TreeView(float itemHeight) : itemHeight_(itemHeight)
{
}

void TreeView::InvalidateLayoutCache()
{
    for (auto &entry : visibleNodes_)
    {
        entry.titleLayout.Reset();
        entry.subtitleLayout.Reset();
        entry.titleWidth = -1.0f;
        entry.subtitleWidth = -1.0f;
        entry.fontFamily.clear();
    }
}

void TreeView::AddRoot(Node node)
{
    roots_.push_back(std::move(node));
    BuildVisibleNodes();
    if (!selectedNode_ && !roots_.empty())
    {
        selectedNode_ = &roots_.front();
    }
    InvalidateMeasure();
}

void TreeView::Clear()
{
    roots_.clear();
    visibleNodes_.clear();
    InvalidateLayoutCache();
    pressedNode_ = nullptr;
    selectedNode_ = nullptr;
    InvalidateMeasure();
}

void TreeView::SetOnSelectionChanged(SelectionChangedHandler handler)
{
    onSelectionChanged_ = std::move(handler);
}

SizeF TreeView::Measure(const SizeF &availableSize)
{
    BuildVisibleNodes();
    const float height = visibleNodes_.empty() ? itemHeight_ : itemHeight_ * static_cast<float>(visibleNodes_.size());
    return {availableSize.width, std::min(height, availableSize.height)};
}

void TreeView::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    for (size_t index = 0; index < visibleNodes_.size(); ++index)
    {
        auto &entry = visibleNodes_[index];
        const float rowY = finalRect.y + itemHeight_ * static_cast<float>(index);
        const float rowInset = static_cast<float>(entry.depth) * kTreeIndent;
        const float rowX = finalRect.x + rowInset;
        const float rowWidth = std::max(finalRect.width - rowInset, 0.0f);
        entry.rowRect = {rowX, rowY, rowWidth, itemHeight_ - 6.0f};
        entry.expanderRect = {rowX + kTreeRowLeadingPadding, rowY + 20.0f, kTreeExpanderSize, kTreeExpanderSize};
    }
}

void TreeView::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    IDWriteFactory *factory = deviceResources.GetDWriteFactory();
    if (!target || !factory)
    {
        return;
    }

    ID2D1SolidColorBrush *lineBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0xCBD5E1));
    if (!lineBrush)
    {
        return;
    }

    for (size_t index = 0; index < visibleNodes_.size(); ++index)
    {
        auto &entry = visibleNodes_[index];
        const bool selected = entry.node == selectedNode_;
        const bool pressed = pressed_ && entry.node == pressedNode_;
        FillRoundedRect(deviceResources, entry.rowRect, kListItemCornerRadius,
                        pressed ? D2D1::ColorF(0xDBEAFE) : (selected ? D2D1::ColorF(0xEFF6FF) : D2D1::ColorF(0xFFFFFF)),
                        selected ? D2D1::ColorF(0x60A5FA) : D2D1::ColorF(0xD6DCE5), selected || focused_ ? 2.0f : 1.0f);

        const float rowTop = entry.rowRect.y - 3.0f;
        const float rowBottom = entry.rowRect.y + entry.rowRect.height + 3.0f;
        for (size_t ancestorDepth = 0; ancestorDepth < entry.depth; ++ancestorDepth)
        {
            const float guideX = bounds_.x + kTreeGuideLineOffset + static_cast<float>(ancestorDepth) * kTreeIndent;
            target->DrawLine(D2D1::Point2F(guideX, rowTop), D2D1::Point2F(guideX, rowBottom), lineBrush, 1.2f);
        }

        if (entry.node->expanded && !entry.node->children.empty())
        {
            size_t lastDescendantIndex = index;
            for (size_t next = index + 1; next < visibleNodes_.size(); ++next)
            {
                if (visibleNodes_[next].depth <= entry.depth)
                {
                    break;
                }
                lastDescendantIndex = next;
            }

            if (lastDescendantIndex > index)
            {
                const float guideX = entry.expanderRect.x + entry.expanderRect.width * 0.5f;
                const float guideTop = entry.expanderRect.y + entry.expanderRect.height * 0.5f + 6.0f;
                const float guideBottom = visibleNodes_[lastDescendantIndex].rowRect.y +
                                          visibleNodes_[lastDescendantIndex].rowRect.height + 3.0f;
                target->DrawLine(D2D1::Point2F(guideX, guideTop), D2D1::Point2F(guideX, guideBottom), lineBrush, 1.2f);
            }
        }

        const float contentX = entry.rowRect.x + kTreeRowLeadingPadding + kTreeExpanderSize + kTreeTextGapAfterExpander;
        if (!entry.node->children.empty())
        {
            const float centerX = entry.expanderRect.x + entry.expanderRect.width * 0.5f;
            const float centerY = entry.expanderRect.y + entry.expanderRect.height * 0.5f;
            if (entry.node->expanded)
            {
                target->DrawLine(D2D1::Point2F(centerX - 4.0f, centerY - 1.0f), D2D1::Point2F(centerX, centerY + 3.0f),
                                 lineBrush, 1.8f);
                target->DrawLine(D2D1::Point2F(centerX, centerY + 3.0f), D2D1::Point2F(centerX + 4.0f, centerY - 1.0f),
                                 lineBrush, 1.8f);
            }
            else
            {
                target->DrawLine(D2D1::Point2F(centerX - 2.0f, centerY - 4.0f), D2D1::Point2F(centerX + 2.0f, centerY),
                                 lineBrush, 1.8f);
                target->DrawLine(D2D1::Point2F(centerX + 2.0f, centerY), D2D1::Point2F(centerX - 2.0f, centerY + 4.0f),
                                 lineBrush, 1.8f);
            }
        }

        const RectF titleRect = {contentX, entry.rowRect.y + 10.0f,
                                 std::max(entry.rowRect.width - (contentX - entry.rowRect.x) - 18.0f, 0.0f), 22.0f};
        const RectF subtitleRect = {contentX, entry.rowRect.y + 34.0f,
                                    std::max(entry.rowRect.width - (contentX - entry.rowRect.x) - 18.0f, 0.0f), 18.0f};
        const Theme &theme = ThemeManager::GetCurrent();
        if (entry.fontFamily != theme.uiFontFamily || entry.titleWidth != titleRect.width)
        {
            entry.titleLayout = CreateCachedTextLayout(factory, theme.uiFontFamily, entry.node->title, 15.0f,
                                                       DWRITE_FONT_WEIGHT_SEMI_BOLD, std::max(titleRect.width, 1.0f),
                                                       std::max(titleRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                                                       DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP);
            entry.titleWidth = titleRect.width;
            entry.fontFamily = theme.uiFontFamily;
        }
        if (entry.fontFamily != theme.uiFontFamily || entry.subtitleWidth != subtitleRect.width)
        {
            entry.subtitleLayout = CreateCachedTextLayout(
                factory, theme.uiFontFamily, entry.node->subtitle, 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                std::max(subtitleRect.width, 1.0f), std::max(subtitleRect.height, 1.0f), DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_WORD_WRAPPING_NO_WRAP);
            entry.subtitleWidth = subtitleRect.width;
            entry.fontFamily = theme.uiFontFamily;
        }

        ID2D1SolidColorBrush *titleBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0x0F172A));
        ID2D1SolidColorBrush *subtitleBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0x64748B));
        if (entry.titleLayout && titleBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(titleRect.x, titleRect.y), entry.titleLayout.Get(), titleBrush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (entry.subtitleLayout && subtitleBrush)
        {
            target->DrawTextLayout(D2D1::Point2F(subtitleRect.x, subtitleRect.y), entry.subtitleLayout.Get(),
                                   subtitleBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

bool TreeView::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool TreeView::IsFocusable() const
{
    return true;
}

void TreeView::OnFocusChanged(bool focused)
{
    focused_ = focused;
    InvalidateVisual();
}

bool TreeView::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    VisibleNode *hit = HitTestVisibleNode(dipPoint);
    if (!hit)
    {
        return false;
    }

    pressed_ = true;
    pressedNode_ = hit->node;
    pressedExpander_ = !hit->node->children.empty() && PointInRect(hit->expanderRect, dipPoint);
    InvalidateVisual();
    return true;
}

bool TreeView::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    VisibleNode *hit = HitTestVisibleNode(dipPoint);
    Node *pressedNode = pressedNode_;
    const bool pressedExpander = pressedExpander_;
    pressed_ = false;
    pressedNode_ = nullptr;
    pressedExpander_ = false;

    if (hit && hit->node == pressedNode)
    {
        if (pressedExpander && !hit->node->children.empty() && PointInRect(hit->expanderRect, dipPoint))
        {
            hit->node->expanded = !hit->node->expanded;
            BuildVisibleNodes();
            InvalidateMeasure();
        }
        else
        {
            SelectNode(hit->node);
        }
    }

    InvalidateVisual();
    return true;
}

HCURSOR TreeView::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

void TreeView::BuildVisibleNodes()
{
    visibleNodes_.clear();
    for (auto &root : roots_)
    {
        AppendVisibleNodes(root, 0);
    }
}

void TreeView::AppendVisibleNodes(Node &node, size_t depth)
{
    visibleNodes_.push_back({&node, depth, {}, {}});
    if (!node.expanded)
    {
        return;
    }

    for (auto &child : node.children)
    {
        AppendVisibleNodes(child, depth + 1);
    }
}

TreeView::VisibleNode *TreeView::HitTestVisibleNode(const PointF &point)
{
    for (auto &entry : visibleNodes_)
    {
        if (PointInRect(entry.rowRect, point))
        {
            return &entry;
        }
    }

    return nullptr;
}

const TreeView::VisibleNode *TreeView::HitTestVisibleNode(const PointF &point) const
{
    for (const auto &entry : visibleNodes_)
    {
        if (PointInRect(entry.rowRect, point))
        {
            return &entry;
        }
    }

    return nullptr;
}

void TreeView::SelectNode(Node *node)
{
    if (!node || selectedNode_ == node)
    {
        return;
    }

    selectedNode_ = node;
    if (onSelectionChanged_)
    {
        onSelectionChanged_(selectedNode_->title);
    }
}

TabControl::TabControl(float headerHeight) : headerHeight_(headerHeight)
{
}

void TabControl::AddTab(std::wstring title, std::shared_ptr<Visual> content)
{
    AdoptChild(content);
    tabs_.push_back({std::move(title), std::move(content)});
    InvalidateMeasure();
}

void TabControl::ClearTabs()
{
    for (auto &tab : tabs_)
    {
        ReleaseChild(tab.content);
    }
    tabs_.clear();
    headerRects_.clear();
    selectedIndex_ = 0;
    pressedIndex_ = static_cast<size_t>(-1);
    pressed_ = false;
    InvalidateMeasure();
}

void TabControl::SetSelectedIndex(size_t index)
{
    if (tabs_.empty())
    {
        selectedIndex_ = 0;
        return;
    }

    const size_t clamped = std::min(index, tabs_.size() - 1);
    if (selectedIndex_ == clamped)
    {
        return;
    }

    selectedIndex_ = clamped;
    if (onSelectionChanged_)
    {
        onSelectionChanged_(selectedIndex_);
    }
    InvalidateMeasure();
}

size_t TabControl::GetSelectedIndex() const
{
    return selectedIndex_;
}

void TabControl::SetOnSelectionChanged(SelectionChangedHandler handler)
{
    onSelectionChanged_ = std::move(handler);
}

SizeF TabControl::Measure(const SizeF &availableSize)
{
    float contentHeight = 0.0f;
    float contentWidth = availableSize.width;
    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        const SizeF contentSize = tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->MeasureInLayout(
            {availableSize.width, std::max(availableSize.height - headerHeight_ - 12.0f, 0.0f)});
        contentHeight = contentSize.height;
        contentWidth = std::max(contentWidth, contentSize.width);
    }

    return {std::min(contentWidth, availableSize.width),
            std::min(headerHeight_ + 12.0f + contentHeight, availableSize.height)};
}

void TabControl::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    headerRects_.clear();

    float cursorX = finalRect.x;
    for (const auto &tab : tabs_)
    {
        const float textWidth = MeasureText(GetSharedDWriteFactory(), tab.title, 14.0f, true, 400.0f).width;
        const float tabWidth = std::max(textWidth + 28.0f, 96.0f);
        headerRects_.push_back({cursorX, finalRect.y, tabWidth, headerHeight_});
        cursorX += tabWidth + 8.0f;
    }

    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->ArrangeInLayout(
            {finalRect.x, finalRect.y + headerHeight_ + 12.0f, finalRect.width,
             std::max(finalRect.height - headerHeight_ - 12.0f, 0.0f)});
    }
}

void TabControl::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    for (size_t index = 0; index < tabs_.size(); ++index)
    {
        const bool selected = index == selectedIndex_;
        const bool pressed = pressed_ && index == pressedIndex_;
        FillRoundedRect(deviceResources, headerRects_[index], kTabCornerRadius,
                        pressed ? D2D1::ColorF(0xDBEAFE) : (selected ? D2D1::ColorF(0xEFF6FF) : D2D1::ColorF(0xF8FAFC)),
                        selected ? D2D1::ColorF(0x60A5FA) : D2D1::ColorF(0xCBD5E1), selected ? 2.0f : 1.0f);
        DrawLabel(deviceResources, tabs_[index].title, 14.0f, true,
                  selected ? D2D1::ColorF(0x1D4ED8) : D2D1::ColorF(0x334155), headerRects_[index],
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    const RectF contentFrame = {bounds_.x, bounds_.y + headerHeight_ + 8.0f, bounds_.width,
                                std::max(bounds_.height - headerHeight_ - 8.0f, 0.0f)};
    FillRoundedRect(deviceResources, contentFrame, 18.0f, D2D1::ColorF(0xFFFFFF), D2D1::ColorF(0xD6DCE5), 1.0f);

    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->Render(deviceResources);
    }
}

void TabControl::Attach(Window *window)
{
    Visual::Attach(window);
    for (auto &tab : tabs_)
    {
        if (tab.content)
        {
            tab.content->Attach(window);
        }
    }
}

Visual *TabControl::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    if (HitTestHeader(point) != static_cast<size_t>(-1))
    {
        return this;
    }

    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        if (Visual *hit = tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->FindVisualAt(point))
        {
            return hit;
        }
    }

    return this;
}

Visual *TabControl::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        return tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->FindFocusableAt(point);
    }

    return nullptr;
}

Visual *TabControl::FindFirstFocusableDescendant()
{
    if (!tabs_.empty() && tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content)
    {
        return tabs_[std::min(selectedIndex_, tabs_.size() - 1)].content->FindFirstFocusableDescendant();
    }

    return nullptr;
}

bool TabControl::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool TabControl::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const size_t hit = HitTestHeader(window_->ClientPixelsToDips(point));
    if (hit == static_cast<size_t>(-1))
    {
        return false;
    }

    pressed_ = true;
    pressedIndex_ = hit;
    InvalidateVisual();
    return true;
}

bool TabControl::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const size_t hit = HitTestHeader(window_->ClientPixelsToDips(point));
    const size_t pressedIndex = pressedIndex_;
    pressed_ = false;
    pressedIndex_ = static_cast<size_t>(-1);
    if (hit != static_cast<size_t>(-1) && hit == pressedIndex)
    {
        SetSelectedIndex(hit);
    }

    InvalidateVisual();
    return true;
}

HCURSOR TabControl::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

size_t TabControl::HitTestHeader(const PointF &point) const
{
    for (size_t index = 0; index < headerRects_.size(); ++index)
    {
        if (PointInRect(headerRects_[index], point))
        {
            return index;
        }
    }

    return static_cast<size_t>(-1);
}

Accordion::Accordion(float headerHeight) : headerHeight_(headerHeight)
{
}

void Accordion::AddSection(std::wstring title, std::shared_ptr<Visual> content, bool expanded)
{
    AdoptChild(content);
    sections_.push_back({std::move(title), std::move(content), expanded});
    if (!allowMultipleExpanded_ && expanded)
    {
        CollapseOtherSections(sections_.size() - 1);
    }
    InvalidateMeasure();
}

void Accordion::ClearSections()
{
    for (auto &section : sections_)
    {
        ReleaseChild(section.content);
    }
    sections_.clear();
    headerRects_.clear();
    contentRects_.clear();
    pressed_ = false;
    pressedIndex_ = static_cast<size_t>(-1);
    InvalidateMeasure();
}

void Accordion::SetAllowMultipleExpanded(bool allowMultipleExpanded)
{
    allowMultipleExpanded_ = allowMultipleExpanded;
    if (!allowMultipleExpanded_)
    {
        size_t firstExpanded = static_cast<size_t>(-1);
        for (size_t index = 0; index < sections_.size(); ++index)
        {
            if (sections_[index].expanded)
            {
                firstExpanded = index;
                break;
            }
        }
        if (firstExpanded != static_cast<size_t>(-1))
        {
            CollapseOtherSections(firstExpanded);
        }
    }
    InvalidateMeasure();
}

SizeF Accordion::Measure(const SizeF &availableSize)
{
    float height = 0.0f;
    float width = availableSize.width;
    for (const auto &section : sections_)
    {
        height += headerHeight_;
        if (section.expanded && section.content)
        {
            const SizeF contentSize = section.content->MeasureInLayout(
                {availableSize.width, std::max(availableSize.height - headerHeight_, 0.0f)});
            height += contentSize.height + 8.0f;
            width = std::max(width, contentSize.width);
        }
        height += 8.0f;
    }

    return {std::min(width, availableSize.width), std::min(height, availableSize.height)};
}

void Accordion::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    headerRects_.assign(sections_.size(), {});
    contentRects_.assign(sections_.size(), {});

    float cursorY = finalRect.y;
    for (size_t index = 0; index < sections_.size(); ++index)
    {
        headerRects_[index] = {finalRect.x, cursorY, finalRect.width, headerHeight_};
        cursorY += headerHeight_;
        if (sections_[index].expanded && sections_[index].content)
        {
            const float contentHeight =
                sections_[index]
                    .content
                    ->MeasureInLayout({finalRect.width, std::max(finalRect.height - (cursorY - finalRect.y), 0.0f)})
                    .height;
            contentRects_[index] = {finalRect.x, cursorY + 8.0f, finalRect.width, contentHeight};
            sections_[index].content->ArrangeInLayout(contentRects_[index]);
            cursorY += 8.0f + contentHeight;
        }
        cursorY += 8.0f;
    }
}

void Accordion::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    ID2D1SolidColorBrush *chevronBrush = deviceResources.GetSolidColorBrush(D2D1::ColorF(0x64748B));
    if (!chevronBrush)
    {
        return;
    }

    for (size_t index = 0; index < sections_.size(); ++index)
    {
        const bool pressed = pressed_ && index == pressedIndex_;
        FillRoundedRect(deviceResources, headerRects_[index], kAccordionCornerRadius,
                        pressed ? D2D1::ColorF(0xE0F2FE) : D2D1::ColorF(0xF8FAFC), D2D1::ColorF(0xCBD5E1), 1.0f);
        RectF titleRect = headerRects_[index];
        titleRect.x += kAccordionHeaderHorizontalPadding;
        titleRect.width = std::max(0.0f, titleRect.width - kAccordionHeaderHorizontalPadding - 36.0f);
        DrawLabel(deviceResources, sections_[index].title, 15.0f, true, D2D1::ColorF(0x0F172A), titleRect,
                  DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const float centerX = headerRects_[index].x + headerRects_[index].width - 22.0f;
        const float centerY = headerRects_[index].y + headerRects_[index].height * 0.5f;
        if (sections_[index].expanded)
        {
            target->DrawLine(D2D1::Point2F(centerX - 4.0f, centerY - 2.0f), D2D1::Point2F(centerX, centerY + 2.0f),
                             chevronBrush, 1.8f);
            target->DrawLine(D2D1::Point2F(centerX, centerY + 2.0f), D2D1::Point2F(centerX + 4.0f, centerY - 2.0f),
                             chevronBrush, 1.8f);
        }
        else
        {
            target->DrawLine(D2D1::Point2F(centerX - 2.0f, centerY - 4.0f), D2D1::Point2F(centerX + 2.0f, centerY),
                             chevronBrush, 1.8f);
            target->DrawLine(D2D1::Point2F(centerX + 2.0f, centerY), D2D1::Point2F(centerX - 2.0f, centerY + 4.0f),
                             chevronBrush, 1.8f);
        }

        if (sections_[index].expanded && sections_[index].content)
        {
            sections_[index].content->Render(deviceResources);
        }
    }
}

void Accordion::Attach(Window *window)
{
    Visual::Attach(window);
    for (auto &section : sections_)
    {
        if (section.content)
        {
            section.content->Attach(window);
        }
    }
}

Visual *Accordion::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    for (size_t index = 0; index < sections_.size(); ++index)
    {
        if (PointInRect(headerRects_[index], point))
        {
            return this;
        }
        if (sections_[index].expanded && sections_[index].content)
        {
            if (Visual *hit = sections_[index].content->FindVisualAt(point))
            {
                return hit;
            }
        }
    }

    return this;
}

Visual *Accordion::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    for (size_t index = 0; index < sections_.size(); ++index)
    {
        if (sections_[index].expanded && sections_[index].content)
        {
            if (Visual *focusable = sections_[index].content->FindFocusableAt(point))
            {
                return focusable;
            }
        }
    }

    return nullptr;
}

Visual *Accordion::FindFirstFocusableDescendant()
{
    for (auto &section : sections_)
    {
        if (section.expanded && section.content)
        {
            if (Visual *focusable = section.content->FindFirstFocusableDescendant())
            {
                return focusable;
            }
        }
    }

    return nullptr;
}

bool Accordion::HitTest(const PointF &point) const
{
    return PointInRect(bounds_, point);
}

bool Accordion::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_)
    {
        return false;
    }

    const size_t hit = HitTestHeader(window_->ClientPixelsToDips(point));
    if (hit == static_cast<size_t>(-1))
    {
        return false;
    }

    pressed_ = true;
    pressedIndex_ = hit;
    InvalidateVisual();
    return true;
}

bool Accordion::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !pressed_)
    {
        return false;
    }

    const size_t hit = HitTestHeader(window_->ClientPixelsToDips(point));
    const size_t pressedIndex = pressedIndex_;
    pressed_ = false;
    pressedIndex_ = static_cast<size_t>(-1);

    if (hit != static_cast<size_t>(-1) && hit == pressedIndex)
    {
        const bool willExpand = !sections_[hit].expanded;
        sections_[hit].expanded = willExpand;
        if (willExpand && !allowMultipleExpanded_)
        {
            CollapseOtherSections(hit);
        }
        InvalidateMeasure();
    }

    return true;
}

HCURSOR Accordion::GetCursor() const
{
    return LoadCursor(nullptr, IDC_HAND);
}

void Accordion::CollapseOtherSections(size_t keepExpandedIndex)
{
    for (size_t index = 0; index < sections_.size(); ++index)
    {
        if (index != keepExpandedIndex)
        {
            sections_[index].expanded = false;
        }
    }
}

size_t Accordion::HitTestHeader(const PointF &point) const
{
    for (size_t index = 0; index < headerRects_.size(); ++index)
    {
        if (PointInRect(headerRects_[index], point))
        {
            return index;
        }
    }

    return static_cast<size_t>(-1);
}
} // namespace msimeui
