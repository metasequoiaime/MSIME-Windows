#pragma once

#include <d2d1.h>
#include <string>

// Colors and corner radii of the D2D floating toolbar. Mirrors the standalone
// WebView2 toolbar pages in ui-html/webview2/ftb/<skin>(_light).html, which are
// the reference for both renderers. Radii are in DIPs before the user scale.
struct FloatingToolbarSkin
{
    D2D1_COLOR_F fill;
    D2D1_COLOR_F border;
    D2D1_COLOR_F glyph;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F divider;
    D2D1_COLOR_F handle;
    float radius;
    float iconRadius;
};

// skinId is a built-in skin id; external skins pass their base skin, because
// their toolbar.css overrides only reach the WebView2 renderer.
FloatingToolbarSkin ResolveFloatingToolbarSkin(const std::string &skinId, bool light);
