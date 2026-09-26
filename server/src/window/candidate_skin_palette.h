#pragma once

#include "skin/candidate_skin_catalog.h"

#include <d2d1.h>
#include <string>
#include <windows.h>

struct CandidateSkinPalette
{
    D2D1_COLOR_F surface;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
};

D2D1_COLOR_F CandidateColorFromRgb(UINT rgb, float alpha = 1.0f);
D2D1_COLOR_F ParseCandidateCssColor(const std::string &text, D2D1_COLOR_F fallback);
CandidateSkinPalette ResolveCandidateSkinPalette(const std::string &skinId, bool light,
                                                 const std::string &configuredTextColor,
                                                 const CandidateSkinCatalog::CandidateColors *packageColors = nullptr,
                                                 const std::string &baseSkinId = {});
CandidateSkinPalette FlattenCandidateSkinPaletteForGdi(const CandidateSkinPalette &palette,
                                                       D2D1_COLOR_F fallbackSurface);
COLORREF FlattenCandidateColor(D2D1_COLOR_F color, D2D1_COLOR_F background);
