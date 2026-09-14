#include "Horo/Physics/CharacterDiagnostics.h"
#include "Horo/Physics/CharacterErrors.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>

namespace Horo::Character {
    namespace {
        CharacterControllerHandle DiagnosticController() {
            const auto world = CharacterWorldId::Create(17);
            REQUIRE(world.HasValue());
            return {23, world.Value(), {4, 5}};
        }

        Physics::PhysicsWorldId DiagnosticPhysicsWorld() {
            const auto world = Physics::PhysicsWorldId::Create(29);
            REQUIRE(world.HasValue());
            return world.Value();
        }

        CharacterDiagnosticContext DiagnosticContext(const std::span<const CharacterDiagnosticMetadataEntry> metadata = {}) {
            return {DiagnosticController(), 23, 31, metadata};
        }

        void RequireDiagnosticFailure(const Result<CharacterDiagnosticRecord> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Character diagnostic categories expose stable presentation names", "[character][diagnostics]") {
        const std::array cases{
            std::pair{CharacterDiagnosticCategory::Command, std::string_view{"character.command"}},
            std::pair{CharacterDiagnosticCategory::Lifecycle, std::string_view{"character.lifecycle"}},
            std::pair{CharacterDiagnosticCategory::Solver, std::string_view{"character.solver"}},
            std::pair{CharacterDiagnosticCategory::Query, std::string_view{"character.query"}},
            std::pair{CharacterDiagnosticCategory::Capacity, std::string_view{"character.capacity"}},
        };
        for (const auto &[category, name] : cases)
            REQUIRE(CharacterDiagnosticCategoryName(category) == name);
        REQUIRE(CharacterDiagnosticCategoryName(static_cast<CharacterDiagnosticCategory>(255)).empty());
    }

    TEST_CASE("Character diagnostic records own mandatory identity timing and bounded metadata", "[character][diagnostics]") {
        const std::array metadata{
            CharacterDiagnosticMetadataEntry{CharacterDiagnosticMetadataKey::PhysicsWorld, DiagnosticPhysicsWorld()},
            CharacterDiagnosticMetadataEntry{CharacterDiagnosticMetadataKey::OperationSequence, std::uint64_t{37}},
            CharacterDiagnosticMetadataEntry{CharacterDiagnosticMetadataKey::RequestedCount, std::uint64_t{41}},
            CharacterDiagnosticMetadataEntry{CharacterDiagnosticMetadataKey::Capacity, std::uint64_t{43}},
        };
        auto error = MakeError(CharacterErrors::CapacityExceeded, "Contact scratch capacity was exhausted.");
        const auto record = MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Capacity, error, DiagnosticContext(metadata));
        REQUIRE(record.HasValue());
        REQUIRE(record.Value().schemaVersion == 1);
        REQUIRE(record.Value().controller == DiagnosticController());
        REQUIRE(record.Value().sceneGeneration == 23);
        REQUIRE(record.Value().simulationTick == 31);
        REQUIRE(record.Value().metadataCount == MaximumCharacterDiagnosticMetadataEntries);
        REQUIRE(record.Value().code.Value() == CharacterErrors::CapacityExceeded.code.Value());
        error.message.front() = 'X';
        REQUIRE(record.Value().Message() == "Contact scratch capacity was exhausted.");
    }

    TEST_CASE("Character diagnostics preserve an exact originating Physics error without parsing text", "[character][diagnostics]") {
        for (const ErrorCodeDescriptor *descriptor : Physics::PhysicsErrors::Descriptors()) {
            const auto declared = MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Solver,
                                                                WrapError(CharacterErrors::InvalidState, MakeError(*descriptor),
                                                                          "Character movement degraded for this tick."),
                                                                DiagnosticContext());
            REQUIRE(declared.HasValue());
            REQUIRE(declared.Value().originatingPhysicsCode.has_value());
            REQUIRE(declared.Value().originatingPhysicsCode->Value() == descriptor->code.Value());
        }

