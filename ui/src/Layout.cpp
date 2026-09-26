#include "msimeui/Layout.h"

#include "msimeui/DeviceResources.h"
#include "msimeui/Fonts.h"
#include "msimeui/Theme.h"
#include "msimeui/Window.h"

#include <algorithm>
#include <cmath>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dwrite_1.h>
#include <limits>
#include <numeric>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace msimeui
{
using Microsoft::WRL::ComPtr;

namespace
{
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

constexpr float kPreeditCaretBarWidth = 1.25f;
constexpr float kPreeditCaretSideAir = 0.85f;
constexpr float kPreeditCaretEndAir = 1.5f;
constexpr float kPreeditCaretInsertGap = kPreeditCaretBarWidth + kPreeditCaretSideAir * 2.0f;
constexpr wchar_t kCaretSlotChar = L'\uFFFC';

class CaretGapInlineObject final : public IDWriteInlineObject
{
  public:
    CaretGapInlineObject(float width, float height, float baseline)
        : width_(width), height_(height), baseline_(baseline)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
    {
        if (ppvObject == nullptr)
        {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject))
        {
            *ppvObject = static_cast<IDWriteInlineObject *>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(&refCount_);
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = InterlockedDecrement(&refCount_);
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Draw(void *, IDWriteTextRenderer *, FLOAT, FLOAT, BOOL, BOOL, IUnknown *) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS *metrics) override
    {
        if (metrics == nullptr)
        {
            return E_POINTER;
        }
        metrics->width = width_;
        metrics->height = height_;
        metrics->baseline = baseline_;
        metrics->supportsSideways = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS *overhangs) override
    {
        if (overhangs == nullptr)
        {
            return E_POINTER;
        }
        *overhangs = {};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION *breakConditionBefore,
                                                 DWRITE_BREAK_CONDITION *breakConditionAfter) override
    {
        if (breakConditionBefore == nullptr || breakConditionAfter == nullptr)
        {
            return E_POINTER;
        }
        *breakConditionBefore = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        *breakConditionAfter = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        return S_OK;
    }

  private:
    ULONG refCount_ = 1;
    float width_ = 0.0f;
    float height_ = 0.0f;
    float baseline_ = 0.0f;
};

bool InsertCaretSlot(IDWriteTextLayout *layout, UINT32 caretIndex, float fontSize)
{
    if (layout == nullptr)
    {
        return false;
    }
    ComPtr<CaretGapInlineObject> slot;
    slot.Attach(new CaretGapInlineObject(kPreeditCaretInsertGap, fontSize * 1.2f, fontSize * 0.96f));
    return SUCCEEDED(layout->SetInlineObject(slot.Get(), DWRITE_TEXT_RANGE{caretIndex, 1}));
}

SizeF DeflateSize(const SizeF &size, const Thickness &thickness)
{
    return {std::max(size.width - thickness.left - thickness.right, 0.0f),
            std::max(size.height - thickness.top - thickness.bottom, 0.0f)};
}

RectF DeflateRect(const RectF &rect, const Thickness &thickness)
{
    return {rect.x + thickness.left, rect.y + thickness.top,
            std::max(rect.width - thickness.left - thickness.right, 0.0f),
            std::max(rect.height - thickness.top - thickness.bottom, 0.0f)};
}

float ClampWithOptionalMax(float value, float minValue, float maxValue)
{
    const float lowerBound = std::max(minValue, 0.0f);
    if (maxValue >= 0.0f)
    {
        return std::clamp(value, lowerBound, std::max(lowerBound, maxValue));
    }

    return std::max(value, lowerBound);
}

float ComputeAlignedStart(float origin, float available, float content, HorizontalAlignment alignment)
{
    if (alignment == HorizontalAlignment::Center)
    {
        return origin + std::max((available - content) * 0.5f, 0.0f);
    }
    if (alignment == HorizontalAlignment::Trailing)
    {
        return origin + std::max(available - content, 0.0f);
    }

    return origin;
}

float ComputeAlignedStart(float origin, float available, float content, VerticalAlignment alignment)
{
    if (alignment == VerticalAlignment::Center)
    {
        return origin + std::max((available - content) * 0.5f, 0.0f);
    }
    if (alignment == VerticalAlignment::Trailing)
    {
        return origin + std::max(available - content, 0.0f);
    }

    return origin;
}

bool PointInRect(const RectF &rect, const PointF &point)
{
    return point.x >= rect.x && point.x <= (rect.x + rect.width) && point.y >= rect.y &&
           point.y <= (rect.y + rect.height);
}

bool IsSameSize(const SizeF &lhs, const SizeF &rhs)
{
    return lhs.width == rhs.width && lhs.height == rhs.height;
}

bool IsSameRect(const RectF &lhs, const RectF &rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

void DrawLayeredMistShadow(ID2D1RenderTarget *target, const RectF &bounds, float radius, float scale, float opacity)
{
    const float s = std::max(scale, 0.15f);
    for (int i = 1; i <= 20; ++i)
    {
        const float t = static_cast<float>(i) / 20.0f;
        const float spread = 1.15f * static_cast<float>(i) * s;
        const float offsetY = 0.4f * static_cast<float>(i) * s;
        const float alpha = 0.14f * (1.0f - t) * (1.0f - t) * opacity;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, alpha), brush.GetAddressOf())))
        {
            continue;
        }
        const auto rounded = D2D1::RoundedRect(D2D1::RectF(bounds.x - spread, bounds.y - spread + offsetY,
                                                           bounds.x + bounds.width + spread,
                                                           bounds.y + bounds.height + spread + offsetY),
                                               radius + spread, radius + spread);
        target->FillRoundedRectangle(rounded, brush.Get());
    }
}

// One gaussian-blurred rounded-rect pass shared by the built-in and explicit-pass card
// shadows; stdDeviation/alpha/offset are already-scaled absolute values.
bool DrawGaussianShadowPass(ID2D1RenderTarget *target, ID2D1DeviceContext *dc, const RectF &bounds, float radius,
                            float stdDeviation, float alpha, float offsetX, float offsetY)
{
    const float pad = stdDeviation * 3.0f + 4.0f;
    const D2D1_SIZE_F bitmapSize = {bounds.width + pad * 2.0f, bounds.height + pad * 2.0f};
    if (bitmapSize.width < 2.0f || bitmapSize.height < 2.0f)
    {
        return false;
    }

    ComPtr<ID2D1BitmapRenderTarget> compatible;
    if (FAILED(target->CreateCompatibleRenderTarget(bitmapSize, compatible.GetAddressOf())))
    {
        return false;
    }

    compatible->BeginDraw();
    compatible->Clear(D2D1::ColorF(0, 0.0f));
    ComPtr<ID2D1SolidColorBrush> fill;
    if (FAILED(compatible->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, alpha), fill.GetAddressOf())))
    {
        compatible->EndDraw();
        return false;
    }
    const auto shape =
        D2D1::RoundedRect(D2D1::RectF(pad, pad, pad + bounds.width, pad + bounds.height), radius, radius);
    compatible->FillRoundedRectangle(shape, fill.Get());
    if (FAILED(compatible->EndDraw()))
    {
        return false;
    }

    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(compatible->GetBitmap(bitmap.GetAddressOf())))
    {
        return false;
    }

    ComPtr<ID2D1Effect> blur;
    if (FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.GetAddressOf())))
    {
        return false;
    }
    blur->SetInput(0, bitmap.Get());
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, stdDeviation);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY);
    dc->DrawImage(blur.Get(), D2D1::Point2F(bounds.x - pad + offsetX, bounds.y - pad + offsetY));
    blur->SetInput(0, nullptr);
    return true;
}

