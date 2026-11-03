#include "PhysicsStateSurfaceBinding.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics::StateDetail {
    namespace {
        SurfaceMaterialId Surface() {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = 7;
            return SurfaceMaterialId::FromBytes(bytes);
        }

        PhysicsSurfaceWorldBinding Binding(std::uint64_t epoch, std::uint64_t revision = 3) {
            auto catalog = SurfaceCatalogSnapshot::Create({"state-surfaces", revision, {{Surface(), "Stone"}}});
            REQUIRE(catalog.HasValue());
            return {std::make_shared<const SurfaceCatalogSnapshot>(std::move(catalog).Value()), epoch, Surface()};
        }
    }  // namespace

    TEST_CASE("Durable state surface rebind uses actual receiving epoch without weakening live evidence checks",
              "[unit][physics][state][surface]") {
        const auto original = Binding(4);
        const auto receiving = Binding(91);
        auto live = MakePhysicsSurfaceEvidence(original, Surface());
        live.collider = 12;
        const auto state = CaptureStateSurface(live);
        auto rebound = RebindStateSurface(receiving, state);
        REQUIRE(rebound.HasValue());
        REQUIRE(rebound.Value().has_value());
        CHECK(rebound.Value()->mappingGeneration == 91);
        CHECK(CaptureStateSurface(*rebound.Value()) == state);
        CHECK_FALSE(MatchesPhysicsSurfaceEvidence(receiving, live));
        CHECK(MatchesPhysicsSurfaceEvidence(receiving, *rebound.Value()));
        CHECK(PhysicsSurfaceWorldFingerprint(original) != PhysicsSurfaceWorldFingerprint(receiving));
    }

    TEST_CASE("State surface source revision fingerprint and catalogue membership are mandatory", "[unit][physics][state][surface]") {
        const auto original = Binding(4);
        auto saved = CaptureStateSurface(MakePhysicsSurfaceEvidence(original, Surface()));
        CHECK(RebindStateSurface(Binding(99, 4), saved).HasError());
        saved.catalogFingerprint.bytes[3] ^= 1;
        CHECK(RebindStateSurface(original, saved).HasError());
        saved = CaptureStateSurface(MakePhysicsSurfaceEvidence(original, Surface()));
        std::array<std::uint8_t, 16> unknown{};
        unknown.back() = 8;
        saved.surface = SurfaceMaterialId::FromBytes(unknown);
        CHECK(RebindStateSurface(original, saved).HasError());
        CHECK(RebindStateSurface({}, saved).HasError());
        CHECK(RebindStateSurface({}, std::nullopt).HasValue());
    }
}  // namespace Horo::Physics::StateDetail
