#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRLightProjection.h"
#include "support/TypedIdentityTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::XR {
    namespace {
        using Horo::Tests::RequireFailureIdentity;

        template <class Id, class Value> Id Make(Value value) {
            const auto result = Id::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        XRLightEstimateScope Scope() {
            const XRSessionId session{{Make<XRRuntimeGeneration>(1), {1, 1}}, {1, 2}};
            return {session, {{session, {2, 1}}, XRSpaceKind::World, Make<XRWorldOriginRevision>(3)}, Make<XRLightEstimateGrant>(5)};
        }

        constexpr XRLightProjectionPolicy Policy() {
            using enum Render::SceneColorEncoding;
            return {.ambientColor = {LinearAcesCg},
                    .directionalColor = {LinearAcesCg},
                    .direction = XRLightDirectionConvention::TowardSource,
                    .ambientScale = 2.0F,
                    .directionalScale = 0.5F,
                    .maximumAbsoluteChannel = 16.0F,
                    .directionalIdentity = 79,
                    .maximumAge = Duration::FromNanoseconds(30),
                    .smoothingWindow = Duration::FromNanoseconds(20)};
        }

        XRLightEstimate Estimate(const XRLightEstimateScope &scope, std::int64_t time) {
            return {.scope = scope,
                    .sampledAt = Make<XRRuntimeTime>(time),
                    .ambient = XRLightEstimateIntensity{{0.25F, 0.5F, 0.75F}},
                    .directional = XRDirectionalLightEstimate{{{0.5F, 1.0F, 2.0F}}, {0, 1, 0}}};
        }
    }  // namespace

    TEST_CASE("XR light projection converts explicit working RGB and smooths only fresh retained channels", "[xr][lighting]") {
        const auto scope = Scope();
        auto created = XRLightProjection::Create(scope, Policy());
        REQUIRE(created.HasValue());
        auto projection = std::move(created).Value();
        auto sample = Estimate(scope, 100);
        const auto first = projection.Update(scope, Make<XRRuntimeTime>(101), &sample);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value());
        CHECK(first.Value()->ambient == Math::Vec3{0.5F, 1.0F, 1.5F});
        REQUIRE(first.Value()->directional);
        CHECK(first.Value()->directional->color == Math::Vec3{0.25F, 0.5F, 1.0F});
        CHECK(first.Value()->directional->direction == Math::Vec3{0, -1, 0});
        CHECK(first.Value()->directional->stableIdentity == 79);

        sample.sampledAt = Make<XRRuntimeTime>(105);
        sample.ambient->relativeRgb.x = 0.75F;
        const auto blend = projection.Update(scope, Make<XRRuntimeTime>(108), &sample);
        REQUIRE(blend.HasValue());
        REQUIRE(blend.Value());
        CHECK(blend.Value()->ambient == Math::Vec3{0.75F, 1.0F, 1.5F});

        sample.directional.reset();
        const auto reduced = projection.Update(scope, Make<XRRuntimeTime>(109), &sample);
        REQUIRE(reduced.HasValue());
        REQUIRE(reduced.Value());
        CHECK(reduced.Value()->ambient == blend.Value()->ambient);
        CHECK_FALSE(reduced.Value()->directional);
        sample.directional = Estimate(scope, 105).directional;
        const auto repeated = projection.Update(scope, Make<XRRuntimeTime>(110), &sample);
        REQUIRE(repeated.HasValue());
        REQUIRE(repeated.Value());
        CHECK_FALSE(repeated.Value()->directional);
    }

    TEST_CASE("XR light projection withdraws stale observations and fences replaced grants", "[xr][lighting]") {
        const auto scope = Scope();
        auto created = XRLightProjection::Create(scope, Policy());
        REQUIRE(created.HasValue());
        auto projection = std::move(created).Value();
        const auto sample = Estimate(scope, 100);
        REQUIRE(projection.Update(scope, Make<XRRuntimeTime>(101), &sample).Value());

        SECTION("explicit withdrawal cannot replay the same sample") {
            CHECK_FALSE(projection.Update(scope, Make<XRRuntimeTime>(102), nullptr).Value());
            CHECK_FALSE(projection.Update(scope, Make<XRRuntimeTime>(103), &sample).Value());
        }
        SECTION("maximum age uses source time rather than repeated update time") {
            REQUIRE(projection.Update(scope, Make<XRRuntimeTime>(130), &sample).Value());
            RequireFailureIdentity(projection.Update(scope, Make<XRRuntimeTime>(131), &sample), XRErrors::TrackingSnapshotStale);
            RequireFailureIdentity(projection.Update(scope, Make<XRRuntimeTime>(130), &sample), XRErrors::TimeDomainIncompatible);
        }
        SECTION("a different origin permanently retires this owner") {
            auto replacement = scope;
            replacement.space.worldOriginRevision = Make<XRWorldOriginRevision>(4);
            RequireFailureIdentity(projection.Update(replacement, Make<XRRuntimeTime>(102), nullptr), XRErrors::IdentityStale);
            RequireFailureIdentity(projection.Update(scope, Make<XRRuntimeTime>(103), &sample), XRErrors::IdentityStale);
        }
    }

    TEST_CASE("XR light projection rejects unknown color policy and invalid samples without retaining partial lighting", "[xr][lighting]") {
        const auto scope = Scope();
        auto invalid = Policy();
        invalid.ambientColor = {};
        RequireFailureIdentity(XRLightProjection::Create(scope, invalid), Render::SceneColorErrors::InvalidPolicy);
        auto created = XRLightProjection::Create(scope, Policy());
        REQUIRE(created.HasValue());
        auto projection = std::move(created).Value();
        const auto valid = Estimate(scope, 100);
        REQUIRE(projection.Update(scope, Make<XRRuntimeTime>(101), &valid).Value());
        auto overLimit = Estimate(scope, 102);
        overLimit.directional->intensity.relativeRgb.x = 100.0F;
        RequireFailureIdentity(projection.Update(scope, Make<XRRuntimeTime>(102), &overLimit), XRErrors::OperationInvalid);
        CHECK_FALSE(projection.Update(scope, Make<XRRuntimeTime>(103), &valid).Value());
    }
}  // namespace Horo::XR