// Built-in Win11-style diffuse shadow: ambient/mid/contact gaussian passes. Kept here so
// the default path and the explicit-pass path share one rendering routine.
constexpr ShadowPass kMistShadowPasses[] = {
    {11.0f, 0.42f, 0.0f, 3.0f},
    {6.0f, 0.30f, 0.0f, 4.0f},
    {2.8f, 0.48f, 0.0f, 3.0f},
};

// Draws each pass through DrawGaussianShadowPass. scale only shrinks the shadow geometry,
// opacity dims every alpha uniformly. Returns true when the caller's shadow is fully
// handled; false means nothing was drawn and the caller may fall back to the layered
// approximation.
bool DrawGaussianPasses(ID2D1RenderTarget *target, const RectF &bounds, float radius, const ShadowPass *passes,
                        size_t count, float scale, float opacity)
{
    if (!target || bounds.width <= 0.0f || bounds.height <= 0.0f)
    {
        return false;
    }

    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(target->QueryInterface(IID_PPV_ARGS(dc.GetAddressOf()))))
    {
        // No device context: degrade to the layered approximation. It does not
        // reproduce the individual passes, only the overall soft-shadow look.
        DrawLayeredMistShadow(target, bounds, radius, scale, opacity);
        return true;
    }

    const float s = std::max(scale, 0.15f);
    bool drew = false;
    for (size_t i = 0; i < count; ++i)
    {
        if (DrawGaussianShadowPass(target, dc.Get(), bounds, radius, passes[i].sigma * s, passes[i].alpha * opacity,
                                   passes[i].offsetX * s, passes[i].offsetY * s))
        {
            drew = true;
        }
    }
    return drew;
}

void DrawWin11WindowShadow(ID2D1RenderTarget *target, const RectF &bounds, float radius, float scale, float opacity)
{
    if (!target || bounds.width <= 0.0f || bounds.height <= 0.0f)
    {
        return;
    }
    if (!DrawGaussianPasses(target, bounds, radius, kMistShadowPasses, std::size(kMistShadowPasses), scale, opacity))
    {
        DrawLayeredMistShadow(target, bounds, radius, scale, opacity);
    }
}
} // namespace

void Visual::Attach(Window *window)
{
    window_ = window;
}

bool Visual::HitTest(const PointF &point) const
{
    return point.x >= bounds_.x && point.x < (bounds_.x + bounds_.width) && point.y >= bounds_.y &&
           point.y < (bounds_.y + bounds_.height);
}

Visual *Visual::FindVisualAt(const PointF &point)
{
    return HitTest(point) ? this : nullptr;
}

Visual *Visual::FindFocusableAt(const PointF &point)
{
    if (HitTest(point) && IsFocusable())
    {
        return this;
    }

    return nullptr;
}

Visual *Visual::FindFirstFocusableDescendant()
{
    return IsFocusable() ? this : nullptr;
}

bool Visual::IsFocusable() const
{
    return false;
}

void Visual::OnFocusChanged(bool focused)
{
    (void)focused;
}

bool Visual::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    return false;
}

bool Visual::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    return false;
}

bool Visual::OnMouseMove(const POINT &point, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    return false;
}

void Visual::OnMouseEnter()
{
}

void Visual::OnMouseLeave()
{
}

bool Visual::OnContextMenu(const POINT &point, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    return false;
}

bool Visual::OnMouseWheel(const POINT &point, short delta, WPARAM keyState)
{
    (void)point;
    (void)delta;
    (void)keyState;
    return false;
}

bool Visual::OnKeyDown(WPARAM key, LPARAM lParam)
{
    (void)key;
    (void)lParam;
    return false;
}

bool Visual::OnChar(wchar_t ch, LPARAM lParam)
{
    (void)ch;
    (void)lParam;
    return false;
}

bool Visual::OnTimer(UINT_PTR timerId)
{
    (void)timerId;
    return false;
}

HCURSOR Visual::GetCursor() const
{
    return LoadCursor(nullptr, IDC_ARROW);
}

void Visual::LayoutOverlay(const SizeF &viewportSize)
{
    (void)viewportSize;
}

bool Visual::KeepsPopupsOpenOnClick() const
{
    return false;
}

void Visual::InvalidateMeasure()
{
    BubbleMeasureInvalidation(this);
}

void Visual::InvalidateArrange()
{
    BubbleArrangeInvalidation(this);
}

void Visual::InvalidateVisual()
{
    if (window_)
    {
        if (bounds_.width > 0.0f && bounds_.height > 0.0f)
        {
            window_->Invalidate(bounds_);
        }
        else
        {
            window_->Invalidate();
        }
    }
}

void Visual::SetMargin(float uniform)
{
    SetMargin({uniform, uniform, uniform, uniform});
}

void Visual::SetMargin(Thickness margin)
{
    margin_ = margin;
    InvalidateMeasure();
}

const Thickness &Visual::GetMargin() const
{
    return margin_;
}

void Visual::SetPadding(float uniform)
{
    SetPadding({uniform, uniform, uniform, uniform});
}

void Visual::SetPadding(Thickness padding)
{
    padding_ = padding;
    InvalidateMeasure();
}

const Thickness &Visual::GetPadding() const
{
    return padding_;
}

void Visual::SetWidth(float width)
{
    explicitWidth_ = std::max(width, 0.0f);
    InvalidateMeasure();
}

void Visual::SetHeight(float height)
{
    explicitHeight_ = std::max(height, 0.0f);
    InvalidateMeasure();
}

void Visual::SetMinWidth(float width)
{
    minWidth_ = std::max(width, 0.0f);
    InvalidateMeasure();
}

void Visual::SetMinHeight(float height)
{
    minHeight_ = std::max(height, 0.0f);
    InvalidateMeasure();
}

void Visual::SetMaxWidth(float width)
{
    maxWidth_ = width >= 0.0f ? width : -1.0f;
    InvalidateMeasure();
}

void Visual::SetMaxHeight(float height)
{
    maxHeight_ = height >= 0.0f ? height : -1.0f;
    InvalidateMeasure();
}

void Visual::ClearWidth()
{
    explicitWidth_ = -1.0f;
    InvalidateMeasure();
}

void Visual::ClearHeight()
{
    explicitHeight_ = -1.0f;
    InvalidateMeasure();
}

void Visual::ClearMinWidth()
{
    minWidth_ = 0.0f;
    InvalidateMeasure();
}

void Visual::ClearMinHeight()
{
    minHeight_ = 0.0f;
    InvalidateMeasure();
}

void Visual::ClearMaxWidth()
{
    maxWidth_ = -1.0f;
    InvalidateMeasure();
}

void Visual::ClearMaxHeight()
{
    maxHeight_ = -1.0f;
    InvalidateMeasure();
}

void Visual::SetHorizontalAlignment(HorizontalAlignment alignment)
{
    horizontalAlignment_ = alignment;
    InvalidateArrange();
}

void Visual::SetVerticalAlignment(VerticalAlignment alignment)
{
    verticalAlignment_ = alignment;
    InvalidateArrange();
}

