#include "PostProcessTestSupport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Test;
}  // namespace

TEST_CASE("Post-process volumes resolve profiles and explicit overrides with owned snapshot storage", "[runtime][renderer][post-process]") {
    PostProcessSettings base;
    base.bloom.emplace();
    std::array profiles{PostProcessProfile{{1}, base}};
    profiles[0].settings.bloom->intensity = 4;
    std::array volumes{PostProcessVolume{.id = 1, .profile = PostProcessProfileId{1}}};
    volumes[0].overrides.motionBlur.mode = PostProcessOverrideMode::Replace;
    auto prepared = PreparePostProcessVolumes(3, base, profiles, volumes);
    REQUIRE(prepared.HasValue());
    profiles[0].settings.bloom->intensity = 9;
    volumes[0].overrides.motionBlur.mode = PostProcessOverrideMode::Disable;
    auto resolved = prepared.Value().Evaluate({});
    REQUIRE(resolved.HasValue());
    CHECK(resolved.Value().bloom->intensity == 4);
    CHECK(resolved.Value().motionBlur.has_value());
    CHECK(prepared.Value().Generation() == 3);
    std::optional<PostProcessSettings> workerSettings;
    std::thread worker{[&] {
        workerSettings = prepared.Value().Evaluate({}).Value();
    }};
    worker.join();
    CHECK(workerSettings == resolved.Value());
}

TEST_CASE("Volume hard cuts include boundaries and positive radii smoothly blend outside finite bounds",
          "[runtime][renderer][post-process]") {
    const bool sphere = GENERATE(false, true);
    PostProcessSettings base;
    base.bloom.emplace();
    PostProcessVolume volume{.id = 1, .blendRadius = 2};
    if (sphere)
        volume.bounds = Math::BoundingSphere{{}, 1};
    else
        volume.bounds = Math::Aabb{{-1, -1, -1}, {1, 1, 1}};
    volume.overrides.bloom = {PostProcessOverrideMode::Replace, BloomSettings{.intensity = 3}};
    auto prepared = PreparePostProcessVolumes(1, base, {}, std::span{&volume, 1});
    REQUIRE(prepared.HasValue());
    CHECK(prepared.Value().Evaluate({1, 0, 0}).Value().bloom->intensity == 3);
    CHECK(prepared.Value().Evaluate({2, 0, 0}).Value().bloom->intensity == Catch::Approx(2));
    CHECK(prepared.Value().Evaluate({3, 0, 0}).Value().bloom->intensity == 1);
    volume.blendRadius = 0;
    auto hard = PreparePostProcessVolumes(2, base, {}, std::span{&volume, 1});
    REQUIRE(hard.HasValue());
    CHECK(hard.Value().Evaluate({1, 0, 0}).Value().bloom->intensity == 3);
    CHECK(hard.Value().Evaluate({1.01F, 0, 0}).Value().bloom->intensity == 1);
}

TEST_CASE("Volume priority and stable ID ordering are deterministic and disables differ from inheritance",
          "[runtime][renderer][post-process]") {
    PostProcessSettings base;
    base.bloom.emplace();
    std::array volumes{PostProcessVolume{.id = 2, .priority = 1}, PostProcessVolume{.id = 1, .priority = 0}};
    volumes[0].overrides.bloom.mode = PostProcessOverrideMode::Disable;
    volumes[1].overrides.bloom = {PostProcessOverrideMode::Replace, BloomSettings{.intensity = 8}};
    auto disabled = PreparePostProcessVolumes(1, base, {}, volumes);
    REQUIRE(disabled.HasValue());
    CHECK_FALSE(disabled.Value().Evaluate({}).Value().bloom.has_value());
    volumes[0].overrides.bloom.mode = PostProcessOverrideMode::Inherit;
    volumes[0].priority = 0;
    auto inherited = PreparePostProcessVolumes(2, base, {}, volumes);
    REQUIRE(inherited.HasValue());
    CHECK(inherited.Value().Evaluate({}).Value().bloom->intensity == 8);
    volumes[0].overrides.bloom = {PostProcessOverrideMode::Replace, BloomSettings{.intensity = 4}};
    auto tied = PreparePostProcessVolumes(3, base, {}, volumes);
    REQUIRE(tied.HasValue());
    CHECK(tied.Value().Evaluate({}).Value().bloom->intensity == 4);
}

TEST_CASE("Volume snapshot preparation rejects invalid references shapes weights and finite capacities",
          "[runtime][renderer][post-process]") {
    const int variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    PostProcessVolume volume{.id = 1};
    if (variant == 0)
        volume.id = 0;
    if (variant == 1)
        volume.profile = PostProcessProfileId{99};
    if (variant == 2)
        volume.weight = 1.1F;
    if (variant == 3)
        volume.blendRadius = -1;
    if (variant == 4)
        volume.priority = std::numeric_limits<float>::infinity();
    if (variant == 5)
        volume.bounds = Math::Aabb{{1, 1, 1}, {-1, -1, -1}};
    if (variant == 6)
        volume.overrides.bloom.mode = static_cast<PostProcessOverrideMode>(255);
    PostProcessVolumeLimits limits;
    if (variant == 7)
        limits.maxVolumes = 257;
    REQUIRE(PreparePostProcessVolumes(1, {}, {}, std::span{&volume, 1}, limits).HasError());
}

TEST_CASE("Post-process snapshots preserve old generations when replacement or evaluation fails", "[runtime][renderer][post-process]") {
    auto old = PreparePostProcessVolumes(1, {}, {}, {});
    REQUIRE(old.HasValue());
    REQUIRE(PreparePostProcessVolumes(0, {}, {}, {}).HasError());
    CHECK(old.Value().Generation() == 1);
    CHECK(old.Value().Evaluate({}).HasValue());
    CHECK(old.Value().Evaluate({std::numeric_limits<float>::quiet_NaN(), 0, 0}).HasError());
    std::array duplicate{PostProcessVolume{.id = 1}, PostProcessVolume{.id = 1}};
    CHECK(PreparePostProcessVolumes(2, {}, {}, duplicate).HasError());
    std::array profiles{PostProcessProfile{{1}, {}}, PostProcessProfile{{1}, {}}};
    CHECK(PreparePostProcessVolumes(2, {}, profiles, {}).HasError());
}
