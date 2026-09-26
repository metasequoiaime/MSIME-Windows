#include "tests/includes/test_framework.h"
#include "window/candidate_skin_palette.h"
#include "window/floating_toolbar_skin.h"

TEST_CASE(floating_toolbar_skin_defaults_to_fluent_toolbar)
{
    const FloatingToolbarSkin dark = ResolveFloatingToolbarSkin("fluent", false);
    REQUIRE_EQ(FlattenCandidateColor(dark.fill, dark.fill), RGB(26, 26, 26));
    REQUIRE_EQ(FlattenCandidateColor(dark.handle, dark.fill), RGB(142, 140, 216));
    REQUIRE_EQ(dark.radius, 8.0f);

    const FloatingToolbarSkin unknown = ResolveFloatingToolbarSkin("no_such_skin", true);
    REQUIRE_EQ(FlattenCandidateColor(unknown.fill, unknown.fill), RGB(255, 255, 255));
    REQUIRE_EQ(FlattenCandidateColor(unknown.glyph, unknown.fill), RGB(26, 26, 26));
}

TEST_CASE(floating_toolbar_skin_follows_builtin_skin_pages)
{
    const FloatingToolbarSkin wechat = ResolveFloatingToolbarSkin("wechat", false);
    REQUIRE_EQ(FlattenCandidateColor(wechat.fill, wechat.fill), RGB(21, 21, 21));
    REQUIRE_EQ(FlattenCandidateColor(wechat.handle, wechat.fill), RGB(7, 193, 96));

    const FloatingToolbarSkin graphite = ResolveFloatingToolbarSkin("graphite", true);
    REQUIRE_EQ(FlattenCandidateColor(graphite.fill, graphite.fill), RGB(251, 251, 252));
    REQUIRE_EQ(FlattenCandidateColor(graphite.glyph, graphite.fill), RGB(55, 65, 81));
    REQUIRE_EQ(graphite.radius, 4.0f);
    REQUIRE_EQ(graphite.iconRadius, 3.0f);

    const FloatingToolbarSkin willow = ResolveFloatingToolbarSkin("willow_green", false);
    REQUIRE_EQ(FlattenCandidateColor(willow.fill, willow.fill), RGB(45, 47, 46));
    REQUIRE_EQ(FlattenCandidateColor(willow.handle, willow.fill), RGB(101, 201, 141));
    REQUIRE_EQ(willow.radius, 9.0f);

    const FloatingToolbarSkin autumnDark = ResolveFloatingToolbarSkin("autumn_osmanthus", false);
    REQUIRE_EQ(FlattenCandidateColor(autumnDark.fill, autumnDark.fill), RGB(125, 146, 159));
    REQUIRE_EQ(FlattenCandidateColor(autumnDark.handle, autumnDark.fill), RGB(249, 125, 10));
    REQUIRE_EQ(autumnDark.radius, 10.0f);

    const FloatingToolbarSkin autumnLight = ResolveFloatingToolbarSkin("autumn_osmanthus", true);
    REQUIRE_EQ(FlattenCandidateColor(autumnLight.fill, autumnLight.fill), RGB(214, 236, 240));
    REQUIRE_EQ(FlattenCandidateColor(autumnLight.glyph, autumnLight.fill), RGB(26, 26, 26));
}