SizeF Visual::MeasureInLayout(const SizeF &availableSize)
{
    if (IsMeasureCacheValid(availableSize))
    {
        return measuredOuterSize_;
    }

    const SizeF innerAvailable = DeflateSize(availableSize, margin_);
    desiredSize_ = Measure(innerAvailable);
    if (HasExplicitWidth())
    {
        desiredSize_.width = explicitWidth_;
    }
    if (HasExplicitHeight())
    {
        desiredSize_.height = explicitHeight_;
    }

    desiredSize_.width = ClampWithOptionalMax(desiredSize_.width, minWidth_, maxWidth_);
    desiredSize_.height = ClampWithOptionalMax(desiredSize_.height, minHeight_, maxHeight_);
    desiredSize_.width = std::min(desiredSize_.width, innerAvailable.width);
    desiredSize_.height = std::min(desiredSize_.height, innerAvailable.height);

    measuredOuterSize_ = {desiredSize_.width + margin_.left + margin_.right,
                          desiredSize_.height + margin_.top + margin_.bottom};
    lastMeasureAvailableSize_ = availableSize;
    measureDirty_ = false;
    hasMeasureCache_ = true;
    return measuredOuterSize_;
}

void Visual::ArrangeInLayout(const RectF &finalRect)
{
    if (IsArrangeCacheValid(finalRect))
    {
        return;
    }

    const RectF innerRect = DeflateRect(finalRect, margin_);

    float arrangedWidth = desiredSize_.width;
    if (HasExplicitWidth())
    {
        arrangedWidth = std::min(explicitWidth_, innerRect.width);
    }
    else if (horizontalAlignment_ == HorizontalAlignment::Stretch)
    {
        arrangedWidth = innerRect.width;
    }
    else
    {
        arrangedWidth = std::min(arrangedWidth, innerRect.width);
    }
    arrangedWidth = ClampWithOptionalMax(arrangedWidth, minWidth_, maxWidth_);
    arrangedWidth = std::min(arrangedWidth, innerRect.width);

    float arrangedHeight = desiredSize_.height;
    if (HasExplicitHeight())
    {
        arrangedHeight = std::min(explicitHeight_, innerRect.height);
    }
    else if (verticalAlignment_ == VerticalAlignment::Stretch)
    {
        arrangedHeight = innerRect.height;
    }
    else
    {
        arrangedHeight = std::min(arrangedHeight, innerRect.height);
    }
    arrangedHeight = ClampWithOptionalMax(arrangedHeight, minHeight_, maxHeight_);
    arrangedHeight = std::min(arrangedHeight, innerRect.height);

    float arrangedX = innerRect.x;
    if (horizontalAlignment_ == HorizontalAlignment::Center)
    {
        arrangedX += (innerRect.width - arrangedWidth) * 0.5f;
    }
    else if (horizontalAlignment_ == HorizontalAlignment::Trailing)
    {
        arrangedX += innerRect.width - arrangedWidth;
    }

    float arrangedY = innerRect.y;
    if (verticalAlignment_ == VerticalAlignment::Center)
    {
        arrangedY += (innerRect.height - arrangedHeight) * 0.5f;
    }
    else if (verticalAlignment_ == VerticalAlignment::Trailing)
    {
        arrangedY += innerRect.height - arrangedHeight;
    }

    Arrange({arrangedX, arrangedY, std::max(arrangedWidth, 0.0f), std::max(arrangedHeight, 0.0f)});
    lastArrangeRect_ = finalRect;
    arrangeDirty_ = false;
    hasArrangeCache_ = true;
}

const RectF &Visual::GetBounds() const
{
    return bounds_;
}

bool Visual::HasMeasureSlot() const
{
    return hasMeasureCache_;
}

bool Visual::HasArrangeSlot() const
{
    return hasArrangeCache_;
}

const SizeF &Visual::GetLastMeasureAvailableSize() const
{
    return lastMeasureAvailableSize_;
}

Visual *Visual::GetParentVisual() const
{
    return parent_;
}

void Visual::BubbleMeasureInvalidation(Visual *source)
{
    MarkMeasureDirty();
    if (parent_)
    {
        parent_->BubbleMeasureInvalidation(source);
        return;
    }

    if (window_)
    {
        window_->InvalidateMeasure(source);
    }
}

void Visual::BubbleArrangeInvalidation(Visual *source)
{
    MarkArrangeDirty();
    if (parent_)
    {
        parent_->BubbleArrangeInvalidation(source);
        return;
    }

    if (window_)
    {
        window_->InvalidateArrange(source);
    }
}

void Visual::MarkMeasureDirty()
{
    measureDirty_ = true;
    arrangeDirty_ = true;
    hasMeasureCache_ = false;
    hasArrangeCache_ = false;
}

void Visual::MarkArrangeDirty()
{
    arrangeDirty_ = true;
    hasArrangeCache_ = false;
}

bool Visual::IsMeasureCacheValid(const SizeF &availableSize) const
{
    return hasMeasureCache_ && !measureDirty_ && IsSameSize(lastMeasureAvailableSize_, availableSize);
}

bool Visual::IsArrangeCacheValid(const RectF &finalRect) const
{
    return hasArrangeCache_ && !arrangeDirty_ && IsSameRect(lastArrangeRect_, finalRect);
}

bool Visual::HasExplicitWidth() const
{
    return explicitWidth_ >= 0.0f;
}

void Visual::AdoptChild(const std::shared_ptr<Visual> &child)
{
    if (!child)
    {
        return;
    }

    child->SetParent(this);
    if (window_)
    {
        child->Attach(window_);
    }
}

void Visual::ReleaseChild(const std::shared_ptr<Visual> &child)
{
    if (child && child->GetParent() == this)
    {
        child->SetParent(nullptr);
    }
}

bool Visual::HasExplicitHeight() const
{
    return explicitHeight_ >= 0.0f;
}

void Visual::SetParent(Visual *parent)
{
    parent_ = parent;
}

Visual *Visual::GetParent() const
{
    return parent_;
}

void Panel::AddChild(std::shared_ptr<Visual> child)
{
    if (!child)
    {
        return;
    }

    AdoptChild(child);
    children_.push_back(std::move(child));
    InvalidateMeasure();
}

void Panel::Attach(Window *window)
{
    Visual::Attach(window);
    for (const auto &child : children_)
    {
        child->Attach(window);
    }
}

Visual *Panel::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
    {
        if (Visual *hit = (*it)->FindVisualAt(point))
        {
            return hit;
        }
    }

    return this;
}

Visual *Panel::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
    {
        if (!(*it)->HitTest(point))
        {
            continue;
        }

        if (Visual *focusable = (*it)->FindFocusableAt(point))
        {
            return focusable;
        }
    }

    return IsFocusable() ? this : nullptr;
}

Visual *Panel::FindFirstFocusableDescendant()
{
    if (IsFocusable())
    {
        return this;
    }

    for (const auto &child : children_)
    {
        if (Visual *focusable = child->FindFirstFocusableDescendant())
        {
            return focusable;
        }
    }

    return nullptr;
}

StackPanel::StackPanel(float spacing) : spacing_(spacing)
{
}

void StackPanel::SetHorizontalContentAlignment(HorizontalAlignment alignment)
{
    horizontalContentAlignment_ = alignment;
}

void StackPanel::SetVerticalContentAlignment(VerticalAlignment alignment)
{
    verticalContentAlignment_ = alignment;
}

SizeF StackPanel::Measure(const SizeF &availableSize)
{
    measuredChildren_.clear();

    const SizeF innerAvailable = DeflateSize(availableSize, padding_);
    float width = 0.0f;
    float height = 0.0f;
    for (const auto &child : children_)
    {
        const SizeF measured = child->MeasureInLayout(innerAvailable);
        measuredChildren_.push_back(measured);
        width = std::max(width, measured.width);
        height += measured.height;
    }

    if (!children_.empty())
    {
        height += spacing_ * static_cast<float>(children_.size() - 1);
    }

    measuredContent_ = {width, height};
    return {std::min(width + padding_.left + padding_.right, availableSize.width),
            std::min(height + padding_.top + padding_.bottom, availableSize.height)};
}

