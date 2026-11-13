#include "Horo/Debug/OverlayLayout.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Horo::Debug;

TEST_CASE("Console overlay registration layout visibility and focus remain bounded and permission filtered", "[console]") {
    const std::array descriptors{OverlayDescriptor{1, "logs", OverlaySource::Logs}, OverlayDescriptor{2, "jobs", OverlaySource::Jobs}};
    auto created = OverlayLayout::Create(descriptors, {});
    REQUIRE(created.HasValue());
    auto layout = std::move(created).Value();
    const OverlayBounds bounds{0.1F, 0.1F, 0.3F, 0.3F};
    CHECK(layout.Configure(1, bounds, true, {}).HasError());
    REQUIRE(layout.Configure(1, bounds, true, {3}).HasValue());
    REQUIRE(layout.Configure(2, bounds, true, {3}).HasValue());
    REQUIRE(layout.SetVisible(1, false, {3}).HasValue());
    REQUIRE(layout.SetVisible(1, true, {3}).HasValue());
    std::array<OverlayPanelSnapshot, 32> snapshot{};
    auto visible = layout.Snapshot({3}, snapshot);
    REQUIRE(visible.HasValue());
    CHECK(visible.Value() == 2);
    CHECK(snapshot[0].bounds.x == bounds.x);
    CHECK(snapshot[0].bounds.width == bounds.width);
    CHECK(layout.Snapshot({}, snapshot).Value() == 0);
    REQUIRE(layout.Focus(1, {3}).HasValue());
    REQUIRE(layout.Snapshot({3}, snapshot).HasValue());
    CHECK(snapshot[0].focusOrder == 1);
    CHECK(snapshot[1].focusOrder == 0);
    for (const auto invalid : {OverlayBounds{-1, 0, 1, 1}, OverlayBounds{0, 0, 0, 1}, OverlayBounds{0, 0, 2, 1},
                               OverlayBounds{std::numeric_limits<float>::quiet_NaN(), 0, 1, 1}})
        CHECK(layout.Configure(1, invalid, true, {3}).HasError());
    CHECK(layout.Snapshot({3}, {}).HasError());
    REQUIRE(layout.Configure(1, bounds, false, {3}).HasValue());
    CHECK(layout.Focus(1, {3}).HasError());
    CHECK(layout.Snapshot({3}, snapshot).Value() == 1);
    const std::array duplicate{descriptors[0], descriptors[0]};
    CHECK(OverlayLayout::Create(duplicate, {}).HasError());
    for (const auto profile : {ProductProfile::Shipping, ProductProfile::Server}) {
        RegistryPolicy policy;
        policy.profile = profile;
        CHECK(OverlayLayout::Create(descriptors, policy).HasError());
    }
}

TEST_CASE("Scene overlay metadata cannot broaden its development-only availability", "[console][profile]") {
    std::array descriptors{OverlayDescriptor{3, "scene", OverlaySource::Scene}};
    CHECK(OverlayLayout::Create(descriptors, {}).HasError());
    descriptors[0].availability = CommandAvailability::DevelopmentOnly;
    for (const auto profile : {ProductProfile::Editor, ProductProfile::Development, ProductProfile::Profile, ProductProfile::Diagnostics}) {
        RegistryPolicy policy;
        policy.profile = profile;
        auto created = OverlayLayout::Create(descriptors, policy);
        REQUIRE(created.HasValue());
        auto layout = std::move(created).Value();
        const bool development = profile == ProductProfile::Editor || profile == ProductProfile::Development;
        CHECK(layout.Configure(3, {}, true, {3}).HasValue() == development);
        std::array<OverlayPanelSnapshot, 1> snapshot{};
        CHECK(layout.Snapshot({3}, snapshot).Value() == (development ? 1U : 0U));
    }
}
