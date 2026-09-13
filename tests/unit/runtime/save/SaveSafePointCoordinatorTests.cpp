#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveSafePointCoordinator.h"
#include "TypedIdentityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using Tests::RequireError;

        constexpr SaveRuntimeGeneration Generation(const std::uint64_t runtime = 1, const std::uint64_t scene = 2,
                                                   const std::uint64_t registry = 3) {
            return {.runtime = runtime, .scene = scene, .registry = registry};
        }

        class RecordingExecutor final : public ISaveSafePointExecutor {
        public:
            Result<void> Capture(const OperationId operation, const SaveRuntimeGeneration generation) override {
                captureThread = std::this_thread::get_id();
                captures.push_back(operation);
                observedGenerations.push_back(generation);
                if (captureHook)
                    captureHook();
                if (throwFromCapture)
                    throw std::runtime_error("unexpected capture callback failure");
                if (captureError.has_value())
                    return Result<void>::Failure(*captureError);
                return Result<void>::Success();
            }

            Result<void> Restore(const OperationId operation, const SaveRuntimeGeneration generation) override {
                restoreThread = std::this_thread::get_id();
                restores.push_back(operation);
                observedGenerations.push_back(generation);
                if (restoreError.has_value())
                    return Result<void>::Failure(*restoreError);
                return Result<void>::Success();
            }

            std::vector<OperationId> captures;
            std::vector<OperationId> restores;
            std::vector<SaveRuntimeGeneration> observedGenerations;
            std::optional<Error> captureError;
            std::optional<Error> restoreError;
            std::function<void()> captureHook;
            bool throwFromCapture{};
            std::thread::id captureThread;
            std::thread::id restoreThread;
        };

        std::unique_ptr<SaveSafePointCoordinator> Coordinator(const SaveRuntimeGeneration generation = Generation(),
                                                              const std::size_t maximumOperations = 8) {
            auto created = SaveSafePointCoordinator::Create(generation, maximumOperations);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        SaveSafePointOperationSnapshot Snapshot(const SaveSafePointCoordinator &coordinator, const OperationId operation) {
            const auto snapshot = coordinator.Snapshot(operation);
            REQUIRE(snapshot.HasValue());
            return snapshot.Value();
        }

        SaveSafePointDrainResult Commit(SaveSafePointCoordinator &coordinator, RecordingExecutor &executor,
                                        const SaveRuntimeGeneration generation = Generation(), const std::size_t limit = 8) {
            const auto result = coordinator.CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation, limit, executor);
            REQUIRE(result.HasValue());
            return result.Value();
        }
    }  // namespace

    TEST_CASE("Save safe-point coordinator admits only bounded current-generation work", "[unit][save][lifecycle]") {
        RequireError(SaveSafePointCoordinator::Create({}, 1), SaveErrors::LifecycleInvalid);
        RequireError(SaveSafePointCoordinator::Create(Generation(), 0), SaveErrors::LifecycleInvalid);
        RequireError(SaveSafePointCoordinator::Create(Generation(), MaximumSaveLifecycleOperations + 1), SaveErrors::LifecycleInvalid);

        auto coordinator = Coordinator(Generation(), 1);
        REQUIRE(coordinator->Admit({.operation = 11, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        RequireError(coordinator->Admit({.operation = 11, .action = SaveSafePointAction::Capture, .generation = Generation()}),
                     SaveErrors::OperationInvalid);
        RequireError(coordinator->Admit({.operation = 12, .action = SaveSafePointAction::Capture, .generation = Generation()}),
                     SaveErrors::LifecycleCapacityExceeded);
        RequireError(coordinator->Snapshot(99), SaveErrors::OperationInvalid);
    }

    TEST_CASE("Capture touches simulation only at the lifecycle commit safe point", "[unit][save][safe-point]") {
        auto coordinator = Coordinator();
        RecordingExecutor executor;
        REQUIRE(coordinator->Admit({.operation = 21, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());

        RequireError(coordinator->CommitAtSafePoint(RuntimePhase::FixedUpdate, Generation(), 1, executor), SaveErrors::SafePointInvalid);
        RequireError(coordinator->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, Generation(), 0, executor),
                     SaveErrors::SafePointInvalid);
        CHECK(executor.captures.empty());

        const SaveSafePointDrainResult drain = Commit(*coordinator, executor, Generation(), 1);
        CHECK(drain.inspected == 1);
        CHECK(drain.captured == 1);
        CHECK(executor.captures == std::vector<OperationId>{21});
        CHECK(executor.captureThread == std::this_thread::get_id());
        CHECK(Snapshot(*coordinator, 21).state == SaveSafePointOperationState::DetachedWork);

        REQUIRE(
            coordinator
                ->PublishWorkerCompletion({.operation = 21, .generation = Generation(), .outcome = SaveWorkerCompletionOutcome::Succeeded})
                .HasValue());
        CHECK(Snapshot(*coordinator, 21).state == SaveSafePointOperationState::Completed);
    }

    TEST_CASE("Restore worker completion is queued until the next exact safe point", "[unit][save][safe-point]") {
        auto coordinator = Coordinator();
        RecordingExecutor executor;
        REQUIRE(coordinator->Admit({.operation = 31, .action = SaveSafePointAction::Restore, .generation = Generation()}).HasValue());

        std::promise<Result<void>> publication;
        std::thread worker([&] {
            publication.set_value(coordinator->PublishWorkerCompletion(
                {.operation = 31, .generation = Generation(), .outcome = SaveWorkerCompletionOutcome::Succeeded}));
        });
        worker.join();
        REQUIRE(publication.get_future().get().HasValue());
        CHECK(executor.restores.empty());
        CHECK(Snapshot(*coordinator, 31).state == SaveSafePointOperationState::ReadyToApply);

        const SaveSafePointDrainResult drain = Commit(*coordinator, executor, Generation(), 1);
        CHECK(drain.restored == 1);
        CHECK(executor.restores == std::vector<OperationId>{31});
        CHECK(executor.restoreThread == std::this_thread::get_id());
        CHECK(Snapshot(*coordinator, 31).state == SaveSafePointOperationState::Completed);
    }

    TEST_CASE("Scene transition rejects uncaptured and restore work but preserves detached capture", "[unit][save][lifecycle]") {
        auto coordinator = Coordinator();
        RecordingExecutor executor;
        REQUIRE(coordinator->Admit({.operation = 41, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        REQUIRE(coordinator->Admit({.operation = 42, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        REQUIRE(coordinator->Admit({.operation = 43, .action = SaveSafePointAction::Restore, .generation = Generation()}).HasValue());
        const auto capture = coordinator->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, Generation(), 1, executor);
        REQUIRE(capture.HasValue());
        REQUIRE(
            coordinator
                ->PublishWorkerCompletion({.operation = 43, .generation = Generation(), .outcome = SaveWorkerCompletionOutcome::Succeeded})
                .HasValue());

        const SaveRuntimeGeneration replacement = Generation(1, 4, 3);
        REQUIRE(coordinator->TransitionScene(replacement).HasValue());
        CHECK(Snapshot(*coordinator, 41).state == SaveSafePointOperationState::DetachedWork);
        CHECK(Snapshot(*coordinator, 42).state == SaveSafePointOperationState::Stale);
        CHECK(Snapshot(*coordinator, 43).state == SaveSafePointOperationState::Stale);
        CHECK(Snapshot(*coordinator, 43).disposition == SaveLifecycleDisposition::SceneTransition);

        REQUIRE(
            coordinator
                ->PublishWorkerCompletion({.operation = 41, .generation = Generation(), .outcome = SaveWorkerCompletionOutcome::Succeeded})
                .HasValue());
        CHECK(Snapshot(*coordinator, 41).state == SaveSafePointOperationState::Completed);
        RequireError(coordinator->PublishWorkerCompletion(
                         {.operation = 43, .generation = Generation(), .outcome = SaveWorkerCompletionOutcome::Succeeded}),
                     SaveErrors::CompletionInvalid);
        RequireError(coordinator->Admit({.operation = 44, .action = SaveSafePointAction::Capture, .generation = Generation()}),
                     SaveErrors::GenerationStale);
    }

    TEST_CASE("Registry rebind is owner-quiescent and fences work to its exact revision", "[unit][save][lifecycle]") {
        auto coordinator = Coordinator();
        RecordingExecutor executor;
        REQUIRE(coordinator->Admit({.operation = 45, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        REQUIRE(coordinator->Admit({.operation = 46, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        REQUIRE(coordinator->Admit({.operation = 47, .action = SaveSafePointAction::Restore, .generation = Generation()}).HasValue());
        CHECK(Commit(*coordinator, executor, Generation(), 1).captured == 1);

        const SaveRuntimeGeneration rebound = Generation(1, 2, 4);
        REQUIRE(coordinator->RebindRegistry(rebound).HasValue());
        CHECK(Snapshot(*coordinator, 45).state == SaveSafePointOperationState::DetachedWork);
        CHECK(Snapshot(*coordinator, 46).disposition == SaveLifecycleDisposition::RegistryRebind);
        CHECK(Snapshot(*coordinator, 47).state == SaveSafePointOperationState::Stale);
        REQUIRE(
            coordinator
                ->PublishWorkerCompletion({.operation = 45, .generation = Generation(), .outcome = SaveWorkerCompletionOutcome::Succeeded})
                .HasValue());
        RequireError(coordinator->Admit({.operation = 48, .action = SaveSafePointAction::Capture, .generation = Generation()}),
                     SaveErrors::GenerationStale);
        RequireError(coordinator->RebindRegistry(Generation(1, 9, 5)), SaveErrors::LifecycleInvalid);
    }

    TEST_CASE("Suspension defers queued work and resume preserves its exact outcome", "[unit][save][lifecycle]") {
        auto coordinator = Coordinator();
        RecordingExecutor executor;
        REQUIRE(coordinator->Admit({.operation = 51, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        REQUIRE(coordinator->SetSuspended(true).HasValue());
        RequireError(coordinator->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, Generation(), 1, executor),
                     SaveErrors::LifecycleSuspended);
        CHECK(Snapshot(*coordinator, 51).state == SaveSafePointOperationState::AwaitingSafePoint);
        CHECK(executor.captures.empty());

        REQUIRE(coordinator->SetSuspended(false).HasValue());
        CHECK(Commit(*coordinator, executor, Generation(), 1).captured == 1);
    }

    TEST_CASE("PIE stop and shutdown deterministically cancel nonterminal operations", "[unit][save][lifecycle]") {
        auto coordinator = Coordinator();
        REQUIRE(coordinator->Admit({.operation = 61, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        REQUIRE(coordinator->Admit({.operation = 62, .action = SaveSafePointAction::Restore, .generation = Generation()}).HasValue());
        REQUIRE(coordinator->OnPieStop().HasValue());
        CHECK(Snapshot(*coordinator, 61).state == SaveSafePointOperationState::Cancelled);
        CHECK(Snapshot(*coordinator, 61).disposition == SaveLifecycleDisposition::PieStop);
        CHECK(Snapshot(*coordinator, 62).state == SaveSafePointOperationState::Cancelled);

        RequireError(coordinator->Admit({.operation = 63, .action = SaveSafePointAction::Capture, .generation = Generation()}),
                     SaveErrors::LifecycleUnavailable);
        const SaveRuntimeGeneration replacement = Generation(2, 4, 5);
        REQUIRE(coordinator->TransitionScene(replacement).HasValue());
        REQUIRE(coordinator->Admit({.operation = 63, .action = SaveSafePointAction::Capture, .generation = replacement}).HasValue());
        REQUIRE(coordinator->BeginShutdown().HasValue());
        REQUIRE(coordinator->BeginShutdown().HasValue());
        CHECK(Snapshot(*coordinator, 63).disposition == SaveLifecycleDisposition::HostShutdown);
        RequireError(coordinator->Admit({.operation = 64, .action = SaveSafePointAction::Capture, .generation = Generation()}),
                     SaveErrors::LifecycleUnavailable);
    }

    TEST_CASE("Owner-thread violations and stale worker generations produce typed diagnostics", "[unit][save][affinity]") {
        auto coordinator = Coordinator();
        RecordingExecutor executor;
        std::promise<Result<void>> admission;
        std::promise<Result<SaveSafePointDrainResult>> commit;
        std::promise<Result<void>> registryRebind;
        std::thread worker([&] {
            admission.set_value(coordinator->Admit({.operation = 71, .action = SaveSafePointAction::Capture, .generation = Generation()}));
            commit.set_value(coordinator->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, Generation(), 1, executor));
            registryRebind.set_value(coordinator->RebindRegistry(Generation(1, 2, 4)));
        });
        worker.join();
        RequireError(admission.get_future().get(), SaveErrors::ThreadAffinityViolation);
        RequireError(commit.get_future().get(), SaveErrors::ThreadAffinityViolation);
        RequireError(registryRebind.get_future().get(), SaveErrors::ThreadAffinityViolation);

        REQUIRE(coordinator->Admit({.operation = 72, .action = SaveSafePointAction::Restore, .generation = Generation()}).HasValue());
        RequireError(coordinator->PublishWorkerCompletion(
                         {.operation = 72, .generation = Generation(1, 99, 3), .outcome = SaveWorkerCompletionOutcome::Succeeded}),
                     SaveErrors::GenerationStale);
    }

    TEST_CASE("Worker and safe-point failures remain terminal with their original typed cause", "[unit][save][lifecycle]") {
        auto coordinator = Coordinator();
        RecordingExecutor executor;
        REQUIRE(coordinator->Admit({.operation = 81, .action = SaveSafePointAction::Restore, .generation = Generation()}).HasValue());
        const Error workerError = MakeError(SaveErrors::CaptureBudgetExceeded);
        REQUIRE(coordinator
                    ->PublishWorkerCompletion(
                        {.operation = 81, .generation = Generation(), .outcome = SaveWorkerCompletionOutcome::Failed, .error = workerError})
                    .HasValue());
        const SaveSafePointOperationSnapshot failedWorker = Snapshot(*coordinator, 81);
        REQUIRE(failedWorker.terminalError.has_value());
        CHECK(failedWorker.terminalError->code.Value() == SaveErrors::CaptureBudgetExceeded.code.Value());

        REQUIRE(coordinator->Admit({.operation = 82, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        executor.captureError = MakeError(SaveErrors::CaptureIncomplete);
        const SaveSafePointDrainResult drain = Commit(*coordinator, executor, Generation(), 1);
        CHECK(drain.failed == 1);
        const SaveSafePointOperationSnapshot failedCapture = Snapshot(*coordinator, 82);
        REQUIRE(failedCapture.terminalError.has_value());
        CHECK(failedCapture.terminalError->code.Value() == SaveErrors::CaptureIncomplete.code.Value());
        CHECK(failedCapture.disposition == SaveLifecycleDisposition::SafePointFailure);
    }

    TEST_CASE("Unexpected callbacks are contained and lifecycle re-entry is rejected", "[unit][save][safe-point]") {
        auto coordinator = Coordinator();
        RecordingExecutor executor;
        REQUIRE(coordinator->Admit({.operation = 91, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        executor.throwFromCapture = true;
        CHECK(Commit(*coordinator, executor, Generation(), 1).failed == 1);
        const SaveSafePointOperationSnapshot thrown = Snapshot(*coordinator, 91);
        REQUIRE(thrown.terminalError.has_value());
        CHECK(thrown.terminalError->code.Value() == SaveErrors::LifecycleCallbackFailed.code.Value());

        REQUIRE(coordinator->Admit({.operation = 92, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        executor.throwFromCapture = false;
        std::optional<Result<void>> reentry;
        executor.captureHook = [&] {
            reentry = coordinator->TransitionScene(Generation(1, 8, 3));
        };
        CHECK(Commit(*coordinator, executor, Generation(), 1).captured == 1);
        REQUIRE(reentry.has_value());
        RequireError(*reentry, SaveErrors::LifecycleReentrant);
        CHECK(Snapshot(*coordinator, 92).state == SaveSafePointOperationState::DetachedWork);
    }

    TEST_CASE("Acknowledgement releases only terminal records", "[unit][save][lifecycle]") {
        auto coordinator = Coordinator(Generation(), 1);
        RecordingExecutor executor;
        REQUIRE(coordinator->Admit({.operation = 101, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
        RequireError(coordinator->Acknowledge(101), SaveErrors::CompletionInvalid);
        RequireError(coordinator->Acknowledge(999), SaveErrors::OperationInvalid);

        CHECK(Commit(*coordinator, executor, Generation(), 1).captured == 1);
        REQUIRE(
            coordinator
                ->PublishWorkerCompletion({.operation = 101, .generation = Generation(), .outcome = SaveWorkerCompletionOutcome::Succeeded})
                .HasValue());
        REQUIRE(coordinator->Acknowledge(101).HasValue());
        RequireError(coordinator->Snapshot(101), SaveErrors::OperationInvalid);
        REQUIRE(coordinator->Admit({.operation = 102, .action = SaveSafePointAction::Capture, .generation = Generation()}).HasValue());
    }
}  // namespace Horo::Runtime