void StackPanel::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    const RectF inner = DeflateRect(finalRect, padding_);

    float cursorY = ComputeAlignedStart(inner.y, inner.height, measuredContent_.height, verticalContentAlignment_);
    for (size_t index = 0; index < children_.size(); ++index)
    {
        const SizeF measured = measuredChildren_[index];
        float slotX = inner.x;
        float slotWidth = inner.width;
        if (horizontalContentAlignment_ != HorizontalAlignment::Stretch)
        {
            slotWidth = std::min(measured.width, inner.width);
            slotX = ComputeAlignedStart(inner.x, inner.width, slotWidth, horizontalContentAlignment_);
        }

        children_[index]->ArrangeInLayout({slotX, cursorY, slotWidth, measured.height});
        cursorY += measured.height + spacing_;
    }
}

void StackPanel::Render(DeviceResources &deviceResources)
{
    for (const auto &child : children_)
    {
        child->Render(deviceResources);
    }
}

HorizontalStackPanel::HorizontalStackPanel(float spacing) : spacing_(spacing)
{
}

void HorizontalStackPanel::SetHorizontalContentAlignment(HorizontalAlignment alignment)
{
    horizontalContentAlignment_ = alignment;
}

void HorizontalStackPanel::SetVerticalContentAlignment(VerticalAlignment alignment)
{
    verticalContentAlignment_ = alignment;
}

SizeF HorizontalStackPanel::Measure(const SizeF &availableSize)
{
    measuredChildren_.clear();

    const SizeF innerAvailable = DeflateSize(availableSize, padding_);
    float width = 0.0f;
    float height = 0.0f;
    for (const auto &child : children_)
    {
        const SizeF measured = child->MeasureInLayout(innerAvailable);
        measuredChildren_.push_back(measured);
        width += measured.width;
        height = std::max(height, measured.height);
    }

    if (!children_.empty())
    {
        width += spacing_ * static_cast<float>(children_.size() - 1);
    }

    measuredContent_ = {width, height};
    return {std::min(width + padding_.left + padding_.right, availableSize.width),
            std::min(height + padding_.top + padding_.bottom, availableSize.height)};
}

void HorizontalStackPanel::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    const RectF inner = DeflateRect(finalRect, padding_);

    float cursorX = ComputeAlignedStart(inner.x, inner.width, measuredContent_.width, horizontalContentAlignment_);
    for (size_t index = 0; index < children_.size(); ++index)
    {
        const SizeF measured = measuredChildren_[index];
        float slotY = inner.y;
        float slotHeight = inner.height;
        if (verticalContentAlignment_ != VerticalAlignment::Stretch)
        {
            slotHeight = std::min(measured.height, inner.height);
            slotY = ComputeAlignedStart(inner.y, inner.height, slotHeight, verticalContentAlignment_);
        }

        children_[index]->ArrangeInLayout({cursorX, slotY, measured.width, slotHeight});
        cursorX += measured.width + spacing_;
    }
}

void HorizontalStackPanel::Render(DeviceResources &deviceResources)
{
    for (const auto &child : children_)
    {
        child->Render(deviceResources);
    }
}

WrapPanel::WrapPanel(float spacing, float runSpacing) : spacing_(spacing), runSpacing_(runSpacing)
{
}

SizeF WrapPanel::Measure(const SizeF &availableSize)
{
    measuredChildren_.clear();
    rowItems_.clear();

    const float maxWidth = std::max(availableSize.width, 1.0f);
    float currentX = 0.0f;
    float currentY = 0.0f;
    float rowHeight = 0.0f;
    float measuredWidth = 0.0f;

    for (size_t index = 0; index < children_.size(); ++index)
    {
        const SizeF childSize = children_[index]->MeasureInLayout(availableSize);
        measuredChildren_.push_back(childSize);

        const bool wrap = currentX > 0.0f && (currentX + childSize.width) > maxWidth;
        if (wrap)
        {
            measuredWidth = std::max(measuredWidth, currentX - spacing_);
            currentX = 0.0f;
            currentY += rowHeight + runSpacing_;
            rowHeight = 0.0f;
        }

        rowItems_.push_back({index, childSize, currentX, currentY});
        currentX += childSize.width + spacing_;
        rowHeight = std::max(rowHeight, childSize.height);
    }

    measuredWidth = std::max(measuredWidth, currentX > 0.0f ? currentX - spacing_ : 0.0f);
    measured_ = {std::min(measuredWidth, availableSize.width), std::min(currentY + rowHeight, availableSize.height)};
    return measured_;
}

void WrapPanel::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;

    for (const RowItem &item : rowItems_)
    {
        children_[item.childIndex]->ArrangeInLayout(
            {finalRect.x + item.x, finalRect.y + item.y, item.size.width, item.size.height});
    }
}

void WrapPanel::Render(DeviceResources &deviceResources)
{
    for (const auto &child : children_)
    {
        child->Render(deviceResources);
    }
}

ScrollViewer::ScrollViewer(std::shared_ptr<Visual> content) : content_(std::move(content))
{
    AdoptChild(content_);
}

SizeF ScrollViewer::Measure(const SizeF &availableSize)
{
    if (!content_)
    {
        return availableSize;
    }

    measuredContent_ = content_->MeasureInLayout({availableSize.width, 1000000.0f});
    return availableSize;
}

void ScrollViewer::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    ClampScrollOffset();
    UpdateContentLayout();
}

void ScrollViewer::Render(DeviceResources &deviceResources)
{
    if (!content_)
    {
        return;
    }

    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    target->PushAxisAlignedClip(
        D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    content_->Render(deviceResources);
    target->PopAxisAlignedClip();

    if (!HasVerticalScrollbar())
    {
        return;
    }

    const RectF trackRect = GetScrollbarTrackRect();
    const RectF thumbRect = GetScrollbarThumbRect();
    const Theme &theme = ThemeManager::GetCurrent();
    ID2D1SolidColorBrush *trackBrush = deviceResources.GetSolidColorBrush(theme.track);
    ID2D1SolidColorBrush *thumbBrush =
        deviceResources.GetSolidColorBrush((scrollbarDragging_ || scrollbarHovered_) ? theme.thumbActive : theme.thumb);
    if (!trackBrush || !thumbBrush)
    {
        return;
    }

    const auto trackRounded = D2D1::RoundedRect(
        D2D1::RectF(trackRect.x, trackRect.y, trackRect.x + trackRect.width, trackRect.y + trackRect.height), 4.0f,
        4.0f);
    const auto thumbRounded = D2D1::RoundedRect(
        D2D1::RectF(thumbRect.x, thumbRect.y, thumbRect.x + thumbRect.width, thumbRect.y + thumbRect.height), 4.0f,
        4.0f);
    target->FillRoundedRectangle(trackRounded, trackBrush);
    target->FillRoundedRectangle(thumbRounded, thumbBrush);
}

void ScrollViewer::Attach(Window *window)
{
    Visual::Attach(window);
    if (content_)
    {
        content_->Attach(window);
    }
}

Visual *ScrollViewer::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    if (HasVerticalScrollbar() && PointInRect(GetScrollbarTrackRect(), point))
    {
        return this;
    }

    if (content_)
    {
        if (Visual *hit = content_->FindVisualAt(point))
        {
            return hit;
        }
    }

    return this;
}

Visual *ScrollViewer::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    if (HasVerticalScrollbar() && PointInRect(GetScrollbarTrackRect(), point))
    {
        return nullptr;
    }

    if (content_)
    {
        return content_->FindFocusableAt(point);
    }

    return nullptr;
}

