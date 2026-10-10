#include "CharacterWorldCommandInternal.h"

#include <chrono>

namespace Horo::Character {
    namespace {
        [[nodiscard]] std::uint64_t OverflowTotal(const auto &impl) noexcept {
            const auto fastPath = impl.fastPath.Snapshot();
            return impl.commandOverflowCount.load() + fastPath.contactOverflowCount + fastPath.hitOverflowCount +
                   fastPath.eventOverflowCount + fastPath.impulseOverflowCount + fastPath.scratchOverflowCount;
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
        if (const auto owner = Detail::RequireOwnerThread(impl_->ownerThread); owner.HasError())
            return owner;
        if (input.metrics != nullptr)
            input.metrics->snapshot = {};
        if (impl_->state.load() != CharacterWorldState::Active || impl_->ticking.load())
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        if (input.tick == 0 || input.sceneGeneration != impl_->descriptor.sceneGeneration || input.fixedDelta <= Duration{} ||
            input.tick != impl_->closedTick.load() + 1)
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));

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

        impl_->fastPath.Canonicalize();
        Detail::PublishTick(*impl_, input, applied.Value());
        impl_->completedTicks.fetch_add(1);
        Detail::ObservePhase(input, CharacterTickPhase::PublishCompletedTick);
        metrics.FinishPhase(CharacterMetricPhase::PublishCompletedTick);
        metrics.Commit(impl_->published.publicationRevision);
        return Result<void>::Success();
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
