#include "Horo/XR/XRSessionErrors.h"
#include "Horo/XR/XRSpatialAnchors.h"
#include "support/AllocationProbe.h"
#include "support/TypedIdentityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>

namespace Horo::XR {
    namespace {
        using Tests::RequireFailureIdentity;

        template <typename T> T Value(const Result<T> &result) {
            REQUIRE(result.HasValue());
            return result.Value();
        }

        XRRuntimeTime At(const std::int64_t nanoseconds) {
            return Value(XRRuntimeTime::Create(nanoseconds));
        }

        struct Resources final : IXRSessionResources {
            Result<void> Prepare(XRSessionPreparation, const XRSessionId &, const XRFeaturePlan &) override {
                return Result<void>::Success();
            }

            void Release(XRSessionPreparation, const XRSessionId &) noexcept override {}
        };

        struct Fixture final {
            Resources resources;
            XRSessionLifecycle lifecycle{resources};
            XRSpatialAnchors anchors{lifecycle};
            XRSessionId session{ActivateSession(1)};

            XRSessionId ActivateSession(const std::uint64_t runtime) {
                const XRSystemLimits limits{.maximumViews = 2, .maximumSpaces = 2, .maximumActions = 4, .maximumDevices = 1};
                XRCapabilityDescriptor descriptor{.system = {.runtime = Value(XRRuntimeGeneration::Create(runtime)), .slot = {0, 1}},
                                                  .revision = Value(XRCapabilityRevision::Create(1)),
                                                  .limits = limits};
                descriptor.states.fill(XRCapabilityState::Available);
                const auto snapshot = Value(XRCapabilitySnapshot::Create(descriptor));
                const auto negotiation = NegotiateXRFeatures(snapshot, snapshot.System(), snapshot.Revision(),
                                                             {.profile = XRFeatureProfile::Projection1_0, .requestedLimits = limits});
                REQUIRE(negotiation.plan);
                const auto active = Value(lifecycle.Activate(snapshot, snapshot.System(), snapshot.Revision(), *negotiation.plan));
                Focus(active);
                return active;
            }

            void Focus(const XRSessionId active) {
                Event(active, XRSessionEvent::Started);
                Event(active, XRSessionEvent::BecameVisible);
                Event(active, XRSessionEvent::BecameFocused);
            }

            void Event(const XRSessionId active, const XRSessionEvent event) {
                REQUIRE(lifecycle.ApplyEvent(active, event).HasValue());
            }

            XRAnchorScope Scope(const std::uint64_t grant = 1, const std::uint64_t origin = 1, const std::uint64_t provider = 1) const {
                return {
                    .space = {.id = {session, {0, 1}},
                              .kind = XRSpaceKind::World,
                              .worldOriginRevision = Value(XRWorldOriginRevision::Create(origin))},
                    .provider = Value(XRAnchorProviderGeneration::Create(provider)),
                    .grant = Value(XRAnchorGrant::Create(grant)),
                };
            }
        };

        constexpr XRAnchorLimits Limits{.maximumAnchors = 1, .maximumSampleAge = Duration::FromNanoseconds(20)};

        XRAnchorObservation Localized(const std::int64_t nanoseconds) {
            return {.sampledAt = At(nanoseconds),
                    .state = XRAnchorLocalization::Localized,
                    .pose = XRAnchorPose{.positionMeters = {1.0F, 2.0F, 3.0F}}};
        }
    }  // namespace