Visual *ScrollViewer::FindFirstFocusableDescendant()
{
    return content_ ? content_->FindFirstFocusableDescendant() : nullptr;
}

bool ScrollViewer::OnMouseDown(const POINT &point, WPARAM keyState)
{
    (void)keyState;
    if (!window_ || !HasVerticalScrollbar())
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const RectF trackRect = GetScrollbarTrackRect();
    if (!PointInRect(trackRect, dipPoint))
    {
        return false;
    }

    const RectF thumbRect = GetScrollbarThumbRect();
    if (PointInRect(thumbRect, dipPoint))
    {
        scrollbarDragging_ = true;
        scrollbarDragOffsetY_ = dipPoint.y - thumbRect.y;
        return true;
    }

    const float trackTravel = std::max(trackRect.height - thumbRect.height, 1.0f);
    const float targetTop = std::clamp(dipPoint.y - thumbRect.height * 0.5f, trackRect.y, trackRect.y + trackTravel);
    const float ratio = (targetTop - trackRect.y) / trackTravel;
    scrollOffsetY_ = ratio * std::max(measuredContent_.height - bounds_.height, 0.0f);
    ClampScrollOffset();
    UpdateContentLayout();
    RefreshViewport();
    return true;
}

bool ScrollViewer::OnMouseUp(const POINT &point, WPARAM keyState)
{
    (void)point;
    (void)keyState;
    if (!scrollbarDragging_)
    {
        return false;
    }

    scrollbarDragging_ = false;
    RefreshViewport();
    return true;
}

bool ScrollViewer::OnMouseMove(const POINT &point, WPARAM keyState)
{
    if (!window_ || !HasVerticalScrollbar())
    {
        return false;
    }

    const PointF dipPoint = window_->ClientPixelsToDips(point);
    const bool hovered = PointInRect(GetScrollbarTrackRect(), dipPoint);
    if (scrollbarHovered_ != hovered)
    {
        scrollbarHovered_ = hovered;
        RefreshViewport();
    }

    if (!scrollbarDragging_ || !(keyState & MK_LBUTTON))
    {
        return hovered;
    }

    const RectF trackRect = GetScrollbarTrackRect();
    const RectF thumbRect = GetScrollbarThumbRect();
    const float trackTravel = std::max(trackRect.height - thumbRect.height, 1.0f);
    const float targetTop = std::clamp(dipPoint.y - scrollbarDragOffsetY_, trackRect.y, trackRect.y + trackTravel);
    const float ratio = (targetTop - trackRect.y) / trackTravel;
    scrollOffsetY_ = ratio * std::max(measuredContent_.height - bounds_.height, 0.0f);
    ClampScrollOffset();
    UpdateContentLayout();
    RefreshViewport();
    return true;
}

bool ScrollViewer::OnMouseWheel(const POINT &point, short delta, WPARAM keyState)
{
    (void)keyState;

    if (!window_ || !HitTest(window_->ClientPixelsToDips(point)))
    {
        return false;
    }

    const float maxOffset = std::max(measuredContent_.height - bounds_.height, 0.0f);
    if (maxOffset <= 0.0f)
    {
        return false;
    }

    scrollOffsetY_ = std::clamp(scrollOffsetY_ - (static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA)) * 72.0f,
                                0.0f, maxOffset);
    UpdateContentLayout();
    RefreshViewport();
    return true;
}

HCURSOR ScrollViewer::GetCursor() const
{
    return Visual::GetCursor();
}

void ScrollViewer::OnMouseEnter()
{
    if (!HasVerticalScrollbar())
    {
        return;
    }

    if (!scrollbarHovered_)
    {
        scrollbarHovered_ = true;
        RefreshViewport();
    }
}

void ScrollViewer::OnMouseLeave()
{
    if (scrollbarHovered_)
    {
        scrollbarHovered_ = false;
        RefreshViewport();
    }
}

void ScrollViewer::ClampScrollOffset()
{
    const float maxOffset = std::max(measuredContent_.height - bounds_.height, 0.0f);
    scrollOffsetY_ = std::clamp(scrollOffsetY_, 0.0f, maxOffset);
}

void ScrollViewer::RefreshViewport()
{
    if (window_ && bounds_.width > 0.0f && bounds_.height > 0.0f)
    {
        window_->Invalidate(bounds_);
        return;
    }

    InvalidateVisual();
}

RectF ScrollViewer::GetScrollbarTrackRect() const
{
    constexpr float kScrollbarWidth = 10.0f;
    return {bounds_.x + bounds_.width - kScrollbarWidth, bounds_.y, kScrollbarWidth, std::max(bounds_.height, 0.0f)};
}

RectF ScrollViewer::GetScrollbarThumbRect() const
{
    const RectF trackRect = GetScrollbarTrackRect();
    const float visibleRatio =
        bounds_.height > 0.0f ? std::clamp(bounds_.height / std::max(measuredContent_.height, 1.0f), 0.0f, 1.0f) : 1.0f;
    const float thumbHeight = std::max(trackRect.height * visibleRatio, 36.0f);
    const float maxOffset = std::max(measuredContent_.height - bounds_.height, 0.0f);
    const float trackTravel = std::max(trackRect.height - thumbHeight, 0.0f);
    const float ratio = maxOffset > 0.0f ? scrollOffsetY_ / maxOffset : 0.0f;
    return {trackRect.x, trackRect.y + trackTravel * ratio, trackRect.width, thumbHeight};
}

bool ScrollViewer::HasVerticalScrollbar() const
{
    return measuredContent_.height > bounds_.height + 0.5f;
}

void ScrollViewer::UpdateContentLayout()
{
    if (!content_)
    {
        return;
    }

    const float scrollbarReserve = HasVerticalScrollbar() ? 20.0f : 0.0f;
    content_->ArrangeInLayout({bounds_.x, bounds_.y - scrollOffsetY_, std::max(bounds_.width - scrollbarReserve, 0.0f),
                               measuredContent_.height});

    if (window_)
    {
        if (Scene *scene = window_->GetScene())
        {
            scene->RelayoutPopups();
        }
    }
}

void Grid::AddRow(GridLength length)
{
    rowDefinitions_.push_back(length);
    InvalidateMeasure();
}

void Grid::AddColumn(GridLength length)
{
    columnDefinitions_.push_back(length);
    InvalidateMeasure();
}

void Grid::AddChild(std::shared_ptr<Visual> child, size_t row, size_t column, size_t rowSpan, size_t columnSpan)
{
    if (!child)
    {
        return;
    }

    AdoptChild(child);
    children_.push_back(
        {std::move(child), {row, column, std::max<size_t>(rowSpan, 1), std::max<size_t>(columnSpan, 1)}, {}});
    InvalidateMeasure();
}

void Grid::SetRowSpacing(float spacing)
{
    rowSpacing_ = std::max(spacing, 0.0f);
    InvalidateMeasure();
}

void Grid::SetColumnSpacing(float spacing)
{
    columnSpacing_ = std::max(spacing, 0.0f);
    InvalidateMeasure();
}

void Grid::Attach(Window *window)
{
    Visual::Attach(window);
    for (auto &child : children_)
    {
        child.visual->Attach(window);
    }
}

Visual *Grid::FindVisualAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
    {
        if (Visual *hit = it->visual->FindVisualAt(point))
        {
            return hit;
        }
    }

    return this;
}

Visual *Grid::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
    {
        if (Visual *focusable = it->visual->FindFocusableAt(point))
        {
            return focusable;
        }
    }

    return nullptr;
}

Visual *Grid::FindFirstFocusableDescendant()
{
    for (auto &child : children_)
    {
        if (Visual *focusable = child.visual->FindFirstFocusableDescendant())
        {
            return focusable;
        }
    }

    return nullptr;
}

