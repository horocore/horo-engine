#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorldSettings.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <span>
#include <type_traits>

namespace Horo::Physics {
    namespace {
        using SettingsMutation = void (*)(PhysicsWorldSettingsDescriptor &);

        /** @brief Checks each independently admitted field edit changes the complete settings identity. */
        void RequireDistinctSettings(const std::span<const SettingsMutation> mutations) {
            const auto original = PhysicsWorldSettings::Capture({});
            REQUIRE(original.HasValue());
            for (const auto mutate : mutations) {
                PhysicsWorldSettingsDescriptor descriptor;
                mutate(descriptor);
                const auto changed = PhysicsWorldSettings::Capture(descriptor);
                REQUIRE(changed.HasValue());
                REQUIRE(changed.Value().Identity() != original.Value().Identity());
            }
        }
    }  // namespace

    TEST_CASE("World settings capture owns immutable values independent of project edits", "[physics][settings]") {
        PhysicsWorldSettingsDescriptor project;
        const auto result = PhysicsWorldSettings::Capture(project);
        REQUIRE(result.HasValue());
        const auto snapshot = result.Value();
        project.world.gravity.y = -5;
        project.budgets.maximumEvents = 32;
        REQUIRE(snapshot.Values().world.gravity.y == -9.81F);
        REQUIRE(snapshot.Values().budgets.maximumEvents == 8'192);
        const auto rebuilt = PhysicsWorldSettings::Capture(project);
        REQUIRE(rebuilt.HasValue());
        REQUIRE(rebuilt.Value().Identity() != snapshot.Identity());
        static_assert(!std::is_default_constructible_v<PhysicsWorldSettings>);
        static_assert(!std::is_copy_assignable_v<PhysicsWorldSettings>);
        static_assert(!std::is_move_assignable_v<PhysicsWorldSettings>);
    }

    TEST_CASE("World settings identity has an independently encoded default fixture", "[physics][settings]") {
        const auto result = PhysicsWorldSettings::Capture({});
        REQUIRE(result.HasValue());
        // SHA-256 of schema-2's 34 little-endian uint64 words, independently encoded with Python struct.pack.
        REQUIRE(FormatSha256(result.Value().Identity().digest) ==
                "sha256:dacddeeb7448d68bce46ef935e79acc15fca20fc3adcaed465411b9d5e7016ea");
        const auto repeated = PhysicsWorldSettings::Capture({});
        REQUIRE(repeated.HasValue());
        REQUIRE(repeated.Value().Identity() == result.Value().Identity());
    }

    TEST_CASE("World settings identity canonicalizes signed zero without mutating values", "[physics][settings]") {
        PhysicsWorldSettingsDescriptor descriptor;
        const auto positive = PhysicsWorldSettings::Capture(descriptor);
        descriptor.world.gravity.x = -0.0F;
        descriptor.world.gravity.z = -0.0F;
        const auto negative = PhysicsWorldSettings::Capture(descriptor);
        REQUIRE(positive.HasValue());
        REQUIRE(negative.HasValue());
        REQUIRE(positive.Value().Identity() == negative.Value().Identity());
        REQUIRE(std::signbit(negative.Value().Values().world.gravity.x));
    }

    TEST_CASE("World capacities bounds and containment contribute to settings identity", "[physics][settings]") {
        const std::array<SettingsMutation, 10> mutations{
            [](auto &v) {
            v.world.gravity.x = 1;
        },
            [](auto &v) {
            v.world.gravity.y = -5;
        },
            [](auto &v) {
            v.world.gravity.z = 1;
        },
            [](auto &v) {
            --v.world.capacity.maximumBodies;
        },
            [](auto &v) {
            --v.world.capacity.maximumColliderSlots;
        },
            [](auto &v) {
            --v.world.capacity.maximumConstraints;
        },
            [](auto &v) {
            --v.world.capacity.maximumPlanBytes;
        },
            [](auto &v) {
            --v.bounds.localHalfExtentMeters;
        },
            [](auto &v) {
            --v.bounds.dynamicContactRadiusMeters;
        },
            [](auto &v) {
            v.nonFinitePolicy = PhysicsNonFinitePolicy::QuarantineBody;
        },
        };
        RequireDistinctSettings(mutations);
    }

    TEST_CASE("Every resource reservation contributes to settings identity", "[physics][settings]") {
        const std::array<SettingsMutation, 12> mutations{
            [](auto &v) {
            --v.budgets.maximumShapes;
        },
            [](auto &v) {
            --v.budgets.maximumContactPairs;
        },
            [](auto &v) {
            --v.budgets.maximumContactConstraints;
        },
            [](auto &v) {
            --v.budgets.maximumInFlightPairs;
        },
            [](auto &v) {
            ++v.budgets.maximumCommands;
        },
            [](auto &v) {
            --v.budgets.maximumEvents;
        },
            [](auto &v) {
            ++v.budgets.maximumQueries;
        },
            [](auto &v) {
            --v.budgets.maximumCommandsPerTick;
        },
            [](auto &v) {
            --v.budgets.maximumQueriesPerTick;
        },
            [](auto &v) {
            --v.budgets.scratchBytes;
        },
            [](auto &v) {
            --v.budgets.residentShapeBytes;
        },
            [](auto &v) {
            v.budgets.eventOverflow = PhysicsEventOverflowPolicy::FailTick;
        },
        };
        RequireDistinctSettings(mutations);
    }

    TEST_CASE("World settings preserve component errors and reject unadmitted schedules", "[physics][settings]") {
        using Mutation = void (*)(PhysicsWorldSettingsDescriptor &);
        const std::array<Mutation, 5> mutations{
            [](auto &v) {
            v.world.contractVersion = 2;
        },
            [](auto &v) {
            v.step.positionIterations = 0;
        },
            [](auto &v) {
            v.budgets.maximumShapes = 0;
        },
            [](auto &v) {
            v.world.fixedDeltaSeconds = 1.0 / 120.0;
        },
            [](auto &v) {
            v.nonFinitePolicy = static_cast<PhysicsNonFinitePolicy>(255);
        },
        };
        const std::array errors{PhysicsErrors::DescriptorInvalid.code.Value(), PhysicsErrors::DescriptorInvalid.code.Value(),
                                PhysicsErrors::CapacityExceeded.code.Value(), PhysicsErrors::ProfileUnsupported.code.Value(),
                                PhysicsErrors::OperationUnsupported.code.Value()};
        for (std::size_t index = 0; index < mutations.size(); ++index) {
            PhysicsWorldSettingsDescriptor descriptor;
            mutations[index](descriptor);
            const auto result = PhysicsWorldSettings::Capture(descriptor);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == errors[index]);
        }
    }

    TEST_CASE("World settings reject malformed and inconsistent local envelopes", "[physics][settings]") {
        const auto infinity = std::numeric_limits<float>::infinity();
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        for (const auto bounds :
             {PhysicsWorldBounds{infinity, 1}, PhysicsWorldBounds{1, nan}, PhysicsWorldBounds{0, 1}, PhysicsWorldBounds{1, 0}}) {
            PhysicsWorldSettingsDescriptor descriptor;
            descriptor.bounds = bounds;
            const auto result = PhysicsWorldSettings::Capture(descriptor);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
        }
        for (const auto bounds : {PhysicsWorldBounds{8'193, 1}, PhysicsWorldBounds{8'192, 4'097}, PhysicsWorldBounds{1, 2}}) {
            PhysicsWorldSettingsDescriptor descriptor;
            descriptor.bounds = bounds;
            const auto result = PhysicsWorldSettings::Capture(descriptor);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::ProfileUnsupported.code.Value());
        }
    }
}  // namespace Horo::Physics
