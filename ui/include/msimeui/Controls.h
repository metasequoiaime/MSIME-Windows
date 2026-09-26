#pragma once

#include "Layout.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

class CTextEditor;

namespace msimeui
{
enum class PopupPlacement
{
    BelowLeading,
    AboveLeading,
};

enum class ImageStretch
{
    None,
    Fill,
    Uniform,
    UniformToFill,
};

class Image : public Visual
{
  public:
    explicit Image(std::wstring filePath);

    void SetSource(std::wstring filePath);
    const std::wstring &GetSource() const;
    void SetStretch(ImageStretch stretch);
    void SetOpacity(float opacity);
    void SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE interpolationMode);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    void LoadNaturalSize();

    std::wstring filePath_;
    SizeF naturalSize_ = {};
    bool naturalSizeLoaded_ = false;
    ImageStretch stretch_ = ImageStretch::Uniform;
    float opacity_ = 1.0f;
    D2D1_BITMAP_INTERPOLATION_MODE interpolationMode_ = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
};

class TextBox : public Visual
{
  public:
    using TextChangedHandler = std::function<void(const std::wstring &text)>;

    TextBox(float height, std::wstring placeholder);
    ~TextBox() override;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    bool OnKeyDown(WPARAM key, LPARAM lParam) override;
    bool OnChar(wchar_t ch, LPARAM lParam) override;
    bool OnTimer(UINT_PTR timerId) override;
    HCURSOR GetCursor() const override;
    std::wstring GetText() const;
    bool IsFocused() const
    {
        return focused_;
    }
    void SetOnTextChanged(TextChangedHandler handler);
    void SetOnFocusChanged(std::function<void(bool focused)> handler);
    void SetFontSize(float fontSizeDips);
    void SetPlaceholderFontSize(float fontSizeDips);
    void SetPlaceholderText(std::wstring placeholder);
    void SetChromeVisible(bool visible);

  private:
    RECT ComputeEditorHostRect() const;
    RECT ComputeEditorContentPadding() const;
    POINT ToLocalPoint(const POINT &point) const;
    bool EnsureInitialized(DeviceResources *deviceResources);
    bool AlertMouseSink(const POINT &point, WPARAM keyState);

    float preferredHeight_ = 44.0f;
    std::wstring placeholder_;
    bool focused_ = false;
    bool tsfInitialized_ = false;
    bool renderInitialized_ = false;
    UINT dragSelectionStart_ = static_cast<UINT>(-1);
    ::CTextEditor *editor_ = nullptr;
    LOGFONT font_ = {};
    TextChangedHandler onTextChanged_;
    std::function<void(bool focused)> onFocusChanged_;
    float fontSizeDips_ = 18.0f;
    float placeholderFontSizeDips_ = 16.0f;
    bool chromeVisible_ = true;
};

class Button : public Visual
{
  public:
    using ClickHandler = std::function<void()>;

    Button(std::wstring text, float height = 44.0f);

    void SetOnClick(ClickHandler handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  protected:
    void InvalidateTextLayoutCache();

    virtual void OnClick();
    virtual D2D1_COLOR_F GetFillColor() const;
    virtual D2D1_COLOR_F GetStrokeColor() const;
    virtual D2D1_COLOR_F GetTextColor() const;

    std::wstring text_;
    float preferredHeight_ = 44.0f;
    bool focused_ = false;
    bool pressed_ = false;
    ClickHandler onClick_;
    std::wstring cachedFontFamily_;
    float cachedLayoutWidth_ = -1.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> cachedTextLayout_;
};

class Popup : public Visual
{
  public:
    explicit Popup(std::shared_ptr<Visual> child);

    void SetAnchorRect(const RectF &anchorRect);
    void SetPlacement(PopupPlacement placement);
    void SetOffset(float x, float y);
    void SetMatchAnchorWidth(bool matchAnchorWidth);
    void SetConstrainToViewport(bool constrainToViewport);
    void SetBackgroundFill(const D2D1_COLOR_F &fill);
    void SetBorderColor(const D2D1_COLOR_F &border);
    void SetCornerRadius(float radius);
    void SetShadowEnabled(bool enabled);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool HitTest(const PointF &point) const override;
    bool KeepsPopupsOpenOnClick() const override;
    void LayoutOverlay(const SizeF &viewportSize) override;