    TEST_CASE("XR spatial anchor publication exposes only fresh current localization", "[xr][anchors]") {
        Fixture fixture;
        const auto scope = fixture.Scope();
        REQUIRE(fixture.anchors.Activate(scope, Limits, XRCapabilityState::Available).HasValue());
        const auto id = Value(fixture.anchors.Create(scope, At(50)));
        const auto pending = Value(fixture.anchors.Read(scope, id, At(50)));
        CHECK(pending.Id() == id);
        CHECK(pending.Scope() == scope);
        CHECK(pending.Observation().state == XRAnchorLocalization::Locating);
        CHECK_FALSE(pending.Observation().pose);

        const auto sample = Localized(60);
        const auto boundary = At(80);
        const auto before = Tests::AllocationProbe::Count();
        const auto published = fixture.anchors.Publish(scope, id, sample);
        const auto current = fixture.anchors.Read(scope, id, boundary);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(published.HasValue());
        REQUIRE(current.HasValue());
        CHECK(before == after);
        REQUIRE(current.Value().Observation().pose);
        CHECK(current.Value().Observation().pose->positionMeters.x == 1.0F);
        const auto expired = Value(fixture.anchors.Read(scope, id, At(81)));
        CHECK(expired.Observation().state == XRAnchorLocalization::Expired);
        CHECK_FALSE(expired.Observation().pose);

        REQUIRE(fixture.anchors.Publish(scope, id, {At(82), XRAnchorLocalization::Limited, {}}).HasValue());
        CHECK_FALSE(Value(fixture.anchors.Read(scope, id, At(82))).Observation().pose);
        REQUIRE(fixture.anchors.Publish(scope, id, {At(83), XRAnchorLocalization::Lost, {}}).HasValue());
        CHECK(Value(fixture.anchors.Read(scope, id, At(83))).Observation().state == XRAnchorLocalization::Lost);
        REQUIRE(fixture.anchors.Publish(scope, id, Localized(84)).HasValue());
        CHECK(Value(fixture.anchors.Read(scope, id, At(85))).Observation().state == XRAnchorLocalization::Localized);
    }

    TEST_CASE("XR spatial anchors reject unsupported, malformed, full and stale operations atomically", "[xr][anchors]") {
        Fixture fixture;
        const auto scope = fixture.Scope();
        RequireFailureIdentity(fixture.anchors.Activate(scope, Limits, XRCapabilityState::Unsupported), XRErrors::OperationUnsupported);
        RequireFailureIdentity(fixture.anchors.Activate(scope, Limits, XRCapabilityState::PermissionRequired), XRErrors::PermissionDenied);
        RequireFailureIdentity(fixture.anchors.Create(scope, At(50)), XRErrors::OperationUnavailable);
        auto limits = Limits;
        limits.maximumAnchors = XRSpatialAnchors::MaximumAnchors + 1;
        RequireFailureIdentity(fixture.anchors.Activate(scope, limits, XRCapabilityState::Available), XRErrors::CapacityExceeded);
        REQUIRE(fixture.anchors.Activate(scope, Limits, XRCapabilityState::Available).HasValue());
        RequireFailureIdentity(fixture.anchors.Create(scope, {}), XRErrors::TimeDomainIncompatible);
        const auto id = Value(fixture.anchors.Create(scope, At(50)));
        RequireFailureIdentity(fixture.anchors.Create(scope, At(50)), XRErrors::CapacityExceeded);
        REQUIRE(fixture.anchors.Publish(scope, id, Localized(60)).HasValue());
        RequireFailureIdentity(fixture.anchors.Publish(scope, id, Localized(60)), XRErrors::TimeDomainIncompatible);
        RequireFailureIdentity(fixture.anchors.Read(scope, id, At(59)), XRErrors::TimeDomainIncompatible);
        auto invalid = Localized(62);
        invalid.pose->positionMeters.x = std::numeric_limits<float>::infinity();
        RequireFailureIdentity(fixture.anchors.Publish(scope, id, invalid), XRErrors::PoseInvalid);
        invalid = Localized(62);
        invalid.state = XRAnchorLocalization::Lost;
        RequireFailureIdentity(fixture.anchors.Publish(scope, id, invalid), XRErrors::PoseInvalid);
        RequireFailureIdentity(fixture.anchors.Activate(fixture.Scope(2, 2), Limits, XRCapabilityState::Lost),
                               XRErrors::OperationUnavailable);
        CHECK(Value(fixture.anchors.Read(scope, id, At(64))).Observation().sampledAt == At(60));

        REQUIRE(fixture.anchors.Retire(scope, id).HasValue());
        const auto newId = Value(fixture.anchors.Create(scope, At(65)));
        CHECK(newId != id);
        RequireFailureIdentity(fixture.anchors.Read(scope, id, At(66)), XRErrors::IdentityStale);
        CHECK(Value(fixture.anchors.Read(scope, newId, At(66))).Observation().state == XRAnchorLocalization::Locating);
    }

