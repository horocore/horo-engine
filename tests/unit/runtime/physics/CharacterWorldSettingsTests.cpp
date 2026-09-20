#include "Horo/Physics/CharacterErrors.h"
#include "Horo/Physics/CharacterWorldSettings.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace Horo::Character {
    namespace {
        void ExpectError(const Result<CharacterWorldSettings> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        TEST_CASE("Character world settings capture deterministic immutable defaults", "[character][settings]") {
            CharacterWorldSettingsDescriptor source;
            const auto equal = CharacterWorldSettings::Capture(source);
            auto captured = CharacterWorldSettings::Capture(source);
            REQUIRE(captured.HasValue());
            REQUIRE(equal.HasValue());
            REQUIRE(captured.Value().Values() == source);
            REQUIRE(captured.Value().Identity() == equal.Value().Identity());
            REQUIRE_FALSE(captured.Value().Values().history.Enabled());

            source.capacities.maximumControllers = 1;
            REQUIRE(captured.Value().Values().capacities.maximumControllers == 4'096);
            static_assert(!std::is_default_constructible_v<CharacterWorldSettings>);
            static_assert(std::is_copy_constructible_v<CharacterWorldSettings>);
            static_assert(std::is_nothrow_move_constructible_v<CharacterWorldSettings>);
            static_assert(!std::is_copy_assignable_v<CharacterWorldSettings>);
            static_assert(!std::is_move_assignable_v<CharacterWorldSettings>);

            auto moved = std::move(captured.Value());
            REQUIRE(moved.Values().capacities.maximumControllers == 4'096);
            REQUIRE(moved.Identity() == equal.Value().Identity());
        }

        TEST_CASE("Character world settings identity has an independent schema-1 fixture", "[character][settings]") {
            const auto captured = CharacterWorldSettings::Capture({});
            REQUIRE(captured.HasValue());
            // SHA-256 of schema-1's 19 little-endian uint64 words, independently encoded with Python struct.pack.
            REQUIRE(FormatSha256(captured.Value().Identity().digest) ==
                    "sha256:3b5f1861da4d0bc20ba8531df70d5f0325477b6682f1df0e826ab8ecafbdd724");
        }

        TEST_CASE("Every retained Character capacity rejects zero and its hard ceiling plus one", "[character][settings]") {
            struct Field final {
                std::uint32_t CharacterWorldCapacities::*member;
                std::uint32_t maximum;
            };

            constexpr std::array fields{
                Field{&CharacterWorldCapacities::maximumControllers, CharacterWorldSettingLimits::MaximumControllers},
                Field{&CharacterWorldCapacities::maximumQueuedCommands, CharacterWorldSettingLimits::MaximumQueuedCommands},
                Field{&CharacterWorldCapacities::maximumRetainedContacts, CharacterWorldSettingLimits::MaximumRetainedContacts},
                Field{&CharacterWorldCapacities::maximumQueuedEvents, CharacterWorldSettingLimits::MaximumQueuedEvents},
                Field{&CharacterWorldCapacities::maximumQueuedQueries, CharacterWorldSettingLimits::MaximumQueuedQueries},
                Field{&CharacterWorldCapacities::maximumStagedImpulses, CharacterWorldSettingLimits::MaximumStagedImpulses},
                Field{&CharacterWorldCapacities::maximumDiagnosticRecords, CharacterWorldSettingLimits::MaximumDiagnosticRecords},
                Field{&CharacterWorldCapacities::maximumDebugPrimitives, CharacterWorldSettingLimits::MaximumDebugPrimitives},
            };

            for (const auto field : fields) {
                CharacterWorldSettingsDescriptor descriptor;
                descriptor.capacities.*field.member = 0;
                ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::DescriptorInvalid);
                descriptor = {};
                descriptor.capacities.*field.member = field.maximum + 1;
                ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::CapacityExceeded);
            }
        }

        TEST_CASE("Character work budgets reject empty non-finite excessive and inconsistent values", "[character][settings]") {
            struct CountField final {
                std::uint32_t CharacterWorldWorkBudgets::*member;
            };

            constexpr std::array countFields{
                CountField{&CharacterWorldWorkBudgets::maximumCommandsPerTick},
                CountField{&CharacterWorldWorkBudgets::maximumQueriesPerTick},
                CountField{&CharacterWorldWorkBudgets::maximumContactsPerMovement},
                CountField{&CharacterWorldWorkBudgets::maximumMovementIterations},
            };
            for (const auto field : countFields) {
                CharacterWorldSettingsDescriptor descriptor;
                descriptor.work.*field.member = 0;
                ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::DescriptorInvalid);
            }
            CharacterWorldSettingsDescriptor zeroRecovery;
            zeroRecovery.work.maximumRecoveryIterations = 0;
            REQUIRE(CharacterWorldSettings::Capture(zeroRecovery).HasValue());

            CharacterWorldSettingsDescriptor descriptor;
            descriptor.work.scratchBytes = 0;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::DescriptorInvalid);
            descriptor = {};
            descriptor.work.maximumDisplacementMetersPerTick = std::numeric_limits<float>::infinity();
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::DescriptorInvalid);
            descriptor.work.maximumDisplacementMetersPerTick = std::numeric_limits<float>::quiet_NaN();
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::DescriptorInvalid);

            descriptor = {};
            descriptor.work.maximumCommandsPerTick = descriptor.capacities.maximumQueuedCommands + 1;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::CapacityExceeded);
            descriptor = {};
            descriptor.work.maximumQueriesPerTick = descriptor.capacities.maximumQueuedQueries + 1;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::CapacityExceeded);
            descriptor = {};
            descriptor.work.maximumContactsPerMovement = MaximumCharacterContacts + 1;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::CapacityExceeded);
            descriptor = {};
            descriptor.work.maximumMovementIterations = CharacterWorldSettingLimits::MaximumMovementIterations + 1;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::CapacityExceeded);
            descriptor = {};
            descriptor.work.maximumRecoveryIterations = CharacterWorldSettingLimits::MaximumRecoveryIterations + 1;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::CapacityExceeded);
            descriptor = {};
            descriptor.work.scratchBytes = CharacterWorldSettingLimits::MaximumScratchBytes + 1;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::CapacityExceeded);
            descriptor = {};
            descriptor.work.maximumDisplacementMetersPerTick = CharacterWorldSettingLimits::MaximumDisplacementMetersPerTick + 1.0F;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::CapacityExceeded);
        }

        TEST_CASE("Retained contacts cover the checked aggregate controller result bound", "[character][settings]") {
            CharacterWorldSettingsDescriptor descriptor;
            descriptor.capacities.maximumControllers = CharacterWorldSettingLimits::MaximumControllers;
            descriptor.work.maximumContactsPerMovement = MaximumCharacterContacts;
            descriptor.capacities.maximumRetainedContacts = CharacterWorldSettingLimits::MaximumRetainedContacts;
            REQUIRE(CharacterWorldSettings::Capture(descriptor).HasValue());

            --descriptor.capacities.maximumRetainedContacts;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::DescriptorInvalid);
        }

        TEST_CASE("Character history is coherently disabled or fully bounded", "[character][settings]") {
            CharacterWorldSettingsDescriptor descriptor;
            REQUIRE(CharacterWorldSettings::Capture(descriptor).HasValue());

            descriptor.history.maximumCheckpoints = 1;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::DescriptorInvalid);

            descriptor.history = {8, 4096, 8};
            REQUIRE(CharacterWorldSettings::Capture(descriptor).HasValue());
            descriptor.history.maximumResimulationTicks = 9;
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::DescriptorInvalid);
            descriptor.history = {8, 7, 8};
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::DescriptorInvalid);
            descriptor.history = {CharacterWorldSettingLimits::MaximumHistoryCheckpoints + 1, 4096, 1};
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::CapacityExceeded);
            descriptor.history = {1, CharacterWorldSettingLimits::MaximumHistoryBytes + 1, 1};
            ExpectError(CharacterWorldSettings::Capture(descriptor), CharacterErrors::CapacityExceeded);
            descriptor.history = {CharacterWorldSettingLimits::MaximumHistoryCheckpoints, CharacterWorldSettingLimits::MaximumHistoryBytes,
                                  CharacterWorldSettingLimits::MaximumResimulationTicks};
            REQUIRE(CharacterWorldSettings::Capture(descriptor).HasValue());
        }

        TEST_CASE("Canonical Character settings identity changes with every policy group", "[character][settings]") {
            CharacterWorldSettingsDescriptor first;
            auto second = first;
            const auto baseline = CharacterWorldSettings::Capture(first);
            REQUIRE(baseline.HasValue());

            ++second.capacities.maximumQueuedEvents;
            const auto capacityIdentity = CharacterWorldSettings::Capture(second);
            REQUIRE(capacityIdentity.HasValue());
            REQUIRE(capacityIdentity.Value().Identity() != baseline.Value().Identity());

            second = first;
            ++second.work.maximumMovementIterations;
            const auto workIdentity = CharacterWorldSettings::Capture(second);
            REQUIRE(workIdentity.HasValue());
            REQUIRE(workIdentity.Value().Identity() != baseline.Value().Identity());

            second = first;
            second.history = {8, 4096, 8};
            const auto historyIdentity = CharacterWorldSettings::Capture(second);
            REQUIRE(historyIdentity.HasValue());
            REQUIRE(historyIdentity.Value().Identity() != baseline.Value().Identity());
        }
    }  // namespace
}  // namespace Horo::Character