SizeF Grid::Measure(const SizeF &availableSize)
{
    if (rowDefinitions_.empty())
    {
        rowDefinitions_.push_back({GridUnitType::Star, 1.0f});
    }
    if (columnDefinitions_.empty())
    {
        columnDefinitions_.push_back({GridUnitType::Star, 1.0f});
    }

    for (auto &child : children_)
    {
        child.measured = child.visual->MeasureInLayout(availableSize);
    }

    auto columns = ResolveTrackSizes(columnDefinitions_, columnSpacing_, availableSize.width, true);
    auto rows = ResolveTrackSizes(rowDefinitions_, rowSpacing_, availableSize.height, false);

    const float totalColumnWidth = std::accumulate(columns.begin(), columns.end(), 0.0f) +
                                   columnSpacing_ * std::max<int>(static_cast<int>(columns.size()) - 1, 0);
    const float totalRowHeight = std::accumulate(rows.begin(), rows.end(), 0.0f) +
                                 rowSpacing_ * std::max<int>(static_cast<int>(rows.size()) - 1, 0);

    measuredContent_ = {std::min(totalColumnWidth, availableSize.width),
                        std::min(totalRowHeight, availableSize.height)};
    return measuredContent_;
}

void Grid::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;

    auto columns = ResolveTrackSizes(columnDefinitions_, columnSpacing_, finalRect.width, true);
    auto rows = ResolveTrackSizes(rowDefinitions_, rowSpacing_, finalRect.height, false);

    std::vector<float> columnOffsets(columns.size(), finalRect.x);
    std::vector<float> rowOffsets(rows.size(), finalRect.y);

    for (size_t i = 1; i < columns.size(); ++i)
    {
        columnOffsets[i] = columnOffsets[i - 1] + columns[i - 1] + columnSpacing_;
    }
    for (size_t i = 1; i < rows.size(); ++i)
    {
        rowOffsets[i] = rowOffsets[i - 1] + rows[i - 1] + rowSpacing_;
    }

    for (auto &child : children_)
    {
        const size_t row = std::min(child.cell.row, rows.size() - 1);
        const size_t column = std::min(child.cell.column, columns.size() - 1);
        const size_t rowEnd = std::min(row + child.cell.rowSpan, rows.size());
        const size_t columnEnd = std::min(column + child.cell.columnSpan, columns.size());

        float width = 0.0f;
        for (size_t i = column; i < columnEnd; ++i)
        {
            width += columns[i];
        }
        if (columnEnd > column)
        {
            width += columnSpacing_ * static_cast<float>(columnEnd - column - 1);
        }

        float height = 0.0f;
        for (size_t i = row; i < rowEnd; ++i)
        {
            height += rows[i];
        }
        if (rowEnd > row)
        {
            height += rowSpacing_ * static_cast<float>(rowEnd - row - 1);
        }

        child.visual->ArrangeInLayout({columnOffsets[column], rowOffsets[row], width, height});
    }
}

void Grid::Render(DeviceResources &deviceResources)
{
    for (auto &child : children_)
    {
        child.visual->Render(deviceResources);
    }
}

std::vector<float> Grid::ResolveTrackSizes(const std::vector<GridLength> &definitions, float spacing, float available,
                                           bool horizontalAxis)
{
    std::vector<float> sizes(definitions.size(), 0.0f);
    if (definitions.empty())
    {
        return sizes;
    }

    const float usable =
        std::max(available - spacing * std::max<int>(static_cast<int>(definitions.size()) - 1, 0), 0.0f);
    float used = 0.0f;
    float totalStar = 0.0f;

    for (size_t index = 0; index < definitions.size(); ++index)
    {
        const GridLength &definition = definitions[index];
        if (definition.unitType == GridUnitType::Pixel)
        {
            sizes[index] = std::max(definition.value, 0.0f);
            used += sizes[index];
        }
        else if (definition.unitType == GridUnitType::Star)
        {
            totalStar += std::max(definition.value, 0.0f);
        }
    }

    for (size_t index = 0; index < definitions.size(); ++index)
    {
        if (definitions[index].unitType != GridUnitType::Auto)
        {
            continue;
        }

        float autoSize = 0.0f;
        for (const auto &child : children_)
        {
            const size_t track = horizontalAxis ? child.cell.column : child.cell.row;
            const size_t span = horizontalAxis ? child.cell.columnSpan : child.cell.rowSpan;
            if (track != index || span != 1)
            {
                continue;
            }

            autoSize = std::max(autoSize, horizontalAxis ? child.measured.width : child.measured.height);
        }

        sizes[index] = autoSize;
        used += autoSize;
    }

    const float remaining = std::max(usable - used, 0.0f);
    if (totalStar > 0.0f)
    {
        for (size_t index = 0; index < definitions.size(); ++index)
        {
            if (definitions[index].unitType != GridUnitType::Star)
            {
                continue;
            }

            sizes[index] = remaining * (std::max(definitions[index].value, 0.0f) / totalStar);
        }
    }

    return sizes;
}

Container::Container(std::shared_ptr<Visual> child) : child_(std::move(child))
{
    AdoptChild(child_);
}

void Container::SetChild(std::shared_ptr<Visual> child)
{
    ReleaseChild(child_);

    child_ = std::move(child);
    AdoptChild(child_);
    InvalidateMeasure();
}

void Container::Attach(Window *window)
{
    Visual::Attach(window);
    if (child_)
    {
        child_->Attach(window);
    }
}

Visual *Container::FindVisualAt(const PointF &point)
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

Visual *Container::FindFocusableAt(const PointF &point)
{
    if (!HitTest(point))
    {
        return nullptr;
    }

    return child_ ? child_->FindFocusableAt(point) : nullptr;
}

Visual *Container::FindFirstFocusableDescendant()
{
    return child_ ? child_->FindFirstFocusableDescendant() : nullptr;
}

SizeF Container::Measure(const SizeF &availableSize)
{
    const SizeF inner = DeflateSize(availableSize, padding_);
    if (!child_)
    {
        return {padding_.left + padding_.right, padding_.top + padding_.bottom};
    }

    const SizeF childSize = child_->MeasureInLayout(inner);
    return {childSize.width + padding_.left + padding_.right, childSize.height + padding_.top + padding_.bottom};
}

void Container::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (child_)
    {
        child_->ArrangeInLayout(GetContentRect());
    }
}

void Container::Render(DeviceResources &deviceResources)
{
    if (child_)
    {
        child_->Render(deviceResources);
    }
}

RectF Container::GetContentRect() const
{
    return DeflateRect(bounds_, padding_);
}

Border::Border(Brush brush, std::shared_ptr<Visual> child) : Container(std::move(child)), brush_(brush)
{
}

SizeF Border::Measure(const SizeF &availableSize)
{
    const float inset = brush_.strokeWidth;
    const Thickness borderInset = {inset, inset, inset, inset};
    const SizeF inner = DeflateSize(DeflateSize(availableSize, padding_), borderInset);
    if (!child_)
    {
        return {padding_.left + padding_.right + inset * 2.0f, padding_.top + padding_.bottom + inset * 2.0f};
    }

    const SizeF childSize = child_->MeasureInLayout(inner);
    return {childSize.width + padding_.left + padding_.right + inset * 2.0f,
            childSize.height + padding_.top + padding_.bottom + inset * 2.0f};
}

void Border::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
    if (!child_)
    {
        return;
    }

    const Thickness borderInset = {brush_.strokeWidth, brush_.strokeWidth, brush_.strokeWidth, brush_.strokeWidth};
    child_->ArrangeInLayout(DeflateRect(DeflateRect(bounds_, borderInset), padding_));
}