  private:
    std::shared_ptr<Visual> child_;
    RectF anchorRect_ = {};
    PopupPlacement placement_ = PopupPlacement::BelowLeading;
    float offsetX_ = 0.0f;
    float offsetY_ = 8.0f;
    bool matchAnchorWidth_ = true;
    bool constrainToViewport_ = true;
    D2D1_COLOR_F backgroundFill_ = D2D1::ColorF(0xFFFFFF);
    D2D1_COLOR_F borderColor_ = D2D1::ColorF(0xD6DCE5);
    float cornerRadius_ = 16.0f;
    bool shadowEnabled_ = false;
};

class MenuFlyoutItem : public Visual
{
  public:
    using ClickHandler = std::function<void()>;
    using HoverHandler = std::function<void(bool hovered)>;

    explicit MenuFlyoutItem(std::wstring text, bool hasSubmenu = false);

    void SetOnClick(ClickHandler handler);
    void SetOnHover(HoverHandler handler);
    void SetColors(const D2D1_COLOR_F &text, const D2D1_COLOR_F &hoverFill);
    void SetHasSubmenu(bool hasSubmenu);
    void SetLeadingSvg(std::string svgUtf8);
    void SetTrailingToggle(bool show);
    void SetToggleOn(bool on);
    bool IsToggleOn() const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    bool KeepsPopupsOpenOnClick() const override;
    HCURSOR GetCursor() const override;

  private:
    void SetHovered(bool hovered);
    bool UsesTrayLayout() const;
    RectF ToggleHitRect() const;

    std::wstring text_;
    std::string leadingSvg_;
    bool showToggle_ = false;
    bool toggleOn_ = false;
    bool hasSubmenu_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
    D2D1_COLOR_F textColor_ = D2D1::ColorF(0xE9E8E8);
    D2D1_COLOR_F hoverFill_ = D2D1::ColorF(0x414141);
    ClickHandler onClick_;
    HoverHandler onHover_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout_;
    Microsoft::WRL::ComPtr<IUnknown> svgDocument_;
    std::wstring cachedFontFamily_;
    float cachedLayoutWidth_ = -1.0f;
    void *svgContext_ = nullptr;
    std::string svgTintCache_;
};

class MenuSeparator : public Visual
{
  public:
    void SetColor(const D2D1_COLOR_F &color);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;

  private:
    D2D1_COLOR_F color_ = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.125f);
};

class PopupHost : public Visual
{
  public:
    PopupHost(std::shared_ptr<Visual> trigger, std::shared_ptr<Popup> popup);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool HitTest(const PointF &point) const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;
    bool KeepsPopupsOpenOnClick() const override;

    void OpenPopup();
    void ClosePopup();
    void TogglePopup();
    bool IsOpen() const;

  private:
    std::shared_ptr<Visual> trigger_;
    std::shared_ptr<Popup> popup_;
    bool pressed_ = false;
    bool open_ = false;
};

class ContextMenuHost : public Visual
{
  public:
    ContextMenuHost(std::shared_ptr<Visual> trigger, std::shared_ptr<Popup> popup);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool HitTest(const PointF &point) const override;
    bool OnContextMenu(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;
    bool KeepsPopupsOpenOnClick() const override;

    void OpenPopupAt(const PointF &anchorPoint);
    void ClosePopup();
    bool IsOpen() const;

  private:
    std::shared_ptr<Visual> trigger_;
    std::shared_ptr<Popup> popup_;
    bool open_ = false;
};

class ComboBox : public Visual
{
  public:
    using SelectionChangedHandler = std::function<void(size_t selectedIndex, const std::wstring &value)>;

    explicit ComboBox(float height = 44.0f);

    void AddItem(std::wstring item);
    void ClearItems();
    void SetSelectedIndex(size_t index);
    size_t GetSelectedIndex() const;
    const std::wstring &GetSelectedText() const;
    void SetOnSelectionChanged(SelectionChangedHandler handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;
    bool KeepsPopupsOpenOnClick() const override;

  private:
    void OpenPopup();
    void ClosePopup();
    void TogglePopup();
    void NotifySelectionChanged();
    void SyncPopupState();

    std::vector<std::wstring> items_;
    float preferredHeight_ = 44.0f;
    bool focused_ = false;
    bool pressed_ = false;
    bool open_ = false;
    size_t selectedIndex_ = static_cast<size_t>(-1);
    SelectionChangedHandler onSelectionChanged_;
    std::shared_ptr<Visual> popupContent_;
    std::shared_ptr<Popup> popup_;
};

class CheckBox : public Visual
{
  public:
    using ChangeHandler = std::function<void(bool checked)>;

