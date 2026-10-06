#pragma once

#include "AllocationProbe.h"
#include "CharacterWorldTestHelpers.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <latch>
#include <semaphore>
#include <system_error>

namespace Horo::Character::QualificationTest {
    using namespace TestDetail;
    constexpr std::size_t ControllerCount = 4;
    constexpr std::size_t TickCount = 32;

    /** @brief Supplies deterministic clear placement evidence without retaining unbounded query history. */
    struct ClearPlacement final {
        std::uint32_t calls{};

        static Result<CharacterOverlapProbeResult> Query(void *context, const CharacterOverlapProbeRequest &) noexcept {
            ++static_cast<ClearPlacement *>(context)->calls;
            return Result<CharacterOverlapProbeResult>::Success({});
        }

        CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick = 0) noexcept {
            return {world.sceneGeneration,  world.identity, world.physicsWorld,           this, Query, world.collisionFilterGeneration,
                    world.originGeneration, tick,           world.physicsSnapshotRevision};
        }
    };

    /** @brief Small fully admitted storage budgets keep qualification independent of production default sizes. */
    inline CharacterWorldSettings QualificationSettings(const std::uint32_t controllers = ControllerCount) {
        CharacterWorldSettingsDescriptor descriptor;
        descriptor.capacities = {controllers, 4, 64, 8, 64, 8, 8, 8};
        descriptor.work.maximumCommandsPerTick = 4;
        descriptor.work.maximumQueriesPerTick = 64;
        descriptor.work.scratchBytes = 4'096;
        const auto captured = CharacterWorldSettings::Capture(descriptor);
        REQUIRE(captured.HasValue());
        return captured.Value();
    }

    inline std::unique_ptr<CharacterWorld> QualificationPreparedWorld(const std::uint32_t controllers = ControllerCount) {
        auto prepared = CharacterWorld::Prepare(WorldDescriptor(), QualificationSettings(controllers));
        REQUIRE(prepared.HasValue());
        return std::move(prepared).Value();
    }

    struct QualificationWorld final {
        std::unique_ptr<CharacterWorld> world{QualificationPreparedWorld()};
        std::array<CharacterControllerHandle, ControllerCount> handles;

        QualificationWorld() {
            ClearPlacement placement;
            const auto descriptor = ControllerDescriptor(world->Descriptor());
            for (auto &handle : handles) {
                const auto created = world->CreateController(descriptor);
                REQUIRE(created.HasValue());
                handle = created.Value();
            }
            REQUIRE(world->Activate().HasValue());
            for (const auto handle : handles)
                REQUIRE(world->SpawnController(handle, placement.Context(world->Descriptor())).HasValue());
        }
    };

    inline SpawnedActiveWorld QualificationSpawnedWorld() {
        auto world = QualificationPreparedWorld(1);
        const auto created = world->CreateController(ControllerDescriptor(world->Descriptor()));
        REQUIRE(created.HasValue());
        const auto handle = created.Value();
        ClearPlacement placement;
        REQUIRE(world->Activate().HasValue());
        REQUIRE(world->SpawnController(handle, placement.Context(world->Descriptor())).HasValue());
        return {std::move(world), handle};
    }

    /** @brief Records final controller order in fixed storage without allocation or test framework calls. */
    struct OrderedFrame final {
        std::array<CharacterControllerHandle, ControllerCount> handles;
        std::size_t count{};
        bool bounded{true};

        static void Movement(void *context, const CharacterMovementRequest &request) noexcept {
            auto &frame = *static_cast<OrderedFrame *>(context);
            if (frame.count == frame.handles.size()) {
                frame.bounded = false;
                return;
            }
            frame.handles[frame.count++] = request.controller;
        }
    };

    /** @brief Parks all created threads before/after the measured window and never depends on scheduler-selected admission. */
    class ConcurrentProducers final {
    public:
        using Requests = std::array<std::array<CharacterMovementRequest, ControllerCount>, TickCount>;
        std::array<std::optional<Result<CharacterCommandAdmission>>, ControllerCount> outcomes;

        ConcurrentProducers(CharacterWorld &world, const Requests &requests, const bool reversed)
            : world_(world), requests_(requests), reversed_(reversed) {}

        ~ConcurrentProducers() {
            retire_.count_down();
        }

        /** @brief Starts producers or reports only a documented unavailable standard-thread facility. */
        bool Start() {
            try {
                for (std::size_t index = 0; index < workers_.size(); ++index)
                    workers_[index] = std::jthread([this, index] {
                        Run(index);
                    });
            } catch (const std::system_error &error) {
                CancelStartup();
                if (error.code() == std::errc::resource_unavailable_try_again || error.code() == std::errc::operation_not_supported)
                    return false;
                throw;
            } catch (...) {
                CancelStartup();
                throw;
            }
            launch_.count_down();
            ready_.wait();
            sync_.arrive_and_wait();  // Warm the synchronization path before observing global allocation counters.
            return true;
        }

        void Admit() {
            sync_.arrive_and_wait();
            sync_.arrive_and_wait();
        }

        void CompleteTick() {
            sync_.arrive_and_wait();
        }

    private:
        void CancelStartup() {
            cancelled_.store(true);
            launch_.count_down();
            for (auto &worker : workers_)
                if (worker.joinable())
                    worker.join();
        }

        void Run(const std::size_t index) {
            launch_.wait();
            if (cancelled_.load())
                return;
            ready_.count_down();
            sync_.arrive_and_wait();
            const auto controller = reversed_ ? ControllerCount - index - 1 : index;
            for (const auto &frame : requests_) {
                sync_.arrive_and_wait();
                outcomes[index] = world_.QueueMovementCommand(frame[controller]);
                sync_.arrive_and_wait();
                sync_.arrive_and_wait();
            }
            retire_.wait();  // Thread teardown is outside the measured fast path.
        }

        CharacterWorld &world_;
        const Requests &requests_;
        bool reversed_{};
        std::atomic<bool> cancelled_{};
        std::latch launch_{1};
        std::latch ready_{ControllerCount};
        std::latch retire_{1};
        std::barrier<> sync_{ControllerCount + 1};
        std::array<std::jthread, ControllerCount> workers_;
    };

    /** @brief Completes busy attempts deterministically after concurrent nonblocking producer calls return. */
    struct AdmissionSummary final {
        bool valid{true};
        std::uint64_t repaired{};
    };

    inline AdmissionSummary CompleteAdmissions(CharacterWorld &world, const ConcurrentProducers &producers,
                                               const std::array<CharacterMovementRequest, ControllerCount> &requests, const bool reversed) {
        AdmissionSummary summary;
        for (std::size_t index = 0; index < ControllerCount; ++index) {
            const auto &outcome = producers.outcomes[index];
            if (!outcome || outcome->HasError()) {
                summary.valid = false;
                continue;
            }
            const auto status = outcome->Value().status;
            if (status == CharacterCommandAdmissionStatus::RejectedBusy) {
                ++summary.repaired;
                const auto controller = reversed ? ControllerCount - index - 1 : index;
                const auto retry = world.QueueMovementCommand(requests[controller]);
                summary.valid &= retry.HasValue() && retry.Value().status == CharacterCommandAdmissionStatus::Deferred;
            } else {
                summary.valid &= status == CharacterCommandAdmissionStatus::Deferred;
            }
        }
        return summary;
    }

    /** @brief Resolves every admitted controller intent each frame and records exactly the scheduler-busy repairs. */
    inline AdmissionSummary AdvanceConcurrentFrames(QualificationWorld &active, ConcurrentProducers &producers,
                                                    const ConcurrentProducers::Requests &requests, const bool reversed) {
        AdmissionSummary summary;
        for (std::size_t frame = 0; frame < TickCount; ++frame) {
            producers.Admit();
            const auto admission = CompleteAdmissions(*active.world, producers, requests[frame], reversed);
            summary.valid &= admission.valid;
            summary.repaired += admission.repaired;
            OrderedFrame order;
            summary.valid &= active.world
                                 ->AdvanceFixedTick({.tick = frame + 1,
                                                     .sceneGeneration = active.world->Descriptor().sceneGeneration,
                                                     .fixedDelta = Duration::FromNanoseconds(250'000'000),
                                                     .observer = {.context = &order, .movement = OrderedFrame::Movement}})
                                 .HasValue();
            summary.valid &= order.bounded && order.count == ControllerCount && order.handles == active.handles;
            for (std::size_t controller = 0; controller < ControllerCount; ++controller) {
                const auto snapshot = active.world->ControllerLocomotionSnapshot(active.handles[controller]);
                summary.valid &= snapshot.HasValue() && snapshot.Value().tick == frame + 1 &&
                                 snapshot.Value().movement.sequence == requests[frame][controller].sequence &&
                                 snapshot.Value().movement.achievedVelocityMetersPerSecond ==
                                     *requests[frame][controller].desiredVelocityMetersPerSecond;
            }
            producers.CompleteTick();
        }
        return summary;
    }

    /** @brief Exercises each explicit teleport reservation and its empty successor movement frame exactly once. */
    inline bool RunTeleportTicks(CharacterWorld &world, const CharacterControllerHandle controller, ClearPlacement &placement) {
        bool valid = true;
        for (std::uint64_t tick = 1; tick <= TickCount; ++tick) {
            const CharacterTeleportRequest target{controller, tick, {static_cast<float>(tick) * 0.25F, 0, 0}};
            valid &= world.TeleportController(target, placement.Context(world.Descriptor(), tick)).HasValue();
            valid &= world
                         .AdvanceFixedTick({.tick = tick,
                                            .sceneGeneration = world.Descriptor().sceneGeneration,
                                            .fixedDelta = Duration::FromNanoseconds(250'000'000)})
                         .HasValue();
        }
        return valid;
    }

    /** @brief Holds the owner after command freeze until an external reader copied the still-committed state. */
    struct PublicationBoundary final {
        std::barrier<> rendezvous{2};

        static void Phase(void *context, const CharacterTickPhase phase, const std::uint64_t) noexcept {
            if (phase != CharacterTickPhase::FreezeCommands)
                return;
            auto &boundary = *static_cast<PublicationBoundary *>(context);
            boundary.rendezvous.arrive_and_wait();
            boundary.rendezvous.arrive_and_wait();
        }
    };

    struct PublicationReadback final {
        CharacterPublishedTick observed;
        std::optional<Result<CharacterTransformPublication>> transform;
        std::optional<Result<CharacterCommandAdmission>> admission;
    };

    /** @brief Starts one reader borrowing the world and readback until the caller joins it. */
    inline std::jthread StartBoundaryReader(CharacterWorld &world, const CharacterControllerHandle controller,
                                            PublicationBoundary &boundary, const CharacterMovementRequest &future,
                                            PublicationReadback &readback) {
        return std::jthread([&world, controller, &boundary, &future, &readback] {
            boundary.rendezvous.arrive_and_wait();
            readback.observed = world.PublishedTick();
            readback.transform = world.ControllerTransform(controller);
            readback.admission = world.QueueMovementCommand(future);
            boundary.rendezvous.arrive_and_wait();
        });
    }

    /** @brief Observes an actual registry-held error construction using the existing allocation fault seam. */
    struct RegistryContention final {
        std::binary_semaphore attempt{0};
        std::binary_semaphore returned{0};
        std::binary_semaphore retire{0};
        std::optional<Result<CharacterCommandAdmission>> admission;
        bool invoked{};
        bool returnedWhileHeld{};
        std::size_t before{};
        std::size_t after{};
        static inline RegistryContention *current{};  // Only the calling owner-thread failure observer reads this pointer.

        static void Observe(const std::size_t) noexcept {
            auto &probe = *current;
            probe.invoked = true;
            probe.before = Tests::AllocationProbe::Count();
            probe.attempt.release();
            probe.returnedWhileHeld = probe.returned.try_acquire_for(std::chrono::seconds{5});
            probe.after = Tests::AllocationProbe::Count();
        }
    };

    /** @brief Owns the fault observer pointer for one owner-thread call; no producer or production code retains it. */
    class RegistryFaultObservation final {
    public:
        explicit RegistryFaultObservation(RegistryContention &probe) {
            RegistryContention::current = &probe;
        }

        ~RegistryFaultObservation() {
            RegistryContention::current = nullptr;
        }

        RegistryFaultObservation(const RegistryFaultObservation &) = delete;
        RegistryFaultObservation &operator=(const RegistryFaultObservation &) = delete;
    };

    /** @brief Retains world/probe borrows through a producer join and releases every test-owned wait on scope exit. */
    class RegistryProducer final {
    public:
        RegistryProducer(CharacterWorld &world, const CharacterMovementRequest &request, RegistryContention &probe)
            : world_(world), request_(request), probe_(probe) {}

        ~RegistryProducer() {
            Finish();
        }

        bool Start() {
            try {
                worker_ = std::jthread([this] {
                    probe_.attempt.acquire();
                    probe_.admission = world_.QueueMovementCommand(request_);
                    probe_.returned.release();
                    probe_.retire.acquire();
                });
            } catch (const std::system_error &error) {
                if (error.code() == std::errc::resource_unavailable_try_again || error.code() == std::errc::operation_not_supported)
                    return false;
                throw;
            }
            return true;
        }

        void Finish() {
            if (!worker_.joinable())
                return;
            if (!probe_.invoked)
                probe_.attempt.release();
            probe_.retire.release();
            worker_.join();
        }

    private:
        CharacterWorld &world_;
        CharacterMovementRequest request_;
        RegistryContention &probe_;
        std::jthread worker_;
    };

    /** @brief Injects the actual copied-state error allocation while its public method owns the registry lock. */
    inline bool ObserveRegistryHold(CharacterWorld &world, const CharacterControllerHandle stale, RegistryContention &probe) {
        RegistryFaultObservation observation{probe};
        Tests::AllocationProbe::ScopedFailure failure{0, RegistryContention::Observe};
        try {
            static_cast<void>(world.ControllerDescriptor(stale));
        } catch (const std::bad_alloc &) {
            return true;
        }
        return false;
    }
}  // namespace Horo::Character::QualificationTest