void Border::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(brush_.fill);
    ID2D1SolidColorBrush *strokeBrush = deviceResources.GetSolidColorBrush(brush_.stroke);
    if (!fillBrush || !strokeBrush)
    {
        return;
    }

    const auto roundedRect =
        D2D1::RoundedRect(D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height),
                          brush_.radiusX, brush_.radiusY);
    target->FillRoundedRectangle(roundedRect, fillBrush);
    target->DrawRoundedRectangle(roundedRect, strokeBrush, brush_.strokeWidth);

    Container::Render(deviceResources);
}

Card::Card(Brush brush, float padding) : brush_(brush), padding_(padding)
{
}

void Card::SetBrush(Brush brush)
{
    brush_ = brush;
    InvalidateVisual();
}

void Card::SetPadding(float padding)
{
    if (padding_ == padding)
    {
        return;
    }
    padding_ = padding;
    InvalidateMeasure();
}

void Card::SetShadowScale(float scale)
{
    const float clamped = std::max(scale, 0.15f);
    if (shadowScale_ == clamped)
    {
        return;
    }
    shadowScale_ = clamped;
    InvalidateVisual();
}

void Card::SetShadowOpacity(float opacity)
{
    const float clamped = std::clamp(opacity, 0.0f, 1.0f);
    if (shadowOpacity_ == clamped)
    {
        return;
    }
    shadowOpacity_ = clamped;
    InvalidateVisual();
}

void Card::SetShadowPasses(const std::vector<ShadowPass> &passes)
{
    if (shadowPasses_ == passes)
    {
        return;
    }
    shadowPasses_ = passes;
    InvalidateVisual();
}

void Card::SetShadowEnabled(bool enabled)
{
    if (shadowEnabled_ == enabled)
    {
        return;
    }
    shadowEnabled_ = enabled;
    InvalidateVisual();
}

SizeF Card::Measure(const SizeF &availableSize)
{
    const SizeF inner = {std::max(availableSize.width - padding_ * 2.0f, 0.0f),
                         std::max(availableSize.height - padding_ * 2.0f, 0.0f)};

    float width = 0.0f;
    float height = 0.0f;
    for (const auto &child : children_)
    {
        childSize_ = child->MeasureInLayout(inner);
        width = std::max(width, childSize_.width);
        height = std::max(height, childSize_.height);
    }

    return {width + padding_ * 2.0f, height + padding_ * 2.0f};
}

void Card::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;

    const RectF inner = {finalRect.x + padding_, finalRect.y + padding_,
                         std::max(finalRect.width - padding_ * 2.0f, 0.0f),
                         std::max(finalRect.height - padding_ * 2.0f, 0.0f)};

    for (const auto &child : children_)
    {
        child->ArrangeInLayout(inner);
    }
}

void Card::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    ID2D1SolidColorBrush *fillBrush = deviceResources.GetSolidColorBrush(brush_.fill);
    ID2D1SolidColorBrush *strokeBrush = deviceResources.GetSolidColorBrush(brush_.stroke);
    if (!fillBrush || !strokeBrush)
    {
        return;
    }

    if (shadowEnabled_)
    {
        if (shadowPasses_.empty())
        {
            DrawWin11WindowShadow(target, bounds_, brush_.radiusX, shadowScale_, shadowOpacity_);
        }
        else
        {
            DrawGaussianPasses(target, bounds_, brush_.radiusX, shadowPasses_.data(), shadowPasses_.size(),
                               shadowScale_, shadowOpacity_);
        }
    }

    const auto roundedRect =
        D2D1::RoundedRect(D2D1::RectF(bounds_.x, bounds_.y, bounds_.x + bounds_.width, bounds_.y + bounds_.height),
                          brush_.radiusX, brush_.radiusY);
    target->FillRoundedRectangle(roundedRect, fillBrush);
    target->DrawRoundedRectangle(roundedRect, strokeBrush, brush_.strokeWidth);

    for (const auto &child : children_)
    {
        child->Render(deviceResources);
    }
}

TextBlock::TextBlock(std::wstring text, float fontSize, D2D1_COLOR_F color, bool bold)
    : text_(std::move(text)), fontSize_(fontSize), color_(color), caretColor_(color), bold_(bold)
{
}

void TextBlock::SetText(std::wstring text)
{
    text_ = std::move(text);
    InvalidateTextLayoutCache();
    InvalidateMeasure();
}

void TextBlock::SetColor(D2D1_COLOR_F color)
{
    color_ = color;
    InvalidateVisual();
}

void TextBlock::SetFontSize(float fontSize)
{
    if (fontSize_ == fontSize)
    {
        return;
    }
    fontSize_ = fontSize;
    InvalidateTextLayoutCache();
    InvalidateMeasure();
}

void TextBlock::SetTextAlignment(DWRITE_TEXT_ALIGNMENT alignment)
{
    if (textAlignment_ == alignment)
    {
        return;
    }

    textAlignment_ = alignment;
    InvalidateTextLayoutCache();
    InvalidateVisual();
}

void TextBlock::SetFontFamily(std::wstring fontFamily)
{
    if (fontFamilyOverride_ == fontFamily)
    {
        return;
    }

    fontFamilyOverride_ = std::move(fontFamily);
    InvalidateTextLayoutCache();
    InvalidateMeasure();
}

void TextBlock::InvalidateTextLayoutCache()
{
    cachedTextLayout_.Reset();
    cachedFontFamily_.clear();
    cachedLayoutWidth_ = -1.0f;
}

void TextBlock::SetFallbackFontFamilies(std::vector<std::wstring> families)
{
    if (hasCustomFontFallback_ && fallbackFontFamilies_ == families)
        return;
    hasCustomFontFallback_ = true;
    fallbackFontFamilies_ = std::move(families);
    InvalidateTextLayoutCache();
    InvalidateMeasure();
}

void TextBlock::SetTextLayoutPadding(Thickness padding)
{
    textLayoutPadding_ = padding;
    InvalidateTextLayoutCache();
    InvalidateMeasure();
}

void TextBlock::SetLetterSpacing(float dips)
{
    const float spacing = (std::max)(dips, 0.0f);
    if (letterSpacing_ == spacing)
    {
        return;
    }
    letterSpacing_ = spacing;
    InvalidateTextLayoutCache();
    InvalidateMeasure();
}

void TextBlock::SetCaretIndex(size_t index)
{
    showCaret_ = true;
    caretIndex_ = index;
    InvalidateTextLayoutCache();
    InvalidateMeasure();
    InvalidateVisual();
}

void TextBlock::SetCaretColor(D2D1_COLOR_F color)
{
    caretColor_ = color;
    InvalidateVisual();
}

void TextBlock::ClearCaret()
{
    if (!showCaret_)
    {
        return;
    }
    showCaret_ = false;
    InvalidateTextLayoutCache();
    InvalidateMeasure();
    InvalidateVisual();
}