    CheckBox(std::wstring text, bool checked = false);

    void SetOnChanged(ChangeHandler handler);
    bool IsChecked() const;
    void SetChecked(bool checked);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    std::wstring text_;
    bool checked_ = false;
    bool focused_ = false;
    bool pressed_ = false;
    ChangeHandler onChanged_;
};

class ProgressBar : public Visual
{
  public:
    explicit ProgressBar(float height = 12.0f);

    void SetValue(float value);
    float GetValue() const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    float preferredHeight_ = 12.0f;
    float value_ = 0.0f;
};

class Slider : public Visual
{
  public:
    using ChangeHandler = std::function<void(float value)>;

    Slider(float minValue, float maxValue, float value, float height = 34.0f);

    void SetOnChanged(ChangeHandler handler);
    void SetValue(float value);
    float GetValue() const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    float NormalizedValue() const;
    void SetValueInternal(float value, bool notify);
    bool UpdateFromPoint(const POINT &point, bool notify);

    float minValue_ = 0.0f;
    float maxValue_ = 100.0f;
    float value_ = 0.0f;
    float preferredHeight_ = 34.0f;
    bool focused_ = false;
    bool dragging_ = false;
    ChangeHandler onChanged_;
};

class Separator : public Visual
{
  public:
    explicit Separator(float height = 1.0f);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;

  private:
    float height_ = 1.0f;
};

class ListView : public Visual
{
  public:
    struct Item
    {
        std::wstring title;
        std::wstring subtitle;
        std::wstring badge;
    };

    using SelectionChangedHandler = std::function<void(size_t selectedIndex)>;

    explicit ListView(float itemHeight = 68.0f);

    void AddItem(Item item);
    void ClearItems();
    void SetOnSelectionChanged(SelectionChangedHandler handler);
    void SetSelectedIndex(size_t index);
    size_t GetSelectedIndex() const;

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    struct ItemLayoutCache
    {
        float titleWidth = -1.0f;
        float subtitleWidth = -1.0f;
        float badgeWidth = -1.0f;
        std::wstring fontFamily;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> titleLayout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> subtitleLayout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> badgeLayout;
    };

    void InvalidateLayoutCache();
    size_t HitTestItem(const PointF &point) const;

    std::vector<Item> items_;
    std::vector<ItemLayoutCache> layoutCache_;
    float itemHeight_ = 68.0f;
    bool focused_ = false;
    bool pressed_ = false;
    size_t pressedIndex_ = static_cast<size_t>(-1);
    size_t selectedIndex_ = 0;
    SelectionChangedHandler onSelectionChanged_;
};

class CandidateList : public Visual
{
  public:
    struct Item
    {
        std::wstring label;
        std::wstring text;
        std::wstring annotation;
        std::wstring translation;
    };

    enum class Orientation
    {
        Vertical,
        Horizontal,
    };

