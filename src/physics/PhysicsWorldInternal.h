#pragma once

/** @file PhysicsWorldInternal.h
 * @brief Target-private ownership definitions shared by PhysicsWorld implementation units.
 */

#include "CanonicalPhysicsRuntime.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsDiagnostics.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsEventProjection.h"
#include "PhysicsWorldTickHelpers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Physics {
    /** @brief Shared only by copied client handles; the world invalidates its borrowed pointer before retirement. */
    struct PhysicsQueryEventCapabilityState final {
        PhysicsWorld *world{};
        PhysicsQueryEventIdentity identity;
        std::thread::id ownerThread;
        bool revoked{};
        bool stale{};
    };

    namespace Detail {
        [[nodiscard]] inline std::array<PhysicsDiagnosticContextEntry, 3> DiagnosticContext(const PhysicsWorldId world,
                                                                                            const std::uint64_t sceneGeneration,
                                                                                            const std::uint64_t simulationTick) {
            using enum PhysicsDiagnosticContextKey;
            return {PhysicsDiagnosticContextEntry{.key = World, .value = world},
                    PhysicsDiagnosticContextEntry{.key = SceneGeneration, .value = sceneGeneration},
                    PhysicsDiagnosticContextEntry{.key = SimulationTick, .value = simulationTick}};
        }
    }  // namespace Detail

    /** @brief Shared only by the process wrapper and its worlds; identity pointers are owner-thread, stable-address registrations. */
    struct PhysicsRuntime::Impl final {
        explicit Impl(const PhysicsRuntimeMode selectedMode, JobSystem *solverJobSystem)
            : mode(selectedMode), solverJobs(solverJobSystem) {}

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;

        ~Impl() {
            Detail::DestroyCanonicalRuntime(native);
        }

        void ReleaseNativeWhenIdle() noexcept {
            if (const std::array releaseConditions{state == PhysicsRuntimeState::Stopped, identities.empty()};
                !std::ranges::all_of(releaseConditions, std::identity{}))
                return;
            Detail::DestroyCanonicalRuntime(native);
            native = {};
        }

        PhysicsRuntimeMode mode;
        PhysicsRuntimeState state{PhysicsRuntimeState::Ready};
        std::thread::id ownerThread{std::this_thread::get_id()};
        Detail::CanonicalRuntimeHandle native;
        JobSystem *solverJobs{};
        std::vector<const PhysicsWorldId *> identities;
        std::uint64_t nextWorldIdentity{1};
    };

    /** @brief Owns one candidate's settings/native state and unregisters its identity before releasing the runtime lease. */
    struct PhysicsWorld::Impl final {
        Impl(std::shared_ptr<PhysicsRuntime::Impl> runtimeOwner, const PhysicsWorldSettings &worldSettings)
            : runtime(std::move(runtimeOwner)), settings(worldSettings), commands(worldSettings.Values().budgets.maximumCommands),
              sourceOrder(worldSettings.Values().budgets.maximumCommands),
              events(worldSettings.Values().budgets.maximumEvents, worldSettings.Values().budgets.maximumInFlightPairs,
                     worldSettings.Values().budgets.eventOverflow) {
            runtime->identities.push_back(&identity);
        }

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;

        ~Impl() {
            Retire(PhysicsWorldLifecycleCause::ProcessShutdown);
        }

        void Retire(const PhysicsWorldLifecycleCause cause) noexcept {
            if (state == PhysicsWorldState::Destroyed)
                return;
            InvalidateQueryEventCapabilities();
            Detail::DestroyCanonicalWorld(native);
            native = {};
            state = PhysicsWorldState::Destroyed;
            lifecycleCause = cause;
            lastFailure.reset();
            lastDiagnostic.reset();
            std::ranges::fill(commands, PhysicsStructuralCommand{});
            commandHead = 0;
            commandCount = 0;
            events.Reset();
            statistics.pendingCommands = 0;
            std::erase(runtime->identities, &identity);
            runtime->ReleaseNativeWhenIdle();
        }

        void RecordDiagnostic(const Error &error, const std::uint64_t sceneGeneration, const std::uint64_t simulationTick) {
            const auto context = Detail::DiagnosticContext(identity, sceneGeneration, simulationTick);
            const auto record = MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, error, context);
            if (record.HasValue())
                lastDiagnostic = record.Value();
        }

        void RecordEventOverflowDiagnostic(const std::uint64_t sceneGeneration, const std::uint64_t simulationTick) {
            const auto context = Detail::DiagnosticContext(identity, sceneGeneration, simulationTick);
            const auto record =
                MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Event,
                                            MakeError(PhysicsErrors::CapacityExceeded, "Physics event projection dropped bounded records."),
                                            context);
            if (record.HasValue())
                lastDiagnostic = record.Value();
        }

        void Fail(Error error, const std::uint64_t sceneGeneration, const std::uint64_t simulationTick) {
            state = PhysicsWorldState::Failed;
            lifecycleCause = PhysicsWorldLifecycleCause::FatalSolverError;
            RecordDiagnostic(error, sceneGeneration, simulationTick);
            lastFailure = std::move(error);
        }

        void ClearForReset() noexcept {
            InvalidateQueryEventCapabilities();
            identity = {};
            std::ranges::fill(commands, PhysicsStructuralCommand{});
            commandHead = 0;
            commandCount = 0;
            activeTick = 0;
            querySceneGeneration = 0;
            commandOrderDirty = false;
            stepping = false;
            events.Reset();
            {
                Detail::PublicationGuard publicationGuard{publicationLock};
                published = {};
            }
            statistics = {};
            lastFailure.reset();
            lastDiagnostic.reset();
            lifecycleCause = PhysicsWorldLifecycleCause::Reset;
        }

        void InvalidateQueryEventCapabilities() noexcept {
            for (const auto &weak : queryEventCapabilities) {
                if (const auto access = weak.lock()) {
                    access->stale = true;
                    access->world = nullptr;
                }
            }
            queryEventCapabilities.clear();
        }

        [[nodiscard]] Result<void> CheckPublicationRevisionCapacity() const {
            if (published.publicationRevision == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
            return Result<void>::Success();
        }

        void InvalidateQueryEventPublication() noexcept {
            Detail::PublicationGuard publicationGuard{publicationLock};
            if (published.completedTick == 0)
                return;
            // An immediate structural edit changes query results without completing a new event tick.
            ++published.publicationRevision;
            published.eventTick = 0;
            published.eventCount = 0;
            published.droppedEventCount = 0;
        }

        [[nodiscard]] Result<void> Reinitialize() {
            using enum PhysicsWorldState;
            Detail::DestroyCanonicalWorld(native);
            native = {};
            ClearForReset();
            if (runtime->mode == PhysicsRuntimeMode::Null) {
                state = PreparedNull;
                return Result<void>::Success();
            }

            try {
                const Result<Detail::CanonicalWorldHandle> created = Detail::CreateCanonicalWorld(runtime->native, settings);
                if (created.HasError()) {
                    state = Failed;
                    lastFailure = created.ErrorValue();
                    return Result<void>::Failure(created.ErrorValue());
                }
                native = created.Value();
                state = PreparedSolver;
                return Result<void>::Success();
            } catch (const std::bad_alloc &) {
                Error error = MakeError(PhysicsErrors::CapacityExceeded, "Unable to rebuild Physics world ownership state during reset.");
                state = Failed;
                lastFailure = error;
                return Result<void>::Failure(std::move(error));
            }
        }

        [[nodiscard]] PhysicsStructuralCommand &CommandAt(const std::uint32_t offset) noexcept {
            return commands[(commandHead + offset) % commands.size()];
        }

        [[nodiscard]] const PhysicsStructuralCommand &CommandAt(const std::uint32_t offset) const noexcept {
            return commands[(commandHead + offset) % commands.size()];
        }

        void DiscardCommands(const std::uint32_t discarded) noexcept {
            if (discarded == 0)
                return;
            commandHead = (commandHead + discarded) % commands.size();
            commandCount -= discarded;
            statistics.pendingCommands = commandCount;
        }

        std::shared_ptr<PhysicsRuntime::Impl> runtime;
        PhysicsWorldSettings settings;
        PhysicsWorldState state{PhysicsWorldState::Preparing};
        PhysicsWorldId identity;
        Detail::CanonicalWorldHandle native;
        std::vector<PhysicsStructuralCommand> commands;
        std::vector<std::uint32_t> sourceOrder;
        Detail::PhysicsEventProjection events;
        std::vector<std::weak_ptr<PhysicsQueryEventCapabilityState>> queryEventCapabilities;
        std::uint64_t nextQueryEventCapabilityGeneration{1};
        std::uint32_t commandHead{};
        std::uint32_t commandCount{};
        std::uint64_t activeTick{};
        std::uint64_t querySceneGeneration{};
        bool commandOrderDirty{};
        bool stepping{};
        // The owner thread alone writes publication state; any live-world thread may take a coherent snapshot.
        // The flag protects only the bounded copy below and owns no worker or shutdown lifetime.
        mutable std::atomic_flag publicationLock = ATOMIC_FLAG_INIT;
        PhysicsPublishedTick published;
        PhysicsTickStatistics statistics;
        PhysicsWorldLifecycleCause lifecycleCause{PhysicsWorldLifecycleCause::None};
        std::optional<Error> lastFailure;
        std::optional<PhysicsDiagnosticRecord> lastDiagnostic;
    };
}  // namespace Horo::Physics