        const Error physics = MakeError(Physics::PhysicsErrors::SolverDeadlineExceeded, "native text may change");
        const Error character = WrapError(CharacterErrors::InvalidState, physics, "Character movement degraded for this tick.");
        const auto record = MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Solver, character, DiagnosticContext());
        REQUIRE(record.HasValue());
        REQUIRE(record.Value().code.Value() == CharacterErrors::InvalidState.code.Value());
        REQUIRE(record.Value().originatingPhysicsCode.has_value());
        REQUIRE(record.Value().originatingPhysicsCode->Value() == Physics::PhysicsErrors::SolverDeadlineExceeded.code.Value());

        Error invented = MakeError(Physics::PhysicsErrors::SolverDeadlineExceeded);
        invented.code = ErrorCode{"physics.invented"};
        const auto unknownCause = MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Solver,
                                                                WrapError(CharacterErrors::InvalidState, invented), DiagnosticContext());
        RequireDiagnosticFailure(unknownCause, CharacterErrors::DescriptorInvalid);

        Error tooDeep = MakeError(Physics::PhysicsErrors::InvalidState);
        for (std::size_t depth = 0; depth <= MaximumCharacterDiagnosticCauseDepth; ++depth)
            tooDeep = WrapError(CharacterErrors::InvalidState, std::move(tooDeep));
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Solver, tooDeep, DiagnosticContext()),
                                 CharacterErrors::DescriptorInvalid);
    }

    TEST_CASE("Character diagnostic construction rejects foreign errors and malformed context transactionally",
              "[character][diagnostics]") {
        const auto error = MakeError(CharacterErrors::RequestInvalid);
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Command,
                                                               MakeError(Physics::PhysicsErrors::InvalidState), DiagnosticContext()),
                                 CharacterErrors::DescriptorInvalid);
        auto badScene = DiagnosticContext();
        badScene.sceneGeneration = 99;
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Command, error, badScene),
                                 CharacterErrors::DescriptorInvalid);
        auto badTick = DiagnosticContext();
        badTick.simulationTick = 0;
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Command, error, badTick),
                                 CharacterErrors::DescriptorInvalid);
        auto badController = DiagnosticContext();
        badController.controller = {};
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Command, error, badController),
                                 CharacterErrors::DescriptorInvalid);
        auto emptyMessage = error;
        emptyMessage.message.clear();
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Command, emptyMessage, DiagnosticContext()),
                                 CharacterErrors::DescriptorInvalid);
        auto badSeverity = error;
        badSeverity.severity = static_cast<ErrorSeverity>(255);
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Command, badSeverity, DiagnosticContext()),
                                 CharacterErrors::DescriptorInvalid);
        const std::array duplicate{
            CharacterDiagnosticMetadataEntry{CharacterDiagnosticMetadataKey::Capacity, std::uint64_t{1}},
            CharacterDiagnosticMetadataEntry{CharacterDiagnosticMetadataKey::Capacity, std::uint64_t{2}},
        };
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Capacity, error, DiagnosticContext(duplicate)),
                                 CharacterErrors::DescriptorInvalid);
        const std::array wrongType{
            CharacterDiagnosticMetadataEntry{CharacterDiagnosticMetadataKey::PhysicsWorld, std::uint64_t{1}},
        };
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Capacity, error, DiagnosticContext(wrongType)),
                                 CharacterErrors::DescriptorInvalid);
        const std::array unknownKey{
            CharacterDiagnosticMetadataEntry{static_cast<CharacterDiagnosticMetadataKey>(255), std::uint64_t{1}},
        };
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Capacity, error, DiagnosticContext(unknownKey)),
                                 CharacterErrors::DescriptorInvalid);
        std::array<CharacterDiagnosticMetadataEntry, MaximumCharacterDiagnosticMetadataEntries + 1> tooMany{};
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Capacity, error, DiagnosticContext(tooMany)),
                                 CharacterErrors::DescriptorInvalid);
    }

    TEST_CASE("Character diagnostic messages accept the exact byte bound", "[character][diagnostics]") {
        auto error = MakeError(CharacterErrors::InvalidState);
        error.message.assign(MaximumCharacterDiagnosticMessageBytes, 'a');
        const auto exactBound = MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Lifecycle, error, DiagnosticContext());
        REQUIRE(exactBound.HasValue());
        REQUIRE(exactBound.Value().Message().size() == MaximumCharacterDiagnosticMessageBytes);
        REQUIRE(exactBound.Value().Message() == error.message);
        error.message.push_back('b');
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory::Lifecycle, error, DiagnosticContext()),
                                 CharacterErrors::DescriptorInvalid);
        RequireDiagnosticFailure(MakeCharacterDiagnosticRecord(static_cast<CharacterDiagnosticCategory>(255),
                                                               MakeError(CharacterErrors::InvalidState), DiagnosticContext()),
                                 CharacterErrors::OperationUnsupported);
    }

    TEST_CASE("Character diagnostic rate limiting is allocation-free bounded and tick ordered", "[character][diagnostics]") {
        const auto created = CharacterDiagnosticRateLimiter::Create({2, 3});
        REQUIRE(created.HasValue());
        auto limiter = created.Value();
        REQUIRE(limiter.TryAdmit(10) == CharacterDiagnosticAdmission::Emit);
        REQUIRE(limiter.TryAdmit(10) == CharacterDiagnosticAdmission::Emit);
        REQUIRE(limiter.TryAdmit(10) == CharacterDiagnosticAdmission::SuppressedRate);
        REQUIRE(limiter.TryAdmit(11) == CharacterDiagnosticAdmission::SuppressedRate);
        REQUIRE(limiter.TryAdmit(13) == CharacterDiagnosticAdmission::Emit);
        REQUIRE(limiter.TryAdmit(12) == CharacterDiagnosticAdmission::RejectedTick);
        REQUIRE(limiter.Statistics().emitted == 3);
        REQUIRE(limiter.Statistics().suppressed == 2);
        REQUIRE(limiter.Statistics().rejectedTicks == 1);

        REQUIRE(CharacterDiagnosticRateLimiter::Create({0, 1}).HasError());
        REQUIRE(CharacterDiagnosticRateLimiter::Create({1, 0}).HasError());
        REQUIRE(CharacterDiagnosticRateLimiter::Create({1'025, 1}).HasError());
        REQUIRE(CharacterDiagnosticRateLimiter::Create({1, 1'000'001}).HasError());
    }
}  // namespace Horo::Character