    struct Appearance
    {
        std::wstring fontFamily;
        std::vector<std::wstring> fallbackFontFamilies;
        float itemHeight = 28.0f;
        float itemGap = 2.0f;
        float fontSize = 16.0f;
        float labelFontSize = 12.8f;
        float annotationFontSize = 16.0f;
        float cornerRadius = 4.0f;
        // Radius for item corners that sit on the list's own corners, so a
        // fill inside an inset rounded frame follows the frame's R there.
        // <= 0 keeps cornerRadius on every corner. outerTopCornersEnabled
        // false leaves the top corners alone when something (e.g. a preedit
        // row) sits between the list and the frame's top edge.
        float outerCornerRadius = 0.0f;
        bool outerTopCornersEnabled = true;
        float contentPadLeft = 5.0f;
        float contentPadRight = 5.0f;
        float textPadLeft = 0.0f;
        float labelGap = 1.5f;
        float selectedBarWidth = 3.0f;
        float selectedBarHeight = 12.8f;
        bool showSelectedBar = true;
        D2D1_COLOR_F rowFillHover = D2D1::ColorF(0x343434);
        D2D1_COLOR_F rowFillPressed = D2D1::ColorF(0x353535);
        D2D1_COLOR_F rowFillSelected = D2D1::ColorF(0x3E3E3E, 0.725f);
        // Text/label colors for the selected (and pressed) row. Alpha 0 keeps
        // the normal textColor/labelColor; skins that fill the selected row
        // with an opaque accent set these to the contrasting color (e.g. white
        // on the WeChat green first row). rowTextSelected also drives the
        // annotation (辅助码) and translation on that row, matching the CSS
        // where both inherit the color of `.first .text`.
        D2D1_COLOR_F rowTextSelected = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F rowLabelSelected = D2D1::ColorF(0, 0.0f);
        D2D1_COLOR_F selectedBarColor = D2D1::ColorF(0x6B69D6);
        D2D1_COLOR_F labelColor = D2D1::ColorF(0xE9E8E8, 0.616f);
        D2D1_COLOR_F textColor = D2D1::ColorF(0xE9E8E8);
        D2D1_COLOR_F annotationColor = D2D1::ColorF(0xE9E8E8);
        // Alpha 0 derives the translation color from the annotation color
        // (including the selected-row override) at 62% opacity, like the
        // CSS default. Otherwise it is drawn as-is on every row.
        D2D1_COLOR_F translationColor = D2D1::ColorF(0, 0.0f);
    };

    using SelectionChangedHandler = std::function<void(size_t selectedIndex)>;
    using ItemActivatedHandler = std::function<void(size_t selectedIndex)>;
    using ContextMenuHandler = std::function<void(size_t selectedIndex, const POINT &clientPoint)>;

    explicit CandidateList(float itemHeight = 40.0f);

    void AddItem(Item item);
    void SetItems(std::vector<Item> items);
    void ClearItems();
    void SetSelectedIndex(size_t index);
    size_t GetSelectedIndex() const;
    const Item *GetItem(size_t index) const;
    // 候选项相对窗口的最终矩形（含铺满后的行宽），供命中测试与布局校验使用。
    RectF GetItemBounds(size_t index) const;
    void SetOnSelectionChanged(SelectionChangedHandler handler);
    void SetOnItemActivated(ItemActivatedHandler handler);
    void SetOnContextMenu(ContextMenuHandler handler);
    void SetAppearance(Appearance appearance);
    void SetOrientation(Orientation orientation);
    void SetHoverEnabled(bool enabled);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    bool OnMouseMove(const POINT &point, WPARAM keyState) override;
    bool OnContextMenu(const POINT &point, WPARAM keyState) override;
    void OnMouseLeave() override;
    bool OnKeyDown(WPARAM key, LPARAM lParam) override;
    HCURSOR GetCursor() const override;

  private:
    struct ItemLayoutCache
    {
        float labelWidth = -1.0f;
        float textWidth = -1.0f;
        float annotationWidth = -1.0f;
        float translationWidth = -1.0f;
        std::wstring fontFamily;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> labelLayout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> annotationLayout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> translationLayout;
    };

    // 均使用逻辑单位：bounds 相对列表，文字矩形相对候选项；测量、绘制与命中测试共用。
    struct ItemGeometry
    {
        RectF bounds;
        RectF label;
        RectF text;
        RectF annotation;
        RectF translation;
    };

    float MeasureTextHeight(const std::wstring &text, float fontSize, float width) const;
    ItemGeometry MeasureItem(size_t index, float width) const;
    void InvalidateLayoutCache();
    size_t HitTestItem(const PointF &point) const;
    float EstimateTextWidth(const std::wstring &text, float fontSize) const;
    RectF ItemRect(size_t index) const;

    std::vector<Item> items_;
    std::vector<ItemLayoutCache> layoutCache_;
    std::vector<ItemGeometry> itemGeometry_;
    float layoutWidth_ = 0.0f;
    Appearance appearance_{};
    Orientation orientation_ = Orientation::Vertical;
    bool focused_ = false;
    bool pressed_ = false;
    bool hoverEnabled_ = true;
    size_t pressedIndex_ = static_cast<size_t>(-1);
    size_t hoveredIndex_ = static_cast<size_t>(-1);
    size_t selectedIndex_ = 0;
    SelectionChangedHandler onSelectionChanged_;
    ItemActivatedHandler onItemActivated_;
    ContextMenuHandler onContextMenu_;
};

class TreeView : public Visual
{
  public:
    struct Node
    {
        std::wstring title;
        std::wstring subtitle;
        bool expanded = true;
        std::vector<Node> children;
    };

