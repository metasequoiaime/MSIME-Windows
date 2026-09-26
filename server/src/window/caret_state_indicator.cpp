#include "window/caret_state_indicator.h"
#include "config/ime_config.h"
#include "skin/candidate_skin_catalog.h"
#include "utils/common_utils.h"
#include "utils/window_utils.h"
#include "webview2/windows_webview2.h"
#include "window/candidate_skin_palette.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>

namespace
{
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT kHideDelayMs = 1500;
constexpr int kCaretGapDip = 6;
constexpr int kCaretLineHeightDip = 24;
constexpr int kFontSizeDip = 20;

struct PaletteCache
{
    std::string skinId;
    std::string textColor;
    bool light = false;
    uint64_t revision = 0;
    CandidateSkinPalette palette{};
    bool valid = false;
};

PaletteCache g_paletteCache;

// Mirrors CandidatePresenter::ApplySkin so the badge matches the candidate
// window. Called from Show only: Paint must stay free of disk I/O.
void RefreshPalette()
{
    const bool light = ResolveConfiguredTheme(GetConfiguredThemeCand()) == "light";
    const std::string skinId = GetConfiguredCandidateSkin();
    const std::string textColor = GetConfiguredCandidateTextColor();
    const uint64_t revision = GetCandidateSkinReloadRevision();
    if (g_paletteCache.valid && g_paletteCache.skinId == skinId && g_paletteCache.textColor == textColor &&
        g_paletteCache.light == light && g_paletteCache.revision == revision)
        return;

    std::optional<CandidateSkinCatalog::Package> package;
    if (!CandidateSkinCatalog::IsBuiltIn(skinId))
        package =
            CandidateSkinCatalog::Load(std::filesystem::path(CommonUtils::get_ime_data_path_w()) / L"skins", skinId);
    const CandidateSkinCatalog::CandidateColors *packageColors =
        package ? &(light ? package->light : package->dark) : nullptr;
    const std::string baseSkinId = package ? package->base : std::string{};
    const CandidateSkinPalette resolved =
        ResolveCandidateSkinPalette(skinId, light, textColor, packageColors, baseSkinId);
    // GDI cannot blend with what is behind the window, so a translucent skin
    // surface is flattened onto its opaque built-in base.
    const CandidateSkinPalette opaqueBase =
        ResolveCandidateSkinPalette(baseSkinId.empty() ? skinId : baseSkinId, light, textColor);
    g_paletteCache = {
        skinId, textColor, light, revision, FlattenCandidateSkinPaletteForGdi(resolved, opaqueBase.surface), true};
}

struct State
{
    FanyImeUi::CaretStateBadge badge;
    UINT dpi = 96;
};

State g_state;

int PixelSize(int dip, UINT dpi)
{
    return dip == 0 ? 0 : (std::max)(1, static_cast<int>(std::lround(dip * static_cast<double>(dpi) / 96.0)));
}

bool PositionWindow(HWND hwnd, POINT caret, bool topmost, bool show)
{
    const float scale = GetScaleForPoint(caret);
    g_state.dpi = static_cast<UINT>(std::lround((scale > 0.0f ? scale : 1.0f) * 96.0f));
    const int height = PixelSize(FanyImeUi::kCaretStateBadgeHeightDip, g_state.dpi);
    const int width = g_state.badge.HasModeSlot()
                          ? PixelSize(FanyImeUi::kCaretStatePunctuationSlotWidthDip, g_state.dpi) +
                                PixelSize(FanyImeUi::kCaretStatePunctuationModeGapDip, g_state.dpi) +
                                PixelSize(FanyImeUi::kCaretStatePunctuationModeSlotWidthDip, g_state.dpi)
                          : PixelSize(FanyImeUi::CaretStateBadgeWidthDip(g_state.badge), g_state.dpi);
    const int gap = PixelSize(kCaretGapDip, g_state.dpi);
    const int caretLineHeight = PixelSize(kCaretLineHeightDip, g_state.dpi);

    RECT work{};
    MONITORINFO info{sizeof(info)};
    const HMONITOR monitor = MonitorFromPoint(caret, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &info))
        work = info.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    const std::string &position = GetConfiguredCaretStateIndicatorPosition();
    const std::optional<int> y = FanyImeUi::CaretStateIndicatorPlacementY(position == "bottom", caret.y, height,
                                                                          caretLineHeight, gap, work.top, work.bottom);
    if (!y)
        return false;
    const int preferredX = FanyImeUi::CaretStateIndicatorX(position, caret.x, width, gap);
    const int x = (std::max)(static_cast<int>(work.left), (std::min)(preferredX, static_cast<int>(work.right) - width));
    const UINT flags = SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0);
    return SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_TOP, x, *y, width, height, flags) != FALSE;
}

void DrawSlot(HDC dc, const wchar_t *text, int length, RECT rect)
{
    DrawTextW(dc, text, length, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}
} // namespace

namespace CaretStateIndicator
{
bool Show(HWND hwnd, const FanyImeUi::CaretStateBadge &badge, POINT caret, bool topmost)
{
    if (!hwnd || !IsWindow(hwnd) || badge.text.empty())
        return false;

    g_state.badge = badge;
    RefreshPalette();
    if (!PositionWindow(hwnd, caret, topmost, true))
    {
        Hide(hwnd);
        return false;
    }
    // Re-arming restarts the countdown when switches arrive in quick succession.
    SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

bool Reposition(HWND hwnd, POINT caret, bool topmost)
{
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || !PositionWindow(hwnd, caret, topmost, false))
        return false;
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

void Hide(HWND hwnd)
{
    if (!hwnd)
        return;
    KillTimer(hwnd, kHideTimer);
    ShowWindow(hwnd, SW_HIDE);
}

bool HandleTimer(HWND hwnd, WPARAM timerId)
{
    if (timerId != kHideTimer)
        return false;
    Hide(hwnd);
    return true;
}

void Paint(HWND hwnd, HDC dc)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const CandidateSkinPalette palette =
        g_paletteCache.valid ? g_paletteCache.palette : ResolveCandidateSkinPalette("fluent", false, "auto");
    HBRUSH background = CreateSolidBrush(FlattenCandidateColor(palette.surface, palette.surface));
    FillRect(dc, &rc, background);
    DeleteObject(background);
    HBRUSH border = CreateSolidBrush(FlattenCandidateColor(palette.border, palette.surface));
    FrameRect(dc, &rc, border);
    DeleteObject(border);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, FlattenCandidateColor(palette.text, palette.surface));
    HFONT font =
        CreateFontW(-PixelSize(kFontSizeDip, g_state.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    const HGDIOBJ previousFont = font ? SelectObject(dc, font) : nullptr;

    const FanyImeUi::CaretStateBadge &badge = g_state.badge;
    if (badge.HasModeSlot())
    {
        RECT modeRect = rc;
        modeRect.left =
            (std::max)(rc.left, rc.right - PixelSize(FanyImeUi::kCaretStatePunctuationModeSlotWidthDip, g_state.dpi));
        RECT textRect = rc;
        textRect.right =
            (std::max)(rc.left, modeRect.left - PixelSize(FanyImeUi::kCaretStatePunctuationModeGapDip, g_state.dpi));
        DrawSlot(dc, badge.text.c_str(), static_cast<int>(badge.text.size()), textRect);
        DrawSlot(dc, &badge.mode, 1, modeRect);
    }
    else
    {
        DrawSlot(dc, badge.text.c_str(), static_cast<int>(badge.text.size()), rc);
    }

    if (previousFont)
        SelectObject(dc, previousFont);
    if (font)
        DeleteObject(font);
}
} // namespace CaretStateIndicator
