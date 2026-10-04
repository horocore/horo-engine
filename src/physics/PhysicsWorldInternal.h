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
#include <mutex>
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
        CancellationToken revocation;
        bool revoked{};
        bool stale{};
    };

    /** @brief Bounded request ownership and mutex-protected terminal publication; only Physics owner reads commands. */
    struct PhysicsQueryBatchState final {
        PhysicsQueryBatchState(const std::span<const PhysicsQueryCommand> commands,
                               std::shared_ptr<PhysicsQueryEventCapabilityState> access)
            : commands_(commands.begin(), commands.end()), access_(std::move(access)) {}

        [[nodiscard]] const std::vector<PhysicsQueryCommand> &Commands() const noexcept {
            return commands_;
        }

        [[nodiscard]] const std::shared_ptr<PhysicsQueryEventCapabilityState> &Access() const noexcept {
            return access_;
        }

        [[nodiscard]] Result<std::shared_ptr<const PhysicsQueryBatchCompletion>> Poll() {
            std::lock_guard lock(terminalMutex);
            // access_ is an immutable, non-null admission pin from QueueQueryBatch. Poll reads
            // only its cancellation token, never the owner-thread-only world pointer/flags.
            // The terminal lock arbitrates revocation and completion; committed results never change.
            if (!terminal && access_->revocation.IsCancellationRequested()) {
                failureCode = &PhysicsErrors::CapabilityRevoked;
                terminal = true;
            }
            if (failureCode)
                return Result<std::shared_ptr<const PhysicsQueryBatchCompletion>>::Failure(MakeError(*failureCode));
            if (failure)
                return Result<std::shared_ptr<const PhysicsQueryBatchCompletion>>::Failure(*failure);
            return Result<std::shared_ptr<const PhysicsQueryBatchCompletion>>::Success(completion);
        }

        [[nodiscard]] bool Fail(Error error) {
            std::lock_guard lock(terminalMutex);
            if (terminal)
                return false;
            failure = std::move(error);
            terminal = true;
            return true;
        }

        [[nodiscard]] bool FailCode(const ErrorCodeDescriptor &code) noexcept {
            std::lock_guard lock(terminalMutex);
            if (terminal)
                return false;
            failureCode = &code;
            terminal = true;
            return true;
        }

        [[nodiscard]] bool Complete(std::shared_ptr<const PhysicsQueryBatchCompletion> value) noexcept {
            std::lock_guard lock(terminalMutex);
            if (terminal)
                return false;
            if (access_->revocation.IsCancellationRequested()) {
                failureCode = &PhysicsErrors::CapabilityRevoked;
                terminal = true;
                return false;
            }
            // This lock is the publication point: a prior Cancel wins and discards all prepared hits.
            completion = std::move(value);
            terminal = true;
            return true;
        }

        [[nodiscard]] bool IsTerminal() const noexcept {
            std::lock_guard lock(terminalMutex);
            return terminal;
        }

    private:
        std::vector<PhysicsQueryCommand> commands_;
        std::shared_ptr<PhysicsQueryEventCapabilityState> access_;
        mutable std::mutex terminalMutex;
        bool terminal{};
        const ErrorCodeDescriptor *failureCode{};
        std::optional<Error> failure;
        std::shared_ptr<const PhysicsQueryBatchCompletion> completion;
    };

    /** @brief Keeps completed events and their world-scoped access registrations together. */
    struct PhysicsQueryEventState final {
        explicit PhysicsQueryEventState(const PhysicsWorldSettings &settings)
            : events(settings.Values().budgets.maximumEvents, settings.Values().budgets.maximumInFlightPairs,
                     settings.Values().budgets.eventOverflow) {}

        Detail::PhysicsEventProjection events;
        std::vector<std::weak_ptr<PhysicsQueryEventCapabilityState>> capabilities;
        std::uint64_t nextCapabilityGeneration{1};
    };

    /** @brief Owner-thread batch admission bookkeeping; the terminal handle has its own synchronization. */
    struct PhysicsQueryBatchAdmissionState final {
        std::shared_ptr<PhysicsQueryBatchState> pending;
        std::uint64_t tick{};
        std::uint32_t admissions{};
    };

    /** @brief Serializes one bounded completed-tick value between its owner writer and foreign snapshot readers. */
    class PhysicsPublicationState final {
    public:
        [[nodiscard]] PhysicsPublishedTick Snapshot() const noexcept {
            Detail::PublicationGuard guard{lock_};
            return value_;
        }

        [[nodiscard]] Result<void> CheckRevisionCapacity() const {
            if (Snapshot().publicationRevision == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
            return Result<void>::Success();
        }

        void Reset() noexcept {
            Detail::PublicationGuard guard{lock_};
            value_ = {};
        }

        void InvalidateQueryEvents() noexcept {
            Detail::PublicationGuard guard{lock_};
            if (value_.completedTick == 0)
                return;
            // An immediate structural edit changes query results without completing a new event tick.
            ++value_.publicationRevision;
            value_.eventTick = 0;
            value_.eventCount = 0;
            value_.droppedEventCount = 0;
        }

        void Commit(const std::uint64_t tick, const std::uint32_t appliedCommands,
                    const Detail::PhysicsEventProjectionResult &eventResult) noexcept {
            Detail::PublicationGuard guard{lock_};
            const std::uint64_t revision = value_.publicationRevision + 1;
            value_ = {.completedTick = tick,
                      .publicationRevision = revision,
                      .transformTick = tick,
                      .queryTick = tick,
                      .eventTick = tick,
                      .appliedCommands = appliedCommands,
                      .eventCount = eventResult.publishedRecordCount,
                      .droppedEventCount = eventResult.droppedRecordCount};
        }

    private:
        // The owner thread writes; foreign readers snapshot through this same bounded guard.
        mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;  // NOSONAR(cpp:S8379) This guards value_.
        PhysicsPublishedTick value_{};
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

        /** @brief World-owned bounded scratch and retirement notifications for body quarantine. */
        struct PhysicsContainmentState final {
            CanonicalRetirementSink quarantineSink;
            std::vector<BodyHandle> quarantined;
            std::vector<PhysicsCommandOrderKey> retiredCommands;
        };

        /** @brief Retained failure evidence and per-tick non-finite diagnostic precedence. */
        struct PhysicsDiagnosticState final {
            std::optional<Error> lastFailure;
            std::optional<PhysicsDiagnosticRecord> lastDiagnostic;
            std::uint64_t nonFiniteDiagnosticTick{};
        };
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
              sourceOrder(worldSettings.Values().budgets.maximumCommands), queryEvents(worldSettings) {
            containment.quarantined.reserve(worldSettings.Values().world.capacity.maximumBodies);
            containment.retiredCommands.reserve(worldSettings.Values().budgets.maximumCommands);
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
            const bool terminalFailure = state == PhysicsWorldState::Failed;
            Detail::DestroyCanonicalWorld(native);
            native = {};
            state = PhysicsWorldState::Destroyed;
            if (!terminalFailure) {
                lifecycleCause = cause;
                diagnostics.lastFailure.reset();
                diagnostics.lastDiagnostic.reset();
            }
            std::ranges::fill(commands, PhysicsStructuralCommand{});
            commandHead = 0;
            commandCount = 0;
            containment.retiredCommands.clear();
            queryEvents.events.Reset();
            statistics.pendingCommands = 0;
            std::erase(runtime->identities, &identity);
            runtime->ReleaseNativeWhenIdle();
        }

        void RecordDiagnostic(const Error &error, const std::uint64_t sceneGeneration, const std::uint64_t simulationTick) {
            if (diagnostics.nonFiniteDiagnosticTick == simulationTick && diagnostics.lastDiagnostic.has_value())
                return;
            const auto context = Detail::DiagnosticContext(identity, sceneGeneration, simulationTick);
            const auto record = MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, error, context);
            if (record.HasValue())
                diagnostics.lastDiagnostic = record.Value();
        }

        void RecordEventDropDiagnostic(const std::uint64_t sceneGeneration, const std::uint64_t simulationTick, const bool overflowed) {
            if (diagnostics.nonFiniteDiagnosticTick == simulationTick && diagnostics.lastDiagnostic.has_value())
                return;
            const auto context = Detail::DiagnosticContext(identity, sceneGeneration, simulationTick);
            const auto error =
                overflowed ? MakeError(PhysicsErrors::CapacityExceeded, "Physics event projection dropped bounded records.")
                           : MakeError(PhysicsErrors::DescriptorInvalid, "Physics event projection dropped invalid contact evidence.");
            const auto record = MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Event, error, context);
            if (record.HasValue())
                diagnostics.lastDiagnostic = record.Value();
        }

        void Fail(Error error, const std::uint64_t sceneGeneration, const std::uint64_t simulationTick) {
            using enum PhysicsWorldState;
            if (state == Failed || state == Destroyed)
                return;
            state = Failed;
            lifecycleCause = PhysicsWorldLifecycleCause::FatalSolverError;
            InvalidateQueryEventCapabilities();
            queryEvents.events.AbortTick();
            RecordDiagnostic(error, sceneGeneration, simulationTick);
            diagnostics.lastFailure = std::move(error);
        }

        void RecordBodyDiagnostic(const Error &error, const Detail::CanonicalNonFiniteBody &body, const std::uint64_t sceneGeneration,
                                  const std::uint64_t simulationTick) {
            if (diagnostics.nonFiniteDiagnosticTick == simulationTick && diagnostics.lastDiagnostic.has_value())
                return;
            using enum PhysicsDiagnosticContextKey;
            std::array<PhysicsDiagnosticContextEntry, 5> context{PhysicsDiagnosticContextEntry{.key = World, .value = identity},
                                                                 PhysicsDiagnosticContextEntry{.key = Body, .value = body.body},
                                                                 PhysicsDiagnosticContextEntry{.key = SceneGeneration,
                                                                                               .value = sceneGeneration},
                                                                 PhysicsDiagnosticContextEntry{.key = SimulationTick,
                                                                                               .value = simulationTick},
                                                                 PhysicsDiagnosticContextEntry{.key = SceneEntity,
                                                                                               .value = body.sceneEntity}};
            const auto record =
                MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, error, {context.data(), body.sceneEntity == 0 ? 4U : 5U});
            if (record.HasValue()) {
                diagnostics.lastDiagnostic = record.Value();
            }
        }

        void RecordNonFiniteDiagnostic(const Error &error, const Detail::CanonicalNonFiniteBody &body, const std::uint64_t sceneGeneration,
                                       const std::uint64_t simulationTick) {
            RecordBodyDiagnostic(error, body, sceneGeneration, simulationTick);
            diagnostics.nonFiniteDiagnosticTick = simulationTick;
        }

        void RecordAdmissionDiagnostic(const Error &error, const std::uint64_t sceneEntity) {
            using enum PhysicsDiagnosticContextKey;
            const std::array context{PhysicsDiagnosticContextEntry{.key = World, .value = identity},
                                     PhysicsDiagnosticContextEntry{.key = SceneEntity, .value = sceneEntity}};
            const auto record =
                MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, error, {context.data(), sceneEntity == 0 ? 1U : 2U});
            if (record.HasValue())
                diagnostics.lastDiagnostic = record.Value();
        }

        void RecordMutationDiagnostic(const Error &error, const PhysicsStructuralCommand &command) {
            if (!command.bodyMutation.has_value()) {
                RecordDiagnostic(error, command.order.sceneGeneration, command.order.simulationTick);
                return;
            }
            const auto body = command.bodyMutation->body;
            RecordBodyDiagnostic(error, {body, Detail::CanonicalSceneEntity(native, body)}, command.order.sceneGeneration,
                                 command.order.simulationTick);
        }

        void ClearForReset() noexcept {
            InvalidateQueryEventCapabilities();
            identity = {};
            std::ranges::fill(commands, PhysicsStructuralCommand{});
            commandHead = 0;
            commandCount = 0;
            containment.retiredCommands.clear();
            activeTick = 0;
            querySceneGeneration = 0;
            queryBatch.tick = 0;
            queryBatch.admissions = 0;
            commandOrderDirty = false;
            stepping = false;
            queryEvents.events.Reset();
            publication.Reset();
            statistics = {};
            diagnostics.lastFailure.reset();
            diagnostics.lastDiagnostic.reset();
            diagnostics.nonFiniteDiagnosticTick = 0;
            lifecycleCause = PhysicsWorldLifecycleCause::Reset;
        }

        void InvalidateQueryEventCapabilities() noexcept {
            if (queryBatch.pending) {
                (void)queryBatch.pending->FailCode(PhysicsErrors::CapabilityStale);
                queryBatch.pending.reset();
            }
            for (const auto &weak : queryEvents.capabilities) {
                if (const auto access = weak.lock()) {
                    access->stale = true;
                    access->world = nullptr;
                }
            }
            queryEvents.capabilities.clear();
        }

        [[nodiscard]] Result<void> CheckPublicationRevisionCapacity() const {
            return publication.CheckRevisionCapacity();
        }

        void InvalidateQueryEventPublication() noexcept {
            publication.InvalidateQueryEvents();
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
                    diagnostics.lastFailure = created.ErrorValue();
                    return Result<void>::Failure(created.ErrorValue());
                }
                native = created.Value();
                state = PreparedSolver;
                return Result<void>::Success();
            } catch (const std::bad_alloc &) {
                Error error = MakeError(PhysicsErrors::CapacityExceeded, "Unable to rebuild Physics world ownership state during reset.");
                state = Failed;
                diagnostics.lastFailure = error;
                return Result<void>::Failure(std::move(error));
            }
        }

        [[nodiscard]] PhysicsStructuralCommand &CommandAt(const std::uint32_t offset) noexcept {
            return commands[(commandHead + offset) % commands.size()];
        }

        [[nodiscard]] const PhysicsStructuralCommand &CommandAt(const std::uint32_t offset) const noexcept {
            return commands[(commandHead + offset) % commands.size()];
        }

        [[nodiscard]] bool IsRetiredCommand(const PhysicsCommandOrderKey &key) const noexcept {
            return std::ranges::find(containment.retiredCommands, key) != containment.retiredCommands.end();
        }

        void DiscardCommands(const std::uint32_t discarded) noexcept {
            if (discarded == 0)
                return;
            for (std::uint32_t index = 0; index < discarded; ++index)
                std::erase(containment.retiredCommands, CommandAt(index).order);
            commandHead = (commandHead + discarded) % commands.size();
            commandCount -= discarded;
            statistics.pendingCommands = commandCount - static_cast<std::uint32_t>(containment.retiredCommands.size());
        }

        std::shared_ptr<PhysicsRuntime::Impl> runtime;
        PhysicsWorldSettings settings;
        Detail::PhysicsContainmentState containment;
        Detail::PhysicsDiagnosticState diagnostics;
        PhysicsWorldState state{PhysicsWorldState::Preparing};
        PhysicsWorldId identity;
        Detail::CanonicalWorldHandle native;
        std::vector<PhysicsStructuralCommand> commands;
        std::vector<std::uint32_t> sourceOrder;
        PhysicsQueryBatchAdmissionState queryBatch;
        PhysicsQueryEventState queryEvents;
        std::uint32_t commandHead{};
        std::uint32_t commandCount{};
        std::uint64_t activeTick{};
        std::uint64_t querySceneGeneration{};
        bool commandOrderDirty{};
        bool stepping{};
        PhysicsPublicationState publication;
        PhysicsTickStatistics statistics;
        PhysicsWorldLifecycleCause lifecycleCause{PhysicsWorldLifecycleCause::None};
    };

    /** @brief Private deterministic corruption seam used only by containment tests. */
    struct PhysicsWorldContainmentTestAccess final {
        [[nodiscard]] static bool Inject(const PhysicsWorld &world, const BodyHandle body, const float value,
                                         const std::uint8_t component = 0, const bool postStep = true) noexcept {
            return world.InjectNonFiniteBodyForTesting(body, value, component, postStep);
        }
    };
}  // namespace Horo::Physics
