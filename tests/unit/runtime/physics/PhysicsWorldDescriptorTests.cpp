#include "Horo/Physics/PhysicsCapabilities.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Horo::Physics {
    namespace {
        void ExpectError(const Result<void> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        PhysicsCapabilities Available() {
            PhysicsCapabilities result{.revision = 7, .availability = PhysicsAvailability::Available};
            result.features.fill(PhysicsCapabilitySupport::Available);
            return result;
        }

        TEST_CASE("Physics world defaults express SI fixed-step policy without activation", "[physics][world]") {
            const PhysicsWorldDescriptor descriptor;
            REQUIRE(ValidatePhysicsWorldDescriptor(descriptor).HasValue());
            REQUIRE(descriptor.gravity == Math::Vec3{0.0F, -9.81F, 0.0F});
            REQUIRE(descriptor.fixedDeltaSeconds == 1.0 / 60.0);
            REQUIRE(descriptor.capacity.maximumBodies == 262'144);
            REQUIRE(descriptor.capacity.maximumColliderSlots == 1'048'576);
            REQUIRE(descriptor.capacity.maximumConstraints == 262'144);
            REQUIRE(descriptor.capacity.maximumPlanBytes == 512ULL * 1024 * 1024);
            static_assert(std::is_trivially_copyable_v<PhysicsWorldDescriptor>);
        }

        TEST_CASE("Physics world descriptors reject unknown schema and profile without fallback", "[physics][world]") {
            PhysicsWorldDescriptor descriptor;
            for (const auto version : {0U, 2U}) {
                descriptor.contractVersion = version;
                ExpectError(ValidatePhysicsWorldDescriptor(descriptor), PhysicsErrors::DescriptorInvalid);
                REQUIRE(descriptor.contractVersion == version);
            }
            descriptor.contractVersion = 1;
            descriptor.profile = static_cast<PhysicsToleranceProfileId>(255);
            ExpectError(ValidatePhysicsWorldDescriptor(descriptor), PhysicsErrors::ProfileUnsupported);
        }

        TEST_CASE("Physics gravity validation measures magnitude and preserves requested vectors", "[physics][world]") {
            PhysicsWorldDescriptor descriptor;
            for (const auto gravity : {Math::Vec3{}, Math::Vec3{20, 0, 0}, Math::Vec3{12, 16, 0}, Math::Vec3{0, 0, -20}}) {
                descriptor.gravity = gravity;
                REQUIRE(ValidatePhysicsWorldDescriptor(descriptor).HasValue());
                REQUIRE(descriptor.gravity == gravity);
            }
            for (const auto gravity :
                 {Math::Vec3{20, 1, 0}, Math::Vec3{0, std::nextafter(20.0F, 21.0F), 0}, Math::Vec3{std::numeric_limits<float>::max(), 0, 0},
                  Math::Vec3{0, 0, std::numeric_limits<float>::infinity()}, Math::Vec3{0, std::numeric_limits<float>::quiet_NaN(), 0}}) {
                descriptor.gravity = gravity;
                ExpectError(ValidatePhysicsWorldDescriptor(descriptor), PhysicsErrors::DescriptorInvalid);
            }
        }

        TEST_CASE("Physics fixed delta rejects unsafe representation without claiming rate qualification", "[physics][world]") {
            PhysicsWorldDescriptor descriptor;
            for (const double seconds : {1.0 / 120.0, 1.0 / 30.0, static_cast<double>(std::numeric_limits<float>::min()),
                                         static_cast<double>(std::numeric_limits<float>::max())}) {
                descriptor.fixedDeltaSeconds = seconds;
                REQUIRE(ValidatePhysicsWorldDescriptor(descriptor).HasValue());
                REQUIRE(descriptor.fixedDeltaSeconds == seconds);
            }
            for (const double seconds :
                 {0.0, -0.0, -1.0, std::numeric_limits<double>::denorm_min(),
                  std::nextafter(static_cast<double>(std::numeric_limits<float>::min()), 0.0), std::numeric_limits<double>::max(),
                  std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
                descriptor.fixedDeltaSeconds = seconds;
                ExpectError(ValidatePhysicsWorldDescriptor(descriptor), PhysicsErrors::DescriptorInvalid);
            }
        }

        TEST_CASE("Physics world capacities reject each bound independently and never clamp", "[physics][world]") {
            using Mutation = void (*)(PhysicsWorldCapacity &);
            const Mutation mutations[]{
                [](auto &value) {
                value.maximumBodies = MaximumPhysicsBodies + 1;
            },
                [](auto &value) {
                value.maximumColliderSlots = MaximumPhysicsColliderSlots + 1;
            },
                [](auto &value) {
                value.maximumConstraints = MaximumPhysicsConstraints + 1;
            },
                [](auto &value) {
                value.maximumPlanBytes = MaximumPhysicsPlanBytes + 1;
            },
                [](auto &value) {
                value.maximumPlanBytes = 0;
            },
            };
            for (const auto mutate : mutations) {
                PhysicsWorldDescriptor descriptor;
                mutate(descriptor.capacity);
                ExpectError(ValidatePhysicsWorldDescriptor(descriptor), PhysicsErrors::CapacityExceeded);
            }
            PhysicsWorldDescriptor empty;
            empty.capacity = {0, 0, 0, 1};
            REQUIRE(ValidatePhysicsWorldDescriptor(empty).HasValue());
            empty.capacity.maximumPlanBytes = std::numeric_limits<std::uint64_t>::max();
            ExpectError(ValidatePhysicsWorldDescriptor(empty), PhysicsErrors::CapacityExceeded);
            REQUIRE(empty.capacity.maximumPlanBytes == std::numeric_limits<std::uint64_t>::max());
        }

        TEST_CASE("Physics capability defaults never advertise available functionality", "[physics][capability]") {
            static_assert(PhysicsCapabilityContractVersion == 2);
            static_assert(static_cast<unsigned>(PhysicsCapability::ContinuousCollision) == 8);
            static_assert(static_cast<unsigned>(PhysicsCapability::StateSnapshots) == 9);
            static_assert(static_cast<unsigned>(PhysicsCapability::AtomicStateRestore) == 10);
            PhysicsCapabilities report;
            REQUIRE(report.contractVersion == 2);
            REQUIRE_FALSE(ValidatePhysicsCapabilities(report));
            report.revision = 1;
            REQUIRE(ValidatePhysicsCapabilities(report));
            ExpectError(RequirePhysicsCapability(report, PhysicsCapability::WorldCreation, 1), PhysicsErrors::CapabilityUnavailable);
            report.availability = PhysicsAvailability::Omitted;
            REQUIRE_FALSE(ValidatePhysicsCapabilities(report));
            report.features.fill(PhysicsCapabilitySupport::Unsupported);
            REQUIRE(ValidatePhysicsCapabilities(report));
            ExpectError(RequirePhysicsCapability(report, PhysicsCapability::WorldCreation, 1), PhysicsErrors::OperationUnsupported);
            static_assert(std::is_trivially_copyable_v<PhysicsCapabilities>);
        }

        TEST_CASE("Physics capabilities distinguish every feature support state", "[physics][capability]") {
            for (std::size_t index = 0; index < static_cast<std::size_t>(PhysicsCapability::Count); ++index) {
                auto report = Available();
                const auto feature = static_cast<PhysicsCapability>(index);
                REQUIRE(RequirePhysicsCapability(report, feature, 7).HasValue());
                report.features[index] = PhysicsCapabilitySupport::Unsupported;
                ExpectError(RequirePhysicsCapability(report, feature, 7), PhysicsErrors::OperationUnsupported);
                for (const auto support : {PhysicsCapabilitySupport::Unknown, PhysicsCapabilitySupport::Unavailable}) {
                    report.features[index] = support;
                    ExpectError(RequirePhysicsCapability(report, feature, 7), PhysicsErrors::CapabilityUnavailable);
                }
            }
        }

        TEST_CASE("Physics capability validation rejects malformed and incoherent complete snapshots", "[physics][capability]") {
            using Mutation = void (*)(PhysicsCapabilities &);
            const std::array<Mutation, 6> mutations{
                [](auto &value) {
                value.contractVersion = 3;
            },
                [](auto &value) {
                value.contractVersion = 1;
            },
                [](auto &value) {
                value.revision = 0;
            },
                [](auto &value) {
                value.availability = static_cast<PhysicsAvailability>(255);
            },
                [](auto &value) {
                value.features.back() = static_cast<PhysicsCapabilitySupport>(255);
            },
                [](auto &value) {
                value.availability = PhysicsAvailability::Unavailable;
            },
            };
            for (const auto mutate : mutations) {
                auto report = Available();
                mutate(report);
                REQUIRE_FALSE(ValidatePhysicsCapabilities(report));
                ExpectError(RequirePhysicsCapability(report, PhysicsCapability::WorldCreation, 7), PhysicsErrors::DescriptorInvalid);
            }
        }

        TEST_CASE("Physics capability admission rejects stale revisions and unknown feature IDs", "[physics][capability]") {
            const auto report = Available();
            ExpectError(RequirePhysicsCapability(report, PhysicsCapability::WorldCreation, 6), PhysicsErrors::CapabilityStale);
            ExpectError(RequirePhysicsCapability(report, PhysicsCapability::WorldCreation, 0), PhysicsErrors::DescriptorInvalid);
            for (const auto feature : {PhysicsCapability::Count, static_cast<PhysicsCapability>(255)})
                ExpectError(RequirePhysicsCapability(report, feature, 7), PhysicsErrors::DescriptorInvalid);
            REQUIRE(report.revision == 7);
        }

        TEST_CASE("Physics world preflight composes policy and revision-scoped capability failures", "[physics][world]") {
            PhysicsWorldDescriptor descriptor;
            auto report = Available();
            REQUIRE(AdmitPhysicsWorldDescriptor(descriptor, report, 7).HasValue());
            report.features[static_cast<std::size_t>(PhysicsCapability::WorldCreation)] = PhysicsCapabilitySupport::Unsupported;
            ExpectError(AdmitPhysicsWorldDescriptor(descriptor, report, 7), PhysicsErrors::OperationUnsupported);
            descriptor.profile = static_cast<PhysicsToleranceProfileId>(255);
            ExpectError(AdmitPhysicsWorldDescriptor(descriptor, report, 6), PhysicsErrors::ProfileUnsupported);
            REQUIRE(descriptor.profile == static_cast<PhysicsToleranceProfileId>(255));
        }
    }  // namespace
}  // namespace Horo::Physics
