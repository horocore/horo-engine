#include "CharacterWorldCommandInternal.h"

#include <chrono>
#include <exception>

namespace Horo::Character {
    namespace {
        [[nodiscard]] std::uint64_t OverflowTotal(const auto &impl) noexcept {
            const auto fastPath = impl.fastPath.Snapshot();
            return impl.commandOverflowCount.load() + fastPath.contactOverflowCount + fastPath.hitOverflowCount +
                   fastPath.eventOverflowCount + fastPath.impulseOverflowCount + fastPath.scratchOverflowCount;
        }

        /** @brief Validate the owner/input frame before taking the non-reentrant tick guard or touching queues. */
        [[nodiscard]] Result<void> ValidateTickAdmission(const auto &impl, const CharacterFixedTickInput &input) {
            if (const auto owner = Detail::RequireOwnerThread(impl.ownerThread); owner.HasError())
                return owner;
            if (input.metrics != nullptr)
                input.metrics->snapshot = {};
            if (impl.state.load() != CharacterWorldState::Active || impl.ticking.load() || impl.placementActive)
                return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
            if (input.tick == 0 || input.sceneGeneration != impl.descriptor.sceneGeneration || input.fixedDelta <= Duration{} ||
                input.tick != impl.closedTick.load() + 1)
                return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));

