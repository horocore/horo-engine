#include "NavigationBoundedQueue.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace Horo::Navigation {
    using namespace CoordinatorTestSupport;

    namespace {
        /** @brief Test-only producer pause after the real queue has reserved its head slot. */
        struct ReservationGate final {
            std::mutex mutex;
            std::condition_variable changed;
            bool reserved{};
            bool released{};

            void HoldReservedMove() {
                std::unique_lock lock(mutex);
                reserved = true;
                changed.notify_all();
                changed.wait(lock, [&] {
                    return released;
                });
            }

            bool AwaitReservation() {
                std::unique_lock lock(mutex);
                return changed.wait_for(lock, std::chrono::seconds(3), [&] {
                    return reserved;
                });
            }

            void Release() {
                std::lock_guard lock(mutex);
                released = true;
                changed.notify_all();
            }
        };

        /** @brief Release a paused producer before the thread's joining destructor on assertion failure. */
        struct ReleaseReservation final {
            ReservationGate &gate;

            ~ReleaseReservation() {
                gate.Release();
            }
        };

        /** @brief Nothrow payload that pauses its first emplacement but leaves later record moves unblocked. */
        struct TransportRecord final {
            unsigned value{};
            std::shared_ptr<ReservationGate> gate;

            explicit TransportRecord(const unsigned id, std::shared_ptr<ReservationGate> barrier = {}) noexcept
                : value(id), gate(std::move(barrier)) {}

            TransportRecord(TransportRecord &&other) noexcept : value(other.value), gate(std::move(other.gate)) {
                if (const auto barrier = std::move(gate); barrier)
                    barrier->HoldReservedMove();
            }
        };

        /** @brief Observe the isolated scheduler's latest accepted partition through its terminal snapshot fence.
         * @param jobs Scheduler containing only this test's navigation work.
         * @return Terminal snapshot observed within a bounded three-second test wait.
         */
        JobSnapshot AwaitLatestPartition(JobSystem &jobs) {
            const auto store = jobs.SnapshotIfChanged(0);
            REQUIRE(store.has_value());
            REQUIRE_FALSE(store->jobs.empty());
            const JobId id = std::ranges::max(store->jobs, {}, &JobSnapshot::id).id;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            auto snapshot = jobs.Find(id);
            while (snapshot.has_value() && !snapshot->terminalResult.has_value() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
                snapshot = jobs.Find(id);
            }
            REQUIRE(snapshot.has_value());
            REQUIRE(snapshot->terminalResult.has_value());
            return *snapshot;
        }
    }  // namespace

    TEST_CASE("A reserved transport head delays already owned later completions", "[unit][navigation][transport]") {
        Detail::BoundedMpmcQueue<TransportRecord> queue{4};
        auto gate = std::make_shared<ReservationGate>();
        TransportRecord first{1, gate};
        TransportRecord second{2};
        NavigationQueueEnqueueResult firstStatus{};
        std::jthread producer([&] {
            firstStatus = queue.TryPush(first);
        });
        // Unwinding a failed assertion releases the producer before jthread's joining destructor.
        ReleaseReservation release{*gate};
        REQUIRE(gate->AwaitReservation());
        REQUIRE(queue.TryPush(second) == NavigationQueueEnqueueResult::Enqueued);
        REQUIRE(queue.Stats().enqueued == 1);
        REQUIRE_FALSE(queue.TryPop());
        REQUIRE_FALSE(queue.TryPop());
        gate->Release();
        producer.join();
        REQUIRE(firstStatus == NavigationQueueEnqueueResult::Enqueued);
        const auto firstResult = queue.TryPop();
        const auto secondResult = queue.TryPop();
        REQUIRE(firstResult.has_value());
        REQUIRE(secondResult.has_value());
        REQUIRE(firstResult->value == 1);
        REQUIRE(secondResult->value == 2);
        REQUIRE_FALSE(queue.TryPop());
    }

    TEST_CASE("A reused partition resets its produced prefix before another query throws", "[unit][navigation][coordinator]") {
        JobSystem jobs{{.workerCount = 1}};
        auto world = std::move(NavigationWorldLifecycle::Create(4)).Value();
        Activate(world, std::make_unique<ControlledBackend>(nullptr, true));
        auto coordinator = std::move(NavigationCoordinator::Create(jobs, Limits())).Value();
        const auto first = Admit(coordinator, world);
        auto throwing = Input(Caller(2));
        throwing.request.start.x = 2.0F;
        const auto second = Admit(coordinator, world, throwing);
        REQUIRE(coordinator.Dispatch(1) == 2);
        REQUIRE(AwaitLatestPartition(jobs).state == JobState::Failed);
        const NavigationPathCaller callers[]{Caller(), Caller(2)};
        REQUIRE(coordinator.Commit(Current(callers)) == 2);
        const auto firstResult = coordinator.Take(first);
        const auto secondResult = coordinator.Take(second);
        REQUIRE(firstResult.has_value());
        REQUIRE(secondResult.has_value());
        REQUIRE(firstResult->result.HasValue());
        REQUIRE(secondResult->result.HasError());
        REQUIRE(coordinator.IsDrained());
        throwing.caller = Caller(3);
        throwing.targetTick = 2;
        auto accepted = coordinator.Submit(world, std::move(throwing), 1);
        REQUIRE(accepted.HasValue());
        REQUIRE(coordinator.Dispatch(2) == 1);
        REQUIRE(AwaitLatestPartition(jobs).state == JobState::Failed);
        const NavigationPathCaller current[]{Caller(3)};
        REQUIRE(coordinator.Commit(Current(current, 2)) == 1);
        const auto failure = coordinator.Take(accepted.Value());
        REQUIRE(failure.has_value());
        REQUIRE(failure->result.HasError());
        REQUIRE_FALSE(coordinator.Take(accepted.Value()));
        REQUIRE(coordinator.IsDrained());
    }
}  // namespace Horo::Navigation
