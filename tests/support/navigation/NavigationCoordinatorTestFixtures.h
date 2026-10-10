#pragma once

#include "Horo/Navigation/NavigationCoordinator.h"
#include "navigation/NavigationRuntimeTestFixtures.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Horo::Navigation::CoordinatorTestSupport {
    using namespace TestSupport;

    inline NavigationOutcomeProvenance Source() {
        return {NavigationSnapshotToken::Create(1).Value(), World(), Topology(), 1, 1, 1, 1, 0};
    }

    inline NavigationPathCaller Caller(const std::uint64_t id = 1, const std::uint64_t generation = 1) {
        return {NavigationDynamicOwnerId::Create(id).Value(), NavigationDynamicOwnerGeneration::Create(generation).Value()};
    }

    inline NavigationPathSubmission Input(const NavigationPathCaller caller = Caller()) {
        return {.caller = caller, .request = Request(), .source = Source(), .capabilityRevision = 1, .targetTick = 1, .deadlineTick = 20};
    }

    inline NavigationPathBatchLimits Limits() {
        return {.requestSlots = 8,
                .maximumJobs = 2,
                .requestsPerJob = 4,
                .requestsPerTick = 4,
                .requestsPerCaller = 2,
                .maximumPendingPerCaller = 4};
    }

    inline void Activate(NavigationWorldLifecycle &world,
                         std::unique_ptr<INavigationQueryBackend> backend = MakeObservedNavigationBackend()) {
        REQUIRE(world.Stage(Activation(), std::move(backend)).HasValue());
        REQUIRE(world.CommitAtSafePoint(Activation().scene, Activation().sceneGeneration).HasValue());
    }

    inline bool ErrorIs(const NavigationPathCompletion &result, const ErrorCodeDescriptor &error) {
        return result.result.HasError() && result.result.ErrorValue().domain.Value() == error.domain.Value() &&
               result.result.ErrorValue().code.Value() == error.code.Value();
    }

    inline NavigationPathPublication Current(const std::span<const NavigationPathCaller> callers, const std::uint64_t tick = 1) {
        return {Activation(), Source(), callers, tick};
    }

    inline NavRequestHandle Admit(NavigationCoordinator &coordinator, NavigationWorldLifecycle &world,
                                  NavigationPathSubmission input = Input()) {
        auto accepted = coordinator.Submit(world, std::move(input), 0);
        REQUIRE(accepted.HasValue());
        return accepted.Value();
    }

    struct Gate final {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered{};
        bool released{};
        bool otherFinished{};

        ~Gate() {
            Release();
        }

        void Release() {
            std::lock_guard lock(mutex);
            released = true;
            changed.notify_all();
        }

        bool AwaitOther() {
            std::unique_lock lock(mutex);
            return changed.wait_for(lock, std::chrono::seconds(3), [&] {
                return otherFinished;
            });
        }

        bool AwaitEntry() {
            std::unique_lock lock(mutex);
            return changed.wait_for(lock, std::chrono::seconds(3), [&] {
                return entered;
            });
        }
    };

    class ControlledBackend final : public INavigationQueryBackend {
    public:
        explicit ControlledBackend(std::shared_ptr<Gate> gate = {}, const bool throwFailure = false, const bool partial = false)
            : gate_(std::move(gate)), throwFailure_(throwFailure), partial_(partial) {}

        NavigationProviderCapabilities Capabilities() const noexcept override {
            return MakeAvailablePathQueryCapabilities(1,
                                                      {.maximumNodeExpansions = 64,
                                                       .maximumResultPoints = 16,
                                                       .maximumSearchDistanceMeters = 100.0F},
                                                      2);
        }

        Result<NavigationPath> FindPath(const NavigationPathRequest &request, const CancellationToken &token) const override {
            if (gate_ && request.start.x == 1.0F) {
                std::unique_lock lock(gate_->mutex);
                gate_->entered = true;
                gate_->changed.notify_all();
                gate_->changed.wait_for(lock, std::chrono::seconds(3), [&] {
                    return gate_->released || token.IsCancellationRequested();
                });
            }
            if (throwFailure_ && request.start.x == 2.0F)
                throw std::runtime_error("controlled provider exception");
            NavigationPath path;
            path.sourceGeneration = request.topology;
            path.status = partial_ ? NavigationPathStatus::Partial : NavigationPathStatus::Reachable;
            path.stopReason = partial_ ? NavigationPathStopReason::NodeBudgetExceeded : NavigationPathStopReason::None;
            path.points = {request.start, request.destination};
            if (gate_ && request.start.x == 2.0F) {
                std::lock_guard lock(gate_->mutex);
                gate_->otherFinished = true;
                gate_->changed.notify_all();
            }
            return Result<NavigationPath>::Success(std::move(path));
        }

    private:
        std::shared_ptr<Gate> gate_;
        bool throwFailure_{};
        bool partial_{};
    };
}  // namespace Horo::Navigation::CoordinatorTestSupport
