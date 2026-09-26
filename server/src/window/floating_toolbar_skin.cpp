#include "window/floating_toolbar_skin.h"

#include "window/candidate_skin_palette.h"

FloatingToolbarSkin ResolveFloatingToolbarSkin(const std::string &skinId, bool light)
{
    // default(_light).html: the base every skin page starts from.
    FloatingToolbarSkin skin = light ? FloatingToolbarSkin{CandidateColorFromRgb(0xFFFFFF),
                                                           D2D1::ColorF(0, 0.12f),
                                                           CandidateColorFromRgb(0x1A1A1A),
                                                           D2D1::ColorF(0, 0.08f),
                                                           D2D1::ColorF(0, 0.12f),
                                                           CandidateColorFromRgb(0x8E8CD8),
                                                           8.0f,
                                                           6.0f}
                                     : FloatingToolbarSkin{CandidateColorFromRgb(0x1A1A1A),
                                                           D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f),
                                                           CandidateColorFromRgb(0xFFFFFF),
                                                           D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f),
                                                           D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f),
                                                           CandidateColorFromRgb(0x8E8CD8),
                                                           8.0f,
                                                           6.0f};
    if (skinId == "wechat")
    {
        skin.fill = CandidateColorFromRgb(light ? 0xF7F7F7 : 0x151515);
        skin.border = CandidateColorFromRgb(light ? 0xDEDEDE : 0x292929);
        skin.handle = CandidateColorFromRgb(0x07C160);
        skin.hover = CandidateColorFromRgb(0x07C160, light ? 0.12f : 0.16f);
    }
    else if (skinId == "graphite")
    {
        skin.fill = CandidateColorFromRgb(light ? 0xFBFBFC : 0x1C1F23);
        skin.border = CandidateColorFromRgb(light ? 0xE2E5E9 : 0x30353B);
        skin.glyph = CandidateColorFromRgb(light ? 0x374151 : 0xD7DCE2);
        skin.handle = CandidateColorFromRgb(light ? 0x5F6B7A : 0x8993A0);
        skin.divider = light ? CandidateColorFromRgb(0x1F2937, 0.10f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
        skin.hover = light ? CandidateColorFromRgb(0x1F2937, 0.055f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f);
        skin.radius = 4.0f;
        skin.iconRadius = 3.0f;
    }
    else if (skinId == "willow_green")
    {
        skin.fill = CandidateColorFromRgb(light ? 0xF4F5F3 : 0x2D2F2E);
        skin.border = CandidateColorFromRgb(light ? 0xDFE3DF : 0x3B3E3C);
        skin.handle = CandidateColorFromRgb(light ? 0x58B980 : 0x65C98D);
        skin.hover = light ? CandidateColorFromRgb(0x58B980, 0.16f) : CandidateColorFromRgb(0x65C98D, 0.20f);
        skin.radius = 9.0f;
    }
    else if (skinId == "autumn_osmanthus")
    {
        skin.fill = CandidateColorFromRgb(light ? 0xD6ECF0 : 0x7D929F);
        skin.border = CandidateColorFromRgb(light ? 0xBCD8DE : 0x8FA3AF);
        skin.handle = CandidateColorFromRgb(light ? 0xE6A817 : 0xF97D0A);
        skin.hover = light ? CandidateColorFromRgb(0xFFE399, 0.70f) : CandidateColorFromRgb(0xF97D0A, 0.28f);
        skin.radius = 10.0f;
    }
    return skin;
}
