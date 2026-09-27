#include "Horo/XR/XRCapabilities.h"
#include "Horo/XR/XRErrors.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string_view>
#include <type_traits>

namespace Horo::XR {
    namespace {
        using Horo::Tests::RequireFailureIdentity;

        template <typename Generation> Generation MakeGeneration(const std::uint64_t value) {
            const auto result = Generation::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        XRSystemId MakeSystem(const std::uint64_t runtime = 1, const std::uint32_t index = 2, const std::uint32_t generation = 3) {
            return {MakeGeneration<XRRuntimeGeneration>(runtime), {index, generation}};
        }

        XRSessionId MakeSession(const XRSystemId &system, const std::uint32_t index = 4, const std::uint32_t generation = 5) {
            return {system, {index, generation}};
        }

        XRCapabilityDescriptor MakeDescriptor(const XRCapabilityState initial = XRCapabilityState::Unsupported) {
            XRCapabilityDescriptor descriptor{
                .system = MakeSystem(),
                .revision = MakeGeneration<XRCapabilityRevision>(7),
                .limits = {.maximumViews = 4, .maximumSpaces = 32, .maximumActions = 64, .maximumDevices = 8},
            };
            descriptor.states.fill(initial);
            return descriptor;
        }

        XRCapabilitySnapshot MakeSnapshot(const XRCapabilityState initial = XRCapabilityState::Unsupported) {
            const auto result = XRCapabilitySnapshot::Create(MakeDescriptor(initial));
            REQUIRE(result.HasValue());
            return result.Value();
        }

        template <typename Identity> void VerifySessionObjectIdentity(const XRSessionId &activeSession) {
            const Identity identity{activeSession, {9, 10}};
            REQUIRE(identity.IsValid());
            REQUIRE(ValidateXRSessionObject(identity, activeSession).HasValue());
            RequireFailureIdentity(ValidateXRSessionObject(Identity{}, activeSession), XRErrors::IdentityInvalid);
            RequireFailureIdentity(ValidateXRSessionObject(identity, {}), XRErrors::IdentityInvalid);
            RequireFailureIdentity(ValidateXRSessionObject(identity, MakeSession(activeSession.system, 4, 6)), XRErrors::IdentityStale);
        }

        TEST_CASE("XR contract versions reject malformed and incompatible producers", "[unit][xr][contract]") {
            REQUIRE(RequireXRContractVersion(CurrentXRContractVersion, CurrentXRContractVersion).HasValue());
            RequireFailureIdentity(RequireXRContractVersion(CurrentXRContractVersion, {1, 0, 0}), XRErrors::ContractVersionIncompatible);
            REQUIRE(RequireXRContractVersion({1, 2, 9}, {1, 3, 0}).HasValue());
            REQUIRE(RequireXRContractVersion({1, 2, 9}, {1, 2, 0}).HasValue());
            RequireFailureIdentity(RequireXRContractVersion({}, CurrentXRContractVersion), XRErrors::ContractVersionInvalid);
            RequireFailureIdentity(RequireXRContractVersion(CurrentXRContractVersion, {}), XRErrors::ContractVersionInvalid);
            RequireFailureIdentity(RequireXRContractVersion({1, 0, 0}, {2, 0, 0}), XRErrors::ContractVersionIncompatible);
            RequireFailureIdentity(RequireXRContractVersion({1, 2, 0}, {1, 1, 99}), XRErrors::ContractVersionIncompatible);
        }

        TEST_CASE("XR identities preserve exact owner generations and reject replacement", "[unit][xr][identity]") {
            REQUIRE(XRRuntimeGeneration::Create(0).HasError());
            REQUIRE(XRCapabilityRevision::Create(0).HasError());
            REQUIRE(MakeGeneration<XRRuntimeGeneration>(9).Value() == 9);
            static_assert(!std::is_same_v<XRRuntimeGeneration, XRCapabilityRevision>);
            static_assert(!std::is_same_v<XRSpaceId, XRViewId>);
            static_assert(!std::is_same_v<XRActionId, XRDeviceId>);
            static_assert(std::is_trivially_copyable_v<XRSystemId>);
            static_assert(std::is_trivially_copyable_v<XRSessionId>);

            const auto system = MakeSystem();
            REQUIRE(system.IsValid());
            REQUIRE(ValidateXRSystem(system, system).HasValue());
            RequireFailureIdentity(ValidateXRSystem({}, system), XRErrors::IdentityInvalid);
            RequireFailureIdentity(ValidateXRSystem(system, {}), XRErrors::IdentityInvalid);
            RequireFailureIdentity(ValidateXRSystem(system, MakeSystem(2)), XRErrors::IdentityStale);
            RequireFailureIdentity(ValidateXRSystem(system, MakeSystem(1, 2, 4)), XRErrors::IdentityStale);

            const auto session = MakeSession(system);
            REQUIRE(session.IsValid());
            REQUIRE(ValidateXRSession(session, session).HasValue());
            RequireFailureIdentity(ValidateXRSession({}, session), XRErrors::IdentityInvalid);
            RequireFailureIdentity(ValidateXRSession(session, {}), XRErrors::IdentityInvalid);
            RequireFailureIdentity(ValidateXRSession(session, MakeSession(system, 4, 6)), XRErrors::IdentityStale);

            VerifySessionObjectIdentity<XRSpaceId>(session);
            VerifySessionObjectIdentity<XRViewId>(session);
            VerifySessionObjectIdentity<XRActionId>(session);
            VerifySessionObjectIdentity<XRDeviceId>(session);
        }

        TEST_CASE("XR identity representations reject every malformed slot component", "[unit][xr][identity]") {
            const auto system = MakeSystem();
            const auto session = MakeSession(system);
            REQUIRE_FALSE(XRSystemId{system.runtime, {Horo::Handle<XRSystemSlotTag>::InvalidIndex, 1}}.IsValid());
            REQUIRE_FALSE(XRSystemId{system.runtime, {1, 0}}.IsValid());
            REQUIRE_FALSE(XRSessionId{system, {Horo::Handle<XRSessionSlotTag>::InvalidIndex, 1}}.IsValid());
            REQUIRE_FALSE(XRSessionId{system, {1, 0}}.IsValid());
            REQUIRE_FALSE(XRViewId{session, {Horo::Handle<XRViewSlotTag>::InvalidIndex, 1}}.IsValid());
            REQUIRE_FALSE(XRViewId{session, {1, 0}}.IsValid());
        }

        TEST_CASE("XR capability snapshot owns exact immutable versioned evidence", "[unit][xr][capability]") {
            auto descriptor = MakeDescriptor();
            descriptor.contractVersion = {1, 1, 4};
            descriptor.states[static_cast<std::size_t>(XRCapability::Projection)] = XRCapabilityState::Available;
            const auto result = XRCapabilitySnapshot::Create(descriptor);
            REQUIRE(result.HasValue());
            const auto snapshot = result.Value();
            descriptor.states.fill(XRCapabilityState::Denied);

            REQUIRE(snapshot.ContractVersion() == XRContractVersion{1, 1, 4});
            REQUIRE(snapshot.System() == MakeSystem());
            REQUIRE(snapshot.Revision().Value() == 7);
            REQUIRE(snapshot.State(XRCapability::Projection) == XRCapabilityState::Available);
            REQUIRE(snapshot.State(XRCapability::BooleanActions) == XRCapabilityState::Unsupported);
            REQUIRE(snapshot.State(XRCapability::Count) == XRCapabilityState::Unsupported);
            REQUIRE(snapshot.Limits() == XRSystemLimits{4, 32, 64, 8});
            REQUIRE(sizeof(XRCapabilitySnapshot) < 128);
        }

        TEST_CASE("XR capability construction rejects invalid versions identities states and limits", "[unit][xr][capability]") {
            auto invalidVersion = MakeDescriptor();
            invalidVersion.contractVersion = {};
            RequireFailureIdentity(XRCapabilitySnapshot::Create(invalidVersion), XRErrors::ContractVersionInvalid);
            invalidVersion.contractVersion = {2, 0, 0};
            RequireFailureIdentity(XRCapabilitySnapshot::Create(invalidVersion), XRErrors::ContractVersionIncompatible);

            using Mutation = void (*)(XRCapabilityDescriptor &);
            const std::array<Mutation, 11> mutations{
                [](auto &value) {
                value.system = {};
            },
                [](auto &value) {
                value.revision = {};
            },
                [](auto &value) {
                value.states.back() = XRCapabilityState::Count;
            },
                [](auto &value) {
                value.limits.maximumViews = 0;
            },
                [](auto &value) {
                value.limits.maximumViews = XRHardLimits::MaximumViews + 1;
            },
                [](auto &value) {
                value.limits.maximumSpaces = 0;
            },
                [](auto &value) {
                value.limits.maximumSpaces = XRHardLimits::MaximumSpaces + 1;
            },
                [](auto &value) {
                value.limits.maximumActions = 0;
            },
                [](auto &value) {
                value.limits.maximumActions = XRHardLimits::MaximumActions + 1;
            },
                [](auto &value) {
                value.limits.maximumDevices = 0;
            },
                [](auto &value) {
                value.limits.maximumDevices = XRHardLimits::MaximumDevices + 1;
            },
            };
            for (const auto mutate : mutations) {
                auto descriptor = MakeDescriptor();
                mutate(descriptor);
                RequireFailureIdentity(XRCapabilitySnapshot::Create(descriptor), XRErrors::CapabilityDescriptorInvalid);
            }
        }

        TEST_CASE("XR capability admission preserves every actionable availability category", "[unit][xr][capability]") {
            const auto revision = MakeGeneration<XRCapabilityRevision>(7);
            const XRCapabilityRequirement request{.capability = XRCapability::Projection, .views = 2};
            for (const auto state : {XRCapabilityState::Unavailable, XRCapabilityState::PermissionRequired, XRCapabilityState::Denied,
                                     XRCapabilityState::DependencyMissing, XRCapabilityState::TemporarilyUnavailable,
                                     XRCapabilityState::Lost, XRCapabilityState::DisabledByPolicy}) {
                RequireFailureIdentity(AdmitXRCapability(MakeSnapshot(state), MakeSystem(), revision, request),
                                       XRErrors::OperationUnavailable);
            }
            RequireFailureIdentity(AdmitXRCapability(MakeSnapshot(XRCapabilityState::Unsupported), MakeSystem(), revision, request),
                                   XRErrors::OperationUnsupported);
            RequireFailureIdentity(AdmitXRCapability(MakeSnapshot(XRCapabilityState::Incompatible), MakeSystem(), revision, request),
                                   XRErrors::OperationIncompatible);
            REQUIRE(AdmitXRCapability(MakeSnapshot(XRCapabilityState::Available), MakeSystem(), revision, request).HasValue());
        }

        TEST_CASE("XR capability admission rejects invalid stale replacement and shutdown operations", "[unit][xr][capability]") {
            const auto snapshot = MakeSnapshot(XRCapabilityState::Available);
            const auto revision = snapshot.Revision();
            const XRCapabilityRequirement request{.capability = XRCapability::Projection};
            RequireFailureIdentity(AdmitXRCapability(snapshot, MakeSystem(), {}, request), XRErrors::OperationInvalid);
            RequireFailureIdentity(AdmitXRCapability(snapshot, MakeSystem(), revision, {.capability = XRCapability::Count}),
                                   XRErrors::OperationInvalid);
            RequireFailureIdentity(AdmitXRCapability(snapshot, {}, revision, request), XRErrors::IdentityInvalid);
            RequireFailureIdentity(AdmitXRCapability(snapshot, MakeSystem(2), revision, request), XRErrors::IdentityStale);
            RequireFailureIdentity(AdmitXRCapability(snapshot, MakeSystem(1, 2, 4), revision, request), XRErrors::IdentityStale);
            RequireFailureIdentity(AdmitXRCapability(snapshot, MakeSystem(), MakeGeneration<XRCapabilityRevision>(8), request),
                                   XRErrors::CapabilityStale);

            REQUIRE(snapshot.State(XRCapability::Projection) == XRCapabilityState::Available);
            REQUIRE(snapshot.System().IsValid());
        }

        TEST_CASE("XR capability admission enforces every fixed capacity without partial mutation", "[unit][xr][capability]") {
            const auto snapshot = MakeSnapshot(XRCapabilityState::Available);
            const XRCapabilityRequirement boundary{.capability = XRCapability::Projection,
                                                   .views = 4,
                                                   .spaces = 32,
                                                   .actions = 64,
                                                   .devices = 8};
            REQUIRE(AdmitXRCapability(snapshot, snapshot.System(), snapshot.Revision(), boundary).HasValue());

            const std::array<XRCapabilityRequirement, 4> excessive{
                XRCapabilityRequirement{.capability = XRCapability::Projection, .views = 5},
                XRCapabilityRequirement{.capability = XRCapability::Projection, .spaces = 33},
                XRCapabilityRequirement{.capability = XRCapability::Projection, .actions = 65},
                XRCapabilityRequirement{.capability = XRCapability::Projection, .devices = 9},
            };
            for (const auto &request : excessive)
                RequireFailureIdentity(AdmitXRCapability(snapshot, snapshot.System(), snapshot.Revision(), request),
                                       XRErrors::CapacityExceeded);
        }

        TEST_CASE("XR error registry contribution is complete unique and actionable", "[unit][xr][errors]") {
            const auto descriptors = XRErrors::Descriptors();
            REQUIRE(descriptors.size() == 31);
            std::set<std::string_view> codes;
            for (const auto *descriptor : descriptors) {
                REQUIRE(descriptor != nullptr);
                REQUIRE(descriptor->domain.Value() == "horo.xr");
                REQUIRE(codes.insert(descriptor->code.Value()).second);
                REQUIRE_FALSE(descriptor->summary.empty());
                REQUIRE_FALSE(descriptor->remediationHint.empty());
            }
        }
    }  // namespace
}  // namespace Horo::XR