    using SelectionChangedHandler = std::function<void(const std::wstring &selectedTitle)>;

    explicit TreeView(float itemHeight = 62.0f);

    void AddRoot(Node node);
    void Clear();
    void SetOnSelectionChanged(SelectionChangedHandler handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    bool HitTest(const PointF &point) const override;
    bool IsFocusable() const override;
    void OnFocusChanged(bool focused) override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    struct VisibleNode
    {
        Node *node = nullptr;
        size_t depth = 0;
        RectF rowRect = {};
        RectF expanderRect = {};
        float titleWidth = -1.0f;
        float subtitleWidth = -1.0f;
        std::wstring fontFamily;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> titleLayout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> subtitleLayout;
    };

    void BuildVisibleNodes();
    void AppendVisibleNodes(Node &node, size_t depth);
    void InvalidateLayoutCache();
    VisibleNode *HitTestVisibleNode(const PointF &point);
    const VisibleNode *HitTestVisibleNode(const PointF &point) const;
    void SelectNode(Node *node);

    std::vector<Node> roots_;
    std::vector<VisibleNode> visibleNodes_;
    float itemHeight_ = 62.0f;
    bool focused_ = false;
    bool pressed_ = false;
    Node *pressedNode_ = nullptr;
    bool pressedExpander_ = false;
    Node *selectedNode_ = nullptr;
    SelectionChangedHandler onSelectionChanged_;
};

class TabControl : public Visual
{
  public:
    struct Tab
    {
        std::wstring title;
        std::shared_ptr<Visual> content;
    };

    using SelectionChangedHandler = std::function<void(size_t selectedIndex)>;

    explicit TabControl(float headerHeight = 46.0f);

    void AddTab(std::wstring title, std::shared_ptr<Visual> content);
    void ClearTabs();
    void SetSelectedIndex(size_t index);
    size_t GetSelectedIndex() const;
    void SetOnSelectionChanged(SelectionChangedHandler handler);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool HitTest(const PointF &point) const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    size_t HitTestHeader(const PointF &point) const;

    std::vector<Tab> tabs_;
    std::vector<RectF> headerRects_;
    float headerHeight_ = 46.0f;
    size_t selectedIndex_ = 0;
    bool pressed_ = false;
    size_t pressedIndex_ = static_cast<size_t>(-1);
    SelectionChangedHandler onSelectionChanged_;
};

class Accordion : public Visual
{
  public:
    struct Section
    {
        std::wstring title;
        std::shared_ptr<Visual> content;
        bool expanded = true;
    };

    explicit Accordion(float headerHeight = 48.0f);

    void AddSection(std::wstring title, std::shared_ptr<Visual> content, bool expanded = true);
    void ClearSections();
    void SetAllowMultipleExpanded(bool allowMultipleExpanded);

    SizeF Measure(const SizeF &availableSize) override;
    void Arrange(const RectF &finalRect) override;
    void Render(DeviceResources &deviceResources) override;
    void Attach(Window *window) override;
    Visual *FindVisualAt(const PointF &point) override;
    Visual *FindFocusableAt(const PointF &point) override;
    Visual *FindFirstFocusableDescendant() override;
    bool HitTest(const PointF &point) const override;
    bool OnMouseDown(const POINT &point, WPARAM keyState) override;
    bool OnMouseUp(const POINT &point, WPARAM keyState) override;
    HCURSOR GetCursor() const override;

  private:
    void CollapseOtherSections(size_t keepExpandedIndex);
    size_t HitTestHeader(const PointF &point) const;

    std::vector<Section> sections_;
    std::vector<RectF> headerRects_;
    std::vector<RectF> contentRects_;
    float headerHeight_ = 48.0f;
    bool allowMultipleExpanded_ = true;
    bool pressed_ = false;
    size_t pressedIndex_ = static_cast<size_t>(-1);
};
} // namespace msimeui
