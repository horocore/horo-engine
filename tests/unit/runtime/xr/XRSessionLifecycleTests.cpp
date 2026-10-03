#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRSessionErrors.h"
#include "Horo/XR/XRSessionLifecycle.h"
#include "support/AllocationProbe.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Horo::XR {
    namespace {
        using Horo::Tests::RequireFailureIdentity;

        template <typename T> T Generation(const std::uint64_t value) {
            auto result = T::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        XRCapabilitySnapshot Capabilities(const std::uint64_t runtime = 1,
                                          const XRCapabilityState projection = XRCapabilityState::Available) {
            XRCapabilityDescriptor descriptor{
                .system = {.runtime = Generation<XRRuntimeGeneration>(runtime), .slot = {.index = 2, .generation = 1}},
                .revision = Generation<XRCapabilityRevision>(runtime),
                .limits = {.maximumViews = 4, .maximumSpaces = 8, .maximumActions = 8, .maximumDevices = 4},
            };
            descriptor.states.fill(XRCapabilityState::Unsupported);
            for (const XRCapability feature :
                 {XRCapability::PrimaryOpaqueStereo, XRCapability::OrientationTracking, XRCapability::PositionTracking,
                  XRCapability::ViewSpace, XRCapability::LocalSpace, XRCapability::BooleanActions, XRCapability::FloatActions,
                  XRCapability::Vector2Actions, XRCapability::PoseActions, XRCapability::PredictedFrames,
                  XRCapability::ExternalColorTargets, XRCapability::SessionLossLifecycle, XRCapability::CanonicalInputProjection})
                descriptor.states[static_cast<std::size_t>(feature)] = XRCapabilityState::Available;
            descriptor.states[static_cast<std::size_t>(XRCapability::Projection)] = projection;
            auto result = XRCapabilitySnapshot::Create(descriptor);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        constexpr XRFeatureNegotiationRequest Projection{.profile = XRFeatureProfile::Projection1_0,
                                                         .requestedLimits = {.maximumViews = 2,
                                                                             .maximumSpaces = 2,
                                                                             .maximumActions = 4,
                                                                             .maximumDevices = 1}};

        class RecordingResources final : public IXRSessionResources {
        public:
            struct Entry final {
                XRSessionPreparation stage;
                XRSessionId session;
                bool release;
            };

            Result<void> Prepare(const XRSessionPreparation stage, const XRSessionId &candidate, const XRFeaturePlan &plan) override {
                REQUIRE(plan.Profile() == XRFeatureProfile::Projection1_0);
                REQUIRE(plan.System() == candidate.system);
                calls.push_back({stage, candidate, false});
                if (stage == failAt)
                    return Result<void>::Failure(MakeError(XRErrors::RuntimeUnavailable));
                return Result<void>::Success();
            }

            void Release(const XRSessionPreparation stage, const XRSessionId &session) noexcept override {
                calls.push_back({stage, session, true});
            }

            XRSessionPreparation failAt{XRSessionPreparation::Count};
            std::vector<Entry> calls;
        };

        XRFeaturePlan PlanFor(const XRCapabilitySnapshot &capabilities) {
            const auto outcome = NegotiateXRFeatures(capabilities, capabilities.System(), capabilities.Revision(), Projection);
            REQUIRE(outcome.status == XRFeatureNegotiationStatus::Ok);
            REQUIRE(outcome.plan.has_value());
            return *outcome.plan;
        }

        Result<XRSessionId> TryActivate(XRSessionLifecycle &lifecycle, const XRCapabilitySnapshot &capabilities) {
            const XRFeaturePlan plan = PlanFor(capabilities);
            return lifecycle.Activate(capabilities, capabilities.System(), capabilities.Revision(), plan);
        }

        XRSessionId Activate(XRSessionLifecycle &lifecycle, const XRCapabilitySnapshot &capabilities) {
            auto result = TryActivate(lifecycle, capabilities);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        TEST_CASE("XRRuntime contributes unique actionable session errors", "[unit][xr][lifecycle]") {
            const auto descriptors = XRSessionErrors::Descriptors();
            REQUIRE(descriptors.size() == 2);
            std::set<std::string> codes;
            for (const auto *descriptor : descriptors) {
                REQUIRE(descriptor != nullptr);
                REQUIRE(descriptor->domain.Value() == "horo.xr");
                REQUIRE_FALSE(descriptor->remediationHint.empty());
                REQUIRE(codes.insert(descriptor->code.Value()).second);
            }
        }

        TEST_CASE("XR session events gate frames and focused actions", "[unit][xr][lifecycle]") {
            RecordingResources resources;
            XRSessionLifecycle lifecycle{resources};
            const auto session = Activate(lifecycle, Capabilities());
            REQUIRE(lifecycle.AcceptedPlan().has_value());
            REQUIRE(lifecycle.AcceptedPlan()->Profile() == XRFeatureProfile::Projection1_0);
            REQUIRE(lifecycle.Snapshot().state == XRSessionState::Ready);
            RequireFailureIdentity(lifecycle.Admit(session, false), XRErrors::OperationUnavailable);
            RequireFailureIdentity(lifecycle.ApplyEvent(session, XRSessionEvent::BecameFocused), XRSessionErrors::TransitionInvalid);

            REQUIRE(lifecycle.ApplyEvent(session, XRSessionEvent::Started).HasValue());
            REQUIRE(lifecycle.ApplyEvent(session, XRSessionEvent::Started).HasValue());
            const auto allocations = Horo::Tests::AllocationProbe::Count();
            REQUIRE(lifecycle.Admit(session, false).HasValue());
            REQUIRE(Horo::Tests::AllocationProbe::Count() == allocations);
            RequireFailureIdentity(lifecycle.Admit(session, true), XRErrors::OperationUnavailable);
            REQUIRE(lifecycle.ApplyEvent(session, XRSessionEvent::BecameVisible).HasValue());
            REQUIRE(lifecycle.ApplyEvent(session, XRSessionEvent::BecameFocused).HasValue());
            REQUIRE(lifecycle.Admit(session, true).HasValue());
            REQUIRE(lifecycle.ApplyEvent(session, XRSessionEvent::VisibilityLost).HasValue());
            REQUIRE(lifecycle.Snapshot().state == XRSessionState::Running);
            RequireFailureIdentity(lifecycle.Admit(session, true), XRErrors::OperationUnavailable);
            REQUIRE(lifecycle.ApplyEvent(session, XRSessionEvent::Stopping).HasValue());
            REQUIRE(lifecycle.ApplyEvent(session, XRSessionEvent::Idle).HasValue());
            REQUIRE_FALSE(lifecycle.Snapshot().session.IsValid());
            REQUIRE_FALSE(lifecycle.AcceptedPlan().has_value());
            RequireFailureIdentity(lifecycle.Admit(session, false), XRErrors::IdentityStale);
        }

        TEST_CASE("XR activation rolls each failed candidate back in reverse order", "[unit][xr][lifecycle]") {
            for (std::uint8_t failed = 0; failed < static_cast<std::uint8_t>(XRSessionPreparation::Count); ++failed) {
                RecordingResources resources;
                resources.failAt = static_cast<XRSessionPreparation>(failed);
                XRSessionLifecycle lifecycle{resources};
                auto result = TryActivate(lifecycle, Capabilities());
                RequireFailureIdentity(result, XRSessionErrors::ActivationFailed);
                REQUIRE(ErrorChainContains(result.ErrorValue(), XRErrors::RuntimeUnavailable.domain, XRErrors::RuntimeUnavailable.code));
                REQUIRE(result.ErrorValue().message.find("failed at ") != std::string::npos);
                REQUIRE(lifecycle.Snapshot().state == XRSessionState::Inactive);
                REQUIRE(resources.calls.size() == static_cast<std::size_t>(failed) * 2 + 1);
                for (std::size_t index = 0; index < failed; ++index) {
                    REQUIRE_FALSE(resources.calls[index].release);
                    REQUIRE(resources.calls[failed + 1 + index].release);
                    REQUIRE(resources.calls[failed + 1 + index].stage == resources.calls[failed - index - 1].stage);
                }
            }
        }

        TEST_CASE("XR replacement retains old publication on failure then fences every old owner", "[unit][xr][lifecycle]") {
            RecordingResources resources;
            XRSessionLifecycle lifecycle{resources};
            const auto original = Activate(lifecycle, Capabilities());
            REQUIRE(lifecycle.ApplyEvent(original, XRSessionEvent::Started).HasValue());
            resources.failAt = XRSessionPreparation::RendererTargets;
            RequireFailureIdentity(TryActivate(lifecycle, Capabilities(2)), XRSessionErrors::ActivationFailed);
            REQUIRE(lifecycle.Snapshot().session == original);
            REQUIRE(lifecycle.AcceptedPlan()->System() == original.system);
            REQUIRE(lifecycle.Admit(original, false).HasValue());

            resources.failAt = XRSessionPreparation::Count;
            const auto replacement = Activate(lifecycle, Capabilities(2));
            REQUIRE(replacement.system.runtime.Value() == 2);
            REQUIRE(replacement != original);
            REQUIRE(replacement.slot.generation > original.slot.generation + 1);
            REQUIRE(lifecycle.Snapshot().state == XRSessionState::Ready);
            REQUIRE(lifecycle.AcceptedPlan()->System() == replacement.system);
            RequireFailureIdentity(lifecycle.ApplyEvent(original, XRSessionEvent::Stopping), XRErrors::IdentityStale);
            RequireFailureIdentity(lifecycle.Admit(original, false), XRErrors::IdentityStale);
            const XRSpaceId oldSpace{original, {.index = 0, .generation = 1}};
            RequireFailureIdentity(ValidateXRSessionObject(oldSpace, lifecycle.Snapshot().session), XRErrors::IdentityStale);
            REQUIRE(resources.calls.back().release);
            REQUIRE(resources.calls.back().session == original);
            REQUIRE(resources.calls.back().stage == XRSessionPreparation::PlatformLoader);
        }

        TEST_CASE("XR loss and shutdown invalidate admission and retire exact resources once", "[unit][xr][lifecycle]") {
            RecordingResources resources;
            XRSessionLifecycle lifecycle{resources};
            const auto old = Activate(lifecycle, Capabilities());
            REQUIRE(lifecycle.ApplyEvent(old, XRSessionEvent::InstanceLost).HasValue());
            REQUIRE(lifecycle.ApplyEvent(old, XRSessionEvent::InstanceLost).HasValue());
            REQUIRE_FALSE(lifecycle.Snapshot().session.IsValid());
            REQUIRE_FALSE(lifecycle.AcceptedPlan().has_value());
            RequireFailureIdentity(lifecycle.Admit(old, false), XRErrors::IdentityStale);
            const auto recovered = Activate(lifecycle, Capabilities());
            REQUIRE(recovered.slot.generation > old.slot.generation);
            lifecycle.Shutdown();
            const auto count = resources.calls.size();
            lifecycle.Shutdown();
            REQUIRE(resources.calls.size() == count);
            REQUIRE(lifecycle.Snapshot().state == XRSessionState::Destroyed);
            REQUIRE_FALSE(lifecycle.AcceptedPlan().has_value());
            RequireFailureIdentity(lifecycle.Admit(recovered, false), XRErrors::IdentityStale);
            RequireFailureIdentity(TryActivate(lifecycle, Capabilities()), XRErrors::OperationUnavailable);
        }

        TEST_CASE("XR admission rejects unsupported invalid and excessive requests before preparation", "[unit][xr][lifecycle]") {
            RecordingResources resources;
            XRSessionLifecycle lifecycle{resources};
            const auto current = Capabilities();
            const XRFeaturePlan plan = PlanFor(current);
            RequireFailureIdentity(lifecycle.Activate(current, Capabilities(2).System(), current.Revision(), plan),
                                   XRErrors::IdentityStale);
            RequireFailureIdentity(lifecycle.Activate(current, current.System(), Generation<XRCapabilityRevision>(99), plan),
                                   XRErrors::CapabilityStale);
            const auto missing = Capabilities(1, XRCapabilityState::Unsupported);
            REQUIRE(NegotiateXRFeatures(missing, missing.System(), missing.Revision(), Projection).status ==
                    XRFeatureNegotiationStatus::RequiredUnsupported);
            auto tooMany = Projection;
            tooMany.requestedLimits.maximumActions = 9;
            REQUIRE(NegotiateXRFeatures(current, current.System(), current.Revision(), tooMany).status ==
                    XRFeatureNegotiationStatus::CapacityExceeded);
            auto invalid = Projection;
            invalid.profile = XRFeatureProfile::Count;
            REQUIRE(NegotiateXRFeatures(current, current.System(), current.Revision(), invalid).status ==
                    XRFeatureNegotiationStatus::InvalidRequest);
            REQUIRE(resources.calls.empty());
        }
    }  // namespace
}  // namespace Horo::XR
