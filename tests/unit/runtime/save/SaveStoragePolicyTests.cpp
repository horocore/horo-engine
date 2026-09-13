#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveStoragePolicy.h"
#include "SaveTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <utility>

namespace Horo::Runtime {
    namespace {
        using namespace Test;

        const ErrorCodeDescriptor NativeFailure{ErrorDomainId{"horo.platform"}, ErrorCode{"platform.filesystem.native_failure"},
                                                ErrorSeverity::Error, "Native storage failure.", "Inspect native storage state."};

        [[nodiscard]] SaveStorageReclaimableGeneration Reclaimable(const std::uint8_t slot, const std::uint8_t generation,
                                                                   const std::uint64_t bytes) {
            return {.slot = Id<SaveGameSlotId>(slot), .generation = Id<SlotGenerationId>(generation), .bytes = bytes};
        }

        [[nodiscard]] SaveStorageCapacitySnapshot Capacity(const std::uint64_t available,
                                                           const std::optional<std::uint64_t> quota = std::nullopt,
                                                           const std::uint64_t reserved = 0,
                                                           const SaveStorageAccess access = SaveStorageAccess::ReadWrite,
                                                           const std::span<const SaveStorageReclaimableGeneration> reclaimable = {}) {
            auto created = SaveStorageCapacitySnapshot::Create({.access = access,
                                                                .availableBytes = available,
                                                                .quotaRemainingBytes = quota,
                                                                .reservedBytes = reserved,
                                                                .reclaimable = reclaimable});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        TEST_CASE("Save storage capacity snapshots own bounded ordered recovery evidence", "[unit][runtime][save][storage-policy]") {
            const std::array reclaimable{Reclaimable(1, 1, 40), Reclaimable(1, 2, 60)};
            const SaveStorageCapacitySnapshot snapshot = Capacity(1'000, 900, 100, SaveStorageAccess::ReadWrite, reclaimable);

            CHECK(snapshot.Access() == SaveStorageAccess::ReadWrite);
            CHECK(snapshot.AvailableBytes() == 1'000);
            CHECK(snapshot.QuotaRemainingBytes() == 900);
            CHECK(snapshot.ReservedBytes() == 100);
            REQUIRE(snapshot.Reclaimable().size() == reclaimable.size());
            CHECK(std::ranges::equal(snapshot.Reclaimable(), reclaimable));
            CHECK(snapshot.ReclaimableBytes() == 100);

            const std::array unordered{reclaimable[1], reclaimable[0]};
            CHECK(SaveStorageCapacitySnapshot::Create({.access = SaveStorageAccess::ReadWrite, .reclaimable = unordered}).HasError());
            CHECK(SaveStorageCapacitySnapshot::Create({.access = SaveStorageAccess::Count}).HasError());
            CHECK(SaveStorageCapacitySnapshot::Create(
                      {.access = SaveStorageAccess::ReadWrite,
                       .reclaimable = std::array{Reclaimable(1, 1, std::numeric_limits<std::uint64_t>::max()), Reclaimable(1, 2, 1)}})
                      .HasError());
        }

        TEST_CASE("Preflight reserves peak and safety bytes without treating reclaimable generations as free",
                  "[unit][runtime][save][storage-policy]") {
            const std::array reclaimable{Reclaimable(1, 1, 500)};
            const SaveStorageCapacitySnapshot snapshot = Capacity(200, std::nullopt, 50, SaveStorageAccess::ReadWrite, reclaimable);

            const auto admitted = EvaluateSaveStoragePreflight(snapshot, {.additionalPeakBytes = 100, .safetyReserveBytes = 50});
            REQUIRE(admitted.HasValue());
            CHECK(admitted.Value().IsAdmitted());
            CHECK(admitted.Value().effectiveAvailableBytes == 150);

            const auto rejected = EvaluateSaveStoragePreflight(snapshot, {.additionalPeakBytes = 151});
            REQUIRE(rejected.HasValue());
            CHECK(rejected.Value().outcome == SaveStoragePreflightOutcome::InsufficientSpace);
            CHECK(rejected.Value().shortfallBytes == 1);
            CHECK(rejected.Value().reclaimableBytes == 500);
            REQUIRE(snapshot.Reclaimable().size() == reclaimable.size());
            CHECK(std::ranges::equal(snapshot.Reclaimable(), reclaimable));
        }

        TEST_CASE("Provider quota and access state produce distinct conservative preflight outcomes",
                  "[unit][runtime][save][storage-policy]") {
            const auto quota = EvaluateSaveStoragePreflight(Capacity(1'000, 300, 100), {.additionalPeakBytes = 201});
            REQUIRE(quota.HasValue());
            CHECK(quota.Value().outcome == SaveStoragePreflightOutcome::QuotaExceeded);
            CHECK(quota.Value().effectiveAvailableBytes == 200);

            const auto readOnly =
                EvaluateSaveStoragePreflight(Capacity(1'000, {}, 0, SaveStorageAccess::ReadOnly), {.additionalPeakBytes = 1});
            const auto unavailable =
                EvaluateSaveStoragePreflight(Capacity(1'000, {}, 0, SaveStorageAccess::Unavailable), {.additionalPeakBytes = 1});
            REQUIRE(readOnly.HasValue());
            REQUIRE(unavailable.HasValue());
            CHECK(readOnly.Value().outcome == SaveStoragePreflightOutcome::ReadOnly);
            CHECK(unavailable.Value().outcome == SaveStoragePreflightOutcome::Unavailable);
        }

        TEST_CASE("Preflight rejects zero and overflowing peak estimates", "[unit][runtime][save][storage-policy]") {
            const auto capacity = Capacity(std::numeric_limits<std::uint64_t>::max());
            CHECK(EvaluateSaveStoragePreflight(capacity, {}).HasError());
            CHECK(EvaluateSaveStoragePreflight(capacity,
                                               {.additionalPeakBytes = std::numeric_limits<std::uint64_t>::max(), .safetyReserveBytes = 1})
                      .HasError());
        }

        TEST_CASE("Storage failures preserve typed native causes and give portable user actions", "[unit][runtime][save][storage-policy]") {
            const std::array cases{
                std::pair{SaveStorageFailureCategory::DiskFull, SaveStorageRecoveryAction::FreeSpace},
                std::pair{SaveStorageFailureCategory::QuotaExceeded, SaveStorageRecoveryAction::FreeSpace},
                std::pair{SaveStorageFailureCategory::PermissionDenied, SaveStorageRecoveryAction::FixPermissions},
                std::pair{SaveStorageFailureCategory::ReadOnly, SaveStorageRecoveryAction::SelectWritableStorage},
                std::pair{SaveStorageFailureCategory::VolumeUnavailable, SaveStorageRecoveryAction::ReconnectVolume},
                std::pair{SaveStorageFailureCategory::PermanentIo, SaveStorageRecoveryAction::Abort},
            };
            for (const auto &[category, action] : cases) {
                auto decision = MakeSaveStorageFailureDecision({.category = category,
                                                                .commitOutcome = SaveOperationCommitOutcome::NotCommitted,
                                                                .nativeCause = MakeError(NativeFailure)});
                REQUIRE(decision.HasValue());
                CHECK(decision.Value().Category() == category);
                CHECK(decision.Value().Action() == action);
                CHECK_FALSE(decision.Value().CanRetryAutomatically());
                REQUIRE(decision.Value().ErrorValue().cause);
                CHECK(decision.Value().ErrorValue().cause.Get()->code.Value() == NativeFailure.code.Value());
            }
        }

        TEST_CASE("Transient storage retry is finite and publication uncertainty always reconciles",
                  "[unit][runtime][save][storage-policy]") {
            auto retry = MakeSaveStorageFailureDecision({.category = SaveStorageFailureCategory::TransientIo,
                                                         .commitOutcome = SaveOperationCommitOutcome::NotCommitted,
                                                         .completedAutomaticRetries = 1,
                                                         .maximumAutomaticRetries = 3,
                                                         .nativeCause = MakeError(NativeFailure)});
            REQUIRE(retry.HasValue());
            CHECK(retry.Value().Action() == SaveStorageRecoveryAction::RetryWithBackoff);
            CHECK(retry.Value().CanRetryAutomatically());
            CHECK(retry.Value().RemainingAutomaticRetries() == 2);

            auto exhausted = MakeSaveStorageFailureDecision({.category = SaveStorageFailureCategory::TransientIo,
                                                             .commitOutcome = SaveOperationCommitOutcome::NotCommitted,
                                                             .completedAutomaticRetries = 3,
                                                             .maximumAutomaticRetries = 3,
                                                             .nativeCause = MakeError(NativeFailure)});
            REQUIRE(exhausted.HasValue());
            CHECK(exhausted.Value().Action() == SaveStorageRecoveryAction::Abort);
            CHECK_FALSE(exhausted.Value().CanRetryAutomatically());

            auto unknown = MakeSaveStorageFailureDecision({.category = SaveStorageFailureCategory::DiskFull,
                                                           .commitOutcome = SaveOperationCommitOutcome::Unknown,
                                                           .maximumAutomaticRetries = 3,
                                                           .nativeCause = MakeError(NativeFailure)});
            REQUIRE(unknown.HasValue());
            CHECK(unknown.Value().Action() == SaveStorageRecoveryAction::ReconcilePublication);
            CHECK_FALSE(unknown.Value().CanRetryAutomatically());
        }

        TEST_CASE("Malformed retry policy is rejected before recovery action selection", "[unit][runtime][save][storage-policy]") {
            CHECK(MakeSaveStorageFailureDecision({.category = SaveStorageFailureCategory::Count, .nativeCause = MakeError(NativeFailure)})
                      .HasError());
            CHECK(MakeSaveStorageFailureDecision({.category = SaveStorageFailureCategory::TransientIo,
                                                  .completedAutomaticRetries = 2,
                                                  .maximumAutomaticRetries = 1,
                                                  .nativeCause = MakeError(NativeFailure)})
                      .HasError());
            CHECK(MakeSaveStorageFailureDecision({.category = SaveStorageFailureCategory::TransientIo,
                                                  .maximumAutomaticRetries = MaximumSaveStorageAutomaticRetries + 1,
                                                  .nativeCause = MakeError(NativeFailure)})
                      .HasError());
        }
    }  // namespace
}  // namespace Horo::Runtime