    TEST_CASE("XR spatial anchor replacement and revocation reject delayed scope and observation work", "[xr][anchors]") {
        Fixture fixture;
        const auto original = fixture.Scope();
        REQUIRE(fixture.anchors.Activate(original, Limits, XRCapabilityState::Available).HasValue());
        const auto id = Value(fixture.anchors.Create(original, At(50)));
        REQUIRE(fixture.anchors.Publish(original, id, Localized(60)).HasValue());
        const auto replacement = fixture.Scope(2, 2, 2);
        REQUIRE(fixture.anchors.Activate(replacement, Limits, XRCapabilityState::Available).HasValue());
        RequireFailureIdentity(fixture.anchors.Withdraw(original, XRAnchorWithdrawal::AccessRevoked), XRErrors::IdentityStale);
        RequireFailureIdentity(fixture.anchors.Publish(original, id, Localized(62)), XRErrors::IdentityStale);
        RequireFailureIdentity(fixture.anchors.Read(replacement, id, At(62)), XRErrors::IdentityStale);
        const auto current = Value(fixture.anchors.Create(replacement, At(62)));
        REQUIRE(fixture.anchors.Withdraw(replacement, XRAnchorWithdrawal::AccessRevoked).HasValue());
        REQUIRE(fixture.anchors.Withdraw(replacement, XRAnchorWithdrawal::ProviderStopped).HasValue());
        RequireFailureIdentity(fixture.anchors.Read(replacement, current, At(65)), XRErrors::PermissionDenied);
        RequireFailureIdentity(fixture.anchors.Activate(replacement, Limits, XRCapabilityState::Available), XRErrors::IdentityStale);
    }

    TEST_CASE("XR canonical focus, idle, runtime replacement and shutdown fence live anchors", "[xr][anchors]") {
        Fixture fixture;
        const auto original = fixture.Scope();
        REQUIRE(fixture.anchors.Activate(original, Limits, XRCapabilityState::Available).HasValue());
        const auto id = Value(fixture.anchors.Create(original, At(50)));
        REQUIRE(fixture.anchors.Publish(original, id, Localized(60)).HasValue());
        const auto sampledAt = At(61);
        std::optional<Result<XRAnchorSnapshot>> foreignLane;
        std::thread worker([&] {
            foreignLane.emplace(fixture.anchors.Read(original, id, sampledAt));
        });
        worker.join();
        REQUIRE(foreignLane);
        RequireFailureIdentity(*foreignLane, XRSessionErrors::WrongThread);

        fixture.Event(fixture.session, XRSessionEvent::FocusLost);
        RequireFailureIdentity(fixture.anchors.Read(original, id, At(61)), XRErrors::IdentityStale);
        fixture.Event(fixture.session, XRSessionEvent::Stopping);
        fixture.Event(fixture.session, XRSessionEvent::Idle);
        fixture.Event(fixture.session, XRSessionEvent::Ready);
        fixture.Focus(fixture.session);
        RequireFailureIdentity(fixture.anchors.Read(original, id, At(62)), XRErrors::IdentityStale);
        REQUIRE(fixture.anchors.Withdraw(original, XRAnchorWithdrawal::ProviderStopped).HasValue());
        const auto renewed = fixture.Scope(2);
        REQUIRE(fixture.anchors.Activate(renewed, Limits, XRCapabilityState::Available).HasValue());
        const auto renewedId = Value(fixture.anchors.Create(renewed, At(63)));

        fixture.session = fixture.ActivateSession(2);
        RequireFailureIdentity(fixture.anchors.Read(renewed, renewedId, At(64)), XRErrors::IdentityStale);
        const auto replacement = fixture.Scope(3, 1, 2);
        REQUIRE(fixture.anchors.Activate(replacement, Limits, XRCapabilityState::Available).HasValue());
        const auto replacedId = Value(fixture.anchors.Create(replacement, At(50)));
        fixture.lifecycle.Shutdown();
        RequireFailureIdentity(fixture.anchors.Read(replacement, replacedId, At(64)), XRErrors::IdentityStale);
        REQUIRE(fixture.anchors.Withdraw(replacement, XRAnchorWithdrawal::ProviderStopped).HasValue());
    }
}  // namespace Horo::XR