            return Result<void>::Success();
        }

        /** @brief Finalizes one optional attempt even when command resolution exits early. */
        template <typename Impl> class MetricAttempt final {
        public:
            MetricAttempt(Impl &impl, CharacterMetricCapture *capture, const std::uint64_t tick) : impl_(impl), capture_(capture) {
                if (capture_ == nullptr)
                    return;
                const auto registryLock = impl_.synchronization.LockRegistry();
                const auto active = impl_.controllers.Statistics().active;
                capture_->snapshot = {.world = impl_.descriptor.identity,
                                      .sceneGeneration = impl_.descriptor.sceneGeneration,
                                      .tick = tick,
                                      .activeControllers = static_cast<std::uint32_t>(active)};
                phaseStart_ = Clock::now();
            }

            MetricAttempt(const MetricAttempt &) = delete;
            MetricAttempt &operator=(const MetricAttempt &) = delete;
            MetricAttempt(MetricAttempt &&) = delete;
            MetricAttempt &operator=(MetricAttempt &&) = delete;

            ~MetricAttempt() {
                if (capture_ == nullptr)
                    return;
                capture_->snapshot.failed = !committed_;
                capture_->snapshot.overflows = OverflowTotal(impl_);
            }

            void FinishPhase(const CharacterMetricPhase phase) noexcept {
                if (capture_ == nullptr)
                    return;
                const auto now = Clock::now();
                capture_->snapshot.phaseSeconds[static_cast<std::size_t>(phase)] = std::chrono::duration<double>(now - phaseStart_).count();
                capture_->snapshot.phaseCompleted[static_cast<std::size_t>(phase)] = true;
                phaseStart_ = now;
            }

            void Commit(const std::uint64_t publicationRevision) noexcept {
                if (capture_ == nullptr)
                    return;
                capture_->snapshot.publicationRevision = publicationRevision;
                committed_ = true;
            }

        private:
            using Clock = std::chrono::steady_clock;
            Impl &impl_;
            CharacterMetricCapture *capture_{};
            Clock::time_point phaseStart_{};
            bool committed_{};
        };
    }  // namespace

    /** @copydoc CharacterWorld::QueueMovementCommand */
    Result<CharacterCommandAdmission> CharacterWorld::QueueMovementCommand(const CharacterMovementRequest &request) {
        return QueueScopedMovementCommand(request, {});
    }

    /** @copydoc CharacterWorld::QueueScopedMovementCommand */
    Result<CharacterCommandAdmission> CharacterWorld::QueueScopedMovementCommand(const CharacterMovementRequest &request,
                                                                                 const CancellationToken &revocation) {
        if (revocation.IsCancellationRequested())
            return Result<CharacterCommandAdmission>::Failure(MakeError(CharacterErrors::CapabilityRevoked));
        const auto rejected = [this](const CharacterCommandAdmissionStatus status) {
            impl_->rejectedCommands.fetch_add(1);
            return Result<CharacterCommandAdmission>::Success({status, impl_->pendingCommands.load()});
        };
        if (const auto valid = Detail::ValidateAdmissionRequest(*impl_, request); valid.HasError()) {
            impl_->rejectedCommands.fetch_add(1);
            return Result<CharacterCommandAdmission>::Failure(valid.ErrorValue());
        }

        if (auto queueLock = impl_->synchronization.TryLockCommands(); queueLock.owns_lock()) {
            if (const auto registryLock = impl_->synchronization.TryLockRegistry(); registryLock.owns_lock()) {
                if (const auto valid = Detail::ValidateLockedAdmission(*impl_, request); valid.HasError()) {
                    impl_->rejectedCommands.fetch_add(1);
                    return Result<CharacterCommandAdmission>::Failure(valid.ErrorValue());
                }
                if (impl_->fastPath.Commands().size() >= impl_->settings.Values().capacities.maximumQueuedCommands) {
                    impl_->commandOverflowCount.fetch_add(1);
                    return rejected(CharacterCommandAdmissionStatus::RejectedFull);
                }

                impl_->fastPath.Commands().emplace_back(request, revocation);
                const auto depth = static_cast<std::uint32_t>(impl_->fastPath.Commands().size());
                impl_->pendingCommands.store(depth);
                impl_->maximumCommandDepth.store(std::max(depth, impl_->maximumCommandDepth.load()));
                impl_->admittedCommands.fetch_add(1);
                return Result<CharacterCommandAdmission>::Success({CharacterCommandAdmissionStatus::Deferred, depth});
            }
        }
        return rejected(CharacterCommandAdmissionStatus::RejectedBusy);
    }

    /** @copydoc CharacterWorld::AdvanceFixedTick */
    Result<void> CharacterWorld::AdvanceFixedTick(const CharacterFixedTickInput &input) {
        if (const auto admission = ValidateTickAdmission(*impl_, input); admission.HasError())
            return admission;

        Detail::TickGuard ticking{*impl_};
        MetricAttempt metrics{*impl_, input.metrics, input.tick};
        impl_->fastPath.ResetTransient();
        impl_->tickQueries = 0;
        if (const auto frozen = Detail::FreezeCommandFrame(*impl_, input); frozen.HasError())
            return frozen;

        Detail::ObservePhase(input, CharacterTickPhase::FreezeCommands);
        metrics.FinishPhase(CharacterMetricPhase::FreezeCommands);
        const auto applied = Detail::ApplyCommandFrame(*impl_, input);
        if (applied.HasError())
            return Result<void>::Failure(applied.ErrorValue());
        if (impl_->state.load() != CharacterWorldState::Active)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        Detail::ObservePhase(input, CharacterTickPhase::ResolveMovement);
        metrics.FinishPhase(CharacterMetricPhase::ResolveMovement);
        if (impl_->state.load() != CharacterWorldState::Active)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));

        impl_->fastPath.Canonicalize();
        {
            const auto publicationLock = impl_->synchronization.LockPublication();
            Detail::PublishTick(*impl_, input.tick, applied.Value());
        }
        impl_->completedTicks.fetch_add(1);
        Detail::ObservePhase(input, CharacterTickPhase::PublishCompletedTick);
        metrics.FinishPhase(CharacterMetricPhase::PublishCompletedTick);
        metrics.Commit(impl_->published.publicationRevision);
        return Result<void>::Success();
    }

    /** @copydoc Detail::CharacterSceneFixedAccess::Stage */
    Result<void> Detail::CharacterSceneFixedAccess::Stage(CharacterWorld &world, const CharacterSceneTickAttempt attempt,
                                                          const CharacterSceneFixedInput &source) {
        auto &impl = *world.impl_;
        const CharacterFixedTickInput input{.tick = attempt.tick,
                                            .sceneGeneration = impl.descriptor.sceneGeneration,
                                            .fixedDelta = source.delta,
                                            .query = source.query,
                                            .platformTargets = source.platformTargets};
        if (const auto valid = ValidateTickAdmission(impl, input); valid.HasError())
            return valid;
        if (attempt.attempt <= impl.lastSceneAttempt)
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));
        impl.lastSceneAttempt = attempt.attempt;

        TickGuard ticking{impl};
        impl.fastPath.ResetTransient();
        impl.tickQueries = 0;
        if (const auto frozen = FreezeCommandFrame(impl, input, true); frozen.HasError())
            return frozen;
        const auto resolved = ResolveCommandFrame(impl, input);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        if (std::ranges::any_of(impl.fastPath.MovementResults(), [attempt](const auto &movement) {
            return movement.rootMotion.has_value() && movement.rootMotion->source.attempt != attempt.attempt;
        }))
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));

        const auto registryLock = impl.synchronization.LockRegistry();
        const auto publicationLock = impl.synchronization.LockPublication();
        if (const auto valid = PreflightCommandFrame(impl, attempt.tick); valid.HasError())
            return valid;
        impl.stagedSceneTick = StagedCharacterSceneTick{attempt, resolved.Value()};
        ticking.RetainForScene();
        return Result<void>::Success();
    }

    /** @copydoc Detail::CharacterSceneFixedAccess::ReadRootMotion */
    Result<std::optional<CharacterRootMotionFeedback>> Detail::CharacterSceneFixedAccess::ReadRootMotion(
        const CharacterWorld &world, const CharacterSceneTickAttempt attempt, const CharacterControllerHandle controller) {
        using Output = Result<std::optional<CharacterRootMotionFeedback>>;
        const auto &impl = *world.impl_;
        if (const auto owner = RequireOwnerThread(impl.ownerThread); owner.HasError())
            return Output::Failure(owner.ErrorValue());
        if (!impl.stagedSceneTick || impl.stagedSceneTick->attempt != attempt)
            return Output::Failure(MakeError(CharacterErrors::InvalidState));
        const auto registryLock = impl.synchronization.LockRegistry();
        if (const auto record = impl.controllers.Resolve(controller); record.HasError())
            return Output::Failure(record.ErrorValue());

        const auto &results = impl.fastPath.MovementResults();
        const auto found = std::ranges::lower_bound(results, controller, {}, &CharacterMovementResult::controller);
        if (found == results.end() || found->controller != controller)
            return Output::Success(std::nullopt);
        return Output::Success(found->rootMotion);
    }

    /** @copydoc Detail::CharacterSceneFixedAccess::Commit */
    void Detail::CharacterSceneFixedAccess::Commit(CharacterWorld &world, const CharacterSceneTickAttempt attempt) noexcept {
        auto &impl = *world.impl_;
        if (impl.ownerThread != std::this_thread::get_id() || !impl.stagedSceneTick || impl.stagedSceneTick->attempt != attempt)
            std::terminate();
        {
            const auto queueLock = impl.synchronization.LockCommands();
            const auto registryLock = impl.synchronization.LockRegistry();
            const auto publicationLock = impl.synchronization.LockPublication();
            CloseCommandFrame(impl, attempt.tick);
            if (const auto committed = CommitPreflightedFrame(impl, attempt.tick); committed.HasError())
                std::terminate();
            impl.fastPath.Canonicalize();
            PublishTick(impl, attempt.tick, impl.stagedSceneTick->applied);
        }
        impl.completedTicks.fetch_add(1);
        impl.stagedSceneTick.reset();
        ReleaseTick(impl);
    }

    /** @copydoc Detail::CharacterSceneFixedAccess::Abort */
    void Detail::CharacterSceneFixedAccess::Abort(CharacterWorld &world, const CharacterSceneTickAttempt attempt) noexcept {
        auto &impl = *world.impl_;
        if (impl.ownerThread != std::this_thread::get_id() || !impl.stagedSceneTick || impl.stagedSceneTick->attempt != attempt)
            return;
        impl.stagedSceneTick.reset();
        impl.fastPath.ResetTransient();
        ReleaseTick(impl);
    }

    /** @copydoc CharacterWorld::PublishedTick */
    CharacterPublishedTick CharacterWorld::PublishedTick() const noexcept {
        const auto publicationLock = impl_->synchronization.LockPublication();
        return impl_->published;
    }

    /** @copydoc CharacterWorld::TickStatistics */
    CharacterTickStatistics CharacterWorld::TickStatistics() const noexcept {
        const auto fastPath = impl_->fastPath.Snapshot();
        return {.completedTicks = impl_->completedTicks.load(),
                .admittedCommands = impl_->admittedCommands.load(),
                .rejectedCommands = impl_->rejectedCommands.load(),
                .pendingCommands = impl_->pendingCommands.load(),
                .maximumCommandDepth = impl_->maximumCommandDepth.load(),
                .commandOverflowCount = impl_->commandOverflowCount.load(),
                .contactOverflowCount = fastPath.contactOverflowCount,
                .hitOverflowCount = fastPath.hitOverflowCount,
                .eventOverflowCount = fastPath.eventOverflowCount,
                .impulseOverflowCount = fastPath.impulseOverflowCount,
                .scratchOverflowCount = fastPath.scratchOverflowCount,
                .invalidInputCount = fastPath.invalidInputCount,
                .retainedContacts = fastPath.retainedContacts,
                .queuedHits = fastPath.queuedHits,
                .queuedEvents = fastPath.queuedEvents,
                .scratchBytesUsed = fastPath.scratchBytesUsed};
    }

    /** @copydoc CharacterWorld::Shutdown */
}  // namespace Horo::Character
