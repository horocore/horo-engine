#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorldBudgets.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics {
    TEST_CASE("World scratch exhaustion never promises unsupported native recovery", "[physics][settings][budget]") {
        PhysicsWorldBudgets budgets;
        REQUIRE(budgets.scratchExhaustion == PhysicsScratchExhaustionPolicy::FatalProcess);
        for (const auto policy : {PhysicsScratchExhaustionPolicy::FailTick, static_cast<PhysicsScratchExhaustionPolicy>(255)}) {
            budgets.scratchExhaustion = policy;
            const auto result = ValidatePhysicsWorldBudgets(budgets);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::OperationUnsupported.code.Value());
        }
    }

    TEST_CASE("World event overflow policy is explicit and closed", "[physics][settings][budget]") {
        PhysicsWorldBudgets budgets;
        for (const auto policy : {PhysicsEventOverflowPolicy::DropNewest, PhysicsEventOverflowPolicy::FailTick}) {
            budgets.eventOverflow = policy;
            REQUIRE(ValidatePhysicsWorldBudgets(budgets).HasValue());
        }
        budgets.eventOverflow = static_cast<PhysicsEventOverflowPolicy>(255);
        const auto result = ValidatePhysicsWorldBudgets(budgets);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::OperationUnsupported.code.Value());
    }

    TEST_CASE("World budget defaults and exact ceilings are admitted", "[physics][settings][budget]") {
        REQUIRE(ValidatePhysicsWorldBudgets({}).HasValue());
        const PhysicsWorldBudgets maximum{.maximumShapes = MaximumPhysicsResourceRecords,
                                          .maximumContactPairs = MaximumPhysicsResourceRecords,
                                          .maximumContactConstraints = MaximumPhysicsResourceRecords,
                                          .maximumInFlightPairs = MaximumPhysicsResourceRecords,
                                          .maximumCommands = MaximumPhysicsBufferEntries,
                                          .maximumEvents = MaximumPhysicsBufferEntries,
                                          .maximumQueries = MaximumPhysicsBufferEntries,
                                          .maximumCommandsPerTick = MaximumPhysicsBufferEntries,
                                          .maximumQueriesPerTick = MaximumPhysicsBufferEntries,
                                          .scratchBytes = MaximumPhysicsScratchBytes,
                                          .residentShapeBytes = MaximumPhysicsResidentShapeBytes};
        REQUIRE(ValidatePhysicsWorldBudgets(maximum).HasValue());
    }

    TEST_CASE("Every independent world count rejects zero and excessive values", "[physics][settings][budget]") {
        struct Field {
            std::uint32_t PhysicsWorldBudgets::*member;
            std::uint32_t maximum;
        };

        const std::array fields{
            Field{&PhysicsWorldBudgets::maximumShapes, MaximumPhysicsResourceRecords},
            Field{&PhysicsWorldBudgets::maximumContactPairs, MaximumPhysicsResourceRecords},
            Field{&PhysicsWorldBudgets::maximumContactConstraints, MaximumPhysicsResourceRecords},
            Field{&PhysicsWorldBudgets::maximumInFlightPairs, MaximumPhysicsResourceRecords},
            Field{&PhysicsWorldBudgets::maximumCommands, MaximumPhysicsBufferEntries},
            Field{&PhysicsWorldBudgets::maximumEvents, MaximumPhysicsBufferEntries},
            Field{&PhysicsWorldBudgets::maximumQueries, MaximumPhysicsBufferEntries},
            Field{&PhysicsWorldBudgets::maximumCommandsPerTick, MaximumPhysicsBufferEntries},
            Field{&PhysicsWorldBudgets::maximumQueriesPerTick, MaximumPhysicsBufferEntries},
        };
        for (const auto &field : fields) {
            for (const std::uint32_t invalid : {0U, field.maximum + 1}) {
                PhysicsWorldBudgets budgets;
                budgets.*field.member = invalid;
                const auto result = ValidatePhysicsWorldBudgets(budgets);
                REQUIRE(result.HasError());
                REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::CapacityExceeded.code.Value());
                REQUIRE(budgets.*field.member == invalid);
            }
        }
    }

    TEST_CASE("World memory budgets reject empty and oversized reservations", "[physics][settings][budget]") {
        PhysicsWorldBudgets budgets;
        for (const std::uint64_t invalid : {std::uint64_t{0}, MaximumPhysicsScratchBytes + 1}) {
            budgets.scratchBytes = invalid;
            const auto result = ValidatePhysicsWorldBudgets(budgets);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::CapacityExceeded.code.Value());
        }
        budgets = {};
        for (const std::uint64_t invalid : {std::uint64_t{0}, MaximumPhysicsResidentShapeBytes + 1}) {
            budgets.residentShapeBytes = invalid;
            const auto result = ValidatePhysicsWorldBudgets(budgets);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::CapacityExceeded.code.Value());
        }
    }

    TEST_CASE("World per-tick and in-flight budgets cannot exceed retained capacity", "[physics][settings][budget]") {
        using Mutation = void (*)(PhysicsWorldBudgets &);
        const std::array<Mutation, 3> mutations{
            [](auto &b) {
            b.maximumInFlightPairs = b.maximumContactPairs + 1;
        },
            [](auto &b) {
            b.maximumCommandsPerTick = b.maximumCommands + 1;
        },
            [](auto &b) {
            b.maximumQueriesPerTick = b.maximumQueries + 1;
        },
        };
        for (const auto mutate : mutations) {
            PhysicsWorldBudgets budgets;
            mutate(budgets);
            const auto result = ValidatePhysicsWorldBudgets(budgets);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
        }
    }
}  // namespace Horo::Physics
