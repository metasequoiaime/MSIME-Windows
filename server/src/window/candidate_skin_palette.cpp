#include "window/candidate_skin_palette.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace
{
std::string TrimCopy(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

BYTE ColorByte(float value)
{
    return static_cast<BYTE>(std::lround((std::clamp)(value, 0.0f, 1.0f) * 255.0f));
}

D2D1_COLOR_F CompositeColor(D2D1_COLOR_F color, D2D1_COLOR_F background)
{
    const float alpha = (std::clamp)(color.a, 0.0f, 1.0f);
    return D2D1::ColorF(color.r * alpha + background.r * (1.0f - alpha),
                        color.g * alpha + background.g * (1.0f - alpha),
                        color.b * alpha + background.b * (1.0f - alpha), 1.0f);
}
} // namespace

D2D1_COLOR_F CandidateColorFromRgb(UINT rgb, float alpha)
{
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
}

D2D1_COLOR_F ParseCandidateCssColor(const std::string &text, D2D1_COLOR_F fallback)
{
    std::string value = TrimCopy(text);
    if (value.empty() || value == "auto" || value == "none" || value == "transparent")
        return value == "transparent" ? D2D1::ColorF(0, 0.0f) : fallback;
    if (value.rfind("rgba(", 0) == 0 || value.rfind("rgb(", 0) == 0)
    {
        const auto open = value.find('(');
        const auto close = value.rfind(')');
        if (open != std::string::npos && close != std::string::npos && close > open)
        {
            std::string inner = value.substr(open + 1, close - open - 1);
            std::replace(inner.begin(), inner.end(), ',', ' ');
            std::istringstream stream(inner);
            float r = 0;
            float g = 0;
            float b = 0;
            float a = 1.0f;
            if (stream >> r >> g >> b)
            {
                stream >> a;
                return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
            }
        }
        return fallback;
    }
    if (value[0] == '#')
        value.erase(value.begin());
    try
    {
        if (value.size() == 3)
        {
            const auto hexByte = [](char digit) {
                return static_cast<int>(std::stoul(std::string(2, digit), nullptr, 16));
            };
            return D2D1::ColorF(hexByte(value[0]) / 255.0f, hexByte(value[1]) / 255.0f, hexByte(value[2]) / 255.0f,
                                1.0f);
        }
        if (value.size() == 6)
            return CandidateColorFromRgb(static_cast<UINT>(std::stoul(value, nullptr, 16)));
        if (value.size() == 8)
        {
            const unsigned long packed = std::stoul(value, nullptr, 16);
            return CandidateColorFromRgb(static_cast<UINT>((packed >> 8) & 0xFFFFFFu),
                                         static_cast<float>(packed & 0xFFu) / 255.0f);
        }
    }
    catch (...)
    {
    }
    return fallback;
}

CandidateSkinPalette ResolveCandidateSkinPalette(const std::string &skinId, bool light,
                                                 const std::string &configuredTextColor,
                                                 const CandidateSkinCatalog::CandidateColors *packageColors,
                                                 const std::string &baseSkinId)
{
    const std::string &paletteSkinId = baseSkinId.empty() ? skinId : baseSkinId;
    CandidateSkinPalette palette{
        CandidateColorFromRgb(light ? 0xFFFFFF : 0x202020),
        light ? D2D1::ColorF(0, 0.12f) : ParseCandidateCssColor("#9b9b9b2e", CandidateColorFromRgb(0x3A3A3A, 0.18f)),
        CandidateColorFromRgb(light ? 0x1A1A1A : 0xE9E8E8),
    };
    if (paletteSkinId == "wechat")
    {
        palette = light ? CandidateSkinPalette{CandidateColorFromRgb(0xF7F7F7), CandidateColorFromRgb(0xDEDEDE),
                                               CandidateColorFromRgb(0x333333)}
                        : CandidateSkinPalette{CandidateColorFromRgb(0x151515), CandidateColorFromRgb(0x292929),
                                               CandidateColorFromRgb(0xB7B7B7)};
    }
    else if (paletteSkinId == "willow_green")
    {
        palette.surface = CandidateColorFromRgb(light ? 0xF4F5F3 : 0x2D2F2E);
        palette.border = D2D1::ColorF(0, 0.0f);
        palette.text = CandidateColorFromRgb(light ? 0x343936 : 0xD8DBD8);
    }
    else if (paletteSkinId == "graphite")
    {
        palette = light ? CandidateSkinPalette{CandidateColorFromRgb(0xFBFBFC), CandidateColorFromRgb(0xE2E5E9),
                                               CandidateColorFromRgb(0x586476)}
                        : CandidateSkinPalette{CandidateColorFromRgb(0x1C1F23), CandidateColorFromRgb(0x30353B),
                                               CandidateColorFromRgb(0xAEB6C2)};
    }
    if (packageColors)
    {
        if (!packageColors->surface.empty())
            palette.surface = ParseCandidateCssColor(packageColors->surface, palette.surface);
        if (!packageColors->border.empty())
            palette.border = ParseCandidateCssColor(packageColors->border, palette.border);
        if (!packageColors->text.empty())
            palette.text = ParseCandidateCssColor(packageColors->text, palette.text);
    }
    palette.text = ParseCandidateCssColor(configuredTextColor, palette.text);
    return palette;
}

CandidateSkinPalette FlattenCandidateSkinPaletteForGdi(const CandidateSkinPalette &palette,
                                                       D2D1_COLOR_F fallbackSurface)
{
    const D2D1_COLOR_F surface = CompositeColor(palette.surface, fallbackSurface);
    return {surface, CompositeColor(palette.border, surface), CompositeColor(palette.text, surface)};
}

COLORREF FlattenCandidateColor(D2D1_COLOR_F color, D2D1_COLOR_F background)
{
    const D2D1_COLOR_F flattened = CompositeColor(color, background);
    return RGB(ColorByte(flattened.r), ColorByte(flattened.g), ColorByte(flattened.b));
}