SizeF TextBlock::Measure(const SizeF &availableSize)
{
    const float maxWidth = std::max(availableSize.width, 1.0f);
    IDWriteFactory *dwriteFactory = GetSharedDWriteFactory();
    if (!dwriteFactory)
    {
        measured_ = {maxWidth, fontSize_ + textLayoutPadding_.top + textLayoutPadding_.bottom};
        return measured_;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const std::wstring &fontFamily = fontFamilyOverride_.empty() ? theme.uiFontFamily : fontFamilyOverride_;
    const bool needsLayout = !cachedTextLayout_ || cachedLayoutWidth_ != maxWidth || cachedFontFamily_ != fontFamily;
    if (needsLayout)
    {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwriteFactory->CreateTextFormat(
                fontFamily.c_str(), nullptr, bold_ ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize_, L"", format.GetAddressOf())))
        {
            if (FAILED(dwriteFactory->CreateTextFormat(
                    UiFontFallbackFamily(), nullptr, bold_ ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize_, L"", format.GetAddressOf())))
            {
                measured_ = {maxWidth, fontSize_ + textLayoutPadding_.top + textLayoutPadding_.bottom};
                return measured_;
            }
        }

        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        format->SetTextAlignment(textAlignment_);
        if (hasCustomFontFallback_)
            ApplyFontFallback(dwriteFactory, format.Get(), fallbackFontFamilies_);
        else
            ApplyUiFontFallback(dwriteFactory, format.Get());

        const UINT32 caretPos = static_cast<UINT32>((std::min)(caretIndex_, text_.size()));
        const bool insertSlot = showCaret_ && caretPos < text_.size();
        std::wstring layoutText = text_;
        if (insertSlot)
        {
            layoutText.insert(layoutText.begin() + static_cast<std::ptrdiff_t>(caretPos), kCaretSlotChar);
        }

        if (FAILED(dwriteFactory->CreateTextLayout(layoutText.c_str(), static_cast<UINT32>(layoutText.size()),
                                                   format.Get(), maxWidth, std::numeric_limits<float>::max(),
                                                   cachedTextLayout_.ReleaseAndGetAddressOf())))
        {
            measured_ = {maxWidth, fontSize_ + textLayoutPadding_.top + textLayoutPadding_.bottom};
            return measured_;
        }

        if (letterSpacing_ > 0.0f && !layoutText.empty())
        {
            Microsoft::WRL::ComPtr<IDWriteTextLayout1> layout1;
            if (SUCCEEDED(cachedTextLayout_.As(&layout1)))
            {
                layout1->SetCharacterSpacing(0.0f, letterSpacing_, 0.0f,
                                             DWRITE_TEXT_RANGE{0, static_cast<UINT32>(layoutText.size())});
            }
        }

        if (insertSlot)
        {
            InsertCaretSlot(cachedTextLayout_.Get(), caretPos, fontSize_);
        }

        cachedLayoutWidth_ = maxWidth;
        cachedFontFamily_ = fontFamily;
    }

    DWRITE_TEXT_METRICS metrics = {};
    if (FAILED(cachedTextLayout_->GetMetrics(&metrics)))
    {
        measured_ = {maxWidth, fontSize_ + textLayoutPadding_.top + textLayoutPadding_.bottom};
        return measured_;
    }

    DWRITE_OVERHANG_METRICS overhang = {};
    cachedTextLayout_->GetOverhangMetrics(&overhang);

    const float extraTop = std::max(overhang.top, 0.0f);
    const float extraBottom = std::max(overhang.bottom, 0.0f);
    const float extraLeft = std::max(overhang.left, 0.0f);
    const float extraRight = std::max(overhang.right, 0.0f);
    const float paddedHeight =
        metrics.height + extraTop + extraBottom + textLayoutPadding_.top + textLayoutPadding_.bottom;
    const float minimumHeight = fontSize_ + textLayoutPadding_.top + textLayoutPadding_.bottom;
    const float measuredHeight = std::ceil(std::max(paddedHeight, minimumHeight));
    const float caretEndReserve =
        (showCaret_ && caretIndex_ >= text_.size()) ? (kPreeditCaretEndAir + kPreeditCaretBarWidth) : 0.0f;
    const float contentWidth =
        metrics.width + extraLeft + extraRight + textLayoutPadding_.left + textLayoutPadding_.right + caretEndReserve;
    const float measuredWidth = std::min(std::max(contentWidth, 1.0f), maxWidth);

    measured_ = {measuredWidth, measuredHeight};
    return measured_;
}

void TextBlock::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void TextBlock::Render(DeviceResources &deviceResources)
{
    ID2D1RenderTarget *target = deviceResources.GetRenderTarget();
    if (!target)
    {
        return;
    }

    const Theme &theme = ThemeManager::GetCurrent();
    const std::wstring &fontFamily = fontFamilyOverride_.empty() ? theme.uiFontFamily : fontFamilyOverride_;
    if (!cachedTextLayout_ || cachedLayoutWidth_ != std::max(bounds_.width, 1.0f) || cachedFontFamily_ != fontFamily)
    {
        Measure({std::max(bounds_.width, 1.0f), std::max(bounds_.height, 1.0f)});
    }

    ID2D1SolidColorBrush *brush = deviceResources.GetSolidColorBrush(color_);
    if (!cachedTextLayout_ || !brush)
    {
        return;
    }

    const float originX = bounds_.x + textLayoutPadding_.left;
    const float originY = bounds_.y + textLayoutPadding_.top;
    if (showCaret_)
    {
        FLOAT caretX = 0.0f;
        FLOAT caretY = 0.0f;
        DWRITE_HIT_TEST_METRICS hit = {};
        const UINT32 caretPos = static_cast<UINT32>((std::min)(caretIndex_, text_.size()));
        const bool caretAtEnd = caretPos == text_.size();
        cachedTextLayout_->HitTestTextPosition(caretPos, caretAtEnd && !text_.empty() ? TRUE : FALSE, &caretX, &caretY,
                                               &hit);
        const float barWidth = kPreeditCaretBarWidth;
        float left = originX + caretX + kPreeditCaretEndAir;
        if (!caretAtEnd)
        {
            const float slotWidth = hit.width > 1.0f ? hit.width : kPreeditCaretInsertGap;
            left = originX + caretX + (slotWidth - barWidth) * 0.5f;
        }
        // Mirrors the CSS caret (`height: 1.2em` inline-block on the baseline,
        // `translate(..., 10%)`): it ends 0.12em below the baseline, well inside
        // the line box. Sizing it to the full line height and nudging it down
        // pushed its bottom past the line into the row below.
        const float barHeight = fontSize_ * 1.2f;
        float baseline = hit.height > 0.0f ? hit.height * 0.8f : fontSize_;
        UINT32 lineCount = 0;
        cachedTextLayout_->GetLineMetrics(nullptr, 0, &lineCount);
        if (lineCount > 0)
        {
            std::vector<DWRITE_LINE_METRICS> lines(lineCount);
            if (SUCCEEDED(cachedTextLayout_->GetLineMetrics(lines.data(), lineCount, &lineCount)))
            {
                float lineTop = 0.0f;
                for (const DWRITE_LINE_METRICS &line : lines)
                {
                    baseline = line.baseline;
                    if (caretY < lineTop + line.height - 0.5f)
                    {
                        break;
                    }
                    lineTop += line.height;
                }
            }
        }
        const float top = originY + caretY + baseline + fontSize_ * 0.12f - barHeight;
        if (ID2D1SolidColorBrush *caretBrush = deviceResources.GetSolidColorBrush(caretColor_))
        {
            target->FillRectangle(D2D1::RectF(left, top, left + barWidth, top + barHeight), caretBrush);
        }
    }

    target->DrawTextLayout(
        D2D1::Point2F(originX, originY), cachedTextLayout_.Get(), brush,
        static_cast<D2D1_DRAW_TEXT_OPTIONS>(D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT));
}

Spacer::Spacer(float height) : height_(height)
{
}

SizeF Spacer::Measure(const SizeF &availableSize)
{
    return {availableSize.width, height_};
}

void Spacer::Arrange(const RectF &finalRect)
{
    bounds_ = finalRect;
}

void Spacer::Render(DeviceResources &deviceResources)
{
    (void)deviceResources;
}
} // namespace msimeui
