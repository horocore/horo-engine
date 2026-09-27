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
            descriptor.states[static_cast<std::size_t>(XRCapability::Projection)] = projection;
            auto result = XRCapabilitySnapshot::Create(descriptor);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        constexpr XRCapabilityRequirement Projection{.capability = XRCapability::Projection, .views = 2};

        class RecordingResources final : public IXRSessionResources {
        public:
            struct Entry final {
                XRSessionPreparation stage;
                XRSessionId session;
                bool release;
            };

            Result<void> Prepare(const XRSessionPreparation stage, const XRSessionId &candidate) override {
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

        Result<XRSessionId> TryActivate(XRSessionLifecycle &lifecycle, const XRCapabilitySnapshot &capabilities,
                                        const XRCapabilityRequirement &requirement = Projection) {
            return lifecycle.Activate(capabilities, capabilities.System(), capabilities.Revision(), requirement);
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
            REQUIRE(lifecycle.Admit(original, false).HasValue());

            resources.failAt = XRSessionPreparation::Count;
            const auto replacement = Activate(lifecycle, Capabilities(2));
            REQUIRE(replacement.system.runtime.Value() == 2);
            REQUIRE(replacement != original);
            REQUIRE(replacement.slot.generation > original.slot.generation + 1);
            REQUIRE(lifecycle.Snapshot().state == XRSessionState::Ready);
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
            RequireFailureIdentity(lifecycle.Admit(old, false), XRErrors::IdentityStale);
            const auto recovered = Activate(lifecycle, Capabilities());
            REQUIRE(recovered.slot.generation > old.slot.generation);
            lifecycle.Shutdown();
            const auto count = resources.calls.size();
            lifecycle.Shutdown();
            REQUIRE(resources.calls.size() == count);
            REQUIRE(lifecycle.Snapshot().state == XRSessionState::Destroyed);
            RequireFailureIdentity(lifecycle.Admit(recovered, false), XRErrors::IdentityStale);
            RequireFailureIdentity(TryActivate(lifecycle, Capabilities()), XRErrors::OperationUnavailable);
        }

        TEST_CASE("XR admission rejects unsupported invalid and excessive requests before preparation", "[unit][xr][lifecycle]") {
            RecordingResources resources;
            XRSessionLifecycle lifecycle{resources};
            const auto current = Capabilities();
            RequireFailureIdentity(lifecycle.Activate(current, Capabilities(2).System(), current.Revision(), Projection),
                                   XRErrors::IdentityStale);
            RequireFailureIdentity(lifecycle.Activate(current, current.System(), Generation<XRCapabilityRevision>(99), Projection),
                                   XRErrors::CapabilityStale);
            RequireFailureIdentity(TryActivate(lifecycle, Capabilities(1, XRCapabilityState::Unsupported)), XRErrors::OperationUnsupported);
            XRCapabilityRequirement tooMany = Projection;
            tooMany.views = 5;
            RequireFailureIdentity(TryActivate(lifecycle, Capabilities(), tooMany), XRErrors::CapacityExceeded);
            XRCapabilityRequirement invalid = Projection;
            invalid.capability = XRCapability::Count;
            RequireFailureIdentity(TryActivate(lifecycle, Capabilities(), invalid), XRErrors::OperationInvalid);
            REQUIRE(resources.calls.empty());
        }
    }  // namespace
}  // namespace Horo::XR
